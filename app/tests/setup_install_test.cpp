// Installed mode (issue 011 v4): player data folder, install marker, and the wizard's install-only run, on the
// synthetic image (no game data).
#include "disc_wait.h"
#include "install_state.h"
#include "setup_pipeline.h"
#include "synthetic_iso.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace dw5xl::setup;
using namespace dw5xl::setup::test;
namespace fs = std::filesystem;

namespace {
void require(bool ok, const char *what)
{
    if (!ok)
        throw std::runtime_error(what);
}

void setEnv(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

const DiscSpec kDiscs[] = {{"test disc", "test", kTestFiles}, {"second disc", "second", kTestFiles}};
} // namespace

int main()
{
    try
    {
        const fs::path dir = fs::temp_directory_path() / "dw5xl_setup_install_test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const fs::path iso = dir / "disc.iso";
        write(iso, buildImage());

        // Where player data goes.
        setEnv("DW5XL_DATA_DIR", (dir / "override").string().c_str());
        require(playerDataDir() == dir / "override", "DW5XL_DATA_DIR wins");
        setEnv("DW5XL_DATA_DIR", nullptr);
#ifdef _WIN32
        setEnv("LOCALAPPDATA", (dir / "local").string().c_str());
        require(playerDataDir() == dir / "local" / "DW5XL", "Windows: %LOCALAPPDATA%\\DW5XL");
#else
        setEnv("XDG_DATA_HOME", (dir / "xdg").c_str());
        require(playerDataDir() == dir / "xdg" / "dw5xl", "Linux: $XDG_DATA_HOME/dw5xl");
        setEnv("XDG_DATA_HOME", nullptr);
        const char *home = std::getenv("HOME");
        const std::string savedHome = home ? home : "";
        setEnv("HOME", (dir / "home").c_str());
        require(playerDataDir() == dir / "home" / ".local" / "share" / "dw5xl", "Linux: ~/.local/share/dw5xl");
        setEnv("HOME", savedHome.c_str());
#endif

        // Marker and fast check.
        const fs::path data = dir / "data";
        std::string problem, error;
        require(discFolder(data, kDiscs[0]) == data / "game" / "test", "disc folder under game/");
        require(installState(data, kDiscs[0]) == InstallState::Missing, "nothing installed yet");

        // The wizard's install-only run: check + extract + marker, no generate / lift / build.
        SetupOptions o;
        o.root = data;
        o.xlIso = iso;
        o.dw5Iso = iso;
        o.discs = kDiscs;
        o.installOnly = true;
        SetupSteps never;
        never.generate = never.liftVu1 = [](const SetupOptions &, StepContext &, std::string &) -> bool {
            throw std::runtime_error("install-only must not generate or lift");
        };
        never.build = [](const SetupOptions &, StepContext &, fs::path &, std::string &) -> bool {
            throw std::runtime_error("install-only must not build");
        };
        {
            SetupRun run(o, never);
            run.start();
            require(run.wait(), "install-only run succeeds");
            const SetupStatus st = run.status();
            require(st.step == Step::Done && st.fraction == 1.0, "ends at Done");
        }
        require(installState(data, kDiscs[0]) == InstallState::Installed, "first disc installed");
        require(installState(data, kDiscs[1]) == InstallState::Installed, "second disc installed");
        require(slurp(discFolder(data, kDiscs[0]) / "A.BIN") == fileA(), "files extracted");

        // Damage is noticed without hashing.
        fs::remove(discFolder(data, kDiscs[1]) / "SUB" / "B.TXT");
        require(installState(data, kDiscs[1], &problem) == InstallState::Incomplete &&
                    problem.find("B.TXT") != std::string::npos, "a missing file makes the install incomplete");
        write(discFolder(data, kDiscs[0]) / "A.BIN", std::vector<uint8_t>(10, 1));
        require(installState(data, kDiscs[0], &problem) == InstallState::Incomplete &&
                    problem.find("A.BIN") != std::string::npos, "a file with another size makes it incomplete");

        // A failed extraction leaves no marker behind (a repair starts from Missing / Incomplete, never Installed).
        const DiscSpec wrong[] = {{"wrong", "wrong", kTestFiles}};
        require(!writeInstallMarker(dir / "nothing-here", wrong[0], error), "no marker without the files");
        require(installState(dir / "nothing-here", wrong[0]) == InstallState::Missing, "still missing");

        { // Load Original without a click (M10): what the game's disc check is told
            using E = DiscWait::Event;
            DiscWait w;
            E e;
            require(w.notReady(false, e) && e == E::WindowOpened, "disc missing: not ready, window opens");
            require(w.notReady(false, e) && e == E::None, "still waiting: no second window");
            require(!w.notReady(true, e) && e == E::DiscReady, "installed: ready, window closes");
            require(!w.notReady(true, e) && e == E::None, "the game's second ready poll: still ready");

            DiscWait c;
            require(c.notReady(false, e) && e == E::WindowOpened, "missing again: window");
            c.cancel();
            require(!c.notReady(false, e) && e == E::None, "after Cancel: ready (state 3)");
            require(!c.notReady(false, e) && e == E::None,
                    "after Cancel: still ready at the game's second poll (state 4) - the window must not reopen");
            c.checked(); // the game found no disc and shows its own question
            require(c.notReady(false, e) && e == E::WindowOpened, "NO in the game's question: the window comes back");

            DiscWait idle;
            idle.cancel(); // nothing open: no effect
            require(idle.notReady(false, e) && e == E::WindowOpened, "a stray cancel changes nothing");
        }
        fs::remove_all(dir);
        std::cout << "setup install tests passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
