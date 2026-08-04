#include <vector>
#include <iostream>
#include <cassert>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <memory>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <typeinfo>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#define INF 0
#define DBG 1
#define ERR 2
#define LOG_LEVEL DBG

#define LOG(level, format, ...)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        if (level < LOG_LEVEL)                                                                                         \
            break;                                                                                                     \
        time_t t = time(NULL);                                                                                         \
        struct tm *ltm = localtime(&t);                                                                                \
        char tmp[32] = {0};                                                                                            \
        strftime(tmp, 31, "%H:%M:%S", ltm);                                                                            \
        fprintf(stdout, "[%p %s %s:%d] " format "\n", (void *)pthread_self(), tmp, __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (0)

#define INF_LOG(format, ...) LOG(INF, format, ##__VA_ARGS__)
#define DBG_LOG(format, ...) LOG(DBG, format, ##__VA_ARGS__)
#define ERR_LOG(format, ...) LOG(ERR, format, ##__VA_ARGS__)

#define BUFFER_DEFAULT_SIZE 1024
class Buffer
{
private:
    std::vector<char> _buffer; // 缓冲区
    uint64_t _reader_idx;      // 读偏移位置
    uint64_t _writer_idx;      // 写偏移位置
public:
    Buffer() : _buffer(BUFFER_DEFAULT_SIZE), _reader_idx(0), _writer_idx(0) {}
    // 获取缓冲区的起始地址
    char *Begin()
    {
        return _buffer.data();
    }
    // 获取读位置起始地址
    char *ReadPosition()
    {
        return Begin() + _reader_idx;
    }
    // 获取写位置起始地址
    char *WritePosition()
    {
        return Begin() + _writer_idx;
    }
    // 获取缓冲区后面的空闲空间-写位置之后的空间
    uint64_t TailIdleSize()
    {
        return _buffer.size() - _writer_idx;
    }
    // 获取缓冲区前面的空间空间-读位置之前的空间
    uint64_t HeadIdleSize()
    {
        return _reader_idx;
    }
    // 获取可读空间的大小
    uint64_t ReadAbleSize()
    {
        return _writer_idx - _reader_idx;
    }
    // 移动读位置
    void MoveReadOffset(uint64_t len)
    {
        if (len == 0)
            return;
        assert(len <= ReadAbleSize());
        _reader_idx += len;
    }
    // 移动写位置
    void MoveWriteOffset(uint64_t len)
    {
        if (len == 0)
            return;
        assert(len <= TailIdleSize());
        _writer_idx += len;
    }
    // 确保可写空间足够
    void EnsureWriteSpace(uint64_t len)
    {
        // 后面空间足够，直接返回
        if (len <= TailIdleSize())
            return;
        // 后面空间不够，但是加上前面空间够，则移动数据
        if (len <= TailIdleSize() + HeadIdleSize())
        {
            uint64_t ras = ReadAbleSize();
            std::copy(ReadPosition(), ReadPosition() + ras, Begin());
            _reader_idx = 0;
            _writer_idx = ras;
        }
        else
        {
            // 总共空间不够，直接在后面扩容，不移动数据
            _buffer.resize(_writer_idx + len);
        }
    }
    // 写入数据
    void Write(const void *data, uint64_t len)
    {
        if (len == 0)
            return;
        // 1.确保空间足够 2.将数据拷贝到可写位置
        EnsureWriteSpace(len);
        const char *d = static_cast<const char *>(data);
        // const char* d=(const char*)data;
        std::copy(d, d + len, WritePosition());
    }
    void WriteAndPush(const void *data, uint64_t len)
    {
        Write(data, len);
        MoveWriteOffset(len);
    }
    void WriteAsString(const std::string &data)
    {
        return Write(data.c_str(), data.size());
    }
    void WriteStringAndPush(const std::string &data)
    {
        WriteAsString(data);
        MoveWriteOffset(data.size());
    }
    void WriteAsBuffer(Buffer &data)
    {
        return Write(data.ReadPosition(), data.ReadAbleSize());
    }
    void WriteBufferAndPush(Buffer &data)
    {
        WriteAsBuffer(data);
        MoveWriteOffset(data.ReadAbleSize());
    }
    // 读取数据
    void Read(void *buf, uint64_t len)
    {
        assert(len <= ReadAbleSize());
        std::copy(ReadPosition(), ReadPosition() + len, (char *)buf);
    }
    void ReadAndPop(void *buf, uint64_t len)
    {
        Read(buf, len);
        MoveReadOffset(len);
    }
    std::string ReadAsString(uint64_t len)
    {
        assert(len <= ReadAbleSize());
        std::string str;
        str.resize(len);
        Read(&str[0], str.size());
        return str;
    }
    std::string ReadAsStringAndPop(uint64_t len)
    {
        assert(len <= ReadAbleSize());
        std::string str = ReadAsString(len);
        MoveReadOffset(len);
        return str;
    }
    char *FindCRLF()
    {
        char *res = (char *)memchr(ReadPosition(), '\n', ReadAbleSize());
        return res;
    }
    /*通常获取一行数据，这种情况针对是*/
    std::string GetLine()
    {
        char *pos = FindCRLF();
        if (pos == nullptr)
        {
            return "";
        }
        // +1是为了把换行字符也取出来
        return ReadAsString(pos - ReadPosition() + 1);
    }
    std::string GetLineAndPop()
    {
        std::string str = GetLine();
        MoveReadOffset(str.size());
        return str;
    }

    // 清空缓冲区
    void clear()
    {
        _reader_idx = 0;
        _writer_idx = 0;
    }
};

#define MAX_BACKLOG 1024
class Socket
{
private:
    int _sockfd;

public:
    Socket() : _sockfd(-1) {}
    Socket(int fd) : _sockfd(fd) {}
    ~Socket() { Close(); }
    int Fd() { return _sockfd; }
    // 创建套接字
    bool CreateSocket()
    {
        _sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (_sockfd < 0)
        {
            ERR_LOG("CREATE SOCKET FATAL");
            return false;
        }
        return true;
    }
    // 地址绑定
    bool Bind(const std::string &ip, uint16_t port)
    {
        // int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
        struct sockaddr_in local;
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        local.sin_addr.s_addr = inet_addr(ip.c_str());
        int ret = bind(_sockfd, (const sockaddr *)&local, sizeof(local));
        if (ret < 0)
        {
            ERR_LOG("BIND SOCKET FATAL");
            return false;
        }
        return true;
    }
    // 开始监听
    bool Listen(int backlog = MAX_BACKLOG)
    {
        int ret = listen(_sockfd, backlog);
        if (ret < 0)
        {
            ERR_LOG("LISTEN SOCKET FATAL");
            return false;
        }
        return true;
    }
    // 连接服务器
    bool Connect(const std::string &ip, uint16_t port)
    {
        struct sockaddr_in local;
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        local.sin_addr.s_addr = inet_addr(ip.c_str());
        int ret = connect(_sockfd, (const sockaddr *)&local, sizeof(local));
        if (ret < 0)
        {
            ERR_LOG("CONNECT SOCKET FATAL");
            std::cout << strerror(errno) << std::endl;
            return false;
        }
        return true;
    }
    // 获取新连接
    int Accept()
    {
        int newfd = accept(_sockfd, nullptr, nullptr);
        if (newfd < 0)
        {
            ERR_LOG("ACCEPT SOCKET FATAL");
            return -1;
        }
        return newfd;
    }
    // 发送数据
    ssize_t Recv(void *buf, size_t len, int flag = 0)
    {
        ssize_t ret = recv(_sockfd, buf, len, flag);
        if (ret <= 0)
        {
            // EINTR表示被信号打断,eintr
            // EAGAIN表示非阻塞状态 eagain
            if (errno == EINTR || errno == EAGAIN)
                return 0;
            ERR_LOG("RECV  SOCKET FATAL");
            return -1;
        }
        return ret;
    }
    ssize_t RecvNoBlock(void *buf, size_t len)
    {
        return Recv(buf, len, MSG_DONTWAIT); // dontwait;
    }
    // 接收数据
    ssize_t Send(const void *buf, size_t len, int flag = 0)
    {
        ssize_t ret = send(_sockfd, buf, len, flag);
        if (ret < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return 0; // 缓冲区满/信号中断，上层后续重试发送
            }
            ERR_LOG("SEND SOCKET FATAL");
            return -1;
        }
        return ret;
    }
    ssize_t SendNoBlock(void *buf, size_t len)
    {
        if (len == 0)
            return 0;
        return Send(buf, len, MSG_DONTWAIT);
    }
    // 关闭套接字
    void Close()
    {
        if (_sockfd != -1)
            close(_sockfd);
        _sockfd = -1;
    }
    // 建立服务端连接
    bool CreateServer(uint16_t port, const std::string &ip = "0.0.0.0", bool block_flag = false)
    {
        // 1.创建套接字，2.设置非堵塞 3.地址绑定 4.监听 5.地址复用
        if (!CreateSocket())
            return false;
        ReuseAddress();
        if (block_flag)
            NonBlock();
        if (!Bind(ip, port))
            return false;
        if (!Listen())
            return false;
        return true;
    }
    // 建立客户端连接
    bool CreateClient(uint16_t port, const std::string &ip)
    {
        // 1。创建套接字 2.连接服务器
        if (!CreateSocket())
            return false;
        if (!Connect(ip, port))
            return false;
        return true;
    }
    // 设置套接字选项-地址复用
    void ReuseAddress()
    {
        // int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
        int opt = 1;
        setsockopt(_sockfd, SOL_SOCKET, SO_REUSEADDR, (void *)&opt, sizeof(opt));
        opt = 1;
        setsockopt(_sockfd, SOL_SOCKET, SO_REUSEPORT, (void *)&opt, sizeof(opt));
    }
    // 设置套接字非堵塞
    void NonBlock()
    {
        int flag = fcntl(_sockfd, F_GETFL, 0);
        fcntl(_sockfd, F_SETFL, flag | O_NONBLOCK);
    }
};
class Poller;
class EventLoop;
class Channel
{
private:
    int _fd;
    EventLoop *_loop;
    uint32_t _events;  // 当前连接需要监控的事件
    uint32_t _revents; // 当前连接需要触发的事件
    using EventCallback = std::function<void()>;
    EventCallback _read_callback;  // 可读事件被触发的回调函数
    EventCallback _write_callback; // 可写事件被触发的回调函数
    EventCallback _error_callback; // 错误事件被触发的回调函数
    EventCallback _close_callback; // 连接断开事件被触发的回调函数
    EventCallback _event_callback; // 所有事件被触发的回调函数
public:
    Channel(EventLoop *loop, int fd) : _fd(fd), _events(0), _revents(0), _loop(loop) {}
    int Fd()
    {
        return _fd;
    }
    uint32_t Events()
    {
        return _events;
    }
    void SetREvent(uint32_t event)
    {
        _revents = event;
    }
    void SetReadCallback(const EventCallback &cb)
    {
        _read_callback = cb;
    }
    void SetWriteCallback(const EventCallback &cb)
    {
        _write_callback = cb;
    }
    void SetErrorCallback(const EventCallback &cb)
    {
        _error_callback = cb;
    }
    void SetCloseCallback(const EventCallback &cb)
    {
        _close_callback = cb;
    }
    void SetEventCallback(const EventCallback &cb)
    {
        _event_callback = cb;
    }

