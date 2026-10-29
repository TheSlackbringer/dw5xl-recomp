// 042: content/producer keys, complete inventories, atomic publication and exclusive setup ownership.
#include "setup_cache.h"
#include "stage_timing.h"
#include "../../external/PS2Recomp/ps2xRecomp/include/ps2recomp/producer_dependencies.h"
#include <set>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace dw5xl::setup;
void require(bool value,const char *what) { if(!value) throw std::runtime_error(what); }
void put(const fs::path &p,const std::string &text) {fs::create_directories(p.parent_path());std::ofstream(p,std::ios::binary)<<text;}

int main()
{
    try
    {
#ifdef _WIN32
        // An unrelated loader/compatibility module must not change a producer's code dependencies.
        const auto dependencies=[] {
            const auto files=ps2recomp::producerDependencies();
            return std::set<fs::path>(files.begin(),files.end());
        };
        const auto beforeModules=dependencies();
        HMODULE unrelated=nullptr;
        for(const wchar_t *name:{L"cabinet.dll",L"msimg32.dll",L"version.dll",L"dwmapi.dll"})
            if(!GetModuleHandleW(name)) {unrelated=LoadLibraryExW(name,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(unrelated) break;}
        require(unrelated!=nullptr,"unrelated system-module fixture available");
        const auto afterModules=dependencies();FreeLibrary(unrelated);
        require(beforeModules==afterModules,"unrelated loaded modules do not invalidate translator cache");
#endif
        const fs::path root=fs::temp_directory_path()/("dw5xl-cache-"+ps2recomp::timing::newRunId());
        const auto input=root/"inputs/profile",moved=root/"moved/profile",output=root/"output",record=root/"cache/main";
        put(input,"same input");put(moved,"same input");put(output/"a.cpp","first");put(output/"sub/b.h","header");
        std::string error,reason;
        const std::map<std::string,std::string> producer={{"producer","stable-generator"},{"layout","legacy"}};
        const auto key=cacheKey({{"profile",input}},producer,error);
        require(!key.empty(),"input key computed");
        require(key=="a2af61a5d1003a463a9064f81ad6ecc8f2747f1ea8e6e3f50f8fc2c84d0e7a74","content key matches independent serial SHA-256 reference");
        require(cacheKey({{"profile",moved}},producer,error)==key,"relocation does not change semantic key");
        require(cacheKey({{"link:tool",{},std::string("first")}},producer,error)!=
                cacheKey({{"link:tool",{},std::string("second")}},producer,error),"link configuration is a semantic input");
        {
            const auto sdk=root/"sdk";put(sdk/"one","same wrapper");put(sdk/"two","same wrapper");
            std::error_code ec;fs::create_symlink("one",sdk/"alias",ec);
            if(!ec)
            {
                std::vector<CacheInput> a,b;require(treeInputs(sdk,"sdk",a,error),"relative tool alias inventory");
                const auto before=cacheKey(a,{},error);fs::remove(sdk/"alias");fs::create_symlink("two",sdk/"alias");
                require(treeInputs(sdk,"sdk",b,error) && cacheKey(b,{},error)!=before,
                        "rewired same-byte wrapper invalidates resolved tool configuration");
                fs::remove(sdk/"alias");fs::create_directory_symlink(".",sdk/"directory-alias");
                std::vector<CacheInput> dirs;require(treeInputs(sdk,"sdk",dirs,error),"internal directory alias is bounded and identified");
            }
            else std::cout<<"tool symlink fixture unavailable on this host; POSIX CI must cover it\n";
        }
        require(cacheKey({{"profile",input}},{{"producer","changed-generator"},{"layout","legacy"}},error)!=key,
                "producer change invalidates");
        require(!cachedOutputs(record,key,output,reason),"missing record misses");
        require(publishCachedOutputs(record,key,output,error),"publish complete output");
        require(cachedOutputs(record,key,output,reason),"valid full output hits");
        const auto oldTime=fs::last_write_time(input);put(input,"different!");fs::last_write_time(input,oldTime);
        require(cacheKey({{"profile",input}},producer,error)!=key,"content change with old timestamp invalidates");
        put(input,"same input");
        const auto oldOutputTime=fs::last_write_time(output/"a.cpp");put(output/"a.cpp","other");fs::last_write_time(output/"a.cpp",oldOutputTime);
        require(!cachedOutputs(record,key,output,reason),"same-sized changed output misses");put(output/"a.cpp","first");
        require(cachedOutputs(record,key,output,reason),"restored exact output hits");
        put(output/"extra.cpp","stale");require(!cachedOutputs(record,key,output,reason),"extra output misses");fs::remove(output/"extra.cpp");
        fs::remove(output/"sub/b.h");require(!cachedOutputs(record,key,output,reason),"missing output misses");put(output/"sub/b.h","header");
        put(record,"corrupt");require(!cachedOutputs(record,key,output,reason),"corrupt metadata misses");
        require(publishCachedOutputs(record,key,output,error),"atomic replacement of old metadata");
        require(cachedOutputs(record,key,output,reason),"replacement is valid");
        put(root/"blocker","not a directory");
        require(!publishCachedOutputs(root/"blocker/record",key,output,error),"write failure cannot publish");
        const auto package=root/"package",build=root/"build";
        for(const char *folder:{"app","external","recomp","tools"}) put(package/folder/"input","source");
        put(package/"BUILD_ID","same-release\n");
        const auto packageKey=packageIdentity(package,error);require(!packageKey.empty(),"complete package identity");
        fs::create_directory(package/"tools/empty-resource-directory");
        require(packageIdentity(package,error)!=packageKey,"SDK directory configuration is a semantic input");
        fs::remove(package/"tools/empty-resource-directory");
        put(build/"built-from.txt","same-release\n");put(build/"ready.record",packageKey+"\n");
        require(buildReady(package,build,"same-release",error),"unchanged package ready");
        put(package/"recomp/input","changed map, same BUILD_ID");
        require(!buildReady(package,build,"same-release",error),"changed input with same release cannot fast launch");
        put(package/"recomp/input","source");put(package/"tools/input","changed compiler");
        require(!buildReady(package,build,"same-release",error),"changed bundled compiler cannot fast launch");
        put(root/"mc0/save.bin","saved-card");put(root/"settings.ini","settings");
        {
            auto first=SetupLease::acquire(root,error);require(bool(first),"first setup owns data root");
            auto second=SetupLease::acquire(root,error);require(!second,"concurrent setup refused before mutation");
        }
        auto next=SetupLease::acquire(root,error);require(bool(next),"released ownership permits retry");next.reset();
        std::ifstream card(root/"mc0/save.bin",std::ios::binary),settings(root/"settings.ini",std::ios::binary);
        require(std::string(std::istreambuf_iterator<char>(card),{})=="saved-card","saved card untouched");
        require(std::string(std::istreambuf_iterator<char>(settings),{})=="settings","settings untouched");
        card.close();settings.close();
        fs::remove_all(root); // uniquely created scratch directory; no dependencies or user data
        std::cout<<"setup cache tests passed\n";return 0;
    }
    catch(const std::exception &e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
