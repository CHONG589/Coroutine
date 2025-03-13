#ifndef __IOMANAGER_H__
#define __IOMANAGER_H__

#include "scheduler.h"
#include "timer.h"

class IOManager : public Scheduler, public TimerManager {
public:
    typedef std::shared_ptr<IOManager> ptr;
    typedef RWMutex RWMutexType;

    //IO事件，继承自epoll对事件的定义
    enum Event {
        /// 无事件
        NONE = 0x0,
        /// 读事件(EPOLLIN)
        READ = 0x1,
        /// 写事件(EPOLLOUT)
        WRITE = 0x4,
    };

    IOManager(size_t threads = 1, bool use_caller = true, const std::string &name = "IOManager");
    ~IOManager();
    //添加事件, fd描述符发生了event事件时执行cb函数
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
    //删除事件，从fd中删除event事件，不会触发事件
    bool delEvent(int fd, Event event);
    //取消事件, 如果该事件被注册过回调，那就触发一次回调事件
    bool cancelEvent(int fd, Event event);
    //取消所有事件, 所有被注册的回调事件在cancel之前都会被执行一次
    bool cancelAll(int fd);
    //返回当前的IOManager
    static IOManager *GetThis();
    int setnonblocking(int fd);

protected:
    //通知调度器有任务要调度, 写pipe让idle协程从epoll_wait
    //退出，待idle协程yield,之后Scheduler::run就可以调度其
    //他任务
    void tickle() override;
    //判断是否可以停止, 判断条件是Scheduler::stopping()
    //外加IOManager的m_pendingEventCount为0，表示没有IO
    //事件可调度了
    bool stopping() override;
    //对于IO协程调度来说，应阻塞在等待IO事件上，idle退出
    //的时机是epoll_wait返回，对应的操作是tickle或注册
    //的IO事件发生
    void idle() override;
    //判断是否可以停止，同时获取最近一个定时器的超时时间
    bool stopping(uint64_t& timeout);
    //当有定时器插入到头部时，要重新更新epoll_wait的超时时间，
    //这里是唤醒idle协程以便于使用新的超时时间
    void onTimerInsertedAtFront() override;
    //*重置socket句柄上下文的容器大小
    void contextResize(size_t size);

private:
    //socket fd上下文类，每个socket fd都对应一个FdContext，
    //包括fd的值，fd上的事件，以及回调函数
    struct FdContext {
        typedef Mutex MutexType;
        //事件上下文类，保存这个事件的回调函数以及执行回调函数的调度器
        struct EventContext {
            // 执行事件回调的调度器
            Scheduler *scheduler = nullptr;
            // 事件回调协程
            Fiber::ptr fiber;
            // 事件回调函数
            std::function<void()> cb;
        };

        //获取事件上下文类，Event 是上面的的 enum 值，是十六进制
        //的值，通过传进去event，返回对应的EventContext read 或
        //EventContext write
        EventContext &getEventContext(Event event);
        //重置事件上下文, ctx 待重置的事件上下文对象
        void resetEventContext(EventContext &ctx);
        //触发事件, 根据事件类型调用对应上下文结构中的调度器去调度
        //回调协程或回调函数
        void triggerEvent(Event event);

        // 读事件上下文
        EventContext read;
        // 写事件上下文
        EventContext write;
        // 事件关联的句柄
        int fd = 0;
        // 该fd添加了哪些事件的回调函数，或者说该fd关心哪些事件
        Event events = NONE;
        // 事件的Mutex
        MutexType mutex;
    };

private:
    //epoll 文件句柄
    int m_epfd = 0;
    //pipe 文件句柄，fd[0]读端，fd[1]写端
    int m_tickleFds[2];
    //当前等待执行的IO事件数量
    std::atomic<size_t> m_pendingEventCount = {0};
    //IOManager的Mutex
    RWMutexType m_mutex;
    //socket事件上下文的容器
    std::vector<FdContext *> m_fdContexts;
};

#endif