    // 判断描述符是否监控可读
    bool ReadAble()
    {
        return (_events & EPOLLIN);
    }
    // 判断描述符是否监控可写
    bool WriteAble()
    {
        return (_events & EPOLLOUT);
    }
    // 启动可读事件监控
    void EnableRead()
    {
        _events |= EPOLLIN;
        Update();
    }
    // 启动可写事件监控
    void EnableWrite()
    {
        _events |= EPOLLOUT;
        Update();
    }
    // 解除可读事件监控
    void DisableRead()
    {
        _events &= ~EPOLLIN;
        Update();
    }
    // 解除可写事件监控
    void DisableWrite()
    {
        _events &= ~EPOLLOUT;
        Update();
    }
    // 解除所有事件监控
    void DisableAll()
    {
        _events = 0;
        Update();
    }
    void Update();
    // 移除监控
    void Remove();
    // 处理事件,
    void HandleEvent()
    {
        if ((_revents & EPOLLIN) || (_revents & EPOLLRDHUP) || (_revents & EPOLLPRI))
        {
            // 任何事件都调用的回调函数
            if (_event_callback)
                _event_callback();
            if (_read_callback)
                _read_callback();
        }
        //
        if (_revents & EPOLLOUT)
        {
            // 任何事件都调用的回调函数，放在事件处理之后刷新活跃读
            if (_event_callback)
                _event_callback();
            if (_write_callback)
                _write_callback();
        }
        // 用else if是读写可以同时进行，tcp全双工，错误和挂断是异常与读写互斥
        else if (_revents & EPOLLERR)
        {
            if (_event_callback) // 一旦出错就会释放，放在前面执行回调函数
                _event_callback();
            if (_error_callback)
                _error_callback();
        }
        else if (_revents & EPOLLHUP)
        {
            // if (_event_callback)
            //     _event_callback();
            if (_close_callback)
                _close_callback();
        }
    }
};
#define MAX_EPOLLEVENTS 1024
class Poller
{
private:
    int _epfd;
    struct epoll_event _evs[MAX_EPOLLEVENTS];
    std::unordered_map<int, Channel *> _channels;

private:
    // 判断一个Channel是否添加事件监控
    bool HasChannel(Channel *channel)
    {
        auto it = _channels.find(channel->Fd());
        if (it == _channels.end())
        {
            return false;
        }
        return true;
    }
    // 对epoll的直接操作
    void Update(Channel *channel, int op)
    {
        // int epoll_ctl(int epfd,int op,int fd,strct epoll_event *ev)
        int fd = channel->Fd();
        struct epoll_event ev;
        ev.data.fd = fd;
        ev.events = channel->Events();
        int ret = epoll_ctl(_epfd, op, fd, &ev);
        if (ret < 0)
        {
            ERR_LOG("EPOLL_CTR ERROR");
        }
        return;
    }

public:
    Poller()
    {
        _epfd = epoll_create(1);
    }
    // 添加或修改描述符的事件监控
    void UpdateEvent(Channel *channel)
    {
        if (!HasChannel(channel)) // 未添加
        {
            _channels.insert(std::make_pair(channel->Fd(), channel));
            return Update(channel, EPOLL_CTL_ADD);
        }
        return Update(channel, EPOLL_CTL_MOD);
    }
    // 移除描述符的事件监控
    void RemoveEvent(Channel *channel)
    {
        auto it = _channels.find(channel->Fd());
        if (it != _channels.end())
        {
            _channels.erase(it);
        }
        Update(channel, EPOLL_CTL_DEL);
    }
    // 开始监控,返回活跃事件
    void Poll(std::vector<Channel *> *active)
    {
        // int epoll_wait(int epfd,struct epoll_evet* events,int maxevents,int timeout)
        int nfd = epoll_wait(_epfd, _evs, MAX_EPOLLEVENTS, -1);
        if (nfd < 0)
        {
            if (errno == EINTR)
            {
                return;
            }
            ERR_LOG("EPOLL_WAIT FATAL");
            abort();
        }
        for (int i = 0; i < nfd; i++)
        {
            auto it = _channels.find(_evs[i].data.fd);
            assert(it != _channels.end());
            it->second->SetREvent(_evs[i].events);
            active->push_back(it->second);
        }
        return;
    }
};

