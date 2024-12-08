#include <unistd.h>    // for pipe()
#include <assert.h>
#include <sys/epoll.h> // for epoll_xxx()
#include <fcntl.h>     // for fcntl()
#include "iomanager.h"
#include "log.h"

enum EpollCtlOp {
};

static std::ostream &operator<<(std::ostream &os, const EpollCtlOp &op) {
    switch ((int)op) {
#define XX(ctl) \
    case ctl:   \
        return os << #ctl;
        XX(EPOLL_CTL_ADD);
        XX(EPOLL_CTL_MOD);
        XX(EPOLL_CTL_DEL);
#undef XX
    default:
        return os << (int)op;
    }
}

static std::ostream &operator<<(std::ostream &os, EPOLL_EVENTS events) {
    if (!events) {
        return os << "0";
    }
    bool first = true;
#define XX(E)          \
    if (events & E) {  \
        if (!first) {  \
            os << "|"; \
        }              \
        os << #E;      \
        first = false; \
    }
    XX(EPOLLIN);
    XX(EPOLLPRI);
    XX(EPOLLOUT);
    XX(EPOLLRDNORM);
    XX(EPOLLRDBAND);
    XX(EPOLLWRNORM);
    XX(EPOLLWRBAND);
    XX(EPOLLMSG);
    XX(EPOLLERR);
    XX(EPOLLHUP);
    XX(EPOLLRDHUP);
    XX(EPOLLONESHOT);
    XX(EPOLLET);
#undef XX
    return os;
}

IOManager::FdContext::EventContext &IOManager::FdContext::getEventContext(IOManager::Event event) {
    switch (event) {
    case IOManager::READ:
        return read;
    case IOManager::WRITE:
        return write;
    default:
        LOG_ERROR("%s", "IOManager::getEventContext event False");
        assert(false);
    }
    throw std::invalid_argument("getContext invalid event");
}

void IOManager::FdContext::resetEventContext(EventContext &ctx) {
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}

void IOManager::FdContext::triggerEvent(IOManager::Event event) {
    // 待触发的事件必须已被注册过
    assert(events & event);
    /**
     *  清除该事件，表示不再关注该事件了
     * 也就是说，注册的IO事件是一次性的，如果想持续关注某个socket fd
     * 的读写事件，那么每次触发事件之后都要重新添加
     */
    events = (Event)(events & ~event);
    // 调度对应的协程
    EventContext &ctx = getEventContext(event);
    if (ctx.cb) {
        ctx.scheduler->schedule(ctx.cb);
    } else {
        ctx.scheduler->schedule(ctx.fiber);
    }
    resetEventContext(ctx);
    return;
}

IOManager::IOManager(size_t threads, bool use_caller, const std::string &name)
    : Scheduler(threads, use_caller, name) {
    m_epfd = epoll_create(5000);
    assert(m_epfd > 0);

    int rt = pipe(m_tickleFds);
    assert(!rt);

    // 关注pipe读句柄的可读事件，用于tickle协程
    epoll_event event;
    memset(&event, 0, sizeof(epoll_event));
    // 增加读事件和 ET 触发
    event.events  = EPOLLIN | EPOLLET;
    event.data.fd = m_tickleFds[0];

    // 非阻塞方式，配合边缘触发
    rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
    assert(!rt);

    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
    assert(!rt);

    // 因为 m_fdContexts 是一个 vector，里面保存的是指针形式，所以这里
    // 初始化时要分配空间给它们（确定大小后）。
    contextResize(32);
    LOG_INFO("iom created");

    start();
}

IOManager::~IOManager() {
    stop();
    close(m_epfd);
    close(m_tickleFds[0]);
    close(m_tickleFds[1]);

    for (size_t i = 0; i < m_fdContexts.size(); ++i) {
        if (m_fdContexts[i]) {
            delete m_fdContexts[i];
        }
    }
    LOG_INFO("iom destroyed");
}

