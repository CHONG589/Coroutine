#include <atomic>
#include <assert.h>
#include "fiber.h"
#include "log.h"
#include "scheduler.h"

/// 全局静态变量，用于生成协程id
static std::atomic<uint64_t> s_fiber_id{0};
/// 全局静态变量，用于统计当前的协程数
static std::atomic<uint64_t> s_fiber_count{0};

/// 线程局部变量，当前线程正在运行的协程
static thread_local Fiber *t_fiber = nullptr;
/// 线程局部变量，当前线程的主协程，切换到这个协程，就相当于切换到了主线程
/// 中运行，智能指针形式
static thread_local Fiber::ptr t_thread_fiber = nullptr;

//协程栈默认大小 128k
static uint32_t fiber_stack_size = 128 * 1024;

//malloc栈内存分配器
class MallocStackAllocator {
public:
    static void *Alloc(size_t size) { return malloc(size); }
    static void Dealloc(void *vp, size_t size) { return free(vp); }
};

using StackAllocator = MallocStackAllocator;

uint64_t Fiber::GetFiberId() {
    if (t_fiber) {
        return t_fiber->getId();
    }
    return 0;
}

void Fiber::SetThis(Fiber *f) { 
    t_fiber = f; 
}

Fiber::Fiber() {
    // 只能由 GetThis() 调用，创建线程的第一个协程，也就是线程主函数对应的
    SetThis(this);// 设置 t_fiber
    m_state = RUNNING;

    if (getcontext(&m_ctx)) {// 获取主协程的上下文
        LOG_ERROR("getcontext error!");
        assert(false);
    }

    ++s_fiber_count;
    m_id = s_fiber_id++; // 协程id从0开始，用完加1

    LOG_INFO("Main fiber %llu created", m_id);
}

//带参数的构造函数用于创建其他协程，需要分配栈
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler)
    : m_id(s_fiber_id++)
    , m_cb(cb)
    , m_runInScheduler(run_in_scheduler) {
    ++s_fiber_count;
    m_stacksize = stacksize ? stacksize : fiber_stack_size;
    m_stack     = StackAllocator::Alloc(m_stacksize);

    if (getcontext(&m_ctx)) {
        LOG_ERROR("getcontext error!");
        assert(false);
    }

    m_ctx.uc_link          = nullptr;
    m_ctx.uc_stack.ss_sp   = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;

    //将此上下文m_ctx与MainFunc函数绑定，当调度器让此上下文的协程
    //运行时，就执行MainFunc函数，就会执行绑定在协程里面的cb。在
    //前面列表初始化中已经为cb初始化。
    //为什么不是直接将 m_cb 绑定在这个 m_ctx 中呢？
    //因为运行到该协程时，不单单就只是执行函数那么简单，你还要为
    //执行完函数后的一系列处理，如状态变化，yield 等操作，都要协程
    //执行完后自动管理，而不是后面通过用户管理。
    makecontext(&m_ctx, &Fiber::MainFunc, 0);

    LOG_INFO("Task fiber %llu created", m_id);
}

//获取当前协程，同时充当初始化当前线程主协程的作用，这个函数在使用协程之
//前要调用一下
Fiber::ptr Fiber::GetThis() {
    if (t_fiber) {
        return t_fiber->shared_from_this();
    }

    // 这里相当于调用了无参构造函数，创建了线程的主协程
    Fiber::ptr main_fiber(new Fiber);
    assert(t_fiber == main_fiber.get());
    t_thread_fiber = main_fiber;
    return t_fiber->shared_from_this();
}

