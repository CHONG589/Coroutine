#include "timer.h"
#include "util.h"

bool Timer::cancel() {
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if(m_cb) {
        m_cb = nullptr;
        //从小根堆中删除这个节点。
        auto it = m_manager->m_timers.find(shared_from_this());
        m_manager->m_timers.erase(it);
        return true;
    }
    return false;
}

bool Timer::refresh() {
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if(!m_cb) {
        return false;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if(it == m_manager->m_timers.end()) {
        return false;
    }
    m_manager->m_timers.erase(it);
    //删除要刷新的节点后，设置好时间后重新插入到小根堆中
    m_next = GetElapsedMS() + m_ms;
    m_manager->m_timers.insert(shared_from_this());
    return true;
}

bool Timer::reset(uint64_t ms, bool from_now) {
    if(ms == m_ms && !from_now) {
        return true;
    }
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if(!m_cb) {
        return false;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if(it == m_manager->m_timers.end()) {
        return false;
    }
    m_manager->m_timers.erase(it);
    uint64_t start = 0;
    if(from_now) {
        start = GetElapsedMS();
    } else {
        //这里可能是因为有一个构造函数是直接以最终执行时间
        //m_next 为参数直接构造的，所以这里需要判断是否是
        //从当前时间开始计算的，是的话就start等于当前时间，
        //不是时 start 等于最终执行时间减去 m_ms 才开始计算。
        start = m_next - m_ms;
    }
    m_ms = ms;
    m_next = start + m_ms;
    m_manager->addTimer(shared_from_this(), lock);
    return true;

}

Timer::Timer(uint64_t ms, std::function<void()> cb,
             bool recurring, TimerManager* manager)
    :m_recurring(recurring)
    ,m_ms(ms)
    ,m_cb(cb)
    ,m_manager(manager) {
    m_next = GetElapsedMS() + m_ms;
}

Timer::Timer(uint64_t next)
    :m_next(next) {
}

bool Timer::Comparator::operator()(const Timer::ptr& lhs
            , const Timer::ptr& rhs) const {

    if(!lhs && !rhs) {
        return false;
    }
    if(!lhs) {
        return true;
    }
    if(!rhs) {
        return false;
    }
    if(lhs->m_next < rhs->m_next) {
        return true;
    }
    if(rhs->m_next < lhs->m_next) {
        return false;
    }
        return lhs.get() < rhs.get();
    }

TimerManager::TimerManager() {
    m_previouseTime = GetElapsedMS();
}

TimerManager::~TimerManager() {
}

Timer::ptr TimerManager::addTimer(uint64_t ms, std::function<void()> cb
                                  ,bool recurring) {
    Timer::ptr timer(new Timer(ms, cb, recurring, this));
    RWMutexType::WriteLock lock(m_mutex);
    addTimer(timer, lock);
    return timer;
}

static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb) {
    std::shared_ptr<void> tmp = weak_cond.lock();
    if(tmp) {
        cb();
    }
}

Timer::ptr TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb
                                    ,std::weak_ptr<void> weak_cond
                                    ,bool recurring) {
    return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
}

uint64_t TimerManager::getNextTimer() {
    RWMutexType::ReadLock lock(m_mutex);
    m_tickled = false;
    if(m_timers.empty()) {
        return ~0ull;
    }

    const Timer::ptr& next = *m_timers.begin();
    uint64_t now_ms = GetElapsedMS();
    if(now_ms >= next->m_next) {
        return 0;
    } else {
        return next->m_next - now_ms;
    }
}

void TimerManager::listExpiredCb(std::vector<std::function<void()> >& cbs) {
    uint64_t now_ms = GetElapsedMS();
    std::vector<Timer::ptr> expired;
    {
        RWMutexType::ReadLock lock(m_mutex);
        if(m_timers.empty()) {
            return;
        }
    }
    RWMutexType::WriteLock lock(m_mutex);
    if(m_timers.empty()) {
        return;
    }
    bool rollover = false;
    if(detectClockRollover(now_ms)) {
        // 使用clock_gettime(CLOCK_MONOTONIC_RAW)，应该不可能出现时间回退的问题
        rollover = true;
    }
    if(!rollover && ((*m_timers.begin())->m_next > now_ms)) {
        //全部都没超时，所以不需要处理
        return;
    }

    Timer::ptr now_timer(new Timer(now_ms));
    //将it 指向第一个没超时的下标节点，即第一个 next_timer->m_next >= now_ms
    auto it = rollover ? m_timers.end() : m_timers.lower_bound(now_timer);
    //而其实next_timer == now_timer 也算是超时了的，而且它只是指向了第一个相等
    //的节点，后面可能还有多个，所以如果后面还有相等的，也要包括进去。
    while(it != m_timers.end() && (*it)->m_next == now_ms) {
        ++it;
    }
    //将m_timers到it之间的节点加入到expired中。
    expired.insert(expired.begin(), m_timers.begin(), it);
    m_timers.erase(m_timers.begin(), it);

    //上面已经将超时的节点已经找出来了，下面只需将回调函数提取
    //出来。
    //传过来接收的cbs是一个指针，所以要为它分配空间，大小即为
    //expired.size()。
    cbs.reserve(expired.size());

    for(auto& timer : expired) {
        cbs.push_back(timer->m_cb);
        if(timer->m_recurring) {
            //如果是循环定时器，因为该定时器已经超时，这时
            //需要重新设置超时时间
            timer->m_next = now_ms + timer->m_ms;
            //重复利用这个Timer，而不是直接销毁。
            m_timers.insert(timer);
        } else {
            timer->m_cb = nullptr;
        }
    }
}

bool TimerManager::hasTimer() {
    RWMutexType::ReadLock lock(m_mutex);
    return !m_timers.empty();
}

void TimerManager::addTimer(Timer::ptr val, RWMutexType::WriteLock& lock) {
    //这句话的意思是将 val 插入到 m_timers 中，插入完后，
    //因为这时是属于一个整体，即 m_timers，而 it 是要获取
    //定时器节点，而它处于 set 的first 位置，所以要 .first
    //取出。
    auto it = m_timers.insert(val).first;
    //因为插入前第一个节点（最小的超时时间）还没过期，从 !m_tickled 
    //可知还没过期，但是现在你插入后，比最小的还小，说明要更新定时器，
    //因为你比它还早过期。所以at_front = true。
    bool at_front = (it == m_timers.begin()) && !m_tickled;
    if(at_front) {
        m_tickled = true;
    }
    lock.unlock();

    if(at_front) {
        onTimerInsertedAtFront();
    }
}

bool TimerManager::detectClockRollover(uint64_t now_ms) {
    bool rollover = false;
    if(now_ms < m_previouseTime &&
            now_ms < (m_previouseTime - 60 * 60 * 1000)) {
        rollover = true;
    }
    m_previouseTime = now_ms;
    return rollover;
}
