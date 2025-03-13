#include <assert.h>

#include "scheduler.h"
#include "util.h"

// 当前线程的调度器，同一个调度器下的所有线程共享同一个实例
static thread_local Scheduler *t_scheduler = nullptr;
// 当前线程的调度协程，每个线程都独有一份
static thread_local Fiber *t_scheduler_fiber = nullptr;

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name) {
    assert(threads > 0);

    m_useCaller = use_caller;
    m_name      = name;

    if (use_caller) {
        --threads;
        //因为是首次创建，所以这里是创建主协程，直接调用GetThis() 
        //会创建。
        Fiber::GetThis();
        assert(GetThis() == nullptr);
        t_scheduler = this;

        //main 线程的调度协程，调度协程没有栈空间和run_in_schedule 为 false
        m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));

        Thread::SetName(m_name);
        t_scheduler_fiber = m_rootFiber.get();
        m_rootThread      = GetThreadId();
        // 将线程 id 加入到线程池。
        m_threadIds.push_back(m_rootThread);
    } 
    else {
        // 非caller线程的调度器，-1 表示不指定线程
        m_rootThread = -1;
    }
    m_threadCount = threads;
}

Scheduler *Scheduler::GetThis() { 
    return t_scheduler; 
}

Fiber *Scheduler::GetMainFiber() { 
    return t_scheduler_fiber;
}

// 设置当前线程的调度器
void Scheduler::setThis() {
    t_scheduler = this;
}

Scheduler::~Scheduler() {
    LOG_DEBUG("Scheduler::~Scheduler %s is deleting", m_name.c_str());
    assert(m_stopping);
    if (GetThis() == this) {
        t_scheduler = nullptr;
    }
}

// 启动调度器，对调度器进行一些列的初始化（初始化线程池）。
void Scheduler::start() {
    LOG_DEBUG("Scheduler::start %s", m_name.c_str());
    MutexType::Lock lock(m_mutex);
    if (m_stopping) {
        LOG_ERROR("Scheduler::start %s error", m_name);
        return;
    }
    // 线程池是否为空
    assert(m_threads.empty());
    // 重新设置线程池的大小
    m_threads.resize(m_threadCount);
    for (size_t i = 0; i < m_threadCount; i++) {
        // 对每一个线程绑定 run 函数，并且命名名字，加入到线程池。
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this),
                                      m_name + "_" + std::to_string(i)));
        m_threadIds.push_back(m_threads[i]->getId());
    }
}

bool Scheduler::stopping() {
    MutexType::Lock lock(m_mutex);
    return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
}

void Scheduler::tickle() { 
    LOG_DEBUG("tickle scheduler..."); 
}

void Scheduler::idle() {
    LOG_DEBUG("Scheduler::idle...");
    while (!stopping()) {
        Fiber::GetThis()->yield();
    }
}

void Scheduler::stop() {
    LOG_DEBUG("Scheduler::stop...");
    if (stopping()) {
        // 满足停止条件
        return ;
    }
    // 标记停止状态
    m_stopping = true;

    // 如果use caller，那只能由caller线程发起stop
    if (m_useCaller) {
        // this表示当前调用stop()的线程，GetThis()表示获取
        // 创建该调度器的线程，即 main。判断是否相等。
        assert(GetThis() == this);
    } 
    else {
        //只有当所有的调度线程都结束后，调度器才算真正停止
        assert(GetThis() != this);
    }

    // 通知所有线程进行调度，让它们退出循环，并等待所有线程结束。
    for (size_t i = 0; i < m_threadCount; i++) {
        tickle();
    }

    // 如果有主协程，则通知主协程结束
    if (m_rootFiber) {
        tickle();
    }

    // 如果 caller 线程的调度协程存在，这里还要进行调度完，即切换到任务协程去消耗任务。
    if (m_rootFiber) {
        m_rootFiber->resume();
        LOG_DEBUG("Scheduler::stop m_rootFiber end");
    }

    std::vector<Thread::ptr> thrs;
    {
        MutexType::Lock lock(m_mutex);
        // 保存所有线程
        thrs.swap(m_threads);
    }
    for (auto &i : thrs) {
        // 等待所有线程结束
        i->join();
    }
}