bool IOManager::stopping(uint64_t& timeout) {
    ///根据返回的下一个定时器任务需要多久执行来判断是否有定时器任务。
    //~0ull表示无任务
    timeout = getNextTimer();
    return timeout == ~0ull
        && m_pendingEventCount == 0
        && Scheduler::stopping();
}

void IOManager::contextResize(size_t size) {
    m_fdContexts.resize(size);

    for (size_t i = 0; i < m_fdContexts.size(); ++i) {
        if (!m_fdContexts[i]) {
            m_fdContexts[i]     = new FdContext;
            m_fdContexts[i]->fd = i;
        }
    }
}

int IOManager::setnonblocking(int fd) {
    //返回给定文件描述符 fd 的文件状态标志。这些标志通常用于描述文件的打开方式（例如只读、只写、读写等）以及其他特性（例如非阻塞模式）
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

int IOManager::addEvent(int fd, Event event, std::function<void()> cb) {
    //为fd增加cb事件，首先肯定要给它加上fdContext.
    FdContext *fd_ctx = nullptr;
    RWMutexType::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) {
        //这个fd之前已经注册过，已经有了fdContext
        fd_ctx = m_fdContexts[fd];
        lock.unlock();
    } else {
        lock.unlock();
        RWMutexType::WriteLock lock2(m_mutex);
        //分配空间
        contextResize(fd * 1.5);
        fd_ctx = m_fdContexts[fd];
    }
    // 上面是将 fd_ctx 指针指向 m_fdContexts[fd] 中，等下才好将对应的信息
    // 保存到对应的位置中。

    // 同一个fd不允许重复添加相同的事件
    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    if (fd_ctx->events & event) {
        LOG_ERROR("IOManager::addEvent same event added, event=%d, fd_ctx.events=%d", (EPOLL_EVENTS)event, (EPOLL_EVENTS)fd_ctx->events);
        assert(!(fd_ctx->events & event));
    }

    // 将新的事件加入epoll_wait，使用epoll_event的私有指针存储FdContext的位置
    // 如果该 fd 此前没有感兴趣的事件，则直接增加就可以，如果此前 events 中有
    // 其它事件，是修改，而不是增加。
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    epevent.events   = EPOLLET | fd_ctx->events | event;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) {
        LOG_ERROR("IOManager::addEvent epoll_ctl add event False, fd=%d", fd);
        return -1;
    }
    setnonblocking(fd);

    // 待执行IO事件数加1
    ++m_pendingEventCount;

    // 注意：epevent.events 由于是要注册到 epoll_wait 上去的，要增加 ET 触发事件，而
    // fd_ctx->events 是记录该 fd 感兴趣的事件的，不能将 ET 也保存进来，所以这里分开
    // 保存。
    // 找到这个fd的event事件对应的EventContext，对其中的scheduler, cb, fiber进行赋值
    fd_ctx->events                     = (Event)(fd_ctx->events | event);
    FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event);
    assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);

    // event_ctx为事件event的上下文，里面包括了执行函数等信息。下面是为它的上下文
    // 赋值。
    // 赋值scheduler和回调函数，如果回调函数为空，则把当前协程当成回调执行体
    event_ctx.scheduler = Scheduler::GetThis();
    if (cb) {
        event_ctx.cb.swap(cb);
    } else {
        event_ctx.fiber = Fiber::GetThis();
        assert(event_ctx.fiber->getState() == Fiber::RUNNING);
    }
    LOG_INFO("Add event fd= %d", fd);
    return 0;
}