using TaskFunc = std::function<void()>;
using ReleaseFunc = std::function<void()>;
class TimerTask
{
private:
    uint64_t _id;         // 定时器的id
    uint32_t _timeout;    // 定时任务的超时时间
    bool _canceled;       // false-表示不取消，true-表示取消
    TaskFunc _task_cb;    // 定时器执行的定时任务
    ReleaseFunc _release; // 用于删除Timerwheel中保存的定时器对象信息
public:
    TimerTask(uint64_t id, uint32_t delay, const TaskFunc &cb) : _id(id), _timeout(delay), _task_cb(cb), _canceled(false) {}
    ~TimerTask()
    {
        if (_canceled == false)
            _task_cb();
        _release();
    }
    void SetRelease(const ReleaseFunc &cb) { _release = cb; }
    uint32_t Delaytime() { return _timeout; }
    void Cancel() { _canceled = true; }
};

class TimerWheel
{
private:
    using PtrTask = std::shared_ptr<TimerTask>;
    using WeakTask = std::weak_ptr<TimerTask>;
    int _capacity;                            // 表盘的最大容量
    std::vector<std::vector<PtrTask>> _wheel; // 时间轮二位数组
    int _tick;                                // 当前的秒针，指到哪里就释放哪里，释放哪里就执行哪里的任务
    std::unordered_map<uint64_t, WeakTask> _timers;

