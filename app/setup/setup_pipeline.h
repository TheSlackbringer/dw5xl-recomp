#pragma once
// Setup wizard pipeline (issue 011): the whole "from your own disc to a playable game" run, shared by the wizard
// window and its command-line mode. Steps: check the disc image(s) → extract → generate the game's C++ → lift the
// VU1 code → compile. Runs on its own thread; the caller polls status() and may cancel().
#include "build_game.h"
#include "disc_setup.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace dw5xl::setup {

enum class Step
{
    Check,    // recognise the disc image(s) by file sizes
    Extract,  // copy the files with checksum verification
    Generate, // recompiler: main program + overlays
    LiftVu1,  // VU1 microprograms → C++
    Build,    // compile and link the game
    Done,
};
inline constexpr int kStepCount = int(Step::Done);
const char *stepName(Step step);

struct SetupOptions
{
    std::filesystem::path root;       // project / release root (contains app/, recomp/, game/)
    std::filesystem::path xlIso;      // Dynasty Warriors 5 Xtreme Legends (PAL), required
    std::filesystem::path dw5Iso;     // Dynasty Warriors 5 (PAL), optional (Load Original)
    std::filesystem::path recompiler; // ps2_recomp(.exe)
    BuildTools tools;
    std::filesystem::path buildDir;   // empty: root/app/build
    std::filesystem::path logDir;     // empty: root/logs/setup
    // Discs the images are checked against: [0] the required one, [1] the optional one.
    std::span<const DiscSpec> discs = kSupportedDiscs;

    std::filesystem::path effectiveBuildDir() const { return buildDir.empty() ? root / "app" / "build" : buildDir; }
    std::filesystem::path effectiveLogDir() const { return logDir.empty() ? root / "logs" / "setup" : logDir; }
};

struct SetupStatus
{
    Step step = Step::Check;
    double fraction = 0.0; // progress within the current step, 0..1
    std::string detail;    // what the step is doing ("LINKDATA.BIN", "ovl01", "512/1147")
    bool running = false;
    bool finished = false; // success
    bool failed = false;
    bool cancelled = false;
    std::string error;     // why it failed (names the step)
    std::filesystem::path executable;
    std::vector<std::string> log; // the last lines
};

class SetupRun;

// What a step gets to report progress and to see a cancel request.
class StepContext
{
public:
    explicit StepContext(SetupRun &run) : m_run(run) {}
    void progress(double fraction, const std::string &detail);
    void log(const std::string &line);
    bool cancelled() const;
    const std::atomic<bool> *cancelFlag() const; // for tools that stop a running program

private:
    SetupRun &m_run;
};

// The replaceable steps (the test swaps in fakes; the defaults run the real tools).
struct SetupSteps
{
    using Fn = std::function<bool(const SetupOptions &, StepContext &, std::string &error)>;
    Fn generate, liftVu1;
    std::function<bool(const SetupOptions &, StepContext &, std::filesystem::path &executable, std::string &error)>
        build;
};
SetupSteps defaultSetupSteps();

class SetupRun
{
public:
    explicit SetupRun(SetupOptions options, SetupSteps steps = defaultSetupSteps());
    ~SetupRun(); // cancels and waits
    SetupRun(const SetupRun &) = delete;
    SetupRun &operator=(const SetupRun &) = delete;

    void start();
    void cancel() { m_cancel = true; }
    bool wait(); // blocks until the run ends; true on success
    SetupStatus status() const;

private:
    friend class StepContext;
    void run();
    void enter(Step step);
    bool fail(const std::string &error);

    SetupOptions m_options;
    SetupSteps m_steps;
    std::atomic<bool> m_cancel{false};
    mutable std::mutex m_mutex;
    SetupStatus m_status;
    std::thread m_thread;
};

} // namespace dw5xl::setup
