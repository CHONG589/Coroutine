#include "hook.h"
#include <dlfcn.h>

#include "timer.h"
#include "log.h"
#include "fiber.h"
#include "iomanager.h"
#include "fd_manager.h"

static thread_local bool t_hook_enable = false;

#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt)

/**
宏的含义:
#define HOOK_FUN(XX)：这是宏的定义，HOOK_FUN 是宏的名称，
(XX)是一个参数。这个参数在调用宏时可以被替换为其他代码。
\符号表示当前行未结束，宏的定义可以继续在下一行。

宏的使用:
当开发者调用这个宏 HOOK_FUN 时，会传入一个参数，比如说 
HOOK_FUN(__HOOK_FUNCTION__)。在这个例子中，XX 会被替
换为 __HOOK_FUNCTION__。

例如，调用 HOOK_FUN(__HOOK_FUNCTION__) 则会展开成：

__HOOK_FUNCTION__(sleep)
__HOOK_FUNCTION__(usleep)
__HOOK_FUNCTION__(nanosleep)
__HOOK_FUNCTION__(socket)
__HOOK_FUNCTION__(connect)
__HOOK_FUNCTION__(accept)
__HOOK_FUNCTION__(read)
__HOOK_FUNCTION__(readv)
__HOOK_FUNCTION__(recv)
__HOOK_FUNCTION__(recvfrom)
__HOOK_FUNCTION__(recvmsg)
__HOOK_FUNCTION__(write)
__HOOK_FUNCTION__(writev)
__HOOK_FUNCTION__(send)
__HOOK_FUNCTION__(sendto)
__HOOK_FUNCTION__(sendmsg)
__HOOK_FUNCTION__(close)
__HOOK_FUNCTION__(fcntl)
__HOOK_FUNCTION__(ioctl)
__HOOK_FUNCTION__(getsockopt)
__HOOK_FUNCTION__(setsockopt)

理解的关键:
函数名的抽象：这个宏定义不会直接实现任何功能，而是将
多个函数的定义统一用一个简洁的方式来表示。你可以在使
用时插入任意操作来处理这些函数。

用法灵活：
只要替换 XX，你就可以很容易地为每个函数实现不同的处
理逻辑，例如记录、替换实现、增加调试信息等。这使得
代码的复用性和灵活性极高。

示例应用：
比如在某个框架中，你希望在调用这些系统函数时加入一
些日志记录或者功能扩展，你只需在定义 XX 的地方写上
这些额外的处理，其他部分不需要改动。

通过这种方式，可以将多个函数的调用逻辑集中管理，而
不需要多次手动书写，这样能提高代码的整洁度和可维护
性。希望这能帮助你更好地理解这个宏的定义和用途！
 */



/**
宏定义和使用:
#define XX(name) name ## _f = (name ## _fun)dlsym(RTLD_NEXT, #name);
宏定义：#define XX(name) 定义了一个新的宏 XX，它接收一个参数 name。
拼接操作符 ##：
name ## _f：这个表达式会把传入的 name 参数与 _f 连接起来，形成一个新的变
量名，比如 sleep_f、connect_f 等等。
(name ## _fun)：类似地，这会把 name 与 _fun 连接，形成**函数指针类型**，
比如 sleep_fun、connect_fun 等等。

dlsym 函数：dlsym(RTLD_NEXT, #name) 用于查找动态库中的符号（函数指针）。
RTLD_NEXT 参数表示查找下一个匹配的符号。#name 会把传入的 name 转换为字符
串，如 sleep、connect，这是获取符号所需的格式。

组合：整体的代码行name ## _f = (name ## _fun)dlsym(RTLD_NEXT, #name);
实际上是将找到的函数指针赋值给相应的 _f 变量。例如，如果 name 是 connect，
那么实际执行的代码将类似于 connect_f = (connect_fun)dlsym(RTLD_NEXT, "connect");。
宏的调用:
HOOK_FUN(XX);
这一行调用了之前定义的 HOOK_FUN 宏，并将 XX 作为参数传入。这会导致 HOOK_FUN 
中的每个条目都用 XX 替换，同时形成一系列赋值语句。
因此，最终将会为每个在 HOOK_FUN 中列出的函数生成对应的 _f 变量，并将其指向动
态库中相应的函数。

解除宏定义:
#undef XX
这一行解除宏 XX 的定义。这样做的目的是避免在后续代码中出现重复定义的错误，
同时保持代码的整洁性。
 */
