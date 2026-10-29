// Setup wizard step "generate the game's C++" (issue 011): config rewriting, running the recompiler and syncing
// its output. The test runs itself as a fake recompiler: with DW5XL_FAKE_RECOMP set (inherited by the children it
// starts) it only translates the one config it is given and never runs the test again. No game data is used.
#include "codegen.h"
#include "run_process.h"
#include "../../external/PS2Recomp/ps2xRecomp/include/ps2recomp/producer_dependencies.h"
#include <iomanip>
#include <thread>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <sstream>

using namespace dw5xl::setup;
namespace fs = std::filesystem;

namespace {
std::string lastError; // the driver's error message, shown when a check fails

void require(bool ok, const char *what)
{
    if (!ok)
        throw std::runtime_error(std::string(what) + (lastError.empty() ? "" : " (" + lastError + ")"));
}

std::string slurp(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void spit(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string tomlValue(const std::string &config, const char *key)
{
    std::smatch m;
    const std::regex re(std::string("(^|\\n)") + key + " = \"([^\"]*)\"");
    return std::regex_search(config, m, re) ? m[2].str() : std::string();
}

fs::path fromUtf8(const std::string &text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

// Fake recompiler: writes func.cpp (the input's text) and a fixed header into the configured output folder.
int fakeRecomp(const fs::path &config)
{
    if (config == "--producer-dependencies")
    {
        std::cout << "PS2RECOMP_PRODUCER_DEPS 1\n";
        for (const auto &p : ps2recomp::producerDependencies())
        { const auto s=p.generic_u8string();std::cout<<std::quoted(std::string(s.begin(),s.end()))<<'\n'; }
        return 0;
    }
    const std::string text = slurp(config);
    const fs::path input = fromUtf8(tomlValue(text, "input"));
    const fs::path output = fromUtf8(tomlValue(text, "output"));
    if (!fs::exists(input))
        return 3;
    if (const char *trace=std::getenv("DW5XL_FAKE_TRACE");trace && *trace)
        std::ofstream(fromUtf8(trace),std::ios::app)<<input.filename().string()<<'\n';
    if (slurp(input)=="cancel-fixture")
    {
        std::cout<<"[setup-timing] {\"schema\":1,\"stage\":\"analysis\",\"event\":\"progress\",\"counts\":{\"done\":256,\"total\":512}}\n"<<std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
    spit(output / "func.cpp", "// from " + slurp(input));
    spit(output / "ps2_recompiled_functions.h", "#pragma once\n");
    std::cout << "fake recompiler wrote " << output.string() << "\n";
    return 0;
}

const char *kConfig = "[general]\ninput = \"game/x\"\nghidra_output = \"recomp/y.csv\"\noutput = \"recomp/out/\"\n"
                      "single_file_output = false\n";
} // namespace

int main(int argc, char **argv)
{
    // Hard guard: a child started as the "recompiler" must never run the test (it would start itself again).
    if (std::getenv("DW5XL_FAKE_RECOMP"))
        return argc >= 2 ? fakeRecomp(fromUtf8(argv[1])) : 2; // optional timing identity flags do not change fake input
#ifdef _WIN32
    _putenv_s("DW5XL_FAKE_RECOMP", "1");
#else
    setenv("DW5XL_FAKE_RECOMP", "1", 1);
#endif
    try
    {
        const fs::path self = fs::absolute(argv[0]);
        const fs::path root = fs::temp_directory_path() / "dw5xl setup codegen Jürgen";
        fs::remove_all(root);

        // 1. The config rewrite keeps other lines and quotes paths with spaces and non-ASCII characters.
        const std::string rewritten =
            rewriteRecompConfig(kConfig, root / "game" / "a b.BIN", root / "f.csv", root / "out");
        require(rewritten.find("single_file_output = false") != std::string::npos, "other lines kept");
        require(fromUtf8(tomlValue(rewritten, "input")) == (root / "game" / "a b.BIN").generic_u8string(),
                "input path rewritten");
        require(tomlValue(rewritten, "output").back() == '/', "output folder ends with /");

        // 2. Starting a missing program fails cleanly.
        require(!runProcess(root / "no-such-program", {}, {}, root / "x.log").started, "missing program not started");
        { // first build: the whole folder is moved in one step
            const fs::path src = root / "sync-src", dst = root / "sync-dst" / "output";
            fs::create_directories(src / "sub");
            std::ofstream(src / "a.cpp") << "a";
            std::ofstream(src / "sub" / "b.cpp") << "b";
            std::string err;
            require(syncDirectory(src, dst, err), "sync into a new folder");
            require(!fs::exists(src) && slurp(dst / "a.cpp") == "a" && slurp(dst / "sub" / "b.cpp") == "b",
                    "moved: everything there, the work folder gone");
        }

        // 3. Full run over the four profiles with the fake recompiler.
        for (const auto &profile : kCodegenProfiles)
        {
            spit(root / profile.config, kConfig);
            spit(root / profile.input, profile.name);
            spit(root / profile.ghidraCsv, "");
        }
        spit(root / kCodegenProfiles[1].output / "stale.cpp", "old");
        std::string &error = lastError;
        int calls = 0;
        require(generateCode(root, self, root / "logs", [&](const char *, int, int count) {
            ++calls;
            return count == 4;
        }, error), "generate all profiles");
        require(calls == 4, "progress for each profile");
        require(slurp(root / kCodegenProfiles[0].output / "func.cpp") == "// from main", "main output");
        require(slurp(root / kCodegenProfiles[3].output / "func.cpp") == "// from ovl02", "ovl02 output");
        require(!fs::exists(root / kCodegenProfiles[1].output / "stale.cpp"), "stale file removed");
        require(slurp(root / "logs" / "main.log").find("fake recompiler wrote") != std::string::npos, "log written");
        {
            std::ostringstream evidence;
            ps2recomp::timing::Session timing(evidence, "profile-fixture", "synthetic");
            require(generateCode(root, self, root / "logs", nullptr, error, {}, &timing), "timed profiles");
            const auto text = evidence.str();
            for (const char *stage : {"profile_prepare", "translator_child", "output_sync", "profile_cleanup"})
                require(text.find(std::string("\"stage\":\"") + stage + "\"") != std::string::npos,
                        "profile interval includes preparation, child, synchronization and cleanup");
            require(text.find("\"profile\":\"main\"") != std::string::npos &&
                        text.find("\"profile\":\"ovl02\"") != std::string::npos, "profile timing identities");
            require(timing.healthy(), "timing output written");
        }

        // 4. Re-run: unchanged files keep their time stamp, changed ones are rewritten.
        const fs::path header = root / kCodegenProfiles[0].output / "ps2_recompiled_functions.h";
        const auto old = fs::file_time_type::clock::now() - std::chrono::hours(24);
        fs::last_write_time(header, old);
        spit(root / kCodegenProfiles[0].input, "main v2");
        require(generateCode(root, self, root / "logs", nullptr, error), "re-generate");
        require(fs::last_write_time(header) == old, "unchanged file not rewritten");
        require(slurp(root / kCodegenProfiles[0].output / "func.cpp") == "// from main v2", "changed file updated");

        // 5. A failing recompiler stops with the profile and log in the message.
        fs::remove(root / kCodegenProfiles[2].input);
        require(!generateCode(root, self, root / "logs", nullptr, error) &&
                    error.find("failed on ovl01") != std::string::npos, "failure reported");

        // 042: explicit cache route, independent profile invalidation and real running-child cancellation.
        spit(root / kCodegenProfiles[2].input,"ovl01");
        const auto trace=root/"invocations";
        const auto traceText=trace.u8string();
        const std::string traceUtf8(traceText.begin(),traceText.end());
#ifdef _WIN32
        _putenv_s("DW5XL_FAKE_TRACE",traceUtf8.c_str());
#else
        setenv("DW5XL_FAKE_TRACE",traceUtf8.c_str(),1);
#endif
        CodegenOptions reuse;reuse.reuse=true;
        const auto cachedRun=[&] {return generateCode(root,self,root/"logs",nullptr,error,{},nullptr,0,reuse);};
        const auto invocations=[&] {const auto s=slurp(trace);return std::count(s.begin(),s.end(),'\n');};
        require(cachedRun() && invocations()==4,"first cache run generates four profiles");
        require(cachedRun() && invocations()==4,"verified outputs skip every translator");
        spit(root/kCodegenProfiles[1].ghidraCsv,"map change");
        require(cachedRun() && invocations()==5,"one overlay map invalidates only that profile");
        spit(root/kCodegenProfiles[0].output/"stale.cpp","stale");
        require(cachedRun() && invocations()==6 && !fs::exists(root/kCodegenProfiles[0].output/"stale.cpp"),
                "extra output regenerates and removes obsolete files");
        reuse.force=true;
        require(cachedRun() && invocations()==10,"explicit rebuild bypasses every generation cache");
        reuse.force=false;
        spit(root/kCodegenProfiles[0].input,"cancel-fixture");
        std::atomic<bool> cancel{false};reuse.cancel=&cancel;bool sawProgress=false;
        reuse.activity=[&](const CodegenProgress &p) {if(p.stage=="analysis" && p.done==256 && p.total==512) {sawProgress=true;cancel=true;}};
        const auto begin=std::chrono::steady_clock::now();
        require(!cachedRun() && sawProgress,"incremental real-child progress can request cancellation");
        require(std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<2.0,
                "running translator cancellation finishes within approved two seconds");
        spit(root/kCodegenProfiles[0].input,"main v2");cancel=false;reuse.activity={};
        require(cachedRun() && invocations()==12,"cancelled output is not cached; retry regenerates just that profile");
        {
            const auto before=invocations();std::ostringstream retained;
            ps2recomp::timing::Session evidence(retained,"cache-decision-fixture","synthetic");
            require(generateCode(root,self,root/"logs",nullptr,error,{},&evidence,0,reuse),"timed verified cache hits");
            require(invocations()==before,"retaining provenance does not launch a cached translator");
            require(retained.str().find("\"reason\":\"verified unchanged generation inputs and complete output\"")!=std::string::npos,
                    "cache hit reason is retained, not only displayed in the UI");
            spit(root/kCodegenProfiles[3].ghidraCsv,"recorded key change");
            require(generateCode(root,self,root/"logs",nullptr,error,{},&evidence,0,reuse),"timed selective cache miss");
            require(invocations()==before+1,"recorded miss regenerates only the changed profile");
            require(retained.str().find("\"reason\":\"generation inputs or producer changed\"")!=std::string::npos &&
                    retained.str().find("\"input_key\":\"")!=std::string::npos,"cache miss reason and semantic key retained");
        }
        fs::remove_all(root);
        std::cout << "setup codegen tests passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
