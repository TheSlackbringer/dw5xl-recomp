// Issue 047: the aspect and FOV keys of dw5xl.ini, in a private settings file.
#include "dw5xl_widescreen.h"
#include "ps2_runtime.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[] = {nullptr};

using namespace dw5xl::widescreen;
namespace {
int failures = 0;
void require(bool ok, const char *what) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; } }
}

int main()
{
    const auto folder = std::filesystem::temp_directory_path() / ("dw5xl-widescreen-settings-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(folder);
    const std::filesystem::path path = folder / "dw5xl.ini";
#ifdef _WIN32
    _putenv_s("PS2X_SETTINGS_FILE", path.string().c_str());
#else
    setenv("PS2X_SETTINGS_FILE", path.string().c_str(), 1);
#endif
    const auto write = [&](const char *text) { std::ofstream(path, std::ios::trunc) << text; };

    require(savedAspect() == Aspect4x3 && savedFov() == 100, "no file: 4:3 and 100 %");

    write("vsync=on\nscale=4\n");
    require(savedAspect() == Aspect4x3 && savedFov() == 100, "an old file without the keys: 4:3 and 100 %");

    saveAspect(Aspect16x9);
    saveFov(120);
    require(savedAspect() == Aspect16x9, "16:9 restored");
    require(savedFov() == 120, "120 % restored");
    std::string text;
    require(ps2x::settings::loadText("vsync", text) && text == "on", "other keys kept");
    require(ps2x::settings::loadText("scale", text) && text == "4", "other keys kept (2)");

    saveAspect(Aspect21x9);
    saveFov(80);
    require(savedAspect() == Aspect21x9 && savedFov() == 80, "21:9 and 80 % restored");
    saveAspect(Aspect4x3);
    saveFov(100);
    require(savedAspect() == Aspect4x3 && savedFov() == 100, "4:3 and 100 % restored");

    for (const char *bad : {"aspect=5:4\nfov=117\n", "aspect=\nfov=135\n", "aspect=16:10\nfov=75\n"}) {
        write(bad);
        require(savedAspect() == Aspect4x3 && savedFov() == 100, "a value the menu cannot show: the default");
    }

    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    if (failures == 0) std::puts("widescreen settings: OK");
    return failures ? 1 : 0;
}
