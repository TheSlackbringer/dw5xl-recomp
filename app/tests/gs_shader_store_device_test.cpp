// DW5XL-TEST issue 066: the Vulkan renderer's own store of compiled shader programs, with a graphics card. No game
// data: the renderer draws the tests' own triangle.
//   1. A renderer with a store file named: nothing is there yet, it starts as always and draws its picture.
//      Written on demand, the file is a store for this renderer's shaders.
//   2. A second renderer takes that file, and draws the same picture.
//   3. No compile held the renderer up: the store is not written again. One did (the library's warning, made up
//      here): it is written once, and not again after that.
//   4. A store for another graphics card or driver (the card's mark in it changed), and a damaged one: left out,
//      the renderer starts as always and draws the same picture.
//   5. PS2X_SHADER_STORE=off: no store is used and none is written.
// Exit code 77: no Vulkan device (skipped).
#include "runtime/gs/gs_shader_store.h"
#include "runtime/gs/gs_vulkan_backend.h"
#include "gs_test_pictures.h"
#include "test_one_program.h"

#include "logging.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gs_test;

namespace
{
int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

void setSwitch(const char *value)
{
#ifdef _WIN32
    _putenv_s("PS2X_SHADER_STORE", value ? value : "");
#else
    if (value)
        setenv("PS2X_SHADER_STORE", value, 1);
    else
        unsetenv("PS2X_SHADER_STORE");
#endif
}

// A renderer made now (it reads the store), the tests' picture A drawn and read back.
std::vector<uint8_t> pictureOfNewRenderer(std::unique_ptr<GSVulkanBackend> &backend, std::vector<uint8_t> &vram, uint64_t tick)
{
    backend = std::make_unique<GSVulkanBackend>();
    backend->Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
    drawA(*backend);
    return readBack(*backend, tick).pixels;
}
}

