/**
 * @file fiber.cpp
 * @brief 协程实现
 * @version 0.1
 * @date 2021-06-15
 */

#include <atomic>
#include <assert.h>
#include "fiber.h"
#include "../log/log.h"
#include "macro.h"
#include "scheduler.h"

/// 全局静态变量，用于生成协程id
static std::atomic<uint64_t> s_fiber_id{0};
/// 全局静态变量，用于统计当前的协程数
static std::atomic<uint64_t> s_fiber_count{0};

/// 线程局部变量，当前线程正在运行的协程
static thread_local Fiber *t_fiber = nullptr;
/// 线程局部变量，当前线程的主协程，切换到这个协程，就相当于切换到了主线程中运行，智能指针形式
static thread_local Fiber::ptr t_thread_fiber = nullptr;

//协程栈默认大小 128k
static uint32_t fiber_stack_size = 128 * 1024;

/**
 * @brief malloc栈内存分配器
 */
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

Fiber::Fiber() {
    // 只能由 GetThis() 调用，创建线程的第一个协程，也就是线程主函数对应的
    // 协程。
    // 谁使用 GetThis() 创建，肯定当前就是谁正在运行
    SetThis(this);// 设置 t_fiber
    m_state = RUNNING;

    if (getcontext(&m_ctx)) {// 获取主协程的上下文
        SYLAR_ASSERT2(false, "getcontext");
    }

    ++s_fiber_count;
    m_id = s_fiber_id++; // 协程id从0开始，用完加1

    LOG_DEBUG("Fiber::Fiber main id = %llu", m_id);
}

void Fiber::SetThis(Fiber *f) { 
    t_fiber = f; 
}

/**
 * 获取当前协程，同时充当初始化当前线程主协程的作用，这个函数在使用协程之
 * 前要调用一下
 */
Fiber::ptr Fiber::GetThis() {
    if (t_fiber) {
        return t_fiber->shared_from_this();
    }

    // 这里相当于调用了无参构造函数，创建了线程的主协程
    Fiber::ptr main_fiber(new Fiber);// 一创建协程对象就初始化好了，详见 Fiber()
    assert(t_fiber == main_fiber.get());
    SYLAR_ASSERT(t_fiber == main_fiber.get());
    t_thread_fiber = main_fiber;
    return t_fiber->shared_from_this();
}

/**
 * 带参数的构造函数用于创建其他协程，需要分配栈
 */
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler)
    : m_id(s_fiber_id++)
    , m_cb(cb)
    , m_runInScheduler(run_in_scheduler) {
    ++s_fiber_count;
    m_stacksize = stacksize ? stacksize : g_fiber_stack_size->getValue();
    m_stack     = StackAllocator::Alloc(m_stacksize);

    if (getcontext(&m_ctx)) {
        LOG_ERROR("%llu Fiber getcontext wrong", s_fiber_id);
    }

    m_ctx.uc_link          = nullptr;
    m_ctx.uc_stack.ss_sp   = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;

    makecontext(&m_ctx, &Fiber::MainFunc, 0);

    LOG_DEBUG("Fiber::Fiber main id = %llu", m_id);
}

/**
 * 线程的主协程析构时需要特殊处理，因为主协程没有分配栈和cb
 */
Fiber::~Fiber() {
    LOG_DEBUG("Fiber::~Fiber() id = %llu", m_id);
    --s_fiber_count;
    if (m_stack) {
        // 有栈，说明是子协程，需要确保子协程一定是结束状态
        if (m_state != TERM) {
            LOG_ERROR("Fiber::~Fiber error not TERM id = %llu", m_id);
        }
        StackAllocator::Dealloc(m_stack, m_stacksize);
        LOG_DEBUG("Fiber::~Fiber deallocate stack id = %llu", m_id);
    } else {
        // 没有栈，说明是线程的主协程
        assert(!m_cb);// 主协程没有cb
        assert(m_state == RUNNING); // 主协程一定是执行状态

        Fiber *cur = t_fiber; // 当前协程就是自己
        if (cur == this) {
            SetThis(nullptr);//析构了就要设置为空
        }
    }
}