    EventLoop *_loop;
    int _timerfd; ////定时器描述符--可读事件回调就是读取计数器，执行定时任务
    std::unique_ptr<Channel> _timer_channel;

private:
    void RemoveTimer(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it != _timers.end())
        {
            _timers.erase(id);
        }
    }
    static int CreateTimerfd()
    {
        int timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
        if (timerfd < 0)
        {
            ERR_LOG("TIMERFD CREATE FATAL");
            abort();
        }
        struct itimerspec itmer;
        itmer.it_value.tv_sec = 1; // 第一次超时事件在1s后
        itmer.it_value.tv_nsec = 0;
        itmer.it_interval.tv_sec = 1; // 第一次之后超时的时间间隔
        itmer.it_interval.tv_nsec = 0;
        timerfd_settime(timerfd, 0, &itmer, nullptr);
        return timerfd;
    }
    void ReadTimerfd()
    {
        uint64_t tmp;
        int ret = read(_timerfd, &tmp, 8);
        if (ret < 0)
        {
            ERR_LOG("TIMERFD READ FATAL");
            abort();
        }
        return;
    }
    // 这个函数每秒向后走一步
    void RunTimeTask()
    {
        _tick = (_tick + 1) % _capacity;
        _wheel[_tick].clear(); // 清空数组，就是把数组中所有管理定时器对象的shared_ptr释放掉
    }
    void OnTime()
    {
        ReadTimerfd();
        RunTimeTask();
    }
    void TimerAddOnLoop(uint64_t id, uint32_t delay, const TaskFunc &cb)
    {
        PtrTask pt = std::make_shared<TimerTask>(id, delay, cb);
        pt->SetRelease(std::bind(&TimerWheel::RemoveTimer, this, id));
        int pos = (_tick + delay) % _capacity;
        _wheel[pos].push_back(pt);
        _timers[id] = WeakTask(pt);
    }
    void TimerCancelOnLoop(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it == _timers.end())
        {
            return; // 任务不存在
        }
        PtrTask pt = it->second.lock();
        pt->Cancel();
    }
    // 刷新/延迟定时任务
    void TimerRefreshOnLoop(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it == _timers.end())
        {
            return; // 任务不存在
        }
        PtrTask pt = it->second.lock(); // 由weak_ptr管理的对象通过lock获得shared_ptr
        int delay = pt->Delaytime();
        int pos = (_tick + delay) % _capacity;
        _wheel[pos].push_back(pt);
    }

public:
    TimerWheel(EventLoop *loop) : _capacity(60), _tick(0), _wheel(_capacity),
                                  _loop(loop), _timerfd(CreateTimerfd()), _timer_channel(std::make_unique<Channel>(loop, _timerfd))
    {
        _timer_channel->SetReadCallback(std::bind(&TimerWheel::OnTime, this));
        _timer_channel->EnableRead();
    }

    // 添加定时任务
    void TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb);
    void TimerCancel(uint64_t id);
    void TimerRefresh(uint64_t id);
    bool HasTimer(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it == _timers.end())
        {
            return false;
        }
        return true;
    }
};

class EventLoop
{
private:
    using Functor = std::function<void()>;
    std::thread::id _thread_id; // 当前线程的线程Id
    int _event_fd;              // 唤醒IO事件监控有可能导致的+堵塞
    std::unique_ptr<Channel> _event_channel;
    Poller _poller;              // 对所有事件进行事件监控
    std::vector<Functor> _tasks; // 任务池
    std::mutex _mutex;           // 实现任务池操作的线程安全

    TimerWheel _timerwheel; // 定时器模块
private:
    void RunAllTask()
    {
        std::vector<Functor> functor;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _tasks.swap(functor);
        }
        for (auto &f : functor)
        {
            f();
        }
        return;
    }
    static int CreateEventfd()
    {
        int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (efd < 0)
        {
            ERR_LOG("CREATE EVENTFD FAILED!");
            abort(); // 让程序异常退出
        }
        return efd;
    }
    void ReadEventfd()
    {
        uint64_t res = 0;
        int ret = read(_event_fd, &res, sizeof(res));
        if (ret < 0)
        {
            // EINTR -- 被信号打断;    EAGAIN -- 表示无数据可读
            if (errno == EINTR || errno == EAGAIN)
            {
                return;
            }
            ERR_LOG("READ EVENTFD FAILED!");
            abort();
        }
        return;
    }
    void WeakUpEventfd()
    {
        uint64_t val = 1;
        int ret = write(_event_fd, &val, sizeof(val));
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                return;
            }
            ERR_LOG("READ EVENTFD FAILED!");
            abort();
        }
        return;
    }

