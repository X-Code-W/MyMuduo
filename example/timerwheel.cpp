#include <iostream>
#include <functional>
#include <memory>
#include <vector>
#include <unistd.h>
#include <unordered_map>
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
    uint32_t Delay_time() { return _timeout; }
    void Cancel() { _canceled = true; }
};

class TimerWheel
{
private:
    using PtrTask = std::shared_ptr<TimerTask>;
    using WeakPtr = std::weak_ptr<TimerTask>;
    int _capacity;                            // 表盘的最大容量
    std::vector<std::vector<PtrTask>> _wheel; // 时间轮二位数组
    int _tick;                                // 当前的秒针，指到哪里就释放哪里，释放哪里就执行哪里的任务
    std::unordered_map<uint64_t, WeakPtr> _timers;

private:
    void RemoveTimer(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it != _timers.end())
        {
            _timers.erase(id);
        }
    }

public:
    TimerWheel() : _capacity(60), _tick(0), _wheel(_capacity) {}
    // 添加定时任务
    void TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb)
    {
        PtrTask pt = std::make_shared<TimerTask>(id, delay, cb);
        pt->SetRelease(std::bind(&TimerWheel::RemoveTimer, this, id));
        int pos = (_tick + delay) % _capacity;
        _wheel[pos].push_back(pt);
        _timers[id] = WeakPtr(pt);
    }
    void TimerCance(uint64_t id)
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
    void TimerRefresh(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it == _timers.end())
        {
            return; // 任务不存在
        }
        PtrTask pt = it->second.lock(); // 由weak_ptr管理的对象通过lock获得shared_ptr
        int delay = pt->Delay_time();
        int pos = (_tick + delay) % _capacity;
        _wheel[pos].push_back(pt);
    }
    // 这个函数每秒向后走一步
    void RunTimeTask()
    {
        _tick = (_tick + 1) % _capacity;
        _wheel[_tick].clear(); // 清空数组，就是把数组中所有管理定时器对象的shared_ptr释放掉
    }
};

class Test
{
public:
    Test() { std::cout << "构造" << std::endl; }
    ~Test() { std::cout << "析构" << std::endl; }
};
void DelTest(Test *t)
{
    delete t;
}

int main()
{
    TimerWheel tw;
    Test *t = new Test();
    tw.TimerAdd(999, 5, std::bind(DelTest, t));
    for (int i = 0; i < 5; i++)
    {
        sleep(1);
        tw.TimerRefresh(999);
        tw.RunTimeTask();
        std::cout << "刷新\n";
    }
    tw.TimerCance(999);
    while (1)
    {
        sleep(1);
        tw.RunTimeTask();
        std::cout << "-----------------\n";
    }
    return 0;
}