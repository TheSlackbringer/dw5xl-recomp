// 042: content/timestamp-preserving source sync, type changes, cancellation and refusal/retry.
#include "codegen.h"
#include "stage_timing.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace dw5xl::setup;
namespace fs=std::filesystem;
void require(bool ok,const char *why) {if(!ok) throw std::runtime_error(why);}
void put(const fs::path &p,const std::string &s) {fs::create_directories(p.parent_path());std::ofstream(p,std::ios::binary)<<s;}
std::string get(const fs::path &p) {std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main()
{
    try
    {
        const auto root=fs::temp_directory_path()/("dw5xl-sync-"+ps2recomp::timing::newRunId());
        const auto src=root/"package",dst=root/"data/builder";
        put(src/"was-file/child","new directory");put(src/"was-directory","new file");
        fs::create_directories(src/"empty-tool-directory");
        put(dst/"was-file","old file");put(dst/"was-directory/child","old directory");
        put(root/"data/mc0/card","saved");put(root/"data/settings.ini","settings");
        std::string error;
        require(syncDirectory(src,dst,error,true),"file/directory transitions synchronize safely");
        require(get(dst/"was-file/child")=="new directory" && get(dst/"was-directory")=="new file","exact changed layout");
        require(fs::is_directory(dst/"empty-tool-directory"),"empty SDK directories are preserved");
        const auto old=fs::file_time_type::clock::now()-std::chrono::hours(24);
        fs::last_write_time(dst/"was-directory",old);
        require(syncDirectory(src,dst,error,true) && fs::last_write_time(dst/"was-directory")==old,
                "identical files retain timestamps");
        for(int i=0;i<128;++i) put(src/"many"/(std::to_string(i)+".h"),std::string(1024,char('a'+i%26)));
        uint64_t last=0;bool refused=false;
        require(!syncDirectory(src,dst,error,true,[&](uint64_t done,uint64_t) {
            require(done>=last,"synchronized progress never goes backwards");last=done;
            if(done>=2) {refused=true;return false;}return true;
        }) && refused,"mid-copy cancellation stops the operation");
        require(syncDirectory(src,dst,error,true),"partial copy permits complete retry");
        for(const auto &e:fs::recursive_directory_iterator(src))
            if(e.is_regular_file()) require(get(e.path())==get(dst/e.path().lexically_relative(src)),"all retry bytes match");
        put(root/"blocked","file blocks destination directory");
        require(!syncDirectory(src,root/"blocked",error,true),"actual destination refusal is reported");
        fs::remove(root/"blocked");require(syncDirectory(src,root/"blocked",error,true),"refusal permits retry");
        require(get(root/"data/mc0/card")=="saved" && get(root/"data/settings.ini")=="settings","cards/settings outside builder untouched");
        fs::remove_all(root); // uniquely created fixture scratch; no user data or dependencies
        std::cout<<"setup sync tests passed\n";return 0;
    }
    catch(const std::exception &e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
