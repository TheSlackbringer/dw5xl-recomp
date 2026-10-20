#include "wizard_ui.h"
#include "codegen.h"
#include "disc_setup.h"
#include "file_dialog.h"
#include "run_process.h"

#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace dw5xl::setup {
namespace fs = std::filesystem;

std::string utf8(const fs::path &path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

fs::path fromUtf8(const std::string &text) { return fs::path(std::u8string(text.begin(), text.end())); }

namespace {
// Which supported disc an image is, or why not.
const DiscSpec *probe(const fs::path &image, std::string &message)
{
    IsoImage iso;
    std::string error;
    if (image.empty())
        return message.clear(), nullptr;
    if (!iso.open(image, error))
        return message = "not a disc image: " + error, nullptr;
    for (const auto &disc : kSupportedDiscs)
    {
        const DiscSpec one[] = {disc};
        if (identifyDisc(iso, one, error))
            return message = disc.name, &disc;
    }
    return message = "not a supported disc (only the PAL discs work)", nullptr;
}

fs::path gameFolder(const fs::path &root) { return root / "game" / kSupportedDiscs[0].folder; }

bool openFolder(const fs::path &dir, std::string &error)
{
#ifdef _WIN32
    return launchDetached("explorer.exe", {utf8(dir)}, {}, error);
#else
    return launchDetached("xdg-open", {utf8(dir)}, {}, error);
#endif
}

// ---- command-line mode ---------------------------------------------------------------------------------------
} // namespace

int runWizardConsole(const SetupOptions &options)
{
    SetupRun run(options);
    run.start();
    Step shown = Step::Done;
    int shownTenth = -1;
    for (;;)
    {
        const SetupStatus st = run.status();
        const int steps = options.installOnly ? int(Step::Extract) + 1 : kStepCount;
        if (st.step != shown && st.step != Step::Done)
        {
            shown = st.step;
            shownTenth = -1;
            std::printf("[%d/%d] %s\n", int(st.step) + 1, steps, stepName(st.step));
        }
        const int tenth = int(st.overall * 20.0); // every 5 % of the whole run
        if (tenth != shownTenth && st.step != Step::Done)
        {
            shownTenth = tenth;
            std::printf("      %3d %%  %s\n", tenth * 5, st.detail.c_str());
        }
        if (!st.running)
        {
            if (st.finished && options.installOnly)
                std::printf("installed in %s\n", utf8(options.root).c_str());
            else if (st.finished)
                std::printf("done: %s\n", utf8(st.executable).c_str());
            else if (st.cancelled)
                std::printf("cancelled\n");
            else
                std::printf("failed: %s\n(logs: %s)\n", st.error.c_str(), utf8(options.effectiveLogDir()).c_str());
            std::fflush(stdout);
            return st.finished ? 0 : 1;
        }
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

namespace {
// ---- the window ------------------------------------------------------------------------------------------------
struct DiscField
{
    const char *label;
    char path[4096] = "";
    std::string message;
    const DiscSpec *disc = nullptr;

    void set(const fs::path &p)
    {
        std::snprintf(path, sizeof(path), "%s", utf8(p).c_str());
        recheck();
    }
    void recheck() { disc = probe(fromUtf8(path), message); }
};

struct Wizard
{
    SetupOptions base; // base.installOnly: player install
    std::string layoutError;
    DiscField xl{"Dynasty Warriors 5 Xtreme Legends (PAL) disc image"};
    DiscField dw5{"Dynasty Warriors 5 (PAL) disc image - optional, for Load Original"};
    std::unique_ptr<SetupRun> run;
    std::chrono::steady_clock::time_point started;
    std::string actionError;

    bool discInstalled = false;
    bool canStart() const
    {
        const bool xlOk = xl.disc == &kSupportedDiscs[0] || (discInstalled && xl.path[0] == '\0');
        return layoutError.empty() && xlOk && (dw5.path[0] == '\0' || dw5.disc == &kSupportedDiscs[1]);
    }

    void start()
    {
        SetupOptions o = base;
        o.xlIso = fromUtf8(xl.path);
        if (dw5.path[0])
            o.dw5Iso = fromUtf8(dw5.path);
        run = std::make_unique<SetupRun>(o);
        // (install mode runs Check + Extract only; see SetupOptions::installOnly)
        run->start();
        started = std::chrono::steady_clock::now();
        actionError.clear();
    }

    // A dropped image goes to the field of the disc it is; anything else to the first empty field.
    void dropped(const fs::path &p)
    {
        std::string message;
        const DiscSpec *disc = probe(p, message);
        if (disc == &kSupportedDiscs[1])
            dw5.set(p);
        else if (disc == &kSupportedDiscs[0] || xl.path[0] == '\0')
            xl.set(p);
        else
            dw5.set(p);
    }

    void discRow(DiscField &f, bool locked)
    {
        ImGui::TextUnformatted(f.label);
        ImGui::PushID(f.label);
        ImGui::BeginDisabled(locked);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110.0f);
        if (ImGui::InputText("##path", f.path, sizeof(f.path)) || ImGui::IsItemDeactivatedAfterEdit())
            f.recheck();
        ImGui::SameLine();
        ImGui::BeginDisabled(!fileDialogAvailable());
        if (ImGui::Button("Browse...", ImVec2(100.0f, 0.0f)))
        {
            const fs::path chosen = pickDiscImage(f.label);
            if (!chosen.empty())
                f.set(chosen);
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (!f.message.empty())
        {
            const bool good = f.disc != nullptr;
            ImGui::TextColored(good ? ImVec4(0.45f, 0.85f, 0.45f, 1) : ImVec4(1.0f, 0.45f, 0.4f, 1), "%s %s",
                               good ? "OK:" : "Not usable:", f.message.c_str());
        }
        ImGui::PopID();
        ImGui::Spacing();
    }

    // One bar for the whole run (owner 2026-10-20): which step, the overall share done, the time left.
    void steps(const SetupStatus &st, double seconds)
    {
        int index = 0;
        for (size_t i = 0; i < st.plan.size(); ++i)
            if (st.plan[i] == st.step)
                index = int(i) + 1;
        const int count = int(st.plan.size());
        if (st.finished)
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "Done.");
        else if (st.failed)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "Stopped at: %s", stepName(st.step));
        else if (index > 0)
            ImGui::Text("Step %d of %d: %s", index, count, stepName(st.step));
        else
            ImGui::TextUnformatted(stepName(st.step));
        char label[64];
        std::snprintf(label, sizeof(label), "%d %%", int(st.overall * 100.0));
        ImGui::ProgressBar(float(st.overall), ImVec2(-1.0f, 28.0f), label);
        if (!st.detail.empty() && !st.finished)
            ImGui::TextDisabled("%s", st.detail.c_str());
        if (st.running && st.overall > 0.15 && seconds > 30.0)
        {
            const int left = int(seconds * (1.0 - st.overall) / st.overall / 60.0 + 0.5);
            ImGui::TextDisabled(left <= 1 ? "About a minute left" : "About %d minutes left", left);
        }
    }

    void frame()
    {
        const SetupStatus st = run ? run->status() : SetupStatus{};
        const bool running = run && st.running;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(float(GetScreenWidth()), float(GetScreenHeight())));
        ImGui::Begin("setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted(base.installOnly ? "Dynasty Warriors 5 Xtreme Legends - your disc, once"
                                                : "Dynasty Warriors 5 Xtreme Legends - build the game from your own disc");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("The game files come from your own disc image. Nothing is downloaded and nothing leaves "
                           "this PC. Drop a disc image here or browse.");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();
        if (!layoutError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "This package is incomplete: %s", layoutError.c_str());

        if (discInstalled && !run)
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1),
                               "Your game files are installed. Start builds the game for this version; "
                               "no disc image needed.");
        discRow(xl, running);
        discRow(dw5, running);

        if (!run)
        {
            ImGui::BeginDisabled(!canStart());
            if (ImGui::Button(base.installOnly ? "Install" : "Start", ImVec2(140.0f, 0.0f)))
                start();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled(base.installOnly ? "Copies and checks the game files from your disc (1-2 minutes)."
                                                 : "Copies the game files, translates and compiles them (10-30 minutes).");
        }
        else
        {
            ImGui::Separator();
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
            steps(st, double(secs));
            if (running)
            {
                ImGui::Text("Running for %lld:%02lld", (long long)(secs / 60), (long long)(secs % 60));
                if (ImGui::Button("Cancel", ImVec2(140.0f, 0.0f)))
                    run->cancel();
            }
            else if (st.finished)
            {
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "Ready. The game is in %s",
                                   utf8(gameFolder(base.root)).c_str());
                if (ImGui::Button("Play", ImVec2(140.0f, 0.0f)))
                    launchDetached(st.executable, {"./" + utf8(fs::path(kCodegenProfiles[0].input).filename())},
                                   gameFolder(base.root), actionError);
            }
            else
            {
                if (st.cancelled)
                    ImGui::TextUnformatted("Cancelled.");
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s", st.error.c_str());
                if (ImGui::Button("Try again", ImVec2(140.0f, 0.0f)))
                    run.reset();
            }
            if (!running)
            {
                ImGui::SameLine();
                if (ImGui::Button("Open the log folder", ImVec2(180.0f, 0.0f)))
                    openFolder(base.effectiveLogDir(), actionError);
            }
            if (!actionError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s", actionError.c_str());
            ImGui::Spacing();
            ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
            for (const auto &line : st.log)
                ImGui::TextUnformatted(line.c_str());
            if (running)
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
        ImGui::End();
    }
};

} // namespace

