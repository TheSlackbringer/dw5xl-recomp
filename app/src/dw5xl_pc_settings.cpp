// Issue 023 step 3S: PC SETTINGS in DW5XL's own Options menu (owner-approved design, notes 023 spec 3S).
// The Options list's 4th entry, SCREEN ADJUST (a TV tool: picture position and brightness bars), becomes PC SETTINGS:
// - its word picture in the list and its screen title are rewritten in the game's own RAM copy of the menu pictures
//   (LINKDATA entry 41, unpacked by the game; pointer at 0x35B568) with "PC SETTINGS" built from the game's letters;
// - the game's own SCREEN ADJUST routine (0x2C86F0) still runs the screen (title, button bar, one frame per loop,
//   Back), but while PC SETTINGS is open its picture-position calls become menu navigation and its own drawing is
//   skipped; the rows are drawn over the game by the overlay.
// Only functions that never wait are replaced (a waiting EE thread unwinds the host stack to the dispatcher).
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "dw5xl_menu_assets.h"
#include "dw5xl_pc_settings.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {
using namespace dw5xl::menu;

constexpr uint32_t kMenuContainerPtr = 0x0035B568u;   // the unpacked Options menu file (gp - 0x7408)
constexpr uint32_t kListOpen = 0x002CB3A0u;           // Options list fade in (1) / out (0)
constexpr uint32_t kTitle = 0x002CBB10u;              // screen title in/out: (slot, 1 = in)
constexpr uint32_t kOffsetX = 0x001AD530u, kOffsetY = 0x001AD510u;   // SCREEN ADJUST: picture position get
constexpr uint32_t kOffsetSet = 0x001AD470u, kOffsetApply = 0x001AD430u;
constexpr uint32_t kAdjustDraw = 0x002C8FA0u;         // SCREEN ADJUST screen drawing (per frame)
constexpr uint32_t kAdjustGuides = 0x002C90C0u;       // SCREEN ADJUST picture-position guides (per frame)
constexpr uint32_t kButtonBar = 0x00264020u;          // button bar text (id, ...)
constexpr uint32_t kAdjustSlot = 3u;                  // SCREEN ADJUST's title slot argument
constexpr uint32_t kAdjustBar = 0x27u, kSettingsBar = 5u;   // "Default / Adjust / Back" -> "Select / Change / Back"
constexpr int kWordRow = 3;                           // SCREEN ADJUST's row in the word strip (sub-file 12)

PS2Runtime::RecompiledFunction g_original[9] = {};
enum Slot { ListOpen, Title, OffsetX, OffsetY, OffsetSet, OffsetApply, AdjustDraw, ButtonBar, AdjustGuides };

uint8_t *ptr(uint8_t *ram, uint32_t address) { return ram + (address & 0x01FFFFFFu); }
uint32_t rd32(uint8_t *ram, uint32_t address) { uint32_t v; std::memcpy(&v, ptr(ram, address), 4); return v; }
void ret(R5900Context *ctx, int32_t value) { setReturnS32(ctx, value); ctx->pc = getRegU32(ctx, 31); }

// Sub-file n of a container: u32 count, count sizes (16-byte units), then the sub-files (16-byte aligned).
bool subFile(uint8_t *ram, uint32_t container, uint32_t n, uint32_t &address, uint32_t &size)
{
    const uint32_t count = rd32(ram, container);
    if (count == 0u || count > 4096u || n >= count) return false;
    address = container + ((count + 4u) >> 2) * 16u;
    for (uint32_t i = 0; i < n; ++i) address += rd32(ram, container + 4u + 4u * i) * 16u;
    size = rd32(ram, container + 4u + 4u * n) * 16u;
    return size != 0u && (address & 0x01FFFFFFu) + size <= 0x02000000u;
}
const Tim2Picture *tim(uint8_t *ram, uint32_t address, uint32_t size, std::vector<Tim2Picture> &store)
{
    store = findTim2(ptr(ram, address), size);
    return store.empty() ? nullptr : &store.front();
}

// Letters of the word strip (sub-file 12) by character: (row, text) of the game's words.
const std::array<std::pair<int, const char *>, 4> kLetterRows{{{0, "SETTINGS"}, {1, "CONTROLS"}, {6, "OPENING"}, {3, "SCREEN ADJUST"}}};