public:
    EventLoop() : _thread_id(std::this_thread::get_id()),
                  _event_fd(CreateEventfd()),
                  _event_channel(new Channel(this, _event_fd)),
                  _timerwheel(this)
    {
        // 给eventfd添加可读事件回调函数，读取eventfd事件通知次数
        _event_channel->SetReadCallback(std::bind(&EventLoop::ReadEventfd, this));
        // 启动eventfd的读事件监控
        _event_channel->EnableRead();
    }
    // 事件监控->就绪事件处理->执行任务
    void Start()
    {
        while (1)
        {
            // 1.事件监控
            std::vector<Channel *> active;
            _poller.Poll(&active);
            // 2.事件处理
            for (auto &channel : active)
            {
                channel->HandleEvent();
            }
            // 3.执行任务
            RunAllTask();
        }
    }
    // 判断当前线程是否是EventLoop对于的线程
    bool IsInLoop()
    {
        return _thread_id == std::this_thread::get_id();
    }
    void AssertInLoop()
    {
        assert(_thread_id == std::this_thread::get_id());
    }
    // 判断要执行任务是否处于当前线程中，如果是则执行，否则压入任务队列
    void RunInLoop(const Functor &cb)
    {
        if (IsInLoop())
        {
            cb();
        }
        else
        {
            QueueInLoop(cb);
        }
    }
    // 将操作压入任务队列
    void QueueInLoop(const Functor &cb)
    {
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _tasks.push_back(cb);
        }
        WeakUpEventfd();
    }

    // 添加/修改描述符的事件监控
    void UpdateEvent(Channel *channel)
    {
        return _poller.UpdateEvent(channel);
    }
    // 移除描述符的监控
    void RemoveEvent(Channel *channel)
    {
        return _poller.RemoveEvent(channel);
    }

    void TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb)
    {
        return _timerwheel.TimerAdd(id, delay, cb);
    }
    void TimerCancel(uint64_t id)
    {
        return _timerwheel.TimerCancel(id);
    }
    void TimerRefresh(uint64_t id)
    {
        return _timerwheel.TimerRefresh(id);
    }
    bool HasTimer(uint64_t id)
    {
        return _timerwheel.HasTimer(id);
    }
};
class LoopThread
{
private:
    /*用于实现_loop获取的同步关系，避免线程创建了，但是_loop还没有实例化之前去获取_loop*/
    std::mutex _mutex;             // 互斥锁
    std::condition_variable _cond; // 条件变量
    EventLoop *_loop;              // EventLoop指针变量，这个对象需要在线程内实例化
    std::thread _thread;           // EventLoop对应的线程
private:
    /*实例化 EventLoop 对象，唤醒_cond上有可能阻塞的线程，并且开始运行EventLoop模块的功能*/
    void ThreadEntry()
    {
        EventLoop loop;
        {
            std::unique_lock<std::mutex> lock(_mutex); // 加锁
            _loop = &loop;
            _cond.notify_all();
        }
        loop.Start();
    }

public:
    /*创建线程，设定线程入口函数*/
    LoopThread() : _loop(nullptr), _thread(std::thread(&LoopThread::ThreadEntry, this)) {}
    /*返回当前线程关联的EventLoop对象指针*/
    EventLoop *GetLoop()
    {
        EventLoop *loop = nullptr;
        {
            std::unique_lock<std::mutex> lock(_mutex); // 加锁
            _cond.wait(lock, [&]()
                       { return _loop != nullptr; }); // loop为nullptr就一直阻塞
            loop = _loop;
        }
        return loop;
    }
};
class LoopThreadPool
{
private:
    int _thread_count;
    int _next_idx;
    EventLoop *_baseloop;
    std::vector<LoopThread *> _threads;
    std::vector<EventLoop *> _loops;

public:
    LoopThreadPool(EventLoop *baseloop) : _thread_count(0), _next_idx(0), _baseloop(baseloop) {}
    void SetThreadCount(int count)
    {
        _thread_count = count;
    }
    void Create()
    {
        if (_thread_count > 0)
        {
            _threads.resize(_thread_count);
            _loops.resize(_thread_count);
            for (int i = 0; i < _thread_count; i++)
            {
                _threads[i] = new LoopThread();
                _loops[i] = _threads[i]->GetLoop();
            }
        }
        return;
    }
    EventLoop *NextLoop()
    {
        if (_thread_count == 0)
        {
            return _baseloop;
        }
        _next_idx = (_next_idx + 1) % _thread_count;
        return _loops[_next_idx];
    }
};
class Any
{
private:
    class holder
    {
    public:
        virtual ~holder() {}
        virtual const std::type_info &type() = 0;
        virtual holder *clone() = 0;
    };
    template <class T>
    class placeholder : public holder
    {
    public:
        placeholder(const T &val) : _val(val) {}
        // 获取子类对象存储的类型
        const std::type_info &type() { return typeid(T); }
        // 通过本身的子类对象，构造出新的子类对象
        holder *clone() { return new placeholder(_val); }

    public:
        T _val;
    };
    holder *_content;

public:
    Any() : _content(nullptr) {}
    template <class T>
    Any(const T &val) : _content(new placeholder<T>(val)) {}
    Any(const Any &other) : _content(other._content ? other._content->clone() : nullptr) {}
    ~Any() { delete _content; }

    Any &swap(Any &other)
    {
        std::swap(_content, other._content);
        return *this;
    }
    // 返回子类对象保存的数据的指针
    template <class T>
    T *get()
    {
        // 想要获取的数据类型，必须和保存的数据类型一致
        assert(typeid(T) == _content->type());
        return &((placeholder<T> *)_content)->_val;
    }
    // 赋值运算符的重载函数
    template <class T>
    Any &operator=(const T &val)
    {
        // 为val构造一个临时的通用容器，然后与当前容器自身进行指针交换，临时对象释放的时候，原先保存的数据也就被释放
        Any(val).swap(*this);
        return *this;
    }
    Any &operator=(const Any &other)
    {
        Any(other).swap(*this);
        return *this;
    }
};
class Connection;
// DISCONNECTED -- 连接关闭状态；   CONNECTING -- 连接建立成功-待处理状态
// CONNECTED -- 连接建立完成，各种设置已完成，可以通信的状态；  DISCONNECTING -- 待关闭状态
typedef enum
{
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    DISCONNECTING
} ConnStatu;
using PtrConnection = std::shared_ptr<Connection>;
class Connection : public std::enable_shared_from_this<Connection>
{
private:
    uint64_t _conn_id; // 连接的唯一ID，便于连接的管理和查找
    // uint64_t _timer_id;   //定时器ID，必须是唯一的，这块为了简化操作使用conn_id作为定时器ID
    int _sockfd;                   // 连接关联的文件描述符
    bool _enable_inactive_release; // 连接是否启动非活跃销毁的判断标志，默认为false
    EventLoop *_loop;              // 连接所关联的一个EventLoop
    ConnStatu _statu;              // 连接状态
    Socket _socket;                // 套接字操作管理
    Channel _channel;              // 连接的事件管理
    Buffer _in_buffer;             // 输入缓冲区---存放从socket中读取到的数据
    Buffer _out_buffer;            // 输出缓冲区---存放要发送给对端的数据
    Any _context;                  // 请求的接收处理上下文