bool IOManager::delEvent(int fd, Event event) {
    // 找到fd对应的FdContext
    RWMutexType::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() <= fd) {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();

    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    if (!(fd_ctx->events & event)) {
        fd_ctx->mutex.unlock();
        return false;
    }

    // 清除指定的事件，表示不关心这个事件了，如果清除之后结果为0，则从epoll_wait中删除该文件描述符
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op           = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) {
        LOG_ERROR("IOManager::delEvent false, fd=%d, event=%d", fd, event);
        return false;
    }

    // 待执行事件数减1
    --m_pendingEventCount;
    // 重置该fd对应的event事件上下文
    fd_ctx->events                     = new_events;
    FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event);
    fd_ctx->resetEventContext(event_ctx);
    return true;
}

bool IOManager::cancelEvent(int fd, Event event) {
    // 找到fd对应的FdContext
    RWMutexType::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() <= fd) {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();

    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    if (!(fd_ctx->events & event)) {
        LOG_ERROR("IOManager::cancelEvent false, fd=%d", fd);
        fd_ctx->mutex.unlock();
        return false;
    }

    // 删除事件
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op           = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) {
        LOG_ERROR("IOManager::cancelEvent false, fd=%d, event=%d", fd, event);
        return false;
    }

    // 删除之前触发一次事件
    fd_ctx->triggerEvent(event);
    // 活跃事件数减1
    --m_pendingEventCount;
    return true;
}

bool IOManager::cancelAll(int fd) {
    // 找到fd对应的FdContext
    RWMutexType::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() <= fd) {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();

    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    if (!fd_ctx->events) {
        return false;
    }

    // 删除全部事件
    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = 0;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) {
        LOG_ERROR("IOManager::cancelAll false, fd=%d", fd);
        fd_ctx->mutex.unlock();
        return false;
    }

    // 触发全部已注册的事件
    if (fd_ctx->events & READ) {
        fd_ctx->triggerEvent(READ);
        --m_pendingEventCount;
    }
    if (fd_ctx->events & WRITE) {
        fd_ctx->triggerEvent(WRITE);
        --m_pendingEventCount;
    }

    assert(fd_ctx->events == 0);
    return true;
}

IOManager *IOManager::GetThis() {
    //dynamic_cast用于基类向派生类转换，本来基类向派生类转换是不安全的，
    //所以这里使用了dynamic_cast
    return dynamic_cast<IOManager *>(Scheduler::GetThis());
}

/**
 * 通知调度协程、也就是Scheduler::run()从idle中退出
 * Scheduler::run()每次从idle协程中退出之后，都会重新把任务队列里的所有任务执行完了再重新进入idle
 * 如果没有调度线程处理于idle状态，那也就没必要发通知了
 */
void IOManager::tickle() {
    LOG_DEBUG("IOManager::tickle...");
    //通过这里可以看出，得有空闲线程才向管道中写进行通知，
    //向管道写的作用：这里涉及到了多线程，如任务队列中还有
    //最后一个任务，被一个线程抢到执行，然后这个线程执行run
    //时，其它线程肯定就阻塞在idle中，假如正在执行的这个线程
    //执行到run中判断任务队列是否为空，看是否要通知其它线程
    //之前，就在这之前，加入了一个任务，然后这个线程判断时自然
    //tickle_me==true，然后就要通知给其它线程去执行，那该怎么
    //通知呢？因为其它线程都阻塞在idle的epoll_wait上，需要借助
    //外部才能退出来，这个通知怎么做就尤其重要，只需要往管道中写
    //数据，因为epoll_wait中也监听了管道的读端，所以往管道写了后，
    //epoll_wait自然就会返回，即解除阻塞，解除阻塞后这个管道读事件
    //和添加的任务自然就可以处理了，因为有线程从阻塞中释放出来，至于
    //这个读管道事件其实不用做啥，就把它读出来就行了，不用处理。
    //就是起到通知的作用。

    /**
     * 至于schedule::tickle()为啥什么都没做，都可以起到通知的作用，
     * 因为它的idle协程和run函数不断切换，也就是在resume和yield之间
     * 切换，这里是轮询的方式，线程不是阻塞在那里，而是busy-wait，所以
     * 有任务时它可以马上知道。
     */
    if(!hasIdleThreads()) {
        return;
    }
    //m_tickleFds[1]是写端，0是读端
    int rt = write(m_tickleFds[1], "T", 1);
    assert(rt == 1);
}