// "PC SETTINGS" into row 3 of the word strip, in the game's own palette indices.
bool patchListWord(uint8_t *ram, uint32_t container)
{
    uint32_t a, n; std::vector<Tim2Picture> store;
    if (!subFile(ram, container, 12, a, n)) return false;
    const Tim2Picture *t = tim(ram, a, n, store);
    if (!t || t->w != 208 || t->h != 768 || t->imageType != 4) return false;
    const uint8_t *data = ptr(ram, a);
    const Image strip = decodeTim2(data, n, *t, 0);
    const std::vector<uint8_t> idx = tim2Indices(data, n, *t);
    if (strip.w == 0 || idx.empty()) return false;
    std::vector<Letter> letters[kLetterRows.size()];
    auto find = [&](char c, size_t skip = 0) -> const Letter * {
        for (size_t r = 0; r < kLetterRows.size(); ++r) {
            const char *text = kLetterRows[r].second;
            for (size_t i = 0, k = 0; text[i]; ++i) {
                if (text[i] == ' ') continue;
                if (text[i] == c && k < letters[r].size() && skip-- == 0) return &letters[r][k];
                ++k;
            }
        }
        return nullptr;
    };
    for (size_t r = 0; r < kLetterRows.size(); ++r) {
        const int y0 = kLetterRows[r].first * 24;
        const Image row = crop(strip, 0, y0, 208, 24);
        std::vector<uint8_t> rowIdx(size_t(208) * 24);
        for (int y = 0; y < 24; ++y) std::memcpy(&rowIdx[size_t(y) * 208], &idx[size_t(y0 + y) * 208], 208);
        size_t count = 0; for (const char *p = kLetterRows[r].second; *p; ++p) count += *p != ' ';
        letters[r] = splitLetters(row, count, &rowIdx);
    }
    // P from OPENING, C from CONTROLS, SETTINGS from SETTINGS (row 0: the letters in order)
    const Letter *P = find('P'), *C = find('C');
    if (!P || !C || letters[0].size() != 8) return false;
    std::vector<const Letter *> word{P, C, nullptr};
    for (const Letter &l : letters[0]) word.push_back(&l);
    std::vector<uint8_t> wordIdx;
    const Image built = buildWord(word, 9, 2, 4, &wordIdx);
    if (built.w > 208) return false;
    // write: clear the row to index 0 (transparent), then the word's indices
    uint8_t *pix = ptr(ram, a) + t->pixels;
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 208; ++x) {
            const size_t k = size_t(kWordRow * 24 + y) * 208 + x;
            uint8_t v = 0;
            if (x < built.w && wordIdx[size_t(y) * built.w + x] != 255) v = wordIdx[size_t(y) * built.w + x];
            pix[k / 2] = uint8_t((k & 1) ? ((pix[k / 2] & 0x0F) | (v << 4)) : ((pix[k / 2] & 0xF0) | v));
        }
    return true;
}

// "PC SETTINGS" title into the SCREEN ADJUST title picture (sub-file 7): solid letters from the OPTIONS (3),
// CONTROLS (5) and SETTINGS (4) titles, the game's glow regenerated, fitted to the picture's palette.
bool patchTitle(uint8_t *ram, uint32_t container)
{
    std::vector<Tim2Picture> s3, s4, s5, s7;
    uint32_t a[4], n[4]; const uint32_t files[4] = {3, 4, 5, 7};
    for (int i = 0; i < 4; ++i) if (!subFile(ram, container, files[i], a[i], n[i])) return false;
    const Tim2Picture *t3 = tim(ram, a[0], n[0], s3), *t4 = tim(ram, a[1], n[1], s4), *t5 = tim(ram, a[2], n[2], s5),
                      *t7 = tim(ram, a[3], n[3], s7);
    if (!t3 || !t4 || !t5 || !t7 || t7->w != 280 || t7->h != 40 || t7->imageType != 5) return false;
    const Image options = decodeTim2(ptr(ram, a[0]), n[0], *t3), settings = decodeTim2(ptr(ram, a[1]), n[1], *t4),
                controls = decodeTim2(ptr(ram, a[2]), n[2], *t5);
    // core column runs of a title (solid letters)
    auto runs = [](const Image &img) {
        std::vector<std::pair<int, int>> out; bool in = false; int start = 0;
        for (int x = 0; x <= img.w; ++x) {
            bool core = false;
            for (int y = 0; x < img.w && y < img.h && !core; ++y) core = img.at(x, y)[3] >= kCore;
            if (core && !in) { in = true; start = x; }
            if (!core && in) { in = false; out.push_back({start, x}); }
        }
        return out;
    };
    const auto ro = runs(options), rc = runs(controls), rs = runs(settings);
    if (ro.size() < 2 || rc.empty() || rs.empty()) return false;
    const Image P = crop(cores(options), ro[1].first, 0, ro[1].second - ro[1].first, 40);
    const Image C = crop(cores(controls), rc[0].first, 0, rc[0].second - rc[0].first, 40);
    const Image S = crop(cores(settings), rs.front().first, 0, rs.back().second - rs.front().first, 40);
    Image word(8 + P.w + 2 + C.w + 15 + S.w + 10, 40);
    auto paste = [&](const Image &src, int x0) { for (int y = 0; y < 40; ++y) std::memcpy(word.at(x0, y), src.at(0, y), size_t(src.w) * 4); };
    paste(P, 8); paste(C, 8 + P.w + 2); paste(S, 8 + P.w + 2 + C.w + 15);
    const Image glowed = applyGlow(word, learnGlow(settings));
    if (glowed.w > 280) return false;
    Image full(280, 40);
    for (int y = 0; y < 40; ++y) std::memcpy(full.at(0, y), glowed.at(0, y), size_t(glowed.w) * 4);
    const std::vector<uint8_t> q = quantize(full, ptr(ram, a[3]), *t7);
    if (q.size() != size_t(280) * 40) return false;
    std::memcpy(ptr(ram, a[3]) + t7->pixels, q.data(), q.size());
    return true;
}