    /*这四个回调函数，是让服务器模块来设置的（其实服务器模块的处理回调也是组件使用者设置的）*/
    /*换句话说，这几个回调都是组件使用者使用的*/
    using ConnectedCallback = std::function<void(const PtrConnection &)>;
    using MessageCallback = std::function<void(const PtrConnection &, Buffer *)>;
    using ClosedCallback = std::function<void(const PtrConnection &)>;
    using AnyEventCallback = std::function<void(const PtrConnection &)>;
    ConnectedCallback _connected_callback;
    MessageCallback _message_callback;
    ClosedCallback _closed_callback;
    AnyEventCallback _event_callback;
    /*组件内的连接关闭回调--组件内设置的，因为服务器组件内会把所有的连接管理起来，一旦某个连接要关闭*/
    /*就应该从管理的地方移除掉自己的信息*/
    ClosedCallback _server_closed_callback;

private:
    /*五个channel的事件回调函数*/
    // 描述符可读事件触发后调用的函数，接收socket数据放到接收缓冲区中，然后调用_message_callback
    void HandleRead()
    {
        // 1. 接收socket的数据，放到缓冲区
        char buf[65536];
        ssize_t ret = _socket.RecvNoBlock(buf, 65535);
        if (ret < 0)
        {
            // 出错了,不能直接关闭连接
            return ShutdownInLoop();
        }
        // 这里的等于0表示的是没有读取到数据，而并不是连接断开了，连接断开返回的是-1
        // 将数据放入输入缓冲区,写入之后顺便将写偏移向后移动
        _in_buffer.WriteAndPush(buf, ret);
        // 2. 调用message_callback进行业务处理
        if (_in_buffer.ReadAbleSize() > 0)
        {
            // shared_from_this--从当前对象自身获取自身的shared_ptr管理对象
            return _message_callback(shared_from_this(), &_in_buffer);
        }
    }
    // 描述符可写事件触发后调用的函数，将发送缓冲区中的数据进行发送
    void HandleWrite()
    {
        //_out_buffer中保存的数据就是要发送的数据
        ssize_t ret = _socket.SendNoBlock(_out_buffer.ReadPosition(), _out_buffer.ReadAbleSize());
        if (ret < 0)
        {
            // 发送错误就该关闭连接了，
            if (_in_buffer.ReadAbleSize() > 0)
            {
                _message_callback(shared_from_this(), &_in_buffer);
            }
            return ReleaseInLoop(); // 这时候就是实际的关闭释放操作了。
        }
        _out_buffer.MoveReadOffset(ret); // 千万不要忘了，将读偏移向后移动
        if (_out_buffer.ReadAbleSize() == 0)
        {
            _channel.DisableWrite(); // 没有数据待发送了，关闭写事件监控
            // 如果当前是连接待关闭状态，则有数据，发送完数据释放连接，没有数据则直接释放
            if (_statu == DISCONNECTING)
            {
                return ReleaseInLoop();
            }
        }
        return;
    }
    // 描述符触发挂断事件
    void HandleClose()
    {
        /*一旦连接挂断了，套接字就什么都干不了了，因此有数据待处理就处理一下，完毕关闭连接*/
        if (_in_buffer.ReadAbleSize() > 0)
        {
            _message_callback(shared_from_this(), &_in_buffer);
        }
        return ReleaseInLoop();
    }
    // 描述符触发出错事件
    void HandleError()
    {
        return HandleClose();
    }
    // 描述符触发任意事件: 1. 刷新连接的活跃度--延迟定时销毁任务；  2. 调用组件使用者的任意事件回调
    void HandleEvent()
    {
        if (_enable_inactive_release == true)
        {
            _loop->TimerRefresh(_conn_id);
        }
        if (_event_callback)
        {
            _event_callback(shared_from_this());
        }
    }
    // 连接获取之后，所处的状态下要进行各种设置（启动读监控,调用回调函数）
    void EstablishedInLoop()
    {
        // 1. 修改连接状态；  2. 启动读事件监控；  3. 调用回调函数
        assert(_statu == CONNECTING); // 当前的状态必须一定是上层的半连接状态
        _statu = CONNECTED;           // 当前函数执行完毕，则连接进入已完成连接状态
        // 一旦启动读事件监控就有可能会立即触发读事件，如果这时候启动了非活跃连接销毁
        _channel.EnableRead();
        if (_connected_callback)
            _connected_callback(shared_from_this());
    }
    // 这个接口才是实际的释放接口
    void ReleaseInLoop()
    {
        // 1. 修改连接状态，将其置为DISCONNECTED
        _statu = DISCONNECTED;
        // 2. 移除连接的事件监控
        _channel.Remove();
        // 3. 关闭描述符
        _socket.Close();
        // 4. 如果当前定时器队列中还有定时销毁任务，则取消任务
        if (_loop->HasTimer(_conn_id))
            CancelInactiveReleaseInLoop();
        // 5. 调用关闭回调函数，避免先移除服务器管理的连接信息导致Connection被释放，再去处理会出错，因此先调用用户的回调函数
        if (_closed_callback)
            _closed_callback(shared_from_this());
        // 移除服务器内部管理的连接信息
        if (_server_closed_callback)
            _server_closed_callback(shared_from_this());
    }
    // 这个接口并不是实际的发送接口，而只是把数据放到了发送缓冲区，启动了可写事件监控
    void SendInLoop(Buffer &buf)
    {
        if (_statu == DISCONNECTED)
            return;
        _out_buffer.WriteBufferAndPush(buf);
        if (_channel.WriteAble() == false)
        {
            _channel.EnableWrite();
        }
    }
    // 这个关闭操作并非实际的连接释放操作，需要判断还有没有数据待处理，待发送
    void ShutdownInLoop()
    {
        _statu = DISCONNECTING; // 设置连接为半关闭状态
        if (_in_buffer.ReadAbleSize() > 0)
        {
            if (_message_callback)
                _message_callback(shared_from_this(), &_in_buffer);
        }
        // 要么就是写入数据的时候出错关闭，要么就是没有待发送数据，直接关闭
        if (_out_buffer.ReadAbleSize() > 0)
        {
            if (_channel.WriteAble() == false)
            {
                _channel.EnableWrite();
            }
        }
        if (_out_buffer.ReadAbleSize() == 0)
        {
            ReleaseInLoop();
        }
    }
    // 启动非活跃连接超时释放规则
    void EnableInactiveReleaseInLoop(int sec)
    {
        // 1. 将判断标志 _enable_inactive_release 置为true
        _enable_inactive_release = true;
        // 2. 如果当前定时销毁任务已经存在，那就刷新延迟一下即可
        if (_loop->HasTimer(_conn_id))
        {
            return _loop->TimerRefresh(_conn_id);
        }
        // 3. 如果不存在定时销毁任务，则新增
        _loop->TimerAdd(_conn_id, sec, std::bind(&Connection::ReleaseInLoop, this));
    }
    void CancelInactiveReleaseInLoop()
    {
        _enable_inactive_release = false;
        if (_loop->HasTimer(_conn_id))
        {
            _loop->TimerCancel(_conn_id);
        }
    }
    void UpgradeInLoop(const Any &context,
                       const ConnectedCallback &conn,
                       const MessageCallback &msg,
                       const ClosedCallback &closed,
                       const AnyEventCallback &event)
    {
        _context = context;
        _connected_callback = conn;
        _message_callback = msg;
        _closed_callback = closed;
        _event_callback = event;
    }

public:
    Connection(EventLoop *loop,
               uint64_t conn_id, int sockfd) : _conn_id(conn_id), _sockfd(sockfd),
                                               _enable_inactive_release(false),
                                               _loop(loop), _statu(CONNECTING), _socket(_sockfd),
                                               _channel(loop, _sockfd)
    {
        _channel.SetCloseCallback(std::bind(&Connection::HandleClose, this));
        _channel.SetEventCallback(std::bind(&Connection::HandleEvent, this));
        _channel.SetReadCallback(std::bind(&Connection::HandleRead, this));
        _channel.SetWriteCallback(std::bind(&Connection::HandleWrite, this));
        _channel.SetErrorCallback(std::bind(&Connection::HandleError, this));
    }
    ~Connection()
    {
        DBG_LOG("RELEASE CONNECTION:%p", this);
    }
    // 获取管理的文件描述符
    int Fd()
    {
        return _sockfd;
    }
    // 获取连接ID
    int Id()
    {
        return _conn_id;
    }
    // 是否处于CONNECTED状态
    bool Connected()
    {
        return (_statu == CONNECTED);
    }
    // 设置上下文--连接建立完成时进行调用
    void SetContext(const Any &context)
    {
        _context = context;
    }
    // 获取上下文，返回的是指针
    Any *GetContext()
    {
        return &_context;
    }
    void SetConnectedCallback(const ConnectedCallback &cb)
    {
        _connected_callback = cb;
    }
    void SetMessageCallback(const MessageCallback &cb)
    {
        _message_callback = cb;
    }
    void SetClosedCallback(const ClosedCallback &cb)
    {
        _closed_callback = cb;
    }
    void SetAnyEventCallback(const AnyEventCallback &cb)
    {
        _event_callback = cb;
    }
    void SetSrvClosedCallback(const ClosedCallback &cb)
    {
        _server_closed_callback = cb;
    }
    // 连接建立就绪后，进行channel回调设置，启动读监控，调用_connected_callback
    void Established()
    {
        _loop->RunInLoop(std::bind(&Connection::EstablishedInLoop, this));
    }
    // 发送数据，将数据放到发送缓冲区，启动写事件监控
    void Send(const char *data, size_t len)
    {
        // 外界传入的data，可能是个临时的空间，我们现在只是把发送操作压入了任务池，有可能并没有被立即执行
        // 因此有可能执行的时候，data指向的空间有可能已经被释放了。
        Buffer buf;
        buf.WriteAndPush(data, len);
        _loop->RunInLoop(std::bind(&Connection::SendInLoop, this, std::move(buf)));
    }
    // 提供给组件使用者的关闭接口--并不实际关闭，需要判断有没有数据待处理
    void Shutdown()
    {
        _loop->RunInLoop(std::bind(&Connection::ShutdownInLoop, this));
    }
    void Release()
    {
        _loop->QueueInLoop(std::bind(&Connection::ReleaseInLoop, this));
    }
    // 启动非活跃销毁，并定义多长时间无通信就是非活跃，添加定时任务
    void EnableInactiveRelease(int sec)
    {
        _loop->RunInLoop(std::bind(&Connection::EnableInactiveReleaseInLoop, this, sec));
    }
    // 取消非活跃销毁
    void CancelInactiveRelease()
    {
        _loop->RunInLoop(std::bind(&Connection::CancelInactiveReleaseInLoop, this));
    }
    // 切换协议---重置上下文以及阶段性回调处理函数 -- 而是这个接口必须在EventLoop线程中立即执行
    // 防备新的事件触发后，处理的时候，切换任务还没有被执行--会导致数据使用原协议处理了。
    void Upgrade(const Any &context, const ConnectedCallback &conn, const MessageCallback &msg,
                 const ClosedCallback &closed, const AnyEventCallback &event)
    {
        _loop->AssertInLoop();
        _loop->RunInLoop(std::bind(&Connection::UpgradeInLoop, this, context, conn, msg, closed, event));
    }
};
class Acceptor
{
private:
    Socket _socket;   // 用于创建监听套接字
    EventLoop *_loop; // 用于对监听套接字进行事件监控
    Channel _channel; // 用于对监听套接字进行事件管理

