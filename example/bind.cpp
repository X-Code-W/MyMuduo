#include<iostream>
#include<string>
#include<functional>
#include<vector>


void Print(const std::string& str,int x,int y)
{
    std::cout<< str <<x+y <<std::endl;
}

int main()
{
    // Print("hello",1,2);
    auto f1=std::bind(Print,"mi",1,std::placeholders::_1);
    auto f2=std::bind(Print,"aho",std::placeholders::_1,2);
    auto f3=std::bind(Print,"xxx",std::placeholders::_1,std::placeholders::_2);

    f1(1);
    f2(2);
    f3(3,3);

    return 0;
}