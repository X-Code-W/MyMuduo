#include <iostream>
#include <sys/timerfd.h>
#include<unistd.h>

int main()
{
    int timerfd=timerfd_create(CLOCK_MONOTONIC,0);
    if(timerfd<0)
    {
        perror("timerfd_create error");
        return -1;
    }
    struct itimerspec itmer;
    itmer.it_value.tv_sec=1;//第一次超时事件在1s后
    itmer.it_value.tv_nsec=0;
    itmer.it_interval.tv_sec=1;//第一次之后超时的时间间隔
    itmer.it_interval.tv_nsec=0;
    timerfd_settime(timerfd,0,&itmer,nullptr);
    while(1)
    {
        uint64_t tmp;
        int ret=read(timerfd,&tmp,8);
        if(ret<0)
        {
            perror("read error");
            return -1;
        }
        std::cout<<"超时第几次"<<std::endl;
    }
    return 0;

}