    using AcceptCallback = std::function<void(int)>;
    AcceptCallback _accept_callback;

private:
    /*监听套接字的读事件回调处理函数---获取新连接，调用_accept_callback函数进行新连接处理*/
    void HandleRead()
    {
        int newfd = _socket.Accept();
        if (newfd < 0)
        {
            return;
        }
        if (_accept_callback)
            _accept_callback(newfd);
    }
    int CreateServer(int port)
    {
        bool ret = _socket.CreateServer(port);
        assert(ret == true);
        return _socket.Fd();
    }

public:
    /*不能将启动读事件监控，放到构造函数中，必须在设置回调函数后，再去启动*/
    /*否则有可能造成启动监控后，立即有事件，处理的时候，回调函数还没设置：新连接得不到处理，且资源泄漏*/
    Acceptor(EventLoop *loop, int port) : _socket(CreateServer(port)), _loop(loop),
                                          _channel(loop, _socket.Fd())
    {
        _channel.SetReadCallback(std::bind(&Acceptor::HandleRead, this));
    }
    void SetAcceptCallback(const AcceptCallback &cb)
    {
        _accept_callback = cb;
    }
    void Listen()
    {
        _channel.EnableRead();
    }
};

class TcpServer
{
private:
    uint64_t _next_id; // 这是一个自动增长的连接ID，
    int _port;
    int _timeout;                                       // 这是非活跃连接的统计时间---多长时间无通信就是非活跃连接
    bool _enable_inactive_release;                      // 是否启动了非活跃连接超时销毁的判断标志
    EventLoop _baseloop;                                // 这是主线程的EventLoop对象，负责监听事件的处理
    Acceptor _acceptor;                                 // 这是监听套接字的管理对象
    LoopThreadPool _pool;                               // 这是从属EventLoop线程池
    std::unordered_map<uint64_t, PtrConnection> _conns; // 保存管理所有连接对应的shared_ptr对象

