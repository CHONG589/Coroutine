#ifndef BUFFER_H
#define BUFFER_H
#include <cstring>   //perror
#include <iostream>
#include <unistd.h>  // write
#include <sys/uio.h> //readv
#include <vector> //readv
#include <atomic>
#include <assert.h>

/**
 * @attention:读写接口有两种，一个是与客户端直接IO交互所需要的读写接口，
 *            第二个是缓冲区收到了HTTP请求后，我们在处理过程中需要对缓
 *            冲区的读写接口。
 */

//可以参考这里对Buffer的讲解https://blog.csdn.net/T_Solotov/article/details/124044175

class Buffer {
public:
    Buffer(int initBuffSize = 1024);
    ~Buffer() = default;

    //还可以写的数量（字节），即还剩多少空间。
    size_t WritableBytes() const;   
    //还可以读的数量，即还有多少数据在缓存    
    size_t ReadableBytes() const ;
    //Buffer对外表现出queue的特性，随着读写数据，前面会空出越来越多的空间，
    //这个空间就是Prependable空间
    size_t PrependableBytes() const;

    //返回可读数据的起始地址
    const char* Peek() const;
    //确保可写的长度
    void EnsureWriteable(size_t len);
    void HasWritten(size_t len);

    //读取len长度的数据，移动读下标，这里读取数据只需要移动下标即可，
    //因为读数据只要根据下标索引就行，最后再用这个函数移动读取完的数
    //据下标
    void Retrieve(size_t len);
    //读取到end位置，如果向要读取len大小的数据，而buffer中没那么多，
    //那么就只读取buffer中的所有数据就可以了。
    void RetrieveUntil(const char* end);

    //读写下标归零,在别的函数中会用到
    void RetrieveAll();
    //注意，这里才是真正的读取数据，读取完数据后才用Retrieve,
    //RetrieveUntil,RetrieveAll,HasWritten这些函数移动下标。
    std::string RetrieveAllToStr();

    //// 写指针的位置
    const char* BeginWriteConst() const;
    char* BeginWrite();

    void Append(const std::string& str);
    void Append(const char* str, size_t len);
    void Append(const void* data, size_t len);
    void Append(const Buffer& buff);

    //与客户端直接IO的读写接口（httpconn中就是调用的该接口）
    ssize_t ReadFd(int fd, int* Errno);
    ssize_t WriteFd(int fd, int* Errno);

private:
    char* BeginPtr_();  // buffer开头
    const char* BeginPtr_() const;
    //vector不够空间是扩容用的
    void MakeSpace_(size_t len);

    //存储数据
    std::vector<char> buffer_;  
    std::atomic<std::size_t> readPos_;  // 读的下标
    std::atomic<std::size_t> writePos_; // 写的下标
};

#endif //BUFFER_H