bool IOManager::stopping() {
    // 对于IOManager而言，必须等所有待调度的IO事件都执行完了才可以退出
    // 这里是使用了Scheduler::stoppint，而不是 IOManager::stopping,
    // 这里是要确定调度器也已经停止。
    return m_pendingEventCount == 0 && Scheduler::stopping();
}

/**
 * 调度器无调度任务时会阻塞idle协程上，对IO调度器而言，idle状态应该关注两件事，
 * 一是有没有**新的调度任务，对应Schduler::schedule()**，如果有新的调度任务，那应
 * 该立即退出idle状态，并执行对应的任务；二是关注当前注册的所有IO事件有没有触发，
 * 如果有触发，那么应该**执行IO事件对应的回调函数**
 */
void IOManager::idle() {
    LOG_DEBUG("IOManager::idle...");

    // 一次epoll_wait最多接收256个fd的就绪事件，如果超过了这个数，那么会在下
    // 轮epoll_wati继续处理
    const uint64_t MAX_EVNETS = 256;
    //用来接收事件
    epoll_event *events       = new epoll_event[MAX_EVNETS]();
    //对于下面这种用法在C++primer中438页，因为删除时shared_ptr默认是delete，
    //而这里要使用 delete[] ptr，所以要自己定义
    std::shared_ptr<epoll_event> shared_events(events, [](epoll_event *ptr) {
        delete[] ptr;
    });

    while (true) {
        // 获取下一个定时器的超时时间，顺便判断调度器是否停止
        uint64_t next_timeout = 0;
        if (stopping()) {
            LOG_DEBUG("IOManager::idle name=%s, idle stopping exit", getName().c_str());
            break;
        }
        int rt = 0;
        do {
            /**
             * 因为最多可能有256个fd的就绪时间，由于采用ET模式，要一次性读完，
             * 不然下次就读不到了，所以这里要采用循环一次性读完。因为可能被中
             * 断等原因打断。打断后errno == EINTR,然后continue是这里的循环，
             * 而不应该是上一层循环，不然这次的数据没读完就打断，然后没读完的
             * 数据就丢失了。
             */
            static const int MAX_TIMEOUT = 5000;
            if(next_timeout != ~0ull) {
                next_timeout = std::min((int)next_timeout, MAX_TIMEOUT);
            } else {
                next_timeout = MAX_TIMEOUT;
            }
            int rt = epoll_wait(m_epfd, events, MAX_EVNETS, MAX_TIMEOUT);
            if(rt < 0) {
                if(errno == EINTR) {
                    continue;
                }
                LOG_ERROR("IOManager::idle, epoll_wait fd=%d, rt=%d, erro=%s", m_epfd, rt, strerror(errno));
                break;
            }
        } while(true);

        // 收集所有已超时的定时器，执行回调函数
        std::vector<std::function<void()>> cbs;
        listExpiredCb(cbs);
        if(!cbs.empty()) {
            for(const auto &cb : cbs) {
                schedule(cb);
            }
            cbs.clear();
        }

        // 遍历所有发生的事件，根据epoll_event的私有指针找到对应的FdContext，进行事件处理
        for (int i = 0; i < rt; ++i) {
            epoll_event &event = events[i];
            if (event.data.fd == m_tickleFds[0]) {
                //管道读的事件是不用处理的，只需读出来就行。
                uint8_t dummy[256];
                /**
                 * 因为这里fd就是m_tickleFds[0]，因为一次从epoll_wait中返回
                 * 的fd就绪事件最多有256个，如果另一边发了256个通知，即256个
                 * 字符，所以这里有必要给空间dummy 256 个。
                 * 其次这里也是要一次性全部读完的，因为为ET模式，所以加了循环。
                 */
                while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0);
                continue;
            }

            //处理单个fd的事件
            FdContext *fd_ctx = (FdContext *)event.data.ptr;
            FdContext::MutexType::Lock lock(fd_ctx->mutex);
            /**
             * EPOLLERR: 出错，比如写读端已经关闭的pipe
             * EPOLLHUP: 套接字对端关闭
             * 出现这两种事件，应该同时触发fd的读和写事件，否则有可能出现注册的事件永远执行不到的情况
             */ 
            if (event.events & (EPOLLERR | EPOLLHUP)) {
                //这个fd的就绪时间中（即从epoll_wait返回的事件）包含了
                //EPOLLERR或EPOLLHUP，表示出错或对端关闭，这时应该触发
                //读和写事件，以便及时处理错误
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
                //如果处理错误的事件也加入到该fd感兴趣的事件中，则要添加到
                //event.events中。
            }
            int real_events = NONE;
            if (event.events & EPOLLIN) {
                //就绪事件event.events中有读事件
                real_events |= READ;
            }
            if (event.events & EPOLLOUT) {
                //就绪事件event.events中有写事件
                real_events |= WRITE;
            }

            //这里是看该fd的感兴趣事件中是否包含real_events，如果都不是fd
            //感兴趣的事件，即使发生了肯定也没必要处理，所以这里要判断一下。
            if ((fd_ctx->events & real_events) == NONE) {
                continue;
            }

            // 剔除已经发生的事件，将剩下的事件重新加入epoll_wait
            int left_events = (fd_ctx->events & ~real_events);
            //如果留下来的事件都为空了，则没必要在监听了，直接从m_epfd中删除，
            //如果有才要修改成left_events。
            int op          = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            event.events    = EPOLLET | left_events;

            int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
            if (rt2) {
                LOG_ERROR("IOManager::idle left_events false, epfd=%d, fd=%d, error=%s", m_epfd, fd_ctx->fd, strerror(errno));
                continue;
            }

            //将对应的事件加入到调度器等待处理，即触发事件加入到调度器中。
            if (real_events & READ) {
                fd_ctx->triggerEvent(READ);
                --m_pendingEventCount;
            }
            if (real_events & WRITE) {
                fd_ctx->triggerEvent(WRITE);
                --m_pendingEventCount;
            }
        } // end for

        //获取当前正在执行的协程
        Fiber::ptr cur = Fiber::GetThis();
        //cur为一个共享指针，这里需要一个指针，所以用get()获取原始指针，
        //然后reset()将其置空，这样cur就不再指向当前协程了。这要就不会
        //增加引用计数了吧？因为刚刚增加了1，现在又将它销毁，即减一，而
        //原始指针应该不会加1吧？
        auto raw_ptr   = cur.get();
        cur.reset();

        // epoll_wait 返回一次，用 for 处理完事件，就要返回去执行调度器的任务。
        // 因为返回一次，肯定就是有任务了，所以要让出执行权。raw_ptr就是当前正在
        // 执行的协程，也就是这个idle协程，让它让出来。
        // 直到调度器的任务队列没任务后，又返回到这里 epoll_wait 阻塞。

        /**
         * 注意：这里的 idle协程以及真正执行任务的协程是任务协程，执行
         * yild 会切回到调度协程中，而调度协程中如果要执行某一个任务，则
         * 会从任务队列中取出一个任务，因为任务本身既可以是协程包装成的，
         * 也可以是函数，所以在任务中就有绑定，至于执行完后，这个协程也不会
         * 销毁，而是等待下次使用。可以看到run中cb_fiber是定义在while循环
         * 外的，这样就可以循环利用，执行完一个任务就cb_fiber.reset()。等
         * 下一轮循环有任务是重复利用这个协程。
         */
        raw_ptr->yield();
    } // end while(true)
}

void IOManager::onTimerInsertedAtFront() {
    tickle();
}
