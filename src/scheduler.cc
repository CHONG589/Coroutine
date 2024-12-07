/**
 * @file scheduler.cc
 * @brief 协程调度器实现
 * @version 0.1
 * @date 2021-06-15
 */
#include <assert.h>
#include "scheduler.h"

/// 当前线程的调度器，同一个调度器下的所有线程共享同一个实例
static thread_local Scheduler *t_scheduler = nullptr;
/// 当前线程的调度协程，每个线程都独有一份
static thread_local Fiber *t_scheduler_fiber = nullptr;

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name) {
    assert(threads > 0);

    m_useCaller = use_caller;
    m_name      = name;

    if (use_caller) {
        --threads;
        // 主线程的调度器，caller线程的主协程,用 GetThis()获取
        sylar::Fiber::GetThis();// 获取主协程
        assert(GetThis() == nullptr);
        t_scheduler = this;

        /**
         * caller线程的主协程不会被线程的调度协程run进行调度，而且，线程的调度协程停止时，应该返回caller线程的主协程
         * 在user caller情况下，把caller线程的主协程暂时保存起来，等调度协程结束时，再resume caller协程
         */
        // 获取调度协程
        m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));

        sylar::Thread::SetName(m_name);
        t_scheduler_fiber = m_rootFiber.get();
        m_rootThread      = sylar::GetThreadId();
        // 将线程 id 加入到线程池。
        m_threadIds.push_back(m_rootThread);
    } else {
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
    SYLAR_ASSERT(m_stopping);
    if (GetThis() == this) {
        t_scheduler = nullptr;
    }
}

// 启动调度器，对调度器进行一些列的初始化（初始化线程池）。
void Scheduler::start() {
    LOG_DEBUG("Scheduler::start %s", m_name.c_str());
    MutexType::Lock lock(m_mutex);
    if (m_stopping) {
        LOG_ERROR("Scheduler::start %s ERROR", m_name);
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
    LOG_DEBUG("Scheduler::tickle %s", "TICKLE NOW"); 
}

void Scheduler::idle() {
    LOG_DEBUG("Sscheduler::idle %s", "IDLE NOW");
    while (!stopping()) {
        Fiber::GetThis()->yield();
    }
}

void Scheduler::stop() {
    LOG_DEBUG("Scheduler::stop %s", "STOP NOW");
    if (stopping()) {
        // 已经停止了
        return;
    }
    m_stopping = true;// 标记停止状态

    if (m_useCaller) {
        // 判断调用 stop 的线程是否为当前调度器的线程。
        // 即使用了 caller 线程作为调度器，只能由 caller 线程发起 stop。
        assert(GetThis() == this);
    } else {
        // 如果 m_useCaller 为 false 时，下面这句是什么意思？
        // 因为 m_useCaller 为 false 时，调度器的主协程是由线程的调度协程创建的，
        // 所以主线程应该为各自线程，而 stop 应该由主线程发起，GetThis() 是获取
        // 调度器线程的，在 main 中，所以这里的主线程不应该是 main 线程。
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
        LOG_DEBUG("Scheduler::stop %s", "m_rootFiber End");
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
    LOG_DEBUG("Scheduler::run %s", "RUN BEGIN");
    //运行到本调度器，设置当前线程的调度器。
    setThis();
    if (sylar::GetThreadId() != m_rootThread) {
        // 非主线程的调度器，将当前线程的调度协程保存起来,
        // 以便在stop时，返回caller线程的主协程
        t_scheduler_fiber = Fiber::GetThis().get();
    }

    Fiber::ptr idle_fiber(new Fiber(std::bind(&Scheduler::idle, this)));
    Fiber::ptr cb_fiber;

    ScheduleTask task;
    while (true) {
        task.reset();
        // 作用：通知其他线程进行任务调度
        bool tickle_me = false; // 是否tickle其他线程进行任务调度
        {
            MutexType::Lock lock(m_mutex);
            auto it = m_tasks.begin();
            // 遍历所有调度任务
            while (it != m_tasks.end()) {
                if (it->thread != -1 && it->thread != Thread::GetThreadId()) {
                    // 任务队列中的任务指定了调度线程，但不是当前线程，跳过这个任务，继续下一个,因为在初始化时
                    // 会指定哪个线程调度，如果 it->thread == -1，说明这个任务没有指定线程。这时也要通知其他
                    // 线程进行调度，直到那个线程来了与 GetThreadId() 相等。所以这里不跳过这个任务，而是继续下一个。
                    ++it;
                    tickle_me = true;
                    continue;
                }

                // 找到一个未指定线程，或是指定了当前线程的任务
                assert(it->fiber || it->cb);
                if (it->fiber) {
                    // 如果是协程，则进去判断是否为 ready 状态。
                    assert(it->fiber->getState() == Fiber::READY);
                }
                // 当前调度线程找到一个任务，准备开始调度，将其从任务队列中剔除，活动线程数加1
                task = *it;// 将这个任务给 task
                m_tasks.erase(it++);
                ++m_activeThreadCount;
                break;
            }
            // 当前线程拿完一个任务后，发现任务队列还有剩余，那么tickle一下其他线程
            // 这里的逻辑是，如果任务队列中还有任务，那么说明有其他线程正在等待，需要通知其他线程进行调度，
            // 否则，说明当前线程已经拿完了所有任务，可以退出循环，等待其他线程的调度,不管 tickle_me 是否
            // 为 true，只要任务队列中还有任务（即it != m_tasks.end() == true），那么 ticlke_me 就为 true。
            tickle_me |= (it != m_tasks.end());
        }
        // tickle_me 为 true，就通知其它线程。
        if (tickle_me) {
            tickle();
        }

        if (task.fiber) {// task 为协程，则resume协程
            // resume协程，resume返回时，协程要么执行完了，要么半路yield了，
            //总之这个任务就算完成了，活跃线程数减一
            task.fiber->resume();
            --m_activeThreadCount;
            task.reset();
        } else if (task.cb) {
            // task 为回调函数，则创建新的协程，执行回调函数
            if (cb_fiber) {
                // 如果cb_fiber已经有协程了，说明上一个任务的回调函数还没执行完，则重新设置这个
                // 协程的回调函数。只需将这个任务的相关信息设置到这个协程上即可。
                cb_fiber->reset(task.cb);
            } else {
                // 如果协程没有被创建，则创建新的协程，并设置回调函数
                cb_fiber.reset(new Fiber(task.cb));
            }
            task.reset();
            cb_fiber->resume();
            --m_activeThreadCount;
            cb_fiber.reset();
        } else {
            // 进到这个分支情况一定是任务队列空了，调度idle协程即可
            if (idle_fiber->getState() == Fiber::TERM) {
                // 如果 idle_fiber 已经终止了，说明调度器已经停止了，则退出循环。
                LOG_DEBUG("Scheduler::run %s", "Idle Fiber TERM");
                break;
            }
            // 不是 TERM 状态的话，就会一直 resume 到 idle_fiber 协程，然后又在
            // idle_fiber 协程中又 yield 回来，所以这里不需要再次判断 idle_fiber 的状态。
            ++m_idleThreadCount;
            idle_fiber->resume();
            --m_idleThreadCount;
        }
    }
    LOG_DEBUG("Scheduler::run %s", "RUN EXIT");
}
