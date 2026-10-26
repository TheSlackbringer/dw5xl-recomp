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
#include "imgui.h"
#include "raylib.h"
#include "runtime/ps2_settings.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>

namespace {
using namespace dw5xl::menu;
using MImage = dw5xl::menu::Image;

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
    const MImage strip = decodeTim2(data, n, *t, 0);
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
        const MImage row = crop(strip, 0, y0, 208, 24);
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
    const MImage built = buildWord(word, 9, 2, 4, &wordIdx);
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
    const MImage options = decodeTim2(ptr(ram, a[0]), n[0], *t3), settings = decodeTim2(ptr(ram, a[1]), n[1], *t4),
                controls = decodeTim2(ptr(ram, a[2]), n[2], *t5);
    // core column runs of a title (solid letters)
    auto runs = [](const MImage &img) {
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
    const MImage P = crop(cores(options), ro[1].first, 0, ro[1].second - ro[1].first, 40);
    const MImage C = crop(cores(controls), rc[0].first, 0, rc[0].second - rc[0].first, 40);
    const MImage S = crop(cores(settings), rs.front().first, 0, rs.back().second - rs.front().first, 40);
    MImage word(8 + P.w + 2 + C.w + 15 + S.w + 10, 40);
    auto paste = [&](const MImage &src, int x0) { for (int y = 0; y < 40; ++y) std::memcpy(word.at(x0, y), src.at(0, y), size_t(src.w) * 4); };
    paste(P, 8); paste(C, 8 + P.w + 2); paste(S, 8 + P.w + 2 + C.w + 15);
    const MImage glowed = applyGlow(word, learnGlow(settings));
    if (glowed.w > 280) return false;
    MImage full(280, 40);
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

PS2Runtime *g_runtime = nullptr;

void install(PS2Runtime &runtime)
{
    g_runtime = &runtime;
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


// --- the rows, drawn over the game by the overlay (window thread) -----------------------------------------------
namespace {
using namespace dw5xl::menu;
using MImage = dw5xl::menu::Image;
using dw5xl::pcsettings::kRows;
using dw5xl::pcsettings::kDefaultRow;

// The menu font's words in the word strip (sub-file 12): row -> text, for finding letters.
const std::array<std::pair<int, const char *>, 30> kWords{{{0, "SETTINGS"}, {1, "CONTROLS"}, {2, "SOUND"}, {3, "SCREEN ADJUST"},
    {4, "SAVE/LOAD"}, {5, "MOVIES"}, {6, "OPENING"}, {8, "EVENTS"}, {9, "DEFAULT"}, {10, "1P CONTROLS"}, {11, "2P CONTROLS"},
    {12, "VIBRATION"}, {13, "BOW CONTROL"}, {14, "SETUP"}, {15, "SYSTEM"}, {16, "BGM VOLUME"}, {17, "SE VOLUME"},
    {18, "SOUND TEST"}, {19, "SAVE"}, {20, "LOAD"}, {22, "ENDING - WEI"}, {23, "ENDING - WU"}, {24, "ENDING - SHU"},
    {25, "ENDING - OTHER"}, {26, "OTHER"}, {27, "OPENING EDIT"}, {28, "VARIATIONS"}, {29, "REPEAT"}, {30, "PLAY"}, {31, "MAP EVENT"}}};
const char *kLabels[4] = {"VSYNC", "RESOLUTION", "ANTI-ALIASING", "DISPLAY"};
const char *kHelp[kRows] = {"Waits for each display refresh: no tearing.", "Internal resolution of the picture.",
                            "Smooths the edges of the picture.", "Plays in a window or fullscreen.",
                            "Restores the default settings."};
constexpr uint32_t kScales[3] = {1, 2, 4}, kSamples[5] = {1, 2, 4, 8, 16};

struct Tex { Texture2D t{}; int w = 0, h = 0; };      // w, h: the picture's size in its source texels
struct Assets {
    bool tried = false, ok = false;
    Tex label[4], labelDefault, onOff[2][2];          // onOff[ON/OFF][white/yellow]
    Tex word[2][2];                                   // WINDOW / FULLSCREEN, white / yellow
    Tex number[2][5][2];                              // [resolution / anti-aliasing][value][white / yellow]
    Tex glow, help[kRows];
    uint8_t line[4] = {};                             // separator colour (already tinted as the game draws it)
} g_assets;

Tex upload(const MImage &img)
{
    Tex t; t.w = img.w; t.h = img.h;
    ::Image ri{const_cast<uint8_t *>(img.rgba.data()), img.w, img.h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    t.t = LoadTextureFromImage(ri);
    SetTextureFilter(t.t, TEXTURE_FILTER_BILINEAR);
    return t;
}
MImage tinted(MImage img, const float k[3]) { for (size_t i = 0; i < img.rgba.size(); i += 4) for (int c = 0; c < 3; ++c) img.rgba[i + c] = uint8_t(std::min(255.0f, img.rgba[i + c] * k[c])); return img; }
std::vector<uint8_t> readFile(const std::filesystem::path &p, uint64_t offset = 0, uint64_t size = 0)
{
    std::ifstream f(p, std::ios::binary); if (!f) return {};
    if (!size) { f.seekg(0, std::ios::end); size = uint64_t(f.tellg()) - offset; }
    std::vector<uint8_t> out(size); f.seekg(std::streamoff(offset)); f.read(reinterpret_cast<char *>(out.data()), std::streamsize(size));
    return f ? out : std::vector<uint8_t>{};
}

void buildAssets(uint8_t *ram)
{
    Assets &A = g_assets; A.tried = true;
    const uint32_t container = rd32(ram, kMenuContainerPtr);
    uint32_t a12, n12, a13, n13; std::vector<Tim2Picture> s12, s13;
    if (!subFile(ram, container, 12, a12, n12) || !subFile(ram, container, 13, a13, n13)) return;
    const Tim2Picture *t12 = tim(ram, a12, n12, s12), *t13 = tim(ram, a13, n13, s13);
    if (!t12 || !t13 || t13->colors < 32) return;
    // letters of every word of the strip (shape-split), and the label words built from them
    const MImage strip = decodeTim2(ptr(ram, a12), n12, *t12, 0);
    std::vector<std::pair<char, Letter>> letters;
    for (const auto &[row, text] : kWords) {
        size_t count = 0; for (const char *p = text; *p; ++p) count += *p != ' ';
        const auto split = splitLetters(crop(strip, 0, row * 24, 208, 24), count);
        size_t k = 0;
        for (const char *p = text; *p && k < split.size(); ++p) if (*p != ' ') letters.push_back({*p, split[k++]});
    }
    auto word = [&](const char *text) {
        std::vector<const Letter *> ls;
        for (const char *p = text; *p; ++p) {
            if (*p == ' ') { ls.push_back(nullptr); continue; }
            const Letter *found = nullptr;
            for (const auto &[c, l] : letters) if (c == *p) { found = &l; break; }
            if (!found) return MImage{};
            ls.push_back(found);
        }
        return buildWord(ls, 9, 2, 4);
    };
    for (int i = 0; i < 4; ++i) { const MImage w = word(kLabels[i]); if (!w.w) return; A.label[i] = upload(w); }
    A.labelDefault = upload(crop(strip, 0, 9 * 24, 208, 24));
    // ON / OFF in the value strip's two palettes; which palette is yellow decides "current"
    const MImage v0 = decodeTim2(ptr(ram, a13), n13, *t13, 0), v1 = decodeTim2(ptr(ram, a13), n13, *t13, 1);
    auto yellowness = [](const MImage &img) { long s = 0; for (size_t i = 0; i < img.rgba.size(); i += 4) if (img.rgba[i + 3] > 200) s += img.rgba[i] - img.rgba[i + 2]; return s; };
    const bool zeroIsYellow = yellowness(v0) > yellowness(v1);
    const MImage &white = zeroIsYellow ? v1 : v0, &yellow = zeroIsYellow ? v0 : v1;
    auto inked = [](const MImage &cell) {          // the word's ink columns, with the outline
        int x0 = cell.w, x1 = -1;
        for (int y = 0; y < cell.h; ++y) for (int x = 0; x < cell.w; ++x) if (cell.at(x, y)[3] > 10) { x0 = std::min(x0, x); x1 = std::max(x1, x); }
        return x1 < x0 ? cell : crop(cell, x0, 0, x1 - x0 + 1, cell.h);
    };
    for (int v = 0; v < 2; ++v) {
        A.onOff[v][0] = upload(inked(crop(white, 0, v * 24, 80, 24)));
        A.onOff[v][1] = upload(inked(crop(yellow, 0, v * 24, 80, 24)));
    }
    // yellow as the game shows a current value: white letters times (yellow / white) of the value strip
    double ys[3] = {}, ws[3] = {}; int yn = 0, wn = 0;
    for (size_t i = 0; i < yellow.rgba.size(); i += 4) if (yellow.rgba[i + 3] > 200) { for (int c = 0; c < 3; ++c) ys[c] += yellow.rgba[i + c]; ++yn; }
    for (size_t i = 0; i < white.rgba.size(); i += 4) if (white.rgba[i + 3] > 200) { for (int c = 0; c < 3; ++c) ws[c] += white.rgba[i + c]; ++wn; }
    float k[3]; for (int c = 0; c < 3; ++c) k[c] = float((ys[c] / std::max(yn, 1)) / std::max(1.0, ws[c] / std::max(wn, 1)));
    const char *words[2] = {"WINDOW", "FULLSCREEN"};
    for (int i = 0; i < 2; ++i) { const MImage w = word(words[i]); if (!w.w) return; A.word[i][0] = upload(w); A.word[i][1] = upload(tinted(w, k)); }
    // the number font, the separator line and the glow: the boot pictures (LINKDATA entry 0, not packed)
    const auto dir = PS2Runtime::getIoPaths().elfDirectory;
    const auto idxFile = readFile(dir / "LINKDATA.IDX");
    const auto idx = parseLinkIndex(idxFile.data(), idxFile.size());
    if (idx.empty()) return;
    const auto boot = readFile(dir / "LINKDATA.BIN", idx[0].offset(), idx[0].size);
    const auto bootPics = findTim2(boot.data(), boot.size());
    if (bootPics.empty() || bootPics[0].w != 512 || bootPics[0].h != 512) return;
    const MImage digits = decodeTim2(boot.data(), boot.size(), bootPics[0], 0);
    auto numberText = [&](const std::string &text) {                 // 16x16 cells, 8 per row from ' '
        MImage out(int(text.size()) * 16, 16);
        for (size_t i = 0; i < text.size(); ++i) {
            const int c = text[i] - 32; const MImage cell = crop(digits, (c % 8) * 16, (c / 8) * 16, 16, 16);
            for (int y = 0; y < 16; ++y) std::memcpy(out.at(int(i) * 16, y), cell.at(0, y), 16 * 4);
        }
        return out;
    };
    const char *res[3] = {"640X512", "1280X1024", "2560X2048"}, *aa[5] = {"OFF", "2X", "4X", "8X", "16X"};
    for (int v = 0; v < 3; ++v) { const MImage n = numberText(res[v]); A.number[0][v][0] = upload(n); A.number[0][v][1] = upload(tinted(n, k)); }
    for (int v = 1; v < 5; ++v) { const MImage n = numberText(aa[v]); A.number[1][v][0] = upload(n); A.number[1][v][1] = upload(tinted(n, k)); }
    const MImage lines = decodeTim2(boot.data(), boot.size(), bootPics[0], 5);
    std::memcpy(A.line, lines.at(194, 305), 4);
    A.line[0] = uint8_t(A.line[0] * 112 / 128); A.line[1] = uint8_t(A.line[1] * 112 / 128); A.line[2] = uint8_t(A.line[2] * 112 / 128);
    A.line[3] = uint8_t(A.line[3] * 104 / 128);
    A.glow = upload(crop(decodeTim2(boot.data(), boot.size(), bootPics[0], 34), 480, 448, 32, 32));
    // the help lines in the small font of the executable (glyphs from '!' at 0x216230, palette 0x215F90)
    const auto exe = readFile(PS2Runtime::getIoPaths().elfPath);
    if (exe.size() < 0x216230 + 95 * 160) return;
    for (int r = 0; r < kRows; ++r) {
        std::vector<MImage> glyphs; std::vector<int> pens; int pen = 0;
        for (const char *p = kHelp[r]; *p; ++p) {
            if (*p == ' ' || *p < 33 || *p > 126) { pen += 7; continue; }        // 5 game pixels at 0.75 per texel
            const MImage g = decodeGlyph(exe.data() + 0x216230 + size_t(*p - 33) * 160, exe.data() + 0x215F90);
            int x0 = 16, x1 = -1;
            for (int y = 0; y < 20; ++y) for (int x = 0; x < 16; ++x) if (g.at(x, y)[3] > 40) { x0 = std::min(x0, x); x1 = std::max(x1, x); }
            if (x1 < x0) { pen += 7; continue; }
            glyphs.push_back(crop(g, x0, 0, x1 - x0 + 1, 20)); pens.push_back(pen); pen += x1 - x0 + 1 + 2;
        }
        MImage line(std::max(pen, 1), 20);
        for (size_t i = 0; i < glyphs.size(); ++i)
            for (int y = 0; y < 20; ++y) for (int x = 0; x < glyphs[i].w; ++x) {
                const uint8_t *src = glyphs[i].at(x, y); uint8_t *dst = line.at(pens[i] + x, y);
                if (src[3] > dst[3]) std::memcpy(dst, src, 4);
            }
        A.help[r] = upload(line);
    }
    A.ok = true;
}

// game picture coordinates (512 x 448) -> window
struct Map { float x, y, sx, sy; ImVec2 at(float gx, float gy) const { return ImVec2(x + gx * sx, y + gy * sy); } };
void image(ImDrawList *dl, const Map &m, const Tex &t, float gx, float gy, float gw, float gh, ImU32 tint = IM_COL32_WHITE)
{
    if (!t.t.id) return;
    dl->AddImage(ImTextureID(t.t.id), m.at(gx, gy), m.at(gx + gw, gy + gh), ImVec2(0, 0), ImVec2(1, 1), tint);
}
void separator(ImDrawList *dl, const Map &m, float gy, float fullEnd, float fadeEnd)
{
    const uint8_t *c = g_assets.line;
    const ImU32 on = IM_COL32(c[0], c[1], c[2], std::min(255, c[3] * 2)), off = IM_COL32(c[0], c[1], c[2], 0);
    dl->AddRectFilled(m.at(0, gy), m.at(fullEnd, gy + 3), on);
    dl->AddRectFilledMultiColor(m.at(fullEnd, gy), m.at(fadeEnd, gy + 3), on, off, off, on);
}

void applyChanges(const dw5xl::pcsettings::Snapshot &s)
{
    using namespace dw5xl::pcsettings;
    GS &gs = g_runtime->gs();
    if (gs.getResolutionScale() != kScales[s.values.resolution]) ps2x::settings::setResolutionScale(gs, kScales[s.values.resolution]);
    if (gs.getAASamples() != kSamples[s.values.antiAliasing]) ps2x::settings::setAntiAliasing(gs, kSamples[s.values.antiAliasing]);
    const bool vsync = s.values.vsync == 0;
    if (vsync != IsWindowState(FLAG_VSYNC_HINT)) { if (vsync) SetWindowState(FLAG_VSYNC_HINT); else ClearWindowState(FLAG_VSYNC_HINT); }
    ps2x::settings::saveText("vsync", vsync ? "on" : "off");
    const bool full = s.values.display == 1, isFull = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
    if (full != isFull && !IsWindowHidden()) ToggleBorderlessWindowed();
    ps2x::settings::saveText("display", full ? "fullscreen" : "window");
    clearChanged();
}
}

namespace dw5xl::pcsettings {
bool overlayWanted() { return isOpen() && g_runtime; }
void drawOverlay()
{
    Snapshot s = snapshot();
    if (!s.open || !g_runtime) return;
    uint8_t *ram = g_runtime->memory().getRDRAM();
    if (s.needsInit) {
        Values v;
        const uint32_t scale = g_runtime->gs().getResolutionScale(), samples = g_runtime->gs().getAASamples();
        v.vsync = IsWindowState(FLAG_VSYNC_HINT) ? 0 : 1;
        v.resolution = scale >= 4 ? 2 : scale >= 2 ? 1 : 0;
        v.antiAliasing = samples >= 16 ? 4 : samples >= 8 ? 3 : samples >= 4 ? 2 : samples >= 2 ? 1 : 0;
        v.display = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) ? 1 : 0;
        setValues(v);
        s = snapshot();
    }
    if (s.changed) applyChanges(s);
    // Cross on DEFAULT (the game's routine does not use Cross)
    if (s.row == kDefaultRow && (IsKeyPressed(KEY_X) || IsKeyPressed(KEY_SPACE) || IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)))
        input(Input::Default);
    if (!g_assets.tried) buildAssets(ram);
    if (!g_assets.ok) return;
    float rx, ry, rw, rh; g_runtime->pictureRect(rx, ry, rw, rh);
    if (rw <= 0 || rh <= 0) return;
    const Map m{rx, ry, rw / 512.0f, rh / 448.0f};
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    const Assets &A = g_assets;
    const int screenRow[kRows] = {0, 1, 2, 3, 5};
    const float rowY = 100.0f + 34.0f * screenRow[s.row];
    image(dl, m, A.glow, 0, rowY - 2, 144, 32, IM_COL32(255, 208, 96, 128));
    int labelW = 0; for (const Tex &t : A.label) labelW = std::max(labelW, t.w);
    const float valueX = std::max(184.0f, 24.0f - 4.0f + float(labelW) + 14.0f);
    const int values[4] = {s.values.vsync, s.values.resolution, s.values.antiAliasing, s.values.display};
    for (int r = 0; r < kRows; ++r) {
        const float y = 100.0f + 34.0f * screenRow[r];
        const ImU32 labelTint = r == s.row ? IM_COL32_WHITE : IM_COL32(160, 160, 160, 255);
        if (r == kDefaultRow) { image(dl, m, A.labelDefault, 24, y, 208, 28, labelTint); separator(dl, m, y + 27, 136, 184); continue; }
        image(dl, m, A.label[r], 24 - 4, y, float(A.label[r].w), 28, labelTint);
        separator(dl, m, y + 27, 448, 496);
        float x = valueX;
        const int count = r == 0 ? 2 : r == 1 ? 3 : r == 2 ? 5 : 2;
        for (int v = 0; v < count; ++v) {
            const int cur = v == values[r] ? 1 : 0;
            if (r == 0 || (r == 2 && v == 0)) {                       // ON / OFF pictures (OFF also for anti-aliasing)
                const Tex &t = A.onOff[r == 0 ? v : 1][cur];
                image(dl, m, t, x, y, float(t.w), 28); x += float(t.w) + 12;
            } else if (r == 3) {
                const Tex &t = A.word[v][cur];
                image(dl, m, t, x - 4, y, float(t.w), 28); x += float(t.w) - 8 + 12;   // built words carry a 4-texel margin
            } else {
                const Tex &t = A.number[r == 1 ? 0 : 1][v][cur];
                const float cw = r == 1 ? 10.0f : 12.0f, w = cw * float(t.w) / 16.0f;
                image(dl, m, t, x, y + 4, w, 20); x += w + 12;
            }
        }
    }
    image(dl, m, A.help[s.row], 24, 372, float(A.help[s.row].w) * 0.75f, 20);
}
}

PS2_REGISTER_GAME_OVERRIDE("DW5XL PC SETTINGS in the Options menu", "SLES_538.60", 0u, 0u, &install)