void hook_init() {
    static bool is_inited = false;
    if(is_inited) {
        return;
    }
#define XX(name) name ## _f = (name ## _fun)dlsym(RTLD_NEXT, #name);
    HOOK_FUN(XX);
#undef XX
}

static uint64_t s_connect_timeout = -1;
struct _HookIniter {
    _HookIniter() {
        hook_init();
        s_connect_timeout = g_tcp_connect_timeout->getValue();

        g_tcp_connect_timeout->addListener([](const int& old_value, const int& new_value){
                SYLAR_LOG_INFO(g_logger) << "tcp connect timeout changed from "
                                         << old_value << " to " << new_value;
                s_connect_timeout = new_value;
        });
    }
};

static _HookIniter s_hook_initer;

static bool is_hook_enable() {
    return t_hook_enable;
}

static void set_hook_enable(bool flag) {
    t_hook_enable = flag;
}

struct timer_info {
    int cancelled = 0;
};

template<typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char* hook_fun_name,
        uint32_t event, int timeout_so, Args&&... args) {
    if(!t_hook_enable) {
        return fun(fd, std::forward<Args>(args)...);
    }

    FdCtx::ptr ctx = FdManager::FdMgr::GetInstance()->get(fd);
    if(!ctx) {
        return fun(fd, std::forward<Args>(args)...);
    }

    if(ctx->isClose()) {
        errno = EBADF;
        return -1;
    }

    if(!ctx->isSocket() || ctx->getUserNonblock()) {
        return fun(fd, std::forward<Args>(args)...);
    }

    uint64_t to = ctx->getTimeout(timeout_so);
    std::shared_ptr<timer_info> tinfo(new timer_info);

retry:
    ssize_t n = fun(fd, std::forward<Args>(args)...);
    while(n == -1 && errno == EINTR) {
        n = fun(fd, std::forward<Args>(args)...);
    }
    if(n == -1 && errno == EAGAIN) {
        IOManager* iom = IOManager::GetThis();
        Timer::ptr timer;
        std::weak_ptr<timer_info> winfo(tinfo);

        if(to != (uint64_t)-1) {
            timer = iom->addConditionTimer(to, [winfo, fd, iom, event]() {
                auto t = winfo.lock();
                if(!t || t->cancelled) {
                    return;
                }
                t->cancelled = ETIMEDOUT;
                iom->cancelEvent(fd, (IOManager::Event)(event));
            }, winfo);
        }

        int rt = iom->addEvent(fd, (IOManager::Event)(event));
        if(rt != 0) {
            LOG_ERROR("addEvent( %d, %d)", fd, (EPOLL_EVENT)event);
            if(timer) {
                timer->cancel();
            }
            return -1;
        } else {
            Fiber::GetThis()->yield();
            if(timer) {
                timer->cancel();
            }
            if(tinfo->cancelled) {
                errno = tinfo->cancelled;
                return -1;
            }
            goto retry;
        }
    }
    
    return n;
}


