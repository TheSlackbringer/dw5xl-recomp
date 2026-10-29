#include "vu1lift/vu1_translator.h"
#include <iostream>
#include <stdexcept>
using namespace dw5xl::vu1lift;
void require(bool ok,const char *why) {if(!ok) throw std::runtime_error(why);}
int main()
{
    try
    {
        const std::vector<std::pair<std::string,std::string>> segments={
            {"first","void first(vl::Run &c) { /* exact body */ }"},
            {"second","void second(vl::Run &c) { /* static timing */ }"},
            {"third","void third(vl::Run &c) { /* exact branches */ }"}};
        const std::string registration="static Registrar owner(first, second, third);";
        const auto files=partitionVu1Sources("// helpers",segments,{"// ranges","// flush"},registration,60);
        require(files.size()==5,"three bounded whole segments, one owner and complete source list");
        const auto &owner=files.at("dw5xl_vu1_lifted.cpp");
        require(owner.find(registration)!=std::string::npos,"one unchanged ordered registration owner");
        require(owner.find("void first(vl::Run &c);")!=std::string::npos,"explicit external segment declaration");
        for(size_t i=0;i<segments.size();++i)
        {
            size_t occurrences=0;
            for(const auto &[name,text]:files)
                if(name.ends_with(".cpp") && text.find(segments[i].second)!=std::string::npos) ++occurrences;
            require(occurrences==1,"every entire segment occurs exactly once");
        }
        require(partitionVu1Sources("// helpers",segments,{"// ranges","// flush"},registration,60)==files,
                "deterministic source names, grouping and bytes");
        const auto one=partitionVu1Sources("// helpers",segments,{},registration,4096);
        require(one.size()==3,"small segments share a bounded native unit");
        std::cout<<"VU layout tests passed\n";return 0;
    }
    catch(const std::exception &e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
