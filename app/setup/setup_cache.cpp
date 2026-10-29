#include "setup_cache.h"
#include "sha256.h"
#include "stage_timing.h"
#include "run_process.h"
#include "file_workers.h"
#include "../../external/PS2Recomp/ps2xRecomp/include/ps2recomp/producer_dependencies.h"
#include <array>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <set>
#include <cctype>
#include <thread>
#include <atomic>
#include "vu_producer.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dw5xl::setup {
namespace fs=std::filesystem;
namespace {
constexpr const char *magic="dw5xl generated-cache 1";
std::string utf8(const fs::path &p) {const auto s=p.generic_u8string();return {s.begin(),s.end()};}
fs::path path(const std::string &s) {return fs::path(std::u8string(s.begin(),s.end()));}
bool digest(const std::string &s)
{
    return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos;
}
void frame(Sha256 &hash,const std::string &value)
{
    uint64_t size=value.size();
    std::array<uint8_t,8> length{};
    for(size_t i=0;i<length.size();++i) length[i]=uint8_t(size>>(i*8));
    hash.update(length.data(),length.size());
    hash.update(reinterpret_cast<const uint8_t *>(value.data()),value.size());
}
bool inputDigests(const std::vector<CacheInput> &inputs,std::vector<std::string> &hashes,std::string &error)
{
    try
    {
        hashes.resize(inputs.size());
        detail::parallelFiles(inputs.size(),[&](size_t i) {
            if(inputs[i].content)
            {
                Sha256 hash;if(!hash.ok()) throw std::runtime_error("SHA-256 initialization failed");
                const auto &bytes=*inputs[i].content;
                hash.update(reinterpret_cast<const uint8_t *>(bytes.data()),bytes.size());hashes[i]=hash.hex();
            }
            else
            {
                std::string why;hashes[i]=fileDigest(inputs[i].path,why);
                if(hashes[i].empty()) throw std::runtime_error(why);
            }
        });
        return true;
    }
    catch(const std::exception &e) {error=e.what();return false;}
}
bool inventory(const fs::path &root,std::map<std::string,std::string> &files,std::string &error)
{
    try
    {
        if(!fs::is_directory(root)) {error="generated output directory is missing";return false;}
        std::vector<CacheInput> inputs;
        for(const auto &entry:fs::recursive_directory_iterator(root))
        {
            if(entry.is_symlink()) {error="generated output contains a link";return false;}
            if(entry.is_directory()) continue;
            if(!entry.is_regular_file()) {error="generated output contains an unsupported entry";return false;}
            inputs.push_back({utf8(entry.path().lexically_relative(root)),entry.path()});
            if(inputs.size()>1000000) {error="generated inventory exceeds the cache schema limit";return false;}
        }
        std::vector<std::string> hashes;
        if(!inputDigests(inputs,hashes,error)) return false;
        for(size_t i=0;i<inputs.size();++i) files.emplace(inputs[i].name,std::move(hashes[i]));
        return true;
    }
    catch(const std::exception &e) {error=e.what();return false;}
}
bool relativeName(const std::string &name)
{
    if(name.empty() || name.find('\0')!=std::string::npos || name.find('\\')!=std::string::npos) return false;
    const auto p=path(name);
    if(p.is_absolute() || p.has_root_name() || p.lexically_normal()!=p) return false;
    for(const auto &part:p) if(part==".." || part==".") return false;
    return true;
}
} // namespace

std::string fileDigest(const fs::path &p,std::string &error)
{
    Sha256 sha;
    if(!sha.ok()) {error="SHA-256 initialization failed";return {};}
    std::ifstream in(p,std::ios::binary);
    if(!in) {error="cannot read "+utf8(p);return {};}
    std::array<char,65536> block;
    while(in.read(block.data(),block.size()) || in.gcount())
        sha.update(reinterpret_cast<const uint8_t *>(block.data()),size_t(in.gcount()));
    if(!in.eof()) {error="reading "+utf8(p)+" failed";return {};}
    return sha.hex();
}

std::string cacheKey(const std::vector<CacheInput> &inputs,const std::map<std::string,std::string> &values,std::string &error)
{
    Sha256 hash;
    if(!hash.ok()) {error="SHA-256 initialization failed";return {};}
    frame(hash,magic);
    for(const auto &[name,value]:values) {frame(hash,"value");frame(hash,name);frame(hash,value);}
    std::map<std::string,CacheInput> ordered;
    for(const auto &input:inputs)
        if(!ordered.emplace(input.name,input).second) {error="duplicate semantic input name";return {};}
    std::vector<CacheInput> files;
    for(const auto &[name,input]:ordered) files.push_back(input);
    std::vector<std::string> hashes;
    if(!inputDigests(files,hashes,error)) return {};
    for(size_t index=0;index<files.size();++index)
    {
        frame(hash,"file");frame(hash,files[index].name);frame(hash,hashes[index]);
    }
    return hash.hex();
}