/**
 * 这里为了简化状态管理，强制只有TERM状态的协程才可以重置，
 * 但其实刚创建好但没执行过的协程也应该允许重置的
 */
void Fiber::reset(std::function<void()> cb) {
    if (!m_stack) LOG_ERROR("Fiber::reset no stack, id = %llu", m_id);
    if (m_state != TERM) LOG_ERROR("Fiber::reset fiber not TERM id = %llu", m_id);
    m_cb = cb;
    if (getcontext(&m_ctx)) {
        LOG_ERROR("%llu Fiber getcontext wrong", s_fiber_id);
    }

    m_ctx.uc_link          = nullptr;
    m_ctx.uc_stack.ss_sp   = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;

    makecontext(&m_ctx, &Fiber::MainFunc, 0);
    m_state = READY;
}

void Fiber::resume() {
    if (m_state == TERM || m_state == RUNNING) 
        LOG_ERROR("Fiber::resume %llu is TERM or RUNNING, can't resume", m_id);
    assert(m_state != TERM && m_state != RUNNING);
    SetThis(this);
    m_state = RUNNING;

    // 如果协程参与调度器调度，那么应该和调度器的主协程进行swap，而不是线程主协程
    if (m_runInScheduler) {
        // 第一个参数为获取调度器协程,m_ctx为调用者的上下文，即调用 resume() 的协程。
        // 使用 swapcontext() 后就切换到调用者的上下文了。
        if (swapcontext(&(Scheduler::GetMainFiber()->m_ctx), &m_ctx)) {
            LOG_ERROR("Fiber::resume %s", "Scheduler Resume Fault");
        }
    } else {
        if (swapcontext(&(t_thread_fiber->m_ctx), &m_ctx)) {
            LOG_ERROR("Fiber::resume %s", "Main Thread Resume Fault");
        }
    }
}

void Fiber::yield() {
    /// 协程运行完之后会自动yield一次，用于回到主协程，此时状态已为结束状态
    if (m_state != TERM && m_state != RUNNING) 
        LOG_ERROR("Fiber::yield %llu not TERM or RUNNING, can't yield, curr state is %d", m_id, m_state);
    assert(m_state == RUNNING || m_state == TERM);

    SetThis(t_thread_fiber.get());
    if (m_state != TERM) {
        m_state = READY;
    }

    // 如果协程参与调度器调度，那么应该和调度器的主协程进行swap，而不是线程主协程
    if (m_runInScheduler) {
        if (swapcontext(&m_ctx, &(Scheduler::GetMainFiber()->m_ctx))) {
            LOG_ERROR("Fiber::yield %s", "Scheduler Yield Fault");
        }
    } else {
        if (swapcontext(&m_ctx, &(t_thread_fiber->m_ctx))) {
            LOG_ERROR("Fiber::yield %s", "Main Thread Yield Fault");
        }
    }
}

/**
 * 这里没有处理协程函数出现异常的情况，同样是为了简化状态管理，并且个人认为协程的异常不应该由框架处理，应该由开发者自行处理
 */
void Fiber::MainFunc() {
    // 执行到这里肯定已经不是该线程第一次创建协程，所以返回的是 t_fiber，
    // 即当前正在运行的协程。
    Fiber::ptr cur = GetThis(); // GetThis()的shared_from_this()方法让引用计数加1
    if (!cur) {
        LOG_ERROR("Fiber::MainFunc cur is nullptr");
    }
    assert(cur);

    cur->m_cb();//执行该协程的任务
    cur->m_cb    = nullptr;
    cur->m_state = TERM;

    auto raw_ptr = cur.get(); // 手动让t_fiber的引用计数减1
    cur.reset();
    raw_ptr->yield();
}