    using ConnectedCallback = std::function<void(const PtrConnection &)>;
    using MessageCallback = std::function<void(const PtrConnection &, Buffer *)>;
    using ClosedCallback = std::function<void(const PtrConnection &)>;
    using AnyEventCallback = std::function<void(const PtrConnection &)>;
    using Functor = std::function<void()>;
    ConnectedCallback _connected_callback;
    MessageCallback _message_callback;
    ClosedCallback _closed_callback;
    AnyEventCallback _event_callback;

private:
    void RunAfterInLoop(const Functor &task, int delay)
    {
        _next_id++;
        _baseloop.TimerAdd(_next_id, delay, task);
    }
    // 为新连接构造一个Connection进行管理
    void NewConnection(int fd)
    {
        _next_id++;
        PtrConnection conn(new Connection(_pool.NextLoop(), _next_id, fd));
        conn->SetMessageCallback(_message_callback);
        conn->SetClosedCallback(_closed_callback);
        conn->SetConnectedCallback(_connected_callback);
        conn->SetAnyEventCallback(_event_callback);
        conn->SetSrvClosedCallback(std::bind(&TcpServer::RemoveConnection, this, std::placeholders::_1));
        if (_enable_inactive_release)
            conn->EnableInactiveRelease(_timeout); // 启动非活跃超时销毁
        conn->Established();                       // 就绪初始化
        _conns.insert(std::make_pair(_next_id, conn));
    }
    void RemoveConnectionInLoop(const PtrConnection &conn)
    {
        int id = conn->Id();
        auto it = _conns.find(id);
        if (it != _conns.end())
        {
            _conns.erase(it);
        }
    }
    // 从管理Connection的_conns中移除连接信息
    void RemoveConnection(const PtrConnection &conn)
    {
        _baseloop.RunInLoop(std::bind(&TcpServer::RemoveConnectionInLoop, this, conn));
    }

public:
    TcpServer(int port) : _port(port),
                          _next_id(0),
                          _enable_inactive_release(false),
                          _acceptor(&_baseloop, port),
                          _pool(&_baseloop)
    {
        _acceptor.SetAcceptCallback(std::bind(&TcpServer::NewConnection, this, std::placeholders::_1));
        _acceptor.Listen() ; // 将监听套接字挂到baseloop上
    }
    void SetThreadCount(int count)
    {
        return _pool.SetThreadCount(count);
    }
    void SetConnectedCallback(const ConnectedCallback &cb)
    {
        _connected_callback = cb;
    }
    void SetMessageCallback(const MessageCallback &cb)
    {
        _message_callback = cb;
    }
    void SetClosedCallback(const ClosedCallback &cb)
    {
        _closed_callback = cb;
    }
    void SetAnyEventCallback(const AnyEventCallback &cb)
    {
        _event_callback = cb;
    }
    void EnableInactiveRelease(int timeout)
    {
        _timeout = timeout;
        _enable_inactive_release = true;
    }
    // 用于添加一个定时任务
    void RunAfter(const Functor &task, int delay)
    {
        _baseloop.RunInLoop(std::bind(&TcpServer::RunAfterInLoop, this, task, delay));
    }
    void Start()
    {
        _pool.Create();
        _baseloop.Start();
    }
};

inline void TimerWheel::TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb)
{
    _loop->RunInLoop(std::bind(&TimerWheel::TimerAddOnLoop, this, id, delay, cb));
}
inline void TimerWheel::TimerCancel(uint64_t id)
{
    _loop->RunInLoop(std::bind(&TimerWheel::TimerCancelOnLoop, this, id));
}
inline void TimerWheel::TimerRefresh(uint64_t id)
{
    _loop->RunInLoop(std::bind(&TimerWheel::TimerRefreshOnLoop, this, id));
}

inline void Channel::Update()
{
    _loop->UpdateEvent(this);
}
inline void Channel::Remove()
{
    _loop->RemoveEvent(this);
}