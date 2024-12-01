/**
 * @file timer.h
 * @brief 定时器封装
 * @author sylar.yin
 * @email 564628276@qq.com
 * @date 2019-05-28
 * @copyright Copyright (c) 2019年 sylar.yin All rights reserved (www.sylar.top)
 */
#ifndef __SYLAR_TIMER_H__
#define __SYLAR_TIMER_H__

#include <memory>
#include <vector>
#include <set>
#include "mutex.h"

namespace sylar {

class TimerManager;
/**
 * @brief 定时器
 */
class Timer : public std::enable_shared_from_this<Timer> {
friend class TimerManager;
public:
    /// 定时器的智能指针类型
    typedef std::shared_ptr<Timer> ptr;

    /**
     * @brief 取消定时器
     */
    bool cancel();

    /**
     * @brief 刷新设置定时器的执行时间
     */
    bool refresh();

    /**
     * @brief 重置定时器时间
     * @param[in] ms 定时器执行间隔时间(毫秒)
     * @param[in] from_now 是否从当前时间开始计算
     */
    bool reset(uint64_t ms, bool from_now);
private:
    /**
     * @brief 构造函数
     * @param[in] ms 定时器执行间隔时间
     * @param[in] cb 回调函数
     * @param[in] recurring 是否循环
     * @param[in] manager 定时器管理器
     */
    Timer(uint64_t ms, std::function<void()> cb,
          bool recurring, TimerManager* manager);

    /**
     * @brief 构造函数
     * @param[in] next 执行的时间戳(毫秒)
     * 直接用具体的执行时间作为参数进行构造
     */
    Timer(uint64_t next);
private:
    /// 是否循环定时器
    bool m_recurring = false;
    /// 执行周期
    uint64_t m_ms = 0;
    // 即添加到定时器时，当前时间加超时
    // 时间后的时间节点，具体执行时间。
    uint64_t m_next = 0;
    /// 回调函数
    std::function<void()> m_cb;
    /// 定时器管理器
    TimerManager* m_manager = nullptr;
private:
    /**
     * @brief 定时器比较仿函数
     * 因为这是给小根堆比较用的，自然是lhs < rhs 时返回true
     */
    struct Comparator {
        /**
         * @brief 比较定时器的智能指针的大小(按执行时间排序)
         * @param[in] lhs 定时器智能指针
         * @param[in] rhs 定时器智能指针
         */
        bool operator()(const Timer::ptr& lhs, const Timer::ptr& rhs) const;
    };
};

/**
 * @brief 定时器管理器
 */
class TimerManager {
friend class Timer;
public:
    /// 读写锁类型
    typedef RWMutex RWMutexType;

    /**
     * @brief 构造函数
     */
    TimerManager();

    /**
     * @brief 析构函数
     */
    virtual ~TimerManager();

    /**
     * @brief 添加定时器
     * @param[in] ms 定时器执行间隔时间
     * @param[in] cb 定时器回调函数
     * @param[in] recurring 是否循环定时器
     */
    Timer::ptr addTimer(uint64_t ms, std::function<void()> cb
                        ,bool recurring = false);

    /**
     * @brief 添加条件定时器
     * @param[in] ms 定时器执行间隔时间
     * @param[in] cb 定时器回调函数
     * @param[in] weak_cond 条件
     * @param[in] recurring 是否循环
     * 
     * sylar支持创建条件定时器，也就是在创建
     * 定时器时绑定一个变量，在定时器触发时判
     * 断一下该变量是否仍然有效，如果变量无效，那就取消触发。
     */
    Timer::ptr addConditionTimer(uint64_t ms, std::function<void()> cb
                        ,std::weak_ptr<void> weak_cond
                        ,bool recurring = false);

    /**
     * @brief 到最近一个定时器执行的时间间隔(毫秒)
     */
    uint64_t getNextTimer();

    /**
     * @brief 获取需要执行的定时器的回调函数列表
     * @param[out] cbs 回调函数数组
     */
    void listExpiredCb(std::vector<std::function<void()> >& cbs);

    /**
     * @brief 是否有定时器
     */
    bool hasTimer();
protected:

    /**
     * @brief 当有新的定时器插入到定时器的首部,执行该函数
     */
    virtual void onTimerInsertedAtFront() = 0;

    /**
     * @brief 将定时器添加到管理器中
     */
    void addTimer(Timer::ptr val, RWMutexType::WriteLock& lock);
private:
    /**
     * @brief 检测服务器时间是否被调后了
     */
    bool detectClockRollover(uint64_t now_ms);
private:
    /// Mutex
    RWMutexType m_mutex;
    // 因为set里的元素总是排序过的，所以总是可以很方便地获取到
    // 当前的最小定时器，至于怎么比较，就由Comparator类来决定。
    std::set<Timer::ptr, Timer::Comparator> m_timers;
    // 是否触发onTimerInsertedAtFront函数，这是一个虚函数，
    // 由IOManager继承时实现，当新的定时器插入到Timer集合的首
    // 部时，TimerManager通过该方法来通知IOManager立刻更新当前
    // 的epoll_wait超时，onTimerInsertedAtFront函数内部tick一下
    // epoll_wait 就会立即退出，并重新设置超时时间。
    bool m_tickled = false;
    /// 上次执行时间
    uint64_t m_previouseTime = 0;
};

}

#endif
