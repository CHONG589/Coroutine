# ifndef BLOCKQUEUE_H
# define BLOCKQUEUE_H

#include <deque>
#include <condition_variable>
#include <mutex>
#include <sys/time.h>
using namespace std;

/**
 * BlockQueue 是一个线程安全的队列，支持阻塞式的生产者消费者模型。
 * 生产者线程通过 push_back() 或 push_front() 向队列中添加元素，消费者线程通过
 * pop() 取出元素。当队列为空时，生产者线程会被阻塞，当队列满时，消费者线程会被阻塞。
 * 它是日志异步写入的基础。
 */

/**
 * std::lock_guard 是 C++ 标准库中提供的一个模板类，用于在其**构造时**自动获
 * 取锁，在析构时自动释放锁。使用 std::lock_guard 的好处是，当 std::lock_guard
 * 对象**离开其作用域时**，会自动调用析构函数，该析构函数会释放锁。这确保了在任
 * 何情况下（包括由于异常等原因导致的提前退出），锁都会被正确释放，从而避免了忘记
 * 手动释放锁而导致的死锁问题。
 */

template<typename T>
class BlockQueue {
public:
    explicit BlockQueue(size_t maxsize = 1000);
    ~BlockQueue();
    bool empty();
    bool full();
    void push_back(const T& item);
    void push_front(const T& item); 
    bool pop(T& item);  // 弹出的任务放入item
    bool pop(T& item, int timeout);  // 等待时间
    void clear();
    T front();
    T back();
    size_t capacity();
    size_t size();

    void flush();
    void Close();

private:
    deque<T> deq_;                      // 底层数据结构
    mutex mtx_;                         // 锁
    bool isClose_;                      // 关闭标志
    size_t capacity_;                   // 容量
    condition_variable condConsumer_;   // 消费者条件变量
    condition_variable condProducer_;   // 生产者条件变量
};

// 构造函数，设置队列容量
template<typename T>
BlockQueue<T>::BlockQueue(size_t maxsize) : capacity_(maxsize) {
    assert(maxsize > 0);
    isClose_ = false;
}

template<typename T>
BlockQueue<T>::~BlockQueue() {
    Close();
}

// 关闭队列，清空队列，并通知所有等待的线程
template<typename T>
void BlockQueue<T>::Close() {
    // lock_guard<mutex> locker(mtx_); // 操控队列之前，都需要上锁
    // deq_.clear();                   // 清空队列
    clear();
    isClose_ = true;
    condConsumer_.notify_all();
    condProducer_.notify_all();
}

//lock_guard用于锁mutex，mutex用于锁资源，即数据之类的，为了保证
//mutex同一时刻只有一个线程使用mutex，所以mutex本身也要加锁。
//定义lock_guard变量时，构造函数会自动获取锁，当这个变量离开作用
//域时会自动调用析构函数。
template<typename T>
void BlockQueue<T>::clear() {
    lock_guard<mutex> locker(mtx_);
    deq_.clear();
}

template<typename T>
bool BlockQueue<T>::empty() {
    lock_guard<mutex> locker(mtx_);
    return deq_.empty();
}

template<typename T>
bool BlockQueue<T>::full() {
    lock_guard<mutex> locker(mtx_);
    return deq_.size() >= capacity_;
}

//lock_guard和unique_lock的区别请看这篇文章：
//https://blog.yanjingang.com/?p=6547
// 向队列中添加元素，如果队列满了，则阻塞生产者线程,直到消费者线程取走元素
// 并通知生产者线程，生产者线程才可以继续添加元素。
template<typename T>
void BlockQueue<T>::push_back(const T& item) {
    // 注意，条件变量需要搭配unique_lock
    unique_lock<mutex> locker(mtx_);    
    while(deq_.size() >= capacity_) {   // 队列满了，需要等待
        condProducer_.wait(locker);     // 暂停生产，等待消费者唤醒生产条件变量
    }
    deq_.push_back(item);
    condConsumer_.notify_one();         // 唤醒消费者
}

// 向队列中添加元素，如果队列满了，则阻塞生产者线程,直到消费者线程取走元素
// 并通知生产者线程，生产者线程才可以继续添加元素。
template<typename T>
void BlockQueue<T>::push_front(const T& item) {
    unique_lock<mutex> locker(mtx_);
    while(deq_.size() >= capacity_) {   // 队列满了，需要等待
        condProducer_.wait(locker);     // 暂停生产，等待消费者唤醒生产条件变量
    }
    deq_.push_front(item);
    condConsumer_.notify_one();         // 唤醒消费者
}

// 弹出任务放入item，如果队列为空，则阻塞消费者线程，直到生产者线程添加元素
// 并通知消费者线程，消费者线程才可以继续取出元素。
template<typename T>
bool BlockQueue<T>::pop(T& item) {
    unique_lock<mutex> locker(mtx_);
    while(deq_.empty()) {
        condConsumer_.wait(locker);     // 队列空了，需要等待
    }
    item = deq_.front();
    deq_.pop_front();
    condProducer_.notify_one();         // 唤醒生产者
    return true;
}

// 弹出任务放入item，如果队列为空，则阻塞消费者线程，直到生产者线程添加元素
// 并通知消费者线程，消费者线程才可以继续取出元素。
// timeout为等待时间，单位为秒。
template<typename T>
bool BlockQueue<T>::pop(T &item, int timeout) {
    unique_lock<std::mutex> locker(mtx_);
    while(deq_.empty()){
        if(condConsumer_.wait_for(locker, std::chrono::seconds(timeout)) 
                == std::cv_status::timeout){
            return false;
        }
        if(isClose_){
            return false;
        }
    }
    item = deq_.front();
    deq_.pop_front();
    condProducer_.notify_one();
    return true;
}

template<typename T>
T BlockQueue<T>::front() {
    lock_guard<std::mutex> locker(mtx_);
    return deq_.front();
}

template<typename T>
T BlockQueue<T>::back() {
    lock_guard<std::mutex> locker(mtx_);
    return deq_.back();
}

template<typename T>
size_t BlockQueue<T>::capacity() {
    lock_guard<std::mutex> locker(mtx_);
    return capacity_;
}

template<typename T>
size_t BlockQueue<T>::size() {
    lock_guard<std::mutex> locker(mtx_);
    return deq_.size();
}

// 唤醒消费者
// 用于在队列满时，通知消费者线程，消费者线程可以取出元素
template<typename T>
void BlockQueue<T>::flush() {
    condConsumer_.notify_one();
}
# endif
