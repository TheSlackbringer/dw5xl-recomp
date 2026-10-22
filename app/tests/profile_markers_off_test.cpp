// Issue 025: without DW5XL_DEVELOPER_DIAGNOSTICS the profiling markers are empty inline functions: nothing is
// recorded and no file is created, so normal builds carry no marker code.
#include "runtime/ps2_profile_markers.h"
#include <filesystem>
#include <iostream>
#include <string>

static_assert(!ps2prof::kCompiledIn, "markers must be compiled out without DW5XL_DEVELOPER_DIAGNOSTICS");

int main()
{
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "dw5xl_profile_markers_off.bin";
    fs::remove(path);
    const bool opened = ps2prof::open(path.string().c_str(), 8);
    ps2prof::record(ps2prof::Event::User, 1);
    const bool ok = !opened && !ps2prof::enabled() && ps2prof::recorded() == 0 && !ps2prof::flush() && !fs::exists(path);
    std::cout << "profile_markers_off: " << (ok ? "ok" : "FAIL") << "\n";
    return ok ? 0 : 1;
}