int DW5XL_TEST_MAIN(gs_shader_store)()
{
    namespace store = ps2x::gs::shader_store;
    // A folder of this run's own, with a name no earlier run had, removed at the run's end. Nothing is removed
    // before use: a file or folder just deleted can linger on Windows, and nothing of its name can be made until
    // it is gone (found 2026-11-14: a folder emptied and used again at once made the first write fail).
    const std::filesystem::path all = std::filesystem::current_path() / "gs_shader_store_device_test_files";
    std::error_code ignored;
    const std::filesystem::path folder =
        all / ("run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    struct Tidy
    {
        std::filesystem::path folder;
        ~Tidy()
        {
            std::error_code failed;
            std::filesystem::remove_all(folder, failed);
        }
    } tidy{folder};
    const std::string path = (folder / "shaders.bin").string();
    setSwitch(nullptr);
    store::setPath(path);

    std::vector<uint8_t> vram(4u * 1024u * 1024u, 0u);
    std::unique_ptr<GSVulkanBackend> backend;
    std::vector<uint8_t> first;
    try
    {
        first = pictureOfNewRenderer(backend, vram, 1u);
    }
    catch (const std::exception &error)
    {
        std::printf("skip  no Vulkan device: %s\n", error.what());
        return 77;
    }

    // 1.
    ps2x::gs::ShaderStoreState state = ps2x::gs::shaderStoreState();
    check(state.inUse && !state.taken && state.readBytes == 0u, "no store yet: the renderer has its file named and starts without one");
    check(!first.empty(), "... and draws its picture");
    check(!std::filesystem::exists(path), "nothing is written while the renderer runs");
    const uint64_t writesAtStart = state.writes;
    store::writeNow(true);
    state = ps2x::gs::shaderStoreState();
    std::vector<uint8_t> content;
    check(state.writes == writesAtStart + 1u && std::filesystem::exists(path), "written on demand: the file is there");
    check(store::read(path, 0u, content) == store::Read::OtherShaders, "... and it is marked with this renderer's shaders (another mark is refused)");
    const uintmax_t size = std::filesystem::file_size(path, ignored);
    std::printf("note  the store after the tests' one picture: %ju bytes\n", ignored ? uintmax_t(0) : size);
    backend.reset();
    check(ps2x::gs::shaderStoreState().writes == writesAtStart + 1u, "the renderer's end does not write it again: no compile held it up since");

    // 2.
    const std::vector<uint8_t> second = pictureOfNewRenderer(backend, vram, 2u);
    state = ps2x::gs::shaderStoreState();
    check(state.inUse && state.taken && state.readBytes > 0u, "a second renderer takes the store");
    check(!second.empty() && sameColours(second, first), "... and draws the same picture");

    // 3.
    const uint64_t writesBefore = state.writes;
    store::writeNow();
    check(ps2x::gs::shaderStoreState().writes == writesBefore, "no compile held the renderer up: the store is not written");
    const uint64_t stalledBefore = ps2x::gs::shaderStoreState().stalled;
    LOGW("Stalled compile (%s, %016llx): thread %u - %.3f us (mode: %s, success: %s).\n", "made up by the test", 0ull, 0u, 9000.0,
         "sync", "yes");
    check(ps2x::gs::shaderStoreState().stalled == stalledBefore + 1u, "the library's warning of a compile that held the renderer up is counted");
    store::writeNow();
    check(ps2x::gs::shaderStoreState().writes == writesBefore + 1u, "after one, the store is written");
    store::writeNow();
    backend.reset();
    check(ps2x::gs::shaderStoreState().writes == writesBefore + 1u, "... once: not again without another, nor at the renderer's end");

    // 4. The library's own data follows our head of 40 bytes and begins with the card's and driver's mark.
    {
        std::vector<uint8_t> whole;
        {
            std::ifstream in(path, std::ios::binary);
            whole.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::vector<uint8_t> taken;
        const bool readable = store::read(path, 0u, taken) == store::Read::OtherShaders && whole.size() > 100u;
        check(readable, "the written store can be looked at");
        if (readable)
        {
            // Another card: the mark changed, our own check sum made right again (so only the library's side differs).
            std::vector<uint8_t> other(whole.begin() + 40, whole.end());
            other[0] ^= 0xFFu;
            // The shaders' mark of the file, from its head (bytes 16 to 23).
            uint64_t mark = 0u;
            for (int i = 0; i < 8; ++i)
                mark |= static_cast<uint64_t>(whole[16u + static_cast<size_t>(i)]) << (8 * i);
            check(store::write(path, mark, other.data(), other.size()), "a store with another card's mark is put in its place");
            const std::vector<uint8_t> third = pictureOfNewRenderer(backend, vram, 3u);
            state = ps2x::gs::shaderStoreState();
            check(state.inUse && !state.taken, "a store for another graphics card or driver is left out");
            check(!third.empty() && sameColours(third, first), "... and the renderer draws the same picture");
            backend.reset();

            std::vector<uint8_t> damaged = whole;
            damaged[damaged.size() / 2u] ^= 0x10u;
            {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char *>(damaged.data()), static_cast<std::streamsize>(damaged.size()));
            }
            const std::vector<uint8_t> fourth = pictureOfNewRenderer(backend, vram, 4u);
            state = ps2x::gs::shaderStoreState();
            check(state.inUse && !state.taken, "a damaged store is left out");
            check(!fourth.empty() && sameColours(fourth, first), "... and the renderer draws the same picture");
            backend.reset();
        }
    }

    // 5.
    {
        std::filesystem::remove(path, ignored);
        setSwitch("off");
        const std::vector<uint8_t> fifth = pictureOfNewRenderer(backend, vram, 5u);
        state = ps2x::gs::shaderStoreState();
        check(!state.inUse && !fifth.empty() && sameColours(fifth, first), "switched off: no store is used, the picture is the same");
        store::writeNow(true);
        backend.reset();
        check(!std::filesystem::exists(path), "... and none is written");
        setSwitch(nullptr);
    }

    store::setPath(std::string());
    if (g_failures)
    {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("PASSED\n");
    return 0;
}
