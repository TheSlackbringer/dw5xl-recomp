// dw5xl_setup — builds the game from the player's own disc (issue 011).
//   dw5xl_setup                         the window: pick the disc image(s), Start, watch the steps, Play
//   dw5xl_setup --iso <DW5XL.iso> [--dw5-iso <DW5.iso>] [--root <dir>] --no-window
//                                       the same run without a window (prints the steps, exit code 0 = done)
// Nothing is downloaded and nothing leaves the PC: every step works on files the player already has.
#include "codegen.h"
#include "disc_setup.h"
#include "file_dialog.h"
#include "run_process.h"
#include "setup_layout.h"
#include "setup_pipeline.h"

#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

void attachParentConsole(); // wizard_console.cpp

using namespace dw5xl::setup;
namespace fs = std::filesystem;

namespace {
#ifdef _WIN32
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

std::string utf8(const fs::path &path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

fs::path fromUtf8(const std::string &text) { return fs::path(std::u8string(text.begin(), text.end())); }

struct Args
{
    fs::path xlIso, dw5Iso, root;
    bool noWindow = false;
};

Args parseArgs(int argc, char **argv)
{
    Args a;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--iso" && hasValue)
            a.xlIso = fromUtf8(argv[++i]);
        else if (arg == "--dw5-iso" && hasValue)
            a.dw5Iso = fromUtf8(argv[++i]);
        else if (arg == "--root" && hasValue)
            a.root = fromUtf8(argv[++i]);
        else if (arg == "--no-window")
            a.noWindow = true;
    }
    return a;
}

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
int runWithoutWindow(const SetupOptions &options)
{
    SetupRun run(options);
    run.start();
    Step shown = Step::Done;
    int shownTenth = -1;
    for (;;)
    {
        const SetupStatus st = run.status();
        if (st.step != shown)
        {
            shown = st.step;
            shownTenth = -1;
            std::printf("[%d/%d] %s\n", int(st.step) + 1, kStepCount, stepName(st.step));
        }
        const int tenth = int(st.fraction * 10.0);
        if (tenth != shownTenth && st.step != Step::Done)
        {
            shownTenth = tenth;
            std::printf("      %3d %%  %s\n", tenth * 10, st.detail.c_str());
        }
        if (!st.running)
        {
            if (st.finished)
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
    SetupOptions base;
    std::string layoutError;
    DiscField xl{"Dynasty Warriors 5 Xtreme Legends (PAL) disc image"};
    DiscField dw5{"Dynasty Warriors 5 (PAL) disc image - optional, for Load Original"};
    std::unique_ptr<SetupRun> run;
    std::chrono::steady_clock::time_point started;
    std::string actionError;

    bool canStart() const
    {
        return layoutError.empty() && xl.disc == &kSupportedDiscs[0] &&
               (dw5.path[0] == '\0' || dw5.disc == &kSupportedDiscs[1]);
    }

    void start()
    {
        SetupOptions o = base;
        o.xlIso = fromUtf8(xl.path);
        if (dw5.path[0])
            o.dw5Iso = fromUtf8(dw5.path);
        run = std::make_unique<SetupRun>(o);
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

    void steps(const SetupStatus &st)
    {
        for (int i = 0; i < kStepCount; ++i)
        {
            const Step s = Step(i);
            const bool done = st.finished || int(st.step) > i;
            const bool current = !st.finished && st.step == s;
            if (done)
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "done   %s", stepName(s));
            else if (current && st.failed)
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "failed %s", stepName(s));
            else if (current)
            {
                ImGui::Text("...    %s", stepName(s));
                ImGui::ProgressBar(float(st.fraction), ImVec2(-1.0f, 0.0f), st.detail.c_str());
            }
            else
                ImGui::TextDisabled("       %s", stepName(s));
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
        ImGui::TextUnformatted("Dynasty Warriors 5 Xtreme Legends - build the game from your own disc");
        ImGui::TextDisabled("Nothing is downloaded and nothing leaves this PC. Drop a disc image here or browse.");
        ImGui::Separator();
        ImGui::Spacing();
        if (!layoutError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "This package is incomplete: %s", layoutError.c_str());

        discRow(xl, running);
        discRow(dw5, running);

        if (!run)
        {
            ImGui::BeginDisabled(!canStart());
            if (ImGui::Button("Start", ImVec2(140.0f, 0.0f)))
                start();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("Copies the game files, translates and compiles them (10-30 minutes).");
        }
        else
        {
            ImGui::Separator();
            steps(st);
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
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

int runWindow(const SetupOptions &options, const std::string &layoutError, const Args &args)
{
    Wizard w;
    w.base = options;
    w.layoutError = layoutError;
    if (!args.xlIso.empty())
        w.xl.set(args.xlIso);
    if (!args.dw5Iso.empty())
        w.dw5.set(args.dw5Iso);

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(820, 600, "DW5XL setup");
    SetWindowMinSize(640, 480);
    SetTargetFPS(30);
    rlImGuiSetup(true);
    ImGui::GetIO().IniFilename = nullptr;
    while (!WindowShouldClose())
    {
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
    }
    w.run.reset(); // cancels a running build and waits for it
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    const Args args = parseArgs(argc, argv);
    if (args.noWindow)
        attachParentConsole();
    std::error_code ec;
    fs::path root = args.root;
    if (root.empty())
        root = findRoot(argc > 0 ? fs::absolute(fromUtf8(argv[0]), ec).parent_path() : fs::current_path(ec));
    if (root.empty())
        root = findRoot(fs::current_path(ec));

    SetupOptions options;
    std::string layoutError;
    SetupLayout layout;
    if (root.empty())
        layoutError = "cannot find the project folder (app/ and recomp/) next to this program";
    else if (findLayout(root, kWindows, layout, layoutError))
    {
        options.recompiler = layout.recompiler;
        options.tools = layout.tools;
    }
    options.root = root;

    if (args.noWindow)
    {
        if (!layoutError.empty())
        {
            std::fprintf(stderr, "dw5xl_setup: %s\n", layoutError.c_str());
            return 2;
        }
        if (args.xlIso.empty())
        {
            std::fprintf(stderr, "usage: dw5xl_setup --iso <DW5XL.iso> [--dw5-iso <DW5.iso>] [--root <dir>] --no-window\n");
            return 2;
        }
        options.xlIso = args.xlIso;
        options.dw5Iso = args.dw5Iso;
        return runWithoutWindow(options);
    }
    return runWindow(options, layoutError, args);
}
