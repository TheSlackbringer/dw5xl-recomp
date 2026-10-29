// Developer-only issue 042 measurement driver. Never bundled as the player launcher or registered with CTest.
#include "codegen.h"
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char **argv)
{
    if (argc != 5)
    {
        std::cerr << "usage: setup_measure <source-root> <isolated-data-root> <recompiler> <log-root>\n";
        return 2;
    }
    namespace fs = std::filesystem;
    auto path = [](const char *s) { const std::string v(s); return fs::path(std::u8string(v.begin(), v.end())); };
    const auto logs = path(argv[4]);
    fs::create_directories(logs);
    std::ofstream out(logs / "translation-timing.jsonl", std::ios::binary);
    ps2recomp::timing::Session timing(out, ps2recomp::timing::newRunId(), "setup_measure");
    ps2recomp::timing::Stage total(&timing, "translation_only");
    std::string error;
    const bool ok = dw5xl::setup::generateCode(path(argv[1]), path(argv[3]), logs, nullptr, error,
                                               path(argv[2]), &timing, total.id());
    total.finish(ok ? "success" : "failed");
    if (!ok) std::cerr << error << '\n';
    return ok && timing.healthy() ? 0 : 1;
}
