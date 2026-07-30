#include <iostream>
#include <string>
#include <regex>

int main()
{
    std::string s="/number/1234";
    std::regex e("/number/(\\d+)");

    std::smatch maches;
    bool ret =std::regex_match(s,maches,e);
    if(ret==false) return-1;

    for(auto& s:maches)
    {
        std::cout<<s<<std::endl;
    }
    return 0;
}