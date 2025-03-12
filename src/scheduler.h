#ifndef __SCHEDULER_H__
#define __SCHEDULER_H__

#include <functional>
#include <list>
#include <memory>
#include <string>
#include "fiber.h"
#include "log.h"
#include "thread.h"

//实现协程调度之后，可以解决前一章协程模块中子协程不能运行另一个子协程的缺陷，
//子协程可以通过向调度器添加调度任务的方式来运行另一个子协程。

/**
 * @brief 协程调度器
 * @details 封装的是N-M的协程调度器
 *          内部有一个线程池,支持协程在线程池里面切换
 */
class Scheduler {
public:
    typedef std::shared_ptr<Scheduler> ptr;
    typedef Mutex MutexType;

    /**
     * @brief 添加调度任务
     * @tparam FiberOrCb 调度任务类型，可以是协程对象或函数指针
     * @param[] fc 协程对象或指针
     * @param[] thread 指定运行该任务的线程号，-1表示任意线程
     */
    template <class FiberOrCb>
    void schedule(FiberOrCb fc, int thread = -1) {
        bool need_tickle = false;
        {
            MutexType::Lock lock(m_mutex);
            need_tickle = scheduleNoLock(fc, thread);
        }

        if (need_tickle) {
            tickle(); // 唤醒idle协程
        }
    }

    //threads 线程数, 
    //use_caller 是否将当前线程也作为调度线程
    Scheduler(size_t threads = 1, bool use_caller = true, const std::string &name = "Scheduler");
    virtual ~Scheduler();
    const std::string &getName() const { return m_name; }
    //在执行调度任务时，还可以通过调度器的GetThis()方法获取到当前调度器，
    //再通过schedule方法继续添加新的任务，这就变相实现了在子协程中创建并
    //运行新的子协程的功能
    static Scheduler *GetThis();
    //获取当前线程的主协程
    static Fiber *GetMainFiber();
    //启动调度器
    void start();
    //停止调度器，等所有调度任务都执行完了再返回
    void stop();

protected:
    //通知协程调度器有任务了
    virtual void tickle();
    //协程调度函数
    void run();
    //无任务调度时执行idle协程
    virtual void idle();
    //返回是否可以停止
    virtual bool stopping();
    //设置当前的协程调度器
    void setThis();
    //返回是否有空闲线程
    bool hasIdleThreads() { return m_idleThreadCount > 0; }

private:
    //调度任务，协程/函数二选一，可指定在哪个线程上调度
    struct ScheduleTask {
        Fiber::ptr fiber;
        std::function<void()> cb;
        int thread;

        ScheduleTask(Fiber::ptr f, int thr) {
            fiber  = f;
            thread = thr;
        }
        ScheduleTask(Fiber::ptr *f, int thr) {
            fiber.swap(*f);
            thread = thr;
        }
        ScheduleTask(std::function<void()> f, int thr) {
            cb     = f;
            thread = thr;
        }
        ScheduleTask() { thread = -1; }

        void reset() {
            fiber  = nullptr;
            cb     = nullptr;
            thread = -1;
        }
    };

    /**
     * @brief 添加调度任务，无锁
     * @tparam FiberOrCb 调度任务类型，可以是协程对象或函数指针
     * @param[] fc 协程对象或指针
     * @param[] thread 指定运行该任务的线程号，-1表示任意线程
     */
    template <class FiberOrCb>
    bool scheduleNoLock(FiberOrCb fc, int thread) {
        //在本来调度器中没有任务（停止状态），然后添加了一个任务后，
        //要通知调度器开启了。如果调度器本来就有任务（运行状态），
        //本来就运行状态，不需要通知它启动。
        bool need_tickle = m_tasks.empty();
        ScheduleTask task(fc, thread);
        if (task.fiber || task.cb) {
            m_tasks.push_back(task);
        }
        return need_tickle;
    }

private:
    // 协程调度器名称
    std::string m_name;
    // 互斥锁
    MutexType m_mutex;
    // 线程池
    std::vector<Thread::ptr> m_threads;
    // 任务队列
    std::list<ScheduleTask> m_tasks;
    // 线程池的线程ID数组
    std::vector<int> m_threadIds;
    // 工作线程数量，不包含use_caller的主线程
    size_t m_threadCount = 0;
    // 活跃线程数
    std::atomic<size_t> m_activeThreadCount = {0};
    // idle线程数
    std::atomic<size_t> m_idleThreadCount = {0};

    // 是否use caller
    bool m_useCaller;
    // use_caller为true时，调度器所在线程的调度协程
    Fiber::ptr m_rootFiber;
    // use_caller为true时，调度器所在线程的id
    int m_rootThread = 0;
    // 是否正在停止
    bool m_stopping = false;
};

#endif
