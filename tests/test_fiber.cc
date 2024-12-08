#include <string>
#include <vector>
#include "fiber.h"
#include "log.h"
#include "thread.h"

void run_in_fiber2() {
    LOG_INFO("thread:%d run_in_fiber2 %d begin", Thread::GetThis()->getId(), Fiber::GetThis()->getId());
    LOG_INFO("thread:%d run_in_fiber2 %d end", Thread::GetThis()->getId(), Fiber::GetThis()->getId());
}

void run_in_fiber() {
    LOG_INFO("thread:%d run_in_fiber %d begin", Thread::GetThis()->getId(), Fiber::GetThis()->getId());
    LOG_INFO("thread:%d before run_in_fiber yield", Thread::GetThis()->getId());
    Fiber::GetThis()->yield();
    LOG_INFO("thread:%d after run_in_fiber yield", Thread::GetThis()->getId());

    LOG_INFO("thread:%d run_in_fiber %d end", Thread::GetThis()->getId(), Fiber::GetThis()->getId());
    // fiber结束之后会自动返回主协程运行
}

void test_fiber() {
    LOG_INFO("Thread %d start", Thread::GetThis()->getId());

    // 初始化线程主协程
    Fiber::GetThis();

    Fiber::ptr fiber(new Fiber(run_in_fiber, 0, false));
    LOG_INFO("thread:%d use_count: %d", Thread::GetThis()->getId(), fiber.use_count());//1

    LOG_INFO("thread:%d before test_fiber resume", Thread::GetThis()->getId());
    fiber->resume();
    LOG_INFO("thread:%d after test_fiber resume", Thread::GetThis()->getId());

    /** 
     * 关于fiber智能指针的引用计数为3的说明：
     * 一份在当前函数的fiber指针，一份在MainFunc的cur指针
     * 还有一份在在run_in_fiber的GetThis()结果的临时变量里
     */
    LOG_INFO("thread:%d use_count: %d", Thread::GetThis()->getId(), fiber.use_count());//3
    
    LOG_INFO("thread:%d fiber status %d", Thread::GetThis()->getId(), (Fiber::State)fiber->getState());//READY

    LOG_INFO("thread:%d before test_fiber resume again", Thread::GetThis()->getId());
    fiber->resume();
    LOG_INFO("thread:%d after test_fiber resume again", Thread::GetThis()->getId());

    LOG_INFO("thread:%d use_count: %d", Thread::GetThis()->getId(), fiber.use_count());//1
    LOG_INFO("thread:%d fiber status %d", Thread::GetThis()->getId(), (Fiber::State)fiber->getState());//TERM

    fiber->reset(run_in_fiber2); // 上一个协程结束之后，复用其栈空间再创建一个新协程
    fiber->resume();

    LOG_INFO("thread:%d use_count: %d", Thread::GetThis()->getId(), fiber.use_count());//1
    LOG_INFO("thread:%d test_fiber end", Thread::GetThis()->getId());
}

int main(int argc, char *argv[]) {

    Log::Instance()->init(1, "./log", ".log", 1024);

    LOG_INFO("main begin");

    std::vector<Thread::ptr> thrs;
    for (int i = 0; i < 2; i++) {
        thrs.push_back(Thread::ptr(
            new Thread(&test_fiber, "thread_" + std::to_string(i))));
    }

    for (auto i : thrs) {
        i->join();
    }

    LOG_INFO("main end");
    return 0;
}