bool runWizardWindow(const WizardConfig &config)
{
    Wizard w;
    w.base = config.base;
    w.layoutError = config.layoutError;
    w.discInstalled = config.discInstalled;
    if (!config.xlIso.empty())
        w.xl.set(config.xlIso);
    if (!config.dw5Iso.empty())
        w.dw5.set(config.dw5Iso);
    bool finished = false;
    // DW5XL-DEBUG: DW5XL_WIZARD_SHOT=<file.png> saves the window after one second (to look at it in tests).
    const char *shot = std::getenv("DW5XL_WIZARD_SHOT");
    int frames = 0;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(820, 600, "DW5XL setup");
    SetWindowMinSize(640, 480);
    SetTargetFPS(30);
    rlImGuiSetup(true);
    ImGui::GetIO().IniFilename = nullptr;
    while (!WindowShouldClose())
    {
        if (w.run && w.run->status().finished)
        {
            finished = true;
            if (w.base.installOnly || config.closeWhenFinished)
                break; // the game starts next
        }
        if (IsFileDropped())
        {
            FilePathList files = LoadDroppedFiles();
            for (unsigned i = 0; i < files.count; ++i)
                w.dropped(fromUtf8(files.paths[i]));
            UnloadDroppedFiles(files);
        }
        BeginDrawing();
        ClearBackground(Color{24, 26, 30, 255});
        rlImGuiBegin();
        w.frame();
        rlImGuiEnd();
        EndDrawing();
        if (shot && *shot && ++frames == 30)
        {
            Image image = LoadImageFromScreen();
            ExportImage(image, shot);
            UnloadImage(image);
        }
    }
    w.run.reset(); // cancels a running build and waits for it
    rlImGuiShutdown();
    CloseWindow();
    return finished;
}

