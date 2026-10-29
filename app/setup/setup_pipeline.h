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
#include <fstream>
#include <memory>
#include <chrono>

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

// One progress bar for the whole run (owner 2026-10-20): the steps that run, weighted by their measured share of a
// first start on Windows (check 1, copy 12, translate 12, VU1 3, compile 72). `current` is in `plan`;
// `fraction` is its progress 0..1. Returns 0..1.
double overallProgress(const std::vector<Step> &plan, Step current, double fraction);

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
    // Player install (issue 011 v4): `root` is the player data folder; Check + Extract only, then each disc gets
    // its install marker. Generate / lift / build are the developer path and are skipped.
    bool installOnly = false;
    // Release (issue 011 v5): the player data folder. Disc files, generated code, the build and its logs go there;
    // configurations, sources and tools stay in `root` (the release package). Empty: everything under `root`.
    // With `xlIso` empty and the XL disc already installed in the data folder, Check + Extract are skipped (a new
    // release rebuilds without asking for the disc again).
    std::filesystem::path dataDir;
    std::string releaseId; // nonempty: release completion belongs to this pipeline, under its lease
    bool usePersistentBuilder=false;
    bool reuseGenerated=false;
    bool forceGenerate=false; // explicit --rebuild bypasses generation reuse

    std::filesystem::path effectiveDataDir() const { return dataDir.empty() ? root : dataDir; }
    std::filesystem::path effectiveBuildDir() const
    {
        return !buildDir.empty() ? buildDir : dataDir.empty() ? root / "app" / "build" : dataDir / "build";
    }
    std::filesystem::path effectiveLogDir() const
    {
        return !logDir.empty() ? logDir : dataDir.empty() ? root / "logs" / "setup" : dataDir / "logs";
    }
};

struct SetupStatus
{
    Step step = Step::Check;
    double fraction = 0.0; // progress within the current step, 0..1
    double overall = 0.0;  // progress of the whole run, 0..1 (overallProgress over `plan`)
    std::vector<Step> plan; // the steps this run does (Check/Extract are skipped when the disc is installed, the
                            // build steps in install-only runs)
    std::string detail;    // what the step is doing ("LINKDATA.BIN", "ovl01", "512/1147")
    double elapsedSeconds=0.0;
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
    ps2recomp::timing::Session *timing() const;
    uint64_t timingParent() const;

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
    void runSteps();
    void enter(Step step);
    bool fail(const std::string &error);

    SetupOptions m_options;
    SetupSteps m_steps;
    std::atomic<bool> m_cancel{false};
    mutable std::mutex m_mutex;
    SetupStatus m_status;
    std::thread m_thread;
    std::ofstream m_timingFile;
    std::unique_ptr<ps2recomp::timing::Session> m_timing;
    uint64_t m_timingParent = 0;
    std::chrono::steady_clock::time_point m_started;
    bool m_completed=false;
};

} // namespace dw5xl::setup
