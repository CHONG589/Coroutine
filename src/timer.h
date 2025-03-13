#ifndef __TIMER_H__
#define __TIMER_H__

#include <memory>
#include <vector>
#include <set>

#include "mutex.h"

class TimerManager;

class Timer : public std::enable_shared_from_this<Timer> {
    friend class TimerManager;
public:
    typedef std::shared_ptr<Timer> ptr;

    //取消定时器
    bool cancel();
    //刷新设置定时器的执行时间
    bool refresh();

    //重置定时器时间, ms 定时器执行间隔时间(毫秒), 
    //from_now 是否从当前时间开始计算
    bool reset(uint64_t ms, bool from_now);

private:
    //ms 定时器执行间隔时间
    //cb 回调函数
    //recurring 是否循环
    //manager 定时器管理器
    Timer(uint64_t ms, std::function<void()> cb,
          bool recurring, TimerManager *manager);

    //直接用具体的执行时间作为参数进行构造
    Timer(uint64_t next);

    //定时器比较仿函数, 因为这是给小根堆比较用的，
    //自然是lhs < rhs 时返回true
    struct Comparator {
        bool operator()(const Timer::ptr& lhs, const Timer::ptr& rhs) const;
    };

private:
    //是否循环定时器
    bool m_recurring = false;
    //执行周期
    uint64_t m_ms = 0;
    //即添加到定时器时，当前时间加超时时间后的
    //时间节点，具体执行时间。
    uint64_t m_next = 0;
    std::function<void()> m_cb;
    //定时器管理器
    TimerManager *m_manager = nullptr;
};

//定时器管理器
class TimerManager {
    friend class Timer;
public:
    //读写锁类型
    typedef RWMutex RWMutexType;

    TimerManager();
    virtual ~TimerManager();
    //添加定时器
    //ms 定时器执行间隔时间
    //cb 定时器回调函数
    //recurring 是否循环定时器
    Timer::ptr addTimer(uint64_t ms, std::function<void()> cb
                        ,bool recurring = false);

    //添加条件定时器, weak_cond 条件
    //sylar支持创建条件定时器，也就是在创建定时器时绑定一个变量，
    //在定时器触发时判断一下该变量是否仍然有效，如果变量无效，那
    //就取消触发。
    Timer::ptr addConditionTimer(uint64_t ms, std::function<void()> cb
                        ,std::weak_ptr<void> weak_cond
                        ,bool recurring = false);

    //到最近一个定时器执行的时间间隔(毫秒)
    uint64_t getNextTimer();
    //获取需要执行的定时器的回调函数列表, cbs 回调函数数组
    void listExpiredCb(std::vector<std::function<void()> >& cbs);
    //是否有定时器
    bool hasTimer();

protected:
    //当有新的定时器插入到定时器的首部,执行该函数
    virtual void onTimerInsertedAtFront() = 0;
    //将定时器添加到管理器中，被public成员中的addTimer调用
    void addTimer(Timer::ptr val, RWMutexType::WriteLock& lock);

private:
    //检测服务器时间是否被调后了
    bool detectClockRollover(uint64_t now_ms);
    static uint64_t GetElapsed() {
        struct timespec ts = {0};
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        return ts.tv_sec * 1000 + ts.tv_nsec / 1000000.0;
    }

private:
    RWMutexType m_mutex;
    // 因为set里的元素总是排序过的，所以总是可以很方便地获取到
    // 当前的最小定时器，至于怎么比较，就由Comparator类来决定。
    std::set<Timer::ptr, Timer::Comparator> m_timers;
    //是否触发onTimerInsertedAtFront函数，这是一个虚函数，
    //由IOManager继承时实现，当新的定时器插入到Timer集合的首
    //部时，TimerManager通过该方法来通知IOManager立刻更新当前
    //的epoll_wait超时，onTimerInsertedAtFront函数内部tick一下
    //epoll_wait 就会立即退出，并重新设置超时时间。
    bool m_tickled = false;
    //上次执行时间
    uint64_t m_previouseTime = 0;
};

#endif