void Scheduler::run() {
    LOG_DEBUG("Scheduler::run begin");
    //运行到本调度器，设置当前线程的调度器。
    setThis();
    if (GetThreadId() != m_rootThread) {
        //这里相当于初始化t_scheduler_fiber，除了 main 线程的，
        //其它的都要初始化，main 的已经在构造的时候已经初始化完成，
        //其它的在这里才初始化
        t_scheduler_fiber = Fiber::GetThis().get();
    }

    Fiber::ptr idle_fiber(new Fiber(std::bind(&Scheduler::idle, this)));
    Fiber::ptr cb_fiber;

    ScheduleTask task;
    while (true) {
        task.reset();
        bool tickle_me = false;
        {
            MutexType::Lock lock(m_mutex);
            auto it = m_tasks.begin();
            // 遍历所有调度任务
            while (it != m_tasks.end()) {
                if (it->thread != -1 && it->thread != GetThreadId()) {
                    // 任务队列中的任务指定了调度线程，但不是当前线程，跳过这个任务，继续下一个,因为在初始化时
                    // 会指定哪个线程调度，如果 it->thread == -1，说明这个任务没有指定线程。这时也要通知其他
                    // 线程进行调度，直到那个线程来了与 GetThreadId() 相等。
                    ++it;
                    tickle_me = true;
                    continue;
                }

                // 找到一个未指定线程，或是指定了当前线程的任务
                assert(it->fiber || it->cb);
                if (it->fiber) {
                    // 任务队列时的协程一定是READY状态，谁会把
                    // RUNNING或TERM状态的协程加入调度呢？
                    assert(it->fiber->getState() == Fiber::READY);
                }
                // 当前调度线程找到一个任务，准备开始调度，将其从任务队列中剔除，活动线程数加1
                task = *it;
                m_tasks.erase(it++);
                ++m_activeThreadCount;
                break;
            }
            // 当前线程拿完一个任务后，发现任务队列还有剩余，那么tickle一下其他线程
            tickle_me |= (it != m_tasks.end());
        }
        if (tickle_me) {
            tickle();
        }

        if (task.fiber) {
            // resume协程，resume返回时，协程要么执行完了，要么半路yield了，
            //总之这个任务就算完成了，活跃线程数减一
            LOG_INFO("run fiber in scheduler");
            task.fiber->resume();
            --m_activeThreadCount;
            task.reset();
        } 
        else if (task.cb) {
            // task 为回调函数，则创建新的协程，执行回调函数
            if (cb_fiber) {
                // 因为任务 task 是以函数的形式传过来的，所以要创建成协程来执行，
                // 又cb_fiber这个临时协程已经创建过了，所以复用一下这个协程。
                cb_fiber->reset(task.cb);
            } 
            else {
                // 临时协程没有被创建过，那就创建它。
                cb_fiber.reset(new Fiber(task.cb));
            }
            task.reset();
            LOG_INFO("run fun in scheduler");
            cb_fiber->resume();
            --m_activeThreadCount;
            cb_fiber.reset();
        } 
        else {
            // 进到这个分支情况一定是任务队列空了，调度idle协程即可
            if (idle_fiber->getState() == Fiber::TERM) {
                // 如果 idle_fiber 已经终止了，说明调度器已经停止了，则退出循环。
                LOG_DEBUG("Scheduler::run idle fiber term");
                break;
            }
            // 不是 TERM 状态的话，就会一直 resume 到 idle_fiber 协程，然后又在
            // idle_fiber 协程中又 yield 回来，所以这里不需要再次判断 idle_fiber 的状态。
            ++m_idleThreadCount;
            idle_fiber->resume();
            --m_idleThreadCount;
        }
    }
    LOG_DEBUG("Scheduler::run exit");
}
