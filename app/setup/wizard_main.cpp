// dw5xl_setup — the developer path (issue 011): builds the game from your own disc.
//   dw5xl_setup                         the window: pick the disc image(s), Start, watch the steps, Play
//   dw5xl_setup --iso <DW5XL.iso> [--dw5-iso <DW5.iso>] [--root <dir>] --no-window
//                                       the same run without a window (prints the steps, exit code 0 = done)
// Players do not need it: the released game installs its files from the disc itself (wizard_ui, install mode).
#include "setup_layout.h"
#include "wizard_ui.h"

#include <cstdio>
#include <string>

void attachParentConsole(); // wizard_console.cpp

using namespace dw5xl::setup;
namespace fs = std::filesystem;

namespace {
#ifdef _WIN32
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

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
        return runWizardConsole(options);
    }
    WizardConfig config{options, layoutError, args.xlIso, args.dw5Iso};
    runWizardWindow(config);
    return 0;
}
