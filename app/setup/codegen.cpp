#include "codegen.h"
#include "run_process.h"
#include "sha256.h"
#include "setup_cache.h"
#include "file_workers.h"
#include <mutex>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <vector>
#include <regex>

namespace dw5xl::setup {
namespace fs = std::filesystem;

namespace {
std::string readText(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string utf8(const fs::path &path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// TOML basic string with forward slashes (valid on Windows too, and no backslash escapes needed).
std::string tomlPath(std::string text)
{
    std::string out = "\"";
    for (char c : text)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

bool sameContent(const fs::path &a, const fs::path &b)
{
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec)
        return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    std::vector<char> ba(1u << 16), bb(1u << 16);
    while (fa && fb)
    {
        fa.read(ba.data(), std::streamsize(ba.size()));
        fb.read(bb.data(), std::streamsize(bb.size()));
        if (fa.gcount() != fb.gcount() || !std::equal(ba.begin(), ba.begin() + fa.gcount(), bb.begin()))
            return false;
    }
    return fa.eof() && fb.eof();
}
} // namespace

std::string rewriteRecompConfig(const std::string &config, const fs::path &input, const fs::path &ghidraCsv,
                                const fs::path &output)
{
    std::istringstream lines(config);
    std::string line, out;
    while (std::getline(lines, line))
    {
        const auto key = line.substr(0, line.find('='));
        const auto trimmed = key.substr(0, key.find_last_not_of(" \t") + 1);
        if (line.find('=') != std::string::npos && trimmed == "input")
            line = "input = " + tomlPath(utf8(input));
        else if (line.find('=') != std::string::npos && trimmed == "ghidra_output")
            line = "ghidra_output = " + tomlPath(utf8(ghidraCsv));
        else if (line.find('=') != std::string::npos && trimmed == "output")
        {
            std::string dir = utf8(output);
            if (dir.empty() || dir.back() != '/')
                dir += '/'; // the recompiler appends file names to it, like regen.sh's "$tmp/output/"
            line = "output = " + tomlPath(dir);
        }
        out += line + "\n";
    }
    return out;
}

bool syncDirectory(const fs::path &source, const fs::path &target, std::string &error, bool keepSource,
                   const std::function<bool(uint64_t,uint64_t)> &progress)
{
    try
    {
        if (!fs::is_directory(source)) {error="source folder is missing: "+source.string();return false;}
        if (fs::exists(target) && fs::equivalent(source,target)) {error="cannot synchronize a folder with itself";return false;}
        if (progress && !progress(0,0)) {error="cancelled";return false;}
        if (!keepSource && !fs::exists(target))
        {
            fs::create_directories(target.parent_path());
            std::error_code ec;fs::rename(source,target,ec);
            if (!ec) {if(progress) progress(1,1);return true;}
        }
        fs::create_directories(target);
        std::set<fs::path> generated,parents;
        std::vector<fs::path> files;
        for(const auto &entry:fs::recursive_directory_iterator(source))
        {
            if(entry.is_symlink() || entry.is_regular_file())
            {
                files.push_back(entry.path());generated.insert(entry.path().lexically_relative(source));
                parents.insert((target/entry.path().lexically_relative(source)).parent_path());
            }
            else if(entry.is_directory()) parents.insert(target/entry.path().lexically_relative(source));
        }
        // Prune owned obsolete entries first: a former file can become a directory, or vice versa.
        std::vector<fs::path> stale,dirs;
        for(const auto &entry:fs::recursive_directory_iterator(target))
        {
            if(entry.is_symlink() || entry.is_regular_file())
            {if(!generated.count(entry.path().lexically_relative(target))) stale.push_back(entry.path());}
            else if(entry.is_directory()) dirs.push_back(entry.path());
        }
        for(const auto &file:stale)
        {
            if(progress && !progress(0,files.size())) {error="cancelled";return false;}
            if(!fs::remove(file)) {error="cannot remove obsolete file: "+file.string();return false;}
        }
        std::sort(dirs.rbegin(),dirs.rend());
        for(const auto &dir:dirs) if(!parents.count(dir) && fs::is_empty(dir)) fs::remove(dir);
        for(const auto &parent:parents) fs::create_directories(parent);
        std::atomic<uint64_t> done{0};std::mutex reportMutex;
        const auto report=[&] {
            std::lock_guard lock(reportMutex);
            if(progress && !progress(done.load(),files.size())) throw std::runtime_error("cancelled");
        };
        detail::parallelFiles(files.size(),[&](size_t index) {
            report();
            const auto &file=files[index];const auto dst=target/file.lexically_relative(source);
            const bool link=fs::is_symlink(file),destLink=fs::is_symlink(dst);
            const bool sameLink=link && destLink && fs::read_symlink(file)==fs::read_symlink(dst);
            const bool sameFile=!link && !destLink && fs::is_regular_file(dst) && sameContent(file,dst);
            if(!sameLink && !sameFile)
            {
                if(destLink || fs::is_directory(dst)) fs::remove(dst);
                if(link) {fs::remove(dst);fs::copy_symlink(file,dst);}
                else
                {
                    fs::copy_file(file,dst,fs::copy_options::overwrite_existing);
                    if(keepSource) fs::permissions(dst,fs::perms::owner_write,fs::perm_options::add);
                }
            }
            ++done;report();
        });
        report();
        return true;
    }
    catch(const std::exception &e) {error=e.what();return false;}
}

bool generateCode(const fs::path &root, const fs::path &recompiler, const fs::path &logDir,
                  const std::function<bool(const char *, int, int)> &progress, std::string &error,
                  const fs::path &dataDir, ps2recomp::timing::Session *timing, uint64_t timingParent, const CodegenOptions &options)
{
    const fs::path data = dataDir.empty() ? root : dataDir;
    std::error_code ec;
    fs::create_directories(logDir, ec);
    const fs::path exe = fs::absolute(recompiler); // the recompiler runs with `root` as its working folder
    std::string identity;
    if (timing)
    {
        ps2recomp::timing::Stage identify(timing, "translator_identity", timingParent);
        Sha256 hash;
        std::ifstream in(exe, std::ios::binary);
        std::array<char, 65536> block;
        while (in.read(block.data(), block.size()) || in.gcount())
            hash.update(reinterpret_cast<const uint8_t *>(block.data()), size_t(in.gcount()));
        identity = in.eof() && hash.ok() ? hash.hex() : "unavailable";
        identify.finish(identity == "unavailable" ? "failed" : "success");
    }
    const auto cancelled=[&] {return options.cancel && *options.cancel;};
    std::vector<CacheInput> producer;
    std::string producerError;
    if(options.reuse && !translatorInputs(exe,root,logDir/"producer.log",producer,producerError,options.cancel))
        producer.clear(); // unknown dependencies are a conservative miss
    const int count = int(std::size(kCodegenProfiles));
    for (int i = 0; i < count; ++i)
    {
        const auto &profile = kCodegenProfiles[i];
        ps2recomp::timing::Stage profileTime(timing, "profile", timingParent, profile.name);
        auto activity=[&](const std::string &stage,uint64_t done=0,uint64_t total=0) {
            if(options.activity) options.activity({profile.name,stage,done,total});
            return !cancelled();
        };
        if(cancelled()) {error="cancelled";return false;}
        ps2recomp::timing::Stage validation(timing,"cache_validation",profileTime.id(),profile.name);
        activity("checking generated output");
        auto inputs=producer;
        inputs.insert(inputs.end(),{{"disc",data/profile.input},{"config",root/profile.config},{"function-map",root/profile.ghidraCsv}});
        std::string keyError,reason=options.force?"explicit rebuild":"reuse disabled";
        const auto key=options.reuse && !producer.empty()?cacheKey(inputs,{{"profile",profile.name}},keyError):std::string();
        const auto record=data/"cache"/"generation"/(std::string(profile.name)+".record");
        const bool hit=options.reuse && !options.force && !key.empty() && cachedOutputs(record,key,data/profile.output,reason);
        if(!options.reuse) reason="reuse disabled";
        else if(options.force) reason="explicit rebuild";
        else if(key.empty()) reason=keyError.empty()?producerError:keyError;
        validation.count("hit",hit);validation.field("reason",reason);
        validation.field("input_key",key.empty()?"unavailable":key);validation.finish();
        profileTime.field("input_key",key.empty()?"unavailable":key);
        profileTime.field("translator_sha256",identity.empty()?"unavailable":identity);
        if(progress && !progress(profile.name,i,count)) {error="cancelled";return false;}
        if(hit) {activity("verified cached output",1,1);profileTime.finish("cache_hit");continue;}
        activity("generating: "+(key.empty() && options.reuse?(keyError.empty()?producerError:keyError):reason));
        if(options.reuse) fs::remove(record); // failed or interrupted work must never retain a success record
        ps2recomp::timing::Stage prepareTime(timing, "profile_prepare", profileTime.id(), profile.name);
        if (cancelled())
        {
            error = "cancelled";
            return false;
        }
        const fs::path work = data / "build-setup" / "codegen" / profile.name;
        fs::remove_all(work);
        fs::create_directories(work / "output");
        const std::string config = readText(root / profile.config);
        if (config.empty())
        {
            error = std::string("missing recompiler configuration ") + profile.config;
            return false;
        }
        if(!atomicWrite(work/"config.toml",rewriteRecompConfig(config,data/profile.input,root/profile.ghidraCsv,work/"output"),error))
            return false;
        const fs::path log = logDir / (std::string(profile.name) + ".log");
        fs::remove(log, ec);
        prepareTime.finish();
        std::vector<std::string> args = {utf8(work / "config.toml")};
        if (timing)
            args.insert(args.end(), {"--timing-run", timing->run(), "--timing-profile", profile.name,
                                     "--timing-identity", identity});
        ps2recomp::timing::Stage childTime(timing, "translator_child", profileTime.id(), profile.name);
        const ProcessResult run = runProcessObserved(exe,args,root,log,[&](const std::string &line) {
            if(line.rfind("[setup-timing] ",0)==0)
            {
                static const std::regex stageRe(R"re("stage":"([a-z_]+)")re");
                static const std::regex eventRe(R"re("event":"(begin|progress)")re");
                static const std::regex doneRe(R"re("done":([0-9]+))re"),totalRe(R"re("total":([0-9]+))re");
                std::smatch stage,event,done,total;
                if(std::regex_search(line,stage,stageRe) && std::regex_search(line,event,eventRe))
                {
                    uint64_t n=0,t=0;
                    try {if(std::regex_search(line,done,doneRe)) n=std::stoull(done[1]);
                         if(std::regex_search(line,total,totalRe)) t=std::stoull(total[1]);} catch(...) {return !cancelled();}
                    if(!t || n<=t) activity(stage[1],n,t);
                }
            }
            return !cancelled();
        },options.cancel);
        childTime.finish(run.started && run.exitCode == 0 ? "success" : run.cancelled ? "cancelled" : "failed");
        if(run.cancelled || cancelled()) {error="cancelled";return false;}
        if (!run.started)
        {
            error = run.error;
            return false;
        }
        if (run.exitCode != 0)
        {
            error = std::string("the recompiler failed on ") + profile.name + " (exit " +
                    std::to_string(run.exitCode) + "); see " + log.string();
            return false;
        }
        ps2recomp::timing::Stage syncTime(timing, "output_sync", profileTime.id(), profile.name);
        if (!syncDirectory(work / "output", data / profile.output, error,false,[&](uint64_t done,uint64_t total) {
            return activity("synchronizing output",done,total);
        }))
            return false;
        syncTime.finish();
        ps2recomp::timing::Stage cleanupTime(timing, "profile_cleanup", profileTime.id(), profile.name);
        fs::remove_all(work);
        cleanupTime.finish();
        if(options.reuse && !key.empty())
        {
            ps2recomp::timing::Stage publish(timing,"cache_publish",profileTime.id(),profile.name);
            if(cancelled()) {error="cancelled";return false;}
            const auto after=cacheKey(inputs,{{"profile",profile.name}},keyError);
            if(after!=key) {error="translation inputs changed during generation";return false;}
            if(!publishCachedOutputs(record,key,data/profile.output,error)) return false;
            publish.finish();
        }
        profileTime.finish();
    }
    return true;
}

} // namespace dw5xl::setup