// Patched once per loaded menu file; the marker is the patched row's first bytes.
uint32_t g_patchedContainer = 0;
std::array<uint8_t, 16> g_patchedMark{};
void listOpen(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, 4) == 1u) {
        const uint32_t container = rd32(ram, kMenuContainerPtr);
        uint32_t a = 0, n = 0; std::vector<Tim2Picture> store;
        const Tim2Picture *t = subFile(ram, container, 12, a, n) ? tim(ram, a, n, store) : nullptr;
        uint8_t *row = t ? ptr(ram, a) + t->pixels + size_t(kWordRow * 24 + 8) * 104 : nullptr;
        if (row && (container != g_patchedContainer || std::memcmp(row, g_patchedMark.data(), 16) != 0)) {
            const bool word = patchListWord(ram, container), title = word && patchTitle(ram, container);
            std::fprintf(stderr, "[dw5xl:pc-settings] menu pictures at 0x%08x: list entry %s, title %s\n", container,
                         word ? "PC SETTINGS" : "unchanged", title ? "PC SETTINGS" : "unchanged");
            g_patchedContainer = container;
            std::memcpy(g_patchedMark.data(), row, 16);
        }
    }
    g_original[ListOpen](ram, ctx, runtime);
}

// --- the screen: the game's SCREEN ADJUST routine runs it --------------------------------------------------------
void title(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, 4) == kAdjustSlot) dw5xl::pcsettings::setOpen(getRegU32(ctx, 5) != 0u);
    g_original[Title](ram, ctx, runtime);
}
void offsetX(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (dw5xl::pcsettings::isOpen()) return ret(ctx, 0);
    g_original[OffsetX](ram, ctx, runtime);
}
void offsetY(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (dw5xl::pcsettings::isOpen()) return ret(ctx, 0);
    g_original[OffsetY](ram, ctx, runtime);
}
// The routine asks for (x, y) = its position +-2 per direction (position read as 0 here), (0, 0) for Start.
void offsetSet(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!dw5xl::pcsettings::isOpen()) return g_original[OffsetSet](ram, ctx, runtime);
    const int32_t x = int32_t(getRegU32(ctx, 4)), y = int32_t(getRegU32(ctx, 5));
    using dw5xl::pcsettings::Input;
    dw5xl::pcsettings::input(y < 0 ? Input::Up : y > 0 ? Input::Down : x < 0 ? Input::Left : x > 0 ? Input::Right : Input::Default);
    ret(ctx, 0);
}
void offsetApply(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (dw5xl::pcsettings::isOpen()) return ret(ctx, 0);
    g_original[OffsetApply](ram, ctx, runtime);
}
void adjustDraw(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (dw5xl::pcsettings::isOpen()) return ret(ctx, 0);
    g_original[AdjustDraw](ram, ctx, runtime);
}
void adjustGuides(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (dw5xl::pcsettings::isOpen()) return ret(ctx, 0);
    g_original[AdjustGuides](ram, ctx, runtime);
}
void buttonBar(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, 4) == kAdjustBar && dw5xl::pcsettings::isOpen()) ctx->r[4] = _mm_set_epi64x(0, int64_t(kSettingsBar));
    g_original[ButtonBar](ram, ctx, runtime);
}