extern "C" {
#define XX(name) name ## _fun name ## _f = nullptr;
    HOOK_FUN(XX);
#undef XX

unsigned int sleep(unsigned int seconds) {
    if(!t_hook_enable) {
        return sleep_f(seconds);
    }

    Fiber::ptr fiber = Fiber::GetThis();
    IOManager* iom = IOManager::GetThis();
    //下面这句用AI试一下，因为IOManager继承了定时器类，所以可以直接
    //使用addTimer方法。回调函数应该是为调度器添加该任务。
    /**
     * 这里的回调函数是IOManager中的schedule方法，它是向调度器添加任务的，
     * 它被显示的强转变为void(Scheduler::*)(Fiber::ptr, int thread)类型，
     * 所以这里的回调函数应该是调度器的schedule方法。
     * 而这个回调函数即添加任务的schedule方法，它本来就只有两个参数，第一个
     * 参数是协程或者函数，第二个是表示线程的，但是这里提供了三个参数？是什么
     * 意思呢？其实iom是指IOManager的this指针，也就是调用添加任务函数schedule
     * 的this指针，因为这里是采用bind的方式，所以将iom作为上下文隐含在绑定中
     * 的，后面两个才是真正的添加任务的参数。
     */
    iom->addTimer(seconds * 1000, std::bind((void(Scheduler::*)
            (Fiber::ptr, int thread))&IOManager::schedule
            ,iom, fiber, -1));
    Fiber::GetThis()->yield();
    return 0;
}

int usleep(useconds_t usec) {
    if(!t_hook_enable) {
        return usleep_f(usec);
    }
    Fiber::ptr fiber = Fiber::GetThis();
    IOManager* iom = IOManager::GetThis();
    iom->addTimer(usec / 1000, std::bind((void(Scheduler::*)
            (Fiber::ptr, int thread))&IOManager::schedule
            ,iom, fiber, -1));
    Fiber::GetThis()->yield();
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    if(!t_hook_enable) {
        return nanosleep_f(req, rem);
    }

    int timeout_ms = req->tv_sec * 1000 + req->tv_nsec / 1000 /1000;
    Fiber::ptr fiber = Fiber::GetThis();
    IOManager* iom = IOManager::GetThis();
    iom->addTimer(timeout_ms, std::bind((void(Scheduler::*)
            (Fiber::ptr, int thread))&IOManager::schedule
            ,iom, fiber, -1));
    Fiber::GetThis()->yield();
    return 0;
}

int socket(int domain, int type, int protocol) {
    if(!t_hook_enable) {
        return socket_f(domain, type, protocol);
    }
    int fd = socket_f(domain, type, protocol);
    if(fd == -1) {
        return fd;
    }
    FdManager::FdMgr::GetInstance()->get(fd, true);
    return fd;
}

// 这个函数的具体作用是实现一个带超时控制的网络连接功能。
// 它允许程序尝试在给定的超时时间内（以毫秒为单位）通过
// 一个 socket 文件描述符连接到指定的网络地址。具体的功
// 能可以概括为以下几点：

// 连接管理：它管理与网络连接相关的上下文，确保在连接过
// 程中考虑到 socket 的状态。

// 超时控制：通过设置定时器，函数能够在指定时间内监控连
// 接的状态，如果超时则自动取消连接请求，避免程序一直阻
// 塞在连接上。

// 异步处理：如果连接在进行中，函数会将当前协程挂起，待
// 连接完成后再继续执行，从而实现异步 IO 的效果。

// 错误处理：在连接失败的情况下，能够根据返回的错误码设
// 置 errno，方便上层调用者进行错误处理。

// 兼容性：如果不满足特定条件（例如钩子未启用，非 socket，
// 用户设置为非阻塞），函数会回退到默认的连接方式，确保
// 基本功能不会丢失。
int connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout_ms) {
    if(!t_hook_enable) {
        return connect_f(fd, addr, addrlen);
    }
    //获取fd的上下文
    FdManager::FdCtx::ptr ctx = FdManager::FdMgr::GetInstance()->get(fd);
    if(!ctx || ctx->isClose()) {
        errno = EBADF;
        return -1;
    }

    if(!ctx->isSocket()) {
        return connect_f(fd, addr, addrlen);
    }

    if(ctx->getUserNonblock()) {
        return connect_f(fd, addr, addrlen);
    }

    int n = connect_f(fd, addr, addrlen);
    if(n == 0) {
        return 0;
    } else if(n != -1 || errno != EINPROGRESS) {
        return n;
    }

    IOManager* iom = IOManager::GetThis();
    Timer::ptr timer;
    std::shared_ptr<timer_info> tinfo(new timer_info);
    std::weak_ptr<timer_info> winfo(tinfo);

    if(timeout_ms != (uint64_t)-1) {
        timer = iom->addConditionTimer(timeout_ms, [winfo, fd, iom]() {
                auto t = winfo.lock();
                if(!t || t->cancelled) {
                    return;
                }
                t->cancelled = ETIMEDOUT;
                iom->cancelEvent(fd, IOManager::WRITE);
        }, winfo);
    }

    int rt = iom->addEvent(fd, IOManager::WRITE);
    if(rt == 0) {
        Fiber::GetThis()->yield();
        if(timer) {
            timer->cancel();
        }
        if(tinfo->cancelled) {
            errno = tinfo->cancelled;
            return -1;
        }
    } else {
        if(timer) {
            timer->cancel();
        }
        LOG_ERROR("connect_addEvent(%d, WRITE) error", fd);
    }

    int error = 0;
    socklen_t len = sizeof(int);
    if(-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len)) {
        return -1;
    }
    if(!error) {
        return 0;
    } else {
        errno = error;
        return -1;
    }
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
}

int accept(int s, struct sockaddr *addr, socklen_t *addrlen) {
    int fd = do_io(s, accept_f, "accept", IOManager::READ, SO_RCVTIMEO, addr, addrlen);
    if(fd >= 0) {
        FdManager::FdMgr::GetInstance()->get(fd, true);
    }
    return fd;
}

