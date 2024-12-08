#ifndef LOG_H
#define LOG_H

#include <mutex>
#include <string>
#include <thread>
#include <sys/time.h>
#include <string.h>
#include <stdarg.h>           // vastart va_end
#include <assert.h>
#include <sys/stat.h>         // mkdir
#include "blockqueue.h"
#include "buffer.h"

/**
 * 能顺利写日志但是又不要占用主线程时间去写，所以我们设计**异步写日志**的模块。
 */

/**
 * C++11的标准类std::thread对线程进行了封装，定义了C++11标准中的一些表示线程的类、
 * 用于互斥访问的类与方法等。应用C++11中的std::thread便于多线程程序的移值。
 * 即将线程封装成类，不用再像C一样要自己创建等一系列操作，只要用类构造出一个线程对象，
 * 并且将线程要处理的操作给线程，就可以启动线程，线程就会自动运行。当然日志类在析构
 * 时要先将线程 join 掉，然后才会销毁。
 */

class Log {
public:
    // 初始化日志实例（阻塞队列最大容量、日志保存路径、日志文件后缀）
    void init(int level, const char* path = "./log", 
                const char* suffix =".log",
                int maxQueueCapacity = 1024);

    static Log* Instance();
    static void FlushLogThread();   // 异步写日志公有方法，调用私有方法asyncWrite
    
    void write(int level, const char *format,...);  // 将输出内容按照标准格式整理
    // 调用 blockqueue中的 flush方法，唤醒写线程将缓冲区中的内容写入文件
    void flush();

    int GetLevel();
    void SetLevel(int level);
    bool IsOpen() { return isOpen_; }
    
private:
    Log();
    void AppendLogLevelTitle_(int level);
    virtual ~Log();
    void AsyncWrite_(); // 异步写日志方法

private:
    static const int LOG_PATH_LEN = 256;    // 日志文件最长文件名
    static const int LOG_NAME_LEN = 256;    // 日志最长名字
    static const int MAX_LINES = 50000;     // 日志文件内的最长日志条数

    const char* path_;          //路径名
    const char* suffix_;        //后缀名

    int MAX_LINES_;             // 最大日志行数

    int lineCount_;             //日志行数记录
    int toDay_;                 //按当天日期区分文件

    bool isOpen_;               
 
    Buffer buff_;       // 输出的内容，缓冲区
    int level_;         // 日志等级
    bool isAsync_;      // 是否开启异步日志

    FILE* fp_;                                          //打开log的文件指针
    std::unique_ptr<BlockQueue<std::string>> deque_;    //阻塞队列
    std::unique_ptr<std::thread> writeThread_;          //写线程的指针
    std::mutex mtx_;                                    //同步日志必需的互斥量
};

// 四个宏定义，主要用于不同类型的日志输出，也是外部使用日志的接口
// ...表示可变参数，__VA_ARGS__就是将...的值复制到这里
// 前面加上##的作用是：当可变参数的个数为0时，这里的##可以把把前面多余的","去掉,否则会编译出错。
#define LOG_DEBUG(format, ...) if (Log::Instance()->IsOpen()) {Log::Instance()->write(0, format, ##__VA_ARGS__); Log::Instance()->flush();}
#define LOG_INFO(format, ...) if(Log::Instance()->IsOpen()) {Log::Instance()->write(1, format, ##__VA_ARGS__); Log::Instance()->flush();}
#define LOG_WARN(format, ...) if(Log::Instance()->IsOpen()) {Log::Instance()->write(2, format, ##__VA_ARGS__); Log::Instance()->flush();}
#define LOG_ERROR(format, ...) if(Log::Instance()->IsOpen()) {Log::Instance()->write(3, format, ##__VA_ARGS__); Log::Instance()->flush();}

#endif //LOG_H
