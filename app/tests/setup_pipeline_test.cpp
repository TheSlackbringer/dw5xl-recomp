// Setup wizard pipeline (issue 011): step order, progress, errors and cancel, on the synthetic image with stand-in
// generate / lift / build steps (the real ones need the player's disc and are checked locally, see notes).
#include "setup_pipeline.h"
#include "synthetic_iso.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace dw5xl::setup;
using namespace dw5xl::setup::test;
namespace fs = std::filesystem;

namespace {
void require(bool ok, const char *what)
{
    if (!ok)
        throw std::runtime_error(what);
}

bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

const DiscSpec kDiscs[] = {{"test disc", "test", kTestFiles}, {"second disc", "second", kTestFiles}};

struct Fakes
{
    std::vector<std::string> calls;
    std::string failIn;     // step that returns an error
    bool waitForCancel = false;

    SetupSteps steps()
    {
        SetupSteps s;
        s.generate = [this](const SetupOptions &o, StepContext &c, std::string &error) {
            calls.push_back("generate");
            require(fs::exists(o.root / "game" / "test" / "A.BIN"), "generate runs after extraction");
            c.progress(0.5, "ovl01");
            c.log("generating ovl01");
            if (waitForCancel)
            {
                for (int i = 0; i < 500 && !c.cancelled(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return false;
            }
            return failIn != "generate" || (error = "boom", false);
        };
        s.liftVu1 = [this](const SetupOptions &, StepContext &, std::string &error) {
            calls.push_back("lift");
            return failIn != "lift" || (error = "boom", false);
        };
        s.build = [this](const SetupOptions &o, StepContext &c, fs::path &exe, std::string &error) {
            calls.push_back("build");
            c.progress(1.0, "1147/1147");
            exe = o.root / "game.exe";
            return failIn != "build" || (error = "boom", false);
        };
        return s;
    }
};

SetupOptions options(const fs::path &root, const fs::path &iso)
{
    SetupOptions o;
    o.root = root;
    o.xlIso = iso;
    o.discs = kDiscs;
    return o;
}

SetupStatus runWith(SetupOptions o, Fakes &fakes, bool cancelSoon = false)
{
    SetupRun run(std::move(o), fakes.steps());
    run.start();
    if (cancelSoon)
    {
        for (int i = 0; i < 500 && run.status().step != Step::Generate; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        run.cancel();
    }
    run.wait();
    return run.status();
}
} // namespace

int main()
{
    try
    {
        const fs::path dir = fs::temp_directory_path() / "dw5xl_setup_pipeline_test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const fs::path iso = dir / "disc.iso";
        write(iso, buildImage());

        { // the whole run, required disc only
            Fakes f;
            const auto st = runWith(options(dir / "a", iso), f);
            require(st.finished && !st.failed && !st.cancelled && !st.running, "run succeeds");
            require(st.step == Step::Done && st.executable == dir / "a" / "game.exe", "ends at Done with the game");
            require(f.calls == std::vector<std::string>{"generate", "lift", "build"}, "steps in order");
            require(slurp(dir / "a" / "game" / "test" / "A.BIN") == fileA(), "extracted into game/<folder>");
            require(!fs::exists(dir / "a" / "game" / "second"), "optional disc not extracted when not given");
            bool sawLog = false;
            for (const auto &line : st.log)
                sawLog |= contains(line, "generating ovl01");
            require(sawLog, "step log lines reach the status");
        }
        { // optional second disc
            Fakes f;
            auto o = options(dir / "b", iso);
            o.dw5Iso = iso;
            const auto st = runWith(o, f);
            require(st.finished, "run with both discs succeeds");
            require(fs::exists(dir / "b" / "game" / "second" / "SUB" / "B.TXT"), "second disc extracted");
        }
        { // not a supported disc: stops at Check, nothing else runs
            write(dir / "junk.iso", std::vector<uint8_t>(64 * 1024, 0x5a));
            Fakes f;
            const auto st = runWith(options(dir / "c", dir / "junk.iso"), f);
            require(st.failed && st.step == Step::Check, "unsupported image fails at Check");
            require(contains(st.error, stepName(Step::Check)), "error names the step");
            require(f.calls.empty() && !fs::exists(dir / "c" / "game"), "nothing extracted or generated");
        }
        { // missing image
            Fakes f;
            const auto st = runWith(options(dir / "d", dir / "missing.iso"), f);
            require(st.failed && st.step == Step::Check && f.calls.empty(), "missing image fails at Check");
        }
        { // a failing step stops the run and is named
            Fakes f;
            f.failIn = "lift";
            const auto st = runWith(options(dir / "e", iso), f);
            require(st.failed && st.step == Step::LiftVu1, "fails at LiftVu1");
            require(contains(st.error, stepName(Step::LiftVu1)) && contains(st.error, "boom"), "step + reason");
            require(f.calls == std::vector<std::string>{"generate", "lift"}, "build not started");
        }
        { // cancel
            Fakes f;
            f.waitForCancel = true;
            const auto st = runWith(options(dir / "f", iso), f, true);
            require(st.cancelled && !st.failed && !st.finished, "cancel is reported as cancelled, not failed");
            require(f.calls == std::vector<std::string>{"generate"}, "nothing runs after cancel");
        }
        for (int i = 0; i <= kStepCount; ++i)
            require(stepName(Step(i)) && *stepName(Step(i)), "every step has a name");

        fs::remove_all(dir);
        std::cout << "setup pipeline tests passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