bool cachedOutputs(const fs::path &record,const std::string &key,const fs::path &output,std::string &reason)
{
    try
    {
        std::ifstream in(record,std::ios::binary);
        if(!in) {reason="no completed generation record";return false;}
        std::string header,stored;
        size_t count=0;
        if(!std::getline(in,header) || header!=magic || !(in>>stored>>count) || !digest(stored) || !count || count>1000000)
        {reason="invalid generation record";return false;}
        if(stored!=key) {reason="generation inputs or producer changed";return false;}
        std::map<std::string,std::string> expected;
        for(size_t i=0;i<count;++i)
        {
            std::string name,hash;
            if(!(in>>std::quoted(name)>>hash) || !relativeName(name) || !digest(hash) || !expected.emplace(name,hash).second)
            {reason="invalid generated inventory";return false;}
        }
        in>>std::ws;
        if(!in.eof()) {reason="extra generation metadata";return false;}
        std::map<std::string,std::string> actual;
        if(!inventory(output,actual,reason)) return false;
        if(actual!=expected) {reason="generated output is missing, modified or contains stale files";return false;}
        reason="verified unchanged generation inputs and complete output";return true;
    }
    catch(const std::exception &e) {reason=e.what();return false;}
}

bool atomicWrite(const fs::path &destination,const std::string &bytes,std::string &error)
{
    const auto temp=destination.parent_path()/(destination.filename().native()+fs::path(".tmp-"+ps2recomp::timing::newRunId()).native());
    try
    {
        fs::create_directories(destination.parent_path());
        std::ofstream out(temp,std::ios::binary|std::ios::trunc);
        out.write(bytes.data(),std::streamsize(bytes.size()));out.flush();out.close();
        if(!out) {error="writing generation metadata failed";fs::remove(temp);return false;}
#ifdef _WIN32
        if(!MoveFileExW(temp.wstring().c_str(),destination.wstring().c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        {error="publishing generation metadata failed (error "+std::to_string(GetLastError())+")";fs::remove(temp);return false;}
#else
        fs::rename(temp,destination);
#endif
        return true;
    }
    catch(const std::exception &e)
    {error=e.what();std::error_code ec;fs::remove(temp,ec);return false;}
}

bool publishCachedOutputs(const fs::path &record,const std::string &key,const fs::path &output,std::string &error)
{
    if(!digest(key)) {error="cannot publish an invalid generation key";return false;}
    std::map<std::string,std::string> files;
    if(!inventory(output,files,error)) return false;
    if(files.empty()) {error="cannot cache empty generated output";return false;}
    std::ostringstream text;text<<magic<<'\n'<<key<<'\n'<<files.size()<<'\n';
    for(const auto &[name,hash]:files) text<<std::quoted(name)<<' '<<hash<<'\n';
    return atomicWrite(record,text.str(),error);
}

bool translatorInputs(const fs::path &program,const fs::path &workingDirectory,const fs::path &log,
                       std::vector<CacheInput> &inputs,std::string &error,const std::atomic<bool> *cancel)
{
    std::error_code ec;fs::remove(log,ec);
    const auto result=runProcess(program,{"--producer-dependencies"},workingDirectory,log,cancel);
    if(!result.started || result.exitCode!=0) {error="translator dependency identity unavailable";return false;}
    std::ifstream in(log,std::ios::binary);std::string line;
    while(std::getline(in,line) && line!="PS2RECOMP_PRODUCER_DEPS 1" && line!="PS2RECOMP_PRODUCER_DEPS 1\r") {}
    if(!in) {error="translator dependency identity unavailable";return false;}
    inputs.push_back({"translator",program});
    std::map<std::string,fs::path> dependencies;
    while(in>>std::quoted(line))
    {
        const auto file=path(line).is_absolute()?path(line):workingDirectory/path(line);
        if(!dependencies.emplace(utf8(file.filename()),file).second)
        {error="ambiguous translator dependency identity";return false;}
    }
    if(!in.eof()) {error="incomplete translator dependency identity";return false;}
    if(dependencies.empty()) {error="empty translator dependency identity";return false;}
    for(const auto &[name,file]:dependencies) inputs.push_back({"translator-dependency/"+name,file});
    return true;
}

bool vuRuntimeInputs(std::vector<CacheInput> &inputs,std::string &error)
{
    try
    {
        bool cxx=false,avutil=false;
        std::map<std::string,fs::path> ordered;
        for(const auto &file:ps2recomp::producerDependencies())
        {
            std::string name=utf8(file.filename());
            for(char &c:name) c=char(std::tolower(static_cast<unsigned char>(c)));
            const bool standard=name.find("libc++")==0 || name.find("libstdc++")==0 || name.find("libunwind")==0 ||
                name.find("libgcc_s")==0 || name=="ucrtbase.dll" || name=="msvcrt.dll" || name=="kernel32.dll" ||
                name=="kernelbase.dll" || name=="ntdll.dll" || name.find("libc.so")==0 || name.find("libm.so")==0 || name.find("ld-linux")==0;
            const bool sha=name.find("avutil-")==0 || name.find("libavutil")==0;
            cxx|=name.find("libc++")==0 || name.find("libstdc++")==0;avutil|=sha;
            if(standard || sha) ordered.emplace(name,file);
        }
        if(!cxx || !avutil) {error="VU producer runtime identity unavailable";return false;}
        for(const auto &[name,file]:ordered) inputs.push_back({"runtime/"+name,file});
        return true;
    }
    catch(const std::exception &e) {error=e.what();return false;}
}
const char *vuProducerIdentity() { return DW5XL_VU_PRODUCER_ID; }

bool treeInputs(const fs::path &root,const std::string &label,std::vector<CacheInput> &inputs,std::string &error)
{
    try
    {
        if(!fs::is_directory(root)) {error="missing source/tool directory: "+utf8(root);return false;}
        const auto canonicalRoot=fs::weakly_canonical(root);
        for(const auto &entry:fs::recursive_directory_iterator(root))
        {
            const auto name=label+"/"+utf8(entry.path().lexically_relative(root));
            if(entry.is_symlink())
            {
                const auto resolved=fs::weakly_canonical(entry.path());
                const auto relative=resolved.lexically_relative(canonicalRoot);
                if(relative.empty() || relative.is_absolute() || *relative.begin()=="..")
                {error="source/tool alias leaves the bundled source tree: "+name;return false;}
                inputs.push_back({"link:"+name,{},utf8(fs::read_symlink(entry.path()))});
                // Directory aliases are not followed; their actual files are enumerated under the same root.
            }
            if(entry.is_regular_file()) inputs.push_back({name,entry.path()});
            else if(entry.is_directory() && !entry.is_symlink()) inputs.push_back({"directory:"+name,{},std::string()});
        }
        return true;
    }
    catch(const std::exception &e) {error=e.what();return false;}
}
std::string packageIdentity(const fs::path &root,std::string &error)
{
    std::vector<CacheInput> inputs{{"BUILD_ID",root/"BUILD_ID"}};
    for(const char *folder:{"app","external","recomp","tools"})
        if(!treeInputs(root/folder,folder,inputs,error)) return {};
    const std::string producer=vuProducerIdentity();
    if(!digest(producer) || !vuRuntimeInputs(inputs,error))
    {if(error.empty()) error="VU producer identity unavailable";return {};}
    return cacheKey(inputs,{{"purpose","completed-release-inputs-1"},{"vu_producer",producer}},error);
}
bool buildReady(const fs::path &root,const fs::path &build,const std::string &release,std::string &error)
{
    const auto first=[](const fs::path &p) {std::ifstream in(p);std::string s;std::getline(in,s);return s;};
    if(first(build/"built-from.txt")!=release || release.empty()) return false;
    const auto stored=first(build/"ready.record");
    if(!digest(stored)) return false;
    const auto current=packageIdentity(root,error);
    return !current.empty() && current==stored;
}

std::unique_ptr<SetupLease> SetupLease::acquire(const fs::path &data,std::string &error)
{
    try
    {
        if(data.empty()) {error="player data directory is empty";return {};}
        fs::create_directories(data);
        auto result=std::unique_ptr<SetupLease>(new SetupLease);
#ifdef _WIN32
        HANDLE handle=CreateFileW((data/"setup.lock").wstring().c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE) {error="another setup is using this data directory, or it cannot be locked";return {};}
        result->m_handle=reinterpret_cast<intptr_t>(handle);
#else
        const int fd=open((data/"setup.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
        if(fd<0) {error="cannot lock player data directory";return {};}
        if(flock(fd,LOCK_EX|LOCK_NB)!=0) {close(fd);error="another setup is using this data directory";return {};}
        result->m_handle=fd;
#endif
        return result;
    }
    catch(const std::exception &e) {error=e.what();return {};}
}
SetupLease::~SetupLease()
{
    if(m_handle==-1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(m_handle));
#else
    flock(int(m_handle),LOCK_UN);close(int(m_handle));
#endif
}
} // namespace dw5xl::setup
