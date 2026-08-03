#include <vector>
#include <iostream>
#include <cassert>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <memory>
#include <algorithm>
#include <cstring>
#include <ctime>
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
    char *Begin() { return _buffer.data(); }
    // 获取读位置起始地址
    char *ReadPosition() { return Begin() + _reader_idx; }
    // 获取写位置起始地址
    char *WritePosition() { return Begin() + _writer_idx; }
    // 获取缓冲区后面的空闲空间-写位置之后的空间
    uint64_t TailIdleSize() { return _buffer.size() - _writer_idx; }
    // 获取缓冲区前面的空间空间-读位置之前的空间
    uint64_t HeadIdleSize() { return _reader_idx; }
    // 获取可读空间的大小
    uint64_t ReadAbleSize() { return _writer_idx - _reader_idx; }
    // 移动读位置
    void MoveReadOffset(uint64_t len)
    {
        assert(len <= ReadAbleSize());
        _reader_idx += len;
    }
    // 移动写位置
    void MoveWriteOffset(uint64_t len)
    {
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
            ERR_LOG("SEND SOCKET FATAL");
            return -1;
        }
        return ret;
    }
    ssize_t SendNoBlock(void *buf, size_t len)
    {
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
    TimerTask(uint64_t id, uint32_t delay, const TaskFunc &cb) :  
              _id(id), _timeout(delay), _task_cb(cb), _canceled(false) {}
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
    // 判断当前线程是否是EventLoop对于的线程
    bool IsInLoop()
    {
        return _thread_id == std::this_thread::get_id();
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