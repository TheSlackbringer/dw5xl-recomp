#include "setup_pipeline.h"
#include "codegen.h"
#include "install_state.h"
#include "vu1lift/vu1_translator.h"

#include <fstream>
#include <iterator>

namespace dw5xl::setup {
namespace fs = std::filesystem;

namespace {
constexpr size_t kLogLines = 200;

std::string readText(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// Writes `text` only if it differs, so an unchanged file does not trigger a recompile.
bool writeIfChanged(const fs::path &path, const std::string &text, std::string &error)
{
    if (fs::exists(path) && readText(path) == text)
        return true;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out.flush())
    {
        error = "cannot write " + path.string();
        return false;
    }
    return true;
}
} // namespace

const char *stepName(Step step)
{
    switch (step)
    {
    case Step::Check: return "Check the disc";
    case Step::Extract: return "Copy the game files";
    case Step::Generate: return "Translate the game code";
    case Step::LiftVu1: return "Translate the VU1 code";
    case Step::Build: return "Compile the game";
    case Step::Done: return "Done";
    }
    return "?";
}

void StepContext::progress(double fraction, const std::string &detail)
{
    std::lock_guard lock(m_run.m_mutex);
    m_run.m_status.fraction = fraction < 0.0 ? 0.0 : fraction > 1.0 ? 1.0 : fraction;
    m_run.m_status.detail = detail;
}

void StepContext::log(const std::string &line)
{
    {
        std::lock_guard lock(m_run.m_mutex);
        auto &log = m_run.m_status.log;
        log.push_back(line);
        if (log.size() > kLogLines)
            log.erase(log.begin(), log.begin() + long(log.size() - kLogLines));
    }
    std::error_code ec;
    const fs::path dir = m_run.m_options.effectiveLogDir();
    fs::create_directories(dir, ec);
    std::ofstream(dir / "setup.log", std::ios::app) << line << "\n";
}

bool StepContext::cancelled() const { return m_run.m_cancel; }
const std::atomic<bool> *StepContext::cancelFlag() const { return &m_run.m_cancel; }

SetupSteps defaultSetupSteps()
{
    SetupSteps s;
    s.generate = [](const SetupOptions &o, StepContext &c, std::string &error) {
        return generateCode(o.root, o.recompiler, o.effectiveLogDir(), [&](const char *name, int index, int count) {
            c.progress(double(index) / count, name);
            c.log(std::string("recompiling ") + name);
            return !c.cancelled();
        }, error);
    };
    s.liftVu1 = [](const SetupOptions &o, StepContext &c, std::string &error) {
        std::string text;
        if (!vu1lift::generateLiftedVu1(o.root / kCodegenProfiles[0].input, text, error, [&](const std::string &line) {
                c.progress(0.5, line);
                c.log(line);
            }))
            return false;
        return writeIfChanged(o.root / "recomp" / "dw5xl" / "vu1" / "dw5xl_vu1_lifted.cpp", text, error);
    };
    s.build = [](const SetupOptions &o, StepContext &c, fs::path &executable, std::string &error) {
        return buildGame(o.root, o.effectiveBuildDir(), o.tools, o.effectiveLogDir() / "build.log",
                         [&](const BuildProgress &p) {
                             if (p.total > 0)
                                 c.progress(double(p.done) / p.total,
                                            std::to_string(p.done) + "/" + std::to_string(p.total));
                             else
                                 c.progress(0.0, p.phase);
                         },
                         executable, error, c.cancelFlag());
    };
    return s;
}

SetupRun::SetupRun(SetupOptions options, SetupSteps steps) : m_options(std::move(options)), m_steps(std::move(steps)) {}

SetupRun::~SetupRun()
{
    cancel();
    if (m_thread.joinable())
        m_thread.join();
}

void SetupRun::start()
{
    {
        std::lock_guard lock(m_mutex);
        m_status = {};
        m_status.running = true;
    }
    m_cancel = false;
    m_thread = std::thread([this] { run(); });
}

bool SetupRun::wait()
{
    if (m_thread.joinable())
        m_thread.join();
    std::lock_guard lock(m_mutex);
    return m_status.finished;
}

SetupStatus SetupRun::status() const
{
    std::lock_guard lock(m_mutex);
    return m_status;
}

void SetupRun::enter(Step step)
{
    {
        std::lock_guard lock(m_mutex);
        m_status.step = step;
        m_status.fraction = 0.0;
        m_status.detail.clear();
    }
    StepContext(*this).log(std::string("== ") + stepName(step));
}

bool SetupRun::fail(const std::string &error)
{
    std::lock_guard lock(m_mutex);
    m_status.running = false;
    if (m_cancel)
    {
        m_status.cancelled = true;
        return false;
    }
    m_status.failed = true;
    m_status.error = std::string(stepName(m_status.step)) + ": " + error;
    return false;
}

void SetupRun::run()
{
    StepContext ctx(*this);
    const auto &discs = m_options.discs;

    // Check: both images are recognised before anything is written.
    enter(Step::Check);
    struct Image
    {
        fs::path path;
        const DiscSpec *disc;
        IsoImage iso;
    };
    std::vector<Image> images;
    images.push_back({m_options.xlIso, discs.empty() ? nullptr : &discs[0], {}});
    if (!m_options.dw5Iso.empty())
        images.push_back({m_options.dw5Iso, discs.size() > 1 ? &discs[1] : nullptr, {}});
    for (auto &image : images)
    {
        std::string error;
        if (!image.disc)
        {
            fail("no disc to check against");
            return;
        }
        if (image.path.empty() || !fs::exists(image.path))
        {
            fail("the disc image " + image.path.string() + " does not exist");
            return;
        }
        if (!image.iso.open(image.path, error))
        {
            fail(image.path.string() + " is not a disc image (" + error + ")");
            return;
        }
        const DiscSpec one[] = {*image.disc};
        if (!identifyDisc(image.iso, one, error))
        {
            fail(image.path.filename().string() + " is not " + image.disc->name + ": " + error);
            return;
        }
        ctx.log(image.path.filename().string() + ": " + image.disc->name);
    }

    // Extract, with checksums.
    enter(Step::Extract);
    uint64_t total = 0, before = 0;
    for (auto &image : images)
        for (const auto &file : image.disc->files)
            total += file.size;
    for (auto &image : images)
    {
        std::string error;
        uint64_t last = 0;
        const bool ok = extractDisc(image.iso, *image.disc, m_options.root / "game" / image.disc->folder,
                                    [&](const ExtractProgress &p) {
                                        last = p.doneBytes;
                                        ctx.progress(total ? double(before + p.doneBytes) / total : 1.0, p.file);
                                        return !m_cancel;
                                    }, error);
        image.iso.close();
        if (!ok)
        {
            fail(error);
            return;
        }
        before += last;
        ctx.log(std::string("copied ") + image.disc->name + " to game/" + image.disc->folder);
        if (m_options.installOnly && !writeInstallMarker(m_options.root, *image.disc, error))
        {
            fail(error);
            return;
        }
    }
    if (m_options.installOnly)
    {
        enter(Step::Done);
        std::lock_guard lock(m_mutex);
        m_status.fraction = 1.0;
        m_status.running = false;
        m_status.finished = true;
        return;
    }

    const auto runStep = [&](Step step, const SetupSteps::Fn &fn) {
        enter(step);
        std::string error;
        if (!fn(m_options, ctx, error) || m_cancel)
            return fail(error.empty() ? "stopped" : error);
        ctx.progress(1.0, {});
        return true;
    };
    if (!runStep(Step::Generate, m_steps.generate) || !runStep(Step::LiftVu1, m_steps.liftVu1))
        return;

    enter(Step::Build);
    fs::path executable;
    std::string error;
    if (!m_steps.build(m_options, ctx, executable, error) || m_cancel)
    {
        fail(error.empty() ? "stopped" : error);
        return;
    }

    enter(Step::Done);
    std::lock_guard lock(m_mutex);
    m_status.fraction = 1.0;
    m_status.executable = executable;
    m_status.running = false;
    m_status.finished = true;
}

} // namespace dw5xl::setup
