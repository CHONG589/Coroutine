// 对于 ucontext 四个 API 函数使用：
// https://blog.csdn.net/qq_62821433/article/details/139480927?ops_
// request_misc=%257B%2522request%255Fid%2522%253A%252282AB315A-6575
// -49FF-B67E-60154FCD887D%2522%252C%2522scm%2522%253A%252220140713.1
// 30102334..%2522%257D&request_id=82AB315A-6575-49FF-B67E-60154FCD88
// 7D&biz_id=0&utm_medium=distribute.pc_search_result.none-task-blog-2
// ~all~top_positive~default-1-139480927-null-null.142%5Ev100%5Econtro
// l&utm_term=%E5%8D%8F%E7%A8%8B&spm=1018.2226.3001.4187

#ifndef __FIBER_H__
#define __FIBER_H__

#include <functional>
#include <memory>
#include <ucontext.h>
#include "thread.h"

/**
 * @brief 协程类
 */
class Fiber : public std::enable_shared_from_this<Fiber> {
public:
    typedef std::shared_ptr<Fiber> ptr;

    //协程状态
    enum State { READY, RUNNING, TERM };

    //cb 协程入口函数，stacksize 栈大小，
    //run_in_scheduler 本协程是否参与调度器调度
    Fiber(std::function<void()> cb, size_t stacksize = 0, 
            bool run_in_scheduler = true);

    ~Fiber();
    //重置协程状态和入口函数，复用栈空间，不重新创建栈
    void reset(std::function<void()> cb);
    void resume();
    void yield();
    uint64_t getId() const { return m_id; }
    State getState() const { return m_state; }

    //设置当前正在运行的协程，即设置线程局部变量t_fiber的值
    static void SetThis(Fiber *f);
    static Fiber::ptr GetThis();
    static uint64_t TotalFibers();
    //协程入口函数
    static void MainFunc();
    static uint64_t GetFiberId();

private:
    //无参构造函数只用于创建线程的第一个协程，也就是线程主函数对应的协程
    //主协程没有分配栈和cb
    Fiber();

private:
    uint64_t m_id = 0;
    // 协程栈大小
    uint32_t m_stacksize = 0;
    State m_state = READY;
    // 协程上下文
    ucontext_t m_ctx;
    // 协程栈地址
    void *m_stack = nullptr;
    // 协程入口函数
    std::function<void()> m_cb;
    // 本协程是否参与调度器调度
    bool m_runInScheduler;
};

#endif