void install(PS2Runtime &runtime)
{
    struct Hook { uint32_t address; std::array<uint32_t, 3> code; PS2Runtime::RecompiledFunction fn; };
    const std::array<Hook, 9> hooks{{
        {kListOpen, {0x27bdffd0u, 0xffbf0020u, 0x7fb10010u}, &listOpen},
        {kTitle, {0x27bdffd0u, 0xffbf0020u, 0x7fb10010u}, &title},
        {kOffsetX, {0x27bdfff0u, 0xffbf0000u, 0x0c06b184u}, &offsetX},
        {kOffsetY, {0x27bdfff0u, 0xffbf0000u, 0x0c06b184u}, &offsetY},
        {kOffsetSet, {0x27bdffd0u, 0xffbf0020u, 0x7fb10010u}, &offsetSet},
        {kOffsetApply, {0x27bdffe0u, 0xffbf0010u, 0x0c06b184u}, &offsetApply},
        {kAdjustDraw, {0x27bdffb0u, 0xffbf0040u, 0x7fb30030u}, &adjustDraw},
        {kButtonBar, {0x27bdffb0u, 0x3c020032u, 0xffbf0030u}, &buttonBar},
        {kAdjustGuides, {0x27bdffb0u, 0xffbf0040u, 0x7fb30030u}, &adjustGuides},
    }};
    for (const Hook &h : hooks)
        for (int i = 0; i < 3; ++i)
            if (runtime.memory().read32(h.address + 4u * i) != h.code[i] || !runtime.hasFunction(h.address)) {
                std::fprintf(stderr, "[dw5xl:pc-settings] declined: unexpected code at 0x%08x\n", h.address);
                return;
            }
    for (size_t i = 0; i < hooks.size(); ++i) {
        const auto current = runtime.lookupFunction(hooks[i].address);
        if (current != hooks[i].fn) g_original[i] = current;
        if (!g_original[i]) return;
    }
    for (const Hook &h : hooks) runtime.replaceFunction(h.address, h.fn);
    std::fprintf(stderr, "[dw5xl:pc-settings] installed (Options: SCREEN ADJUST -> PC SETTINGS)\n");
}
}

namespace dw5xl::pcsettings {
namespace {
std::mutex g_mutex;
Snapshot g_state;                                  // guarded by g_mutex
std::atomic<bool> g_open{false};
constexpr int kValueCount[kRows] = {2, 3, 5, 2, 0};
}
// DW5XL-DEBUG issue 023 3S: PS2X_PCSETTINGS_TRACE=1 logs opening, closing and every navigation input.
bool trace() { static const bool on = [] { const char *v = std::getenv("PS2X_PCSETTINGS_TRACE"); return v && *v == '1'; }(); return on; }
void setOpen(bool open)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (trace()) std::fprintf(stderr, "[pc-settings] %s\n", open ? "open" : "close");
    if (open && !g_state.open) { g_state.row = 0; g_state.needsInit = true; }
    g_state.open = open; g_open.store(open, std::memory_order_release);
    ++g_state.version;
}
bool isOpen() { return g_open.load(std::memory_order_acquire); }
void input(Input in)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    Snapshot &s = g_state;
    int *value[kRows] = {&s.values.vsync, &s.values.resolution, &s.values.antiAliasing, &s.values.display, nullptr};
    switch (in) {
    case Input::Up: do s.row = (s.row + kRows - 1) % kRows; while (kValueCount[s.row] == 0 && s.row != kDefaultRow); break;
    case Input::Down: do s.row = (s.row + 1) % kRows; while (kValueCount[s.row] == 0 && s.row != kDefaultRow); break;
    case Input::Left: if (value[s.row] && *value[s.row] > 0) --*value[s.row], s.changed = true; break;
    case Input::Right: if (value[s.row] && *value[s.row] + 1 < kValueCount[s.row]) ++*value[s.row], s.changed = true; break;
    case Input::Default: s.values = Values{}; s.changed = true; break;
    }
    ++s.version;
    if (trace())
        std::fprintf(stderr, "[pc-settings] input %d -> row %d values vsync=%d res=%d aa=%d display=%d\n", int(in), s.row,
                     s.values.vsync, s.values.resolution, s.values.antiAliasing, s.values.display);
}
Snapshot snapshot() { std::lock_guard<std::mutex> lock(g_mutex); return g_state; }
void setValues(const Values &v) { std::lock_guard<std::mutex> lock(g_mutex); g_state.values = v; g_state.needsInit = false; ++g_state.version; }
void clearChanged() { std::lock_guard<std::mutex> lock(g_mutex); g_state.changed = false; }
}

PS2_REGISTER_GAME_OVERRIDE("DW5XL PC SETTINGS in the Options menu", "SLES_538.60", 0u, 0u, &install)