ssize_t read(int fd, void *buf, size_t count) {
    return do_io(fd, read_f, "read", IOManager::READ, SO_RCVTIMEO, buf, count);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    return do_io(fd, readv_f, "readv", IOManager::READ, SO_RCVTIMEO, iov, iovcnt);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return do_io(sockfd, recv_f, "recv", IOManager::READ, SO_RCVTIMEO, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
    return do_io(sockfd, recvfrom_f, "recvfrom", IOManager::READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    return do_io(sockfd, recvmsg_f, "recvmsg", IOManager::READ, SO_RCVTIMEO, msg, flags);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return do_io(fd, write_f, "write", IOManager::WRITE, SO_SNDTIMEO, buf, count);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    return do_io(fd, writev_f, "writev", IOManager::WRITE, SO_SNDTIMEO, iov, iovcnt);
}

ssize_t send(int s, const void *msg, size_t len, int flags) {
    return do_io(s, send_f, "send", IOManager::WRITE, SO_SNDTIMEO, msg, len, flags);
}

ssize_t sendto(int s, const void *msg, size_t len, int flags, const struct sockaddr *to, socklen_t tolen) {
    return do_io(s, sendto_f, "sendto", IOManager::WRITE, SO_SNDTIMEO, msg, len, flags, to, tolen);
}

ssize_t sendmsg(int s, const struct msghdr *msg, int flags) {
    return do_io(s, sendmsg_f, "sendmsg", IOManager::WRITE, SO_SNDTIMEO, msg, flags);
}

int close(int fd) {
    if(!t_hook_enable) {
        return close_f(fd);
    }

    FdCtx::ptr ctx = FdManager::FdMgr::GetInstance()->get(fd);
    if(ctx) {
        auto iom = IOManager::GetThis();
        if(iom) {
            iom->cancelAll(fd);
        }
        FdManager::FdMgr::GetInstance()->del(fd);
    }
    return close_f(fd);
}

int fcntl(int fd, int cmd, ... /* arg */ ) {
    va_list va;
    va_start(va, cmd);
    switch(cmd) {
        case F_SETFL:
            {
                int arg = va_arg(va, int);
                va_end(va);
                FdCtx::ptr ctx = FdManager::FdMgr::GetInstance()->get(fd);
                if(!ctx || ctx->isClose() || !ctx->isSocket()) {
                    return fcntl_f(fd, cmd, arg);
                }
                ctx->setUserNonblock(arg & O_NONBLOCK);
                if(ctx->getSysNonblock()) {
                    arg |= O_NONBLOCK;
                } else {
                    arg &= ~O_NONBLOCK;
                }
                return fcntl_f(fd, cmd, arg);
            }
            break;
        case F_GETFL:
            {
                va_end(va);
                int arg = fcntl_f(fd, cmd);
                FdCtx::ptr ctx = FdManager::FdMgr::GetInstance()->get(fd);
                if(!ctx || ctx->isClose() || !ctx->isSocket()) {
                    return arg;
                }
                if(ctx->getUserNonblock()) {
                    return arg | O_NONBLOCK;
                } else {
                    return arg & ~O_NONBLOCK;
                }
            }
            break;
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
            {
                int arg = va_arg(va, int);
                va_end(va);
                return fcntl_f(fd, cmd, arg); 
            }
            break;
        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
            {
                va_end(va);
                return fcntl_f(fd, cmd);
            }
            break;
        case F_SETLK:
        case F_SETLKW:
        case F_GETLK:
            {
                struct flock* arg = va_arg(va, struct flock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;
        case F_GETOWN_EX:
        case F_SETOWN_EX:
            {
                struct f_owner_exlock* arg = va_arg(va, struct f_owner_exlock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;
        default:
            va_end(va);
            return fcntl_f(fd, cmd);
    }
}

int ioctl(int d, unsigned long int request, ...) {
    va_list va;
    va_start(va, request);
    void* arg = va_arg(va, void*);
    va_end(va);

    if(FIONBIO == request) {
        bool user_nonblock = !!*(int*)arg;
        FdCtx::ptr ctx = FdManager::FdMgr::GetInstance()->get(d);
        if(!ctx || ctx->isClose() || !ctx->isSocket()) {
            return ioctl_f(d, request, arg);
        }
        ctx->setUserNonblock(user_nonblock);
    }
    return ioctl_f(d, request, arg);
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    return getsockopt_f(sockfd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    if(!t_hook_enable) {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }
    if(level == SOL_SOCKET) {
        if(optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
            FdCtx::ptr ctx = FdManager::FdMgr::GetInstance()->get(sockfd);
            if(ctx) {
                const timeval* v = (const timeval*)optval;
                ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
            }
        }
    }
    return setsockopt_f(sockfd, level, optname, optval, optlen);
}
}
