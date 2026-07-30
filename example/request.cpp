#include <iostream>
#include <string>
#include <regex>

int main()
{
    std::string str = "GET /bbbssaaacom/chat?channelname HTTP/1.1";
    // std::string str = "GET /bbbssaaacom/chat HTTP/1.1\r\n";

    // std::regex e("(GET|POST|PUT|HEAD) ([^?]*)\\?(.*) (HTTP/1\\.[01])(?:\n|\r\n)?");
    //可能没有解析字符串
    std::regex e("(GET|POST|PUT|HEAD) ([^?]*)(?:\\?(.*))? (HTTP/1\\.[01])(?:\n|\r\n)?");

    //(GET|POST|PUT|HEAD) 表示提取(GET|POST|PUT|HEAD)
    //([^?]*) [^?]表示非？的字符，*表示0次或无数次
    //\\?(.*) \\?表示原始的？ （.*)表示提取？之后的任意字符，0次或无数次，直到遇到空格
    //（HTTP/1\\.[01]） \\.表示原始.
    //(?:\n|\r\n)? (?:...)表示只匹配,不提取

    std::smatch maches;

    bool ret =std::regex_match(str,maches,e);
    if(ret==false) return -1;

    for(auto& s:maches)
    {
        std::cout<<s<<std::endl;
    }
    return 0;
}