//线程的主协程析构时需要特殊处理，因为主协程没有分配栈和cb
Fiber::~Fiber() {
    LOG_DEBUG("fiber %llu destroyed", m_id);
    --s_fiber_count;
    if (m_stack) {
        // 有栈，说明是子协程，需要确保子协程一定是结束状态
        if (m_state != TERM) {
            LOG_ERROR("fiber %llu not TERM state, can't destroy", m_id);
            assert(false);
        }
        StackAllocator::Dealloc(m_stack, m_stacksize);
    } 
    else {
        // 没有栈，说明是线程的主协程

        // 主协程没有cb
        assert(!m_cb);
        // 主协程一定是执行状态
        assert(m_state == RUNNING); 

        // 当前协程就是自己
        Fiber *cur = t_fiber; 
        if (cur == this) {
            //析构了就要设置为空
            SetThis(nullptr);
        }
    }
}

//这里为了简化状态管理，强制只有TERM状态的协程才可以重置，
//但其实刚创建好但没执行过的协程也应该允许重置的
void Fiber::reset(std::function<void()> cb) {
    if (!m_stack) {
        LOG_ERROR("fiber %llu want reset(), but it not m_stack, can't reset()!", m_id);
        assert(false);
    }
    if (m_state != TERM) {
        LOG_ERROR("fiber %llu want reset(), but it not TERM, can't reset()!", m_id);
        assert(false);
    }
    m_cb = cb;
    if (getcontext(&m_ctx)) {
        LOG_ERROR("getcontext error!");
        assert(false);
    }

    m_ctx.uc_link          = nullptr;
    m_ctx.uc_stack.ss_sp   = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;

    makecontext(&m_ctx, &Fiber::MainFunc, 0);
    m_state = READY;
}

void Fiber::resume() {
    //resume时协程的状态只能为 READY 
    if (m_state != READY) {
        LOG_ERROR("fiber %llu resume not READY, can't resume, resume only is READY", m_id);
        assert(false);
    }
    //设置t_fiber
    SetThis(this);
    m_state = RUNNING;

    // 如果协程参与调度器调度，那么应该和调度器的主协程进行swap，而不是线程主协程
    if (m_runInScheduler) {
        // 第一个参数为获取调度器协程,m_ctx为调用者的上下文，即调用 resume() 的协程。
        // 使用 swapcontext() 后就切换到调用者的上下文了。
        if (swapcontext(&(Scheduler::GetMainFiber()->m_ctx), &m_ctx)) {
            LOG_ERROR("fiber: %llu and schedulemainfiber swap fail!", GetFiberId());
            assert(false);
        }
    } 
    else {
        if (swapcontext(&(t_thread_fiber->m_ctx), &m_ctx)) {
            LOG_ERROR("fiber: %llu and mainfiber swap fail!", GetFiberId());
            assert(false);
        }
    }
}

void Fiber::yield() {
    //协程里面的函数执行完后会先将状态置为TERM,所以yield时，状态可以为 TERM
    //也可以为RUNNING，所以就是不能是READY
    if (m_state == READY) {
        LOG_ERROR("fiber %llu yield is READY, can't yield, yield not is READY", m_id);
        assert(false);
    }

    SetThis(t_thread_fiber.get());

    if (m_state != TERM) {
        //说明是函数执行到一半被切换掉的。
        m_state = READY;
    }

    // 如果协程参与调度器调度，那么应该和调度器的主协程进行swap，而不是线程主协程
    if (m_runInScheduler) {
        if (swapcontext(&m_ctx, &(Scheduler::GetMainFiber()->m_ctx))) {
            LOG_ERROR("fiber: %llu and schedulemainfiber swap fail!", GetFiberId());
            assert(false);
        }
    } else {
        if (swapcontext(&m_ctx, &(t_thread_fiber->m_ctx))) {
            LOG_ERROR("fiber: %llu and mainfiber swap fail!", GetFiberId());
            assert(false);
        }
    }
}

void Fiber::MainFunc() {
    Fiber::ptr cur = GetThis();
    if (!cur) {
        LOG_ERROR("Fiber::MainFunc cur is nullptr");
    }
    assert(cur);

    //执行该协程的任务
    cur->m_cb();
    cur->m_cb    = nullptr;
    cur->m_state = TERM;

    // 手动让t_fiber的引用计数减1
    auto raw_ptr = cur.get(); 
    cur.reset();
    raw_ptr->yield();
}
