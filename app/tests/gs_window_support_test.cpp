// DW5XL-TEST issue 049 (amendment v3.12, step 2): the Vulkan renderer's graphics device is made with window support
// where the system offers it, so that a presenter can show the renderer's pictures without reading them back.
// No game data.
//   1. What the system offers is asked of the Vulkan loader itself, not of the renderer: VK_KHR_surface and the
//      surface extension of a window system (Windows, X11 through xcb or Xlib, Wayland).
//   2. A renderer is made. Its device must have window support exactly when the loader offers both.
//   3. A second renderer of its own device gives the same answer, and so does a second GS memory on the first
//      renderer's device (it shares that device).
// Not covered: a graphics card that has no swap chain extension although its loader offers a window system's
// surfaces (the renderer then falls back to a device without window support, and check 2 would fail).
// Exit code 77: no Vulkan device (skipped).
#include "runtime/gs/gs_vulkan_backend.h"
#include "test_one_program.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

// VkExtensionProperties, written out so that the test needs no Vulkan header.
struct ExtensionProperties
{
    char name[256];
    uint32_t version;
};

// 1 = the loader offers VK_KHR_surface and a window system's surface extension, 0 = it does not, -1 = no loader.
int loaderOffersWindows()
{
    using Enumerate = int32_t (*)(const char *, uint32_t *, ExtensionProperties *);
    Enumerate enumerate = nullptr;
#ifdef _WIN32
    if (HMODULE loader = LoadLibraryA("vulkan-1.dll"))
        enumerate = reinterpret_cast<Enumerate>(
            reinterpret_cast<void *>(GetProcAddress(loader, "vkEnumerateInstanceExtensionProperties")));
#else
    void *loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader)
        loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (loader)
        enumerate = reinterpret_cast<Enumerate>(dlsym(loader, "vkEnumerateInstanceExtensionProperties"));
#endif
    if (!enumerate)
        return -1;
    uint32_t count = 0;
    if (enumerate(nullptr, &count, nullptr) != 0)
        return -1;
    std::vector<ExtensionProperties> list(count);
    if (count && enumerate(nullptr, &count, list.data()) < 0)
        return -1;
    bool surface = false, windowSystem = false;
    for (uint32_t i = 0; i < count; ++i)
    {
        const char *name = list[i].name;
        surface = surface || std::strcmp(name, "VK_KHR_surface") == 0;
        windowSystem = windowSystem || std::strcmp(name, "VK_KHR_win32_surface") == 0 ||
                       std::strcmp(name, "VK_KHR_xcb_surface") == 0 || std::strcmp(name, "VK_KHR_xlib_surface") == 0 ||
                       std::strcmp(name, "VK_KHR_wayland_surface") == 0;
    }
    return surface && windowSystem ? 1 : 0;
}
} // namespace

int DW5XL_TEST_MAIN(gs_window_support)()
{
    std::unique_ptr<GSVulkanBackend> renderer;
    try
    {
        renderer = std::make_unique<GSVulkanBackend>(); // throws without a Vulkan device
    }
    catch (const std::exception &error)
    {
        std::printf("SKIPPED: no Vulkan GS renderer here (%s)\n", error.what());
        return 77;
    }
    const int offered = loaderOffersWindows();
    check(offered >= 0, "the Vulkan loader answers what the system offers");
    if (offered < 0)
        return 1;
    std::printf("      the loader offers a window system's surfaces: %s\n", offered ? "yes" : "no");
    check(renderer->WindowSupport() == (offered == 1),
          offered ? "the renderer's device has window support (the system offers it)"
                  : "the renderer's device has no window support (the system offers none)");

    GSVulkanBackend other;
    check(other.WindowSupport() == renderer->WindowSupport(), "a second renderer gives the same answer");

    std::vector<uint8_t> vram(4u * 1024u * 1024u, 0u);
    renderer->Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
    std::unique_ptr<GSRasterBackend> second = renderer->MakeSecondMemory();
    auto *secondVulkan = dynamic_cast<GSVulkanBackend *>(second.get());
    check(secondVulkan != nullptr, "the first renderer makes a second GS memory on its device");
    if (secondVulkan)
        check(secondVulkan->WindowSupport() == renderer->WindowSupport(),
              "the second GS memory shares the device's answer");

    std::printf(g_failures ? "FAILED: %d check(s)\n" : "PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