// ---- the "insert disc" panel (inside the running game) -----------------------------------------------------------
struct DiscInstallPanel::State
{
    fs::path dataDir;
    const DiscSpec *disc;
    DiscField field{"Disc image"};
    std::unique_ptr<SetupRun> run;
};

DiscInstallPanel::DiscInstallPanel(fs::path dataDir, const DiscSpec &disc) : m(std::make_unique<State>())
{
    m->dataDir = std::move(dataDir);
    m->disc = &disc;
    m->field.label = disc.name;
}

DiscInstallPanel::~DiscInstallPanel() = default; // a running copy is cancelled and waited for by ~SetupRun

bool DiscInstallPanel::running() const { return m->run && m->run->status().running; }

void DiscInstallPanel::dropped(const fs::path &image)
{
    if (!running())
        m->field.set(image);
}

bool DiscInstallPanel::draw()
{
    const SetupStatus st = m->run ? m->run->status() : SetupStatus{};
    if (st.finished)
    {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%s is ready.", m->disc->name);
        return true;
    }
    const bool busy = m->run && st.running;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Choose your own disc image (.iso). Its files are checked and copied once. "
                       "You can also drop the image on the game window.");
    ImGui::PopStyleColor();
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110.0f);
    if (ImGui::InputText("##disc", m->field.path, sizeof(m->field.path)) || ImGui::IsItemDeactivatedAfterEdit())
        m->field.recheck();
    ImGui::SameLine();
    ImGui::BeginDisabled(!fileDialogAvailable());
    if (ImGui::Button("Browse...", ImVec2(100.0f, 0.0f)))
    {
        const fs::path chosen = pickDiscImage(m->disc->name);
        if (!chosen.empty())
            m->field.set(chosen);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    const bool right = m->field.disc && std::string(m->field.disc->name) == m->disc->name;
    if (!m->field.message.empty())
    {
        if (right)
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "OK: %s", m->field.message.c_str());
        else if (m->field.disc)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "This is %s, not %s.", m->field.message.c_str(),
                               m->disc->name);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "Not usable: %s", m->field.message.c_str());
    }
    if (busy)
    {
        ImGui::ProgressBar(float(st.overall), ImVec2(-1.0f, 0.0f), st.detail.c_str());
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
            m->run->cancel();
        return false;
    }
    if (st.failed)
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s", st.error.c_str());
    else if (st.cancelled)
        ImGui::TextUnformatted("Cancelled.");
    ImGui::BeginDisabled(!right);
    if (ImGui::Button("Install", ImVec2(120.0f, 0.0f)))
    {
        SetupOptions o;
        o.root = m->dataDir;
        o.installOnly = true;
        o.logDir = m->dataDir / "logs";
        o.xlIso = fromUtf8(m->field.path);
        o.discs = std::span<const DiscSpec>(m->disc, 1);
        m->run = std::make_unique<SetupRun>(o);
        m->run->start();
    }
    ImGui::EndDisabled();
    return false;
}

} // namespace dw5xl::setup
