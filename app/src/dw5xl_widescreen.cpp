// Issue 047: widescreen. Native overrides around the game's camera, view volume and 2D queue; see dw5xl_widescreen.h.
#include "dw5xl_widescreen.h"
#include "dw5xl_gif_squeeze.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
using dw5xl::widescreen::Aspect;
constexpr uint32_t kCamera = 0x001469D0u;        // rebuilds one camera's matrices
constexpr uint32_t kViewAngle = 0x001474E0u;     // the view angle for the view volume FUN_00198880, its only caller
constexpr uint32_t kScene = 0x001C4320u;         // draws one battle picture: 3D, then FUN_0013ED70, HUD, fades
constexpr uint32_t k2dStart = 0x0013ED70u, kFades = 0x001C8B70u;
constexpr uint32_t k2dStartReturn = 0x001C4420u, kFadesReturn = 0x001C4438u;   // their calls in kScene
constexpr uint32_t kPause = 0x001C51C0u;         // the battle's pause menu, a full-screen 2D screen with its own loop
constexpr uint32_t kBattleEnd = 0x00141990u, kBattleEndReturn = 0x001C42A8u;   // its call when the battle loop ends
constexpr uint32_t kQueue = 0x00113478u;         // adds a ready packet to the display list
// Each camera's matrices, 0x40 apart (written by kCamera).
constexpr uint32_t kViewport = 0x004740E0u, kProjection = 0x004743E0u, kScreen = 0x00474560u;
constexpr uint32_t kGsRangeClip = 0x00473F60u, kWideClip = 0x00473DE0u;
constexpr uint32_t kBufferIndex = 0x70003F80u;   // which of the two display lists the game is writing
constexpr uint32_t kAllocate = 0x0013EB70u, kFree = 0x0013EA50u;   // the game's heap: (alignment, size) and (block)
constexpr uint32_t kHeapRoot = 0x0036B160u;      // its first block header: +4 next, +8 the block, +12 size; 0 = free block
constexpr uint32_t kCopyBytes = 128u * 1024u;    // room for one picture's HUD packets
constexpr uint32_t kCopiesHeader = 64u;          // the block starts with kCopiesMark, so a loaded state's block is found
constexpr char kCopiesMark[8] = {'D', 'W', '5', 'W', 'I', 'D', 'E', '1'};
constexpr int kCameras = 6;
constexpr uint32_t kCameraAngles = 0x004737B8u;  // two floats averaged to the view angle in degrees, 0x100 per camera
constexpr uint32_t kStageObjectCheck = 0x0019C140u, kDrawCharacter = 0x001A8210u;   // DW5XL-DEBUG: what PS2X_WIDESCREEN_TRACE counts
constexpr uint32_t kMathConstants[2] = {0x00330180u, 0x003301D8u};                  // DW5XL-DEBUG: the view must not change these

constexpr float kFactors[dw5xl::widescreen::kAspects] = {1.0f, 0.75f, 0.5625f};
constexpr float kShapes[dw5xl::widescreen::kAspects] = {4.0f / 3.0f, 16.0f / 9.0f, 21.0f / 9.0f};

std::atomic<int> g_aspect{dw5xl::widescreen::Aspect4x3}, g_fov{dw5xl::widescreen::kFovDefault};
PS2Runtime *g_runtime = nullptr;
PS2Runtime::RecompiledFunction originalCamera, originalViewAngle, originalScene, original2dStart, originalFades, originalPause, originalBattleEnd, originalQueue, allocate, release;
bool g_scene = false;                            // inside the battle loop: 3D is widened and the picture is wide
bool g_hud = false;                              // from the game's 2D start to its fades
int g_sceneAspect = dw5xl::widescreen::Aspect4x3, g_sceneFov = dw5xl::widescreen::kFovDefault;   // the settings of the picture being drawn
uint32_t g_copies = 0;                           // a game heap block for the squeezed copies; two lists; battle long
bool g_searchCopies = false;                     // a state was loaded: its heap may hold a block of ours
uint32_t g_used = 0;
uint64_t g_stateLoads = 0;
bool g_loggedCamera = false, g_loggedFull = false, g_loggedMemory = false;
// DW5XL-DEBUG: PS2X_WIDESCREEN_TRACE logs each battle picture: stage objects and characters drawn, camera 0's view.
bool g_trace = false;
PS2Runtime::RecompiledFunction originalStageObjectCheck, originalDrawCharacter;
int g_stageObjects = 0, g_characters = 0;
float g_gameAngle = 0.0f, g_builtAngle = 0.0f, g_yScale = 0.0f, g_volumeAngle = 0.0f;

void scaleX(uint8_t *matrix, float factor)
{
    for (int column = 0; column < 4; ++column) {
        float value;
        std::memcpy(&value, matrix + column * 16, 4);
        value *= factor;
        std::memcpy(matrix + column * 16, &value, 4);
    }
}
float readFloat(const uint8_t *at) { float value; std::memcpy(&value, at, 4); return value; }
uint32_t readWord(const uint8_t *ram, uint32_t address) { uint32_t value; std::memcpy(&value, ram + address, 4); return value; }

void widen(uint8_t *ram, int camera, float factor)
{
    const uint32_t offset = uint32_t(camera) * 0x40u;
    uint8_t *projection = ram + kProjection + offset, *screen = ram + kScreen + offset;
    // For the log: the matrix also holds the camera's turn, so take the x row's largest value.
    int largest = 0;
    for (int column = 1; column < 4; ++column)
        if (std::fabs(readFloat(projection + column * 16)) > std::fabs(readFloat(projection + largest * 16))) largest = column;
    const float wasX = readFloat(projection + largest * 16), y = readFloat(projection + largest * 16 + 4);
    scaleX(projection, factor);
    // screen = viewport * projection: only the viewport's scale part narrows, not its centre.
    const float centreX = readFloat(ram + kViewport + offset + 0x30);
    for (int column = 0; column < 4; ++column) {
        const float x = readFloat(screen + column * 16), w = readFloat(screen + column * 16 + 12);
        const float narrowed = factor * (x - centreX * w) + centreX * w;
        std::memcpy(screen + column * 16, &narrowed, 4);
    }
    scaleX(ram + kGsRangeClip + offset, factor);
    scaleX(ram + kWideClip + offset, factor);
    if (camera == 0 && !g_loggedCamera) {
        g_loggedCamera = true;
        std::fprintf(stderr, "[widescreen] cam=0 x=%.6f was=%.6f y=%.6f was=%.6f\n", readFloat(projection + largest * 16), wasX,
                     readFloat(projection + largest * 16 + 4), y);
    }
}

// The player's FOV: the battle camera and the two-player cameras. Cut-scene (4) and other cameras keep Koei's framing.
bool usesFov(int cam) { return g_scene && g_sceneFov != dw5xl::widescreen::kFovDefault && cam >= 0 && cam <= 2; }

void camera(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int cam = int(getRegU32(ctx, 4));
    const bool fov = usesFov(cam);
    float angles[2];
    uint8_t *angleData = ram + kCameraAngles + (cam >= 0 && cam < kCameras ? uint32_t(cam) * 0x100u : 0u);
    if (fov) {                                         // only while the matrices are built: the game's value stays
        std::memcpy(angles, angleData, sizeof(angles));
        const float wider[2] = {angles[0] * float(g_sceneFov) / 100.0f, angles[1] * float(g_sceneFov) / 100.0f};
        std::memcpy(angleData, wider, sizeof(wider));
    }
    originalCamera(ram, ctx, runtime);
    if (g_trace && cam == 0) {
        const float game0 = readFloat(angleData), game1 = readFloat(angleData + 4);
        g_builtAngle = (game0 + game1) * 0.5f;
        g_gameAngle = fov ? (readFloat(reinterpret_cast<const uint8_t *>(angles)) + readFloat(reinterpret_cast<const uint8_t *>(angles) + 4)) * 0.5f : g_builtAngle;
        g_yScale = 0.0f;                               // the y row's length: the camera's turn leaves it alone
        for (int column = 0; column < 3; ++column) {
            const float y = readFloat(ram + kProjection + column * 16 + 4);
            g_yScale += y * y;
        }
        g_yScale = std::sqrt(g_yScale);
    }
    if (fov) std::memcpy(angleData, angles, sizeof(angles));
    if (g_scene && g_sceneAspect != dw5xl::widescreen::Aspect4x3 && cam >= 0 && cam < kCameras)
        widen(ram, cam, kFactors[g_sceneAspect]);
}

void viewAngle(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int cam = int(getRegU32(ctx, 4));
    originalViewAngle(ram, ctx, runtime);
    if (usesFov(cam)) ctx->f[0] *= float(g_sceneFov) / 100.0f;
    if (g_trace && cam == 0) g_volumeAngle = ctx->f[0];
    if (g_scene && g_sceneAspect != dw5xl::widescreen::Aspect4x3)      // the wider half-angle, in x only
        ctx->f[0] = 2.0f * std::atan(std::tan(ctx->f[0] * 0.5f) / kFactors[g_sceneAspect]);
}

// The loaded game may be in a menu, and its heap is the state's. A loaded battle comes back with its next picture.
void afterStateLoad(PS2Runtime *runtime)
{
    if (runtime->stateLoadCount() == g_stateLoads) return;
    g_stateLoads = runtime->stateLoadCount();
    g_scene = g_hud = false;
    g_copies = 0;
    g_searchCopies = true;
}

void scene(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    afterStateLoad(runtime);
    g_scene = true;
    const int aspect = g_aspect.load(std::memory_order_relaxed), fov = g_fov.load(std::memory_order_relaxed);
    if (aspect != g_sceneAspect || fov != g_sceneFov) {
        g_sceneAspect = aspect;
        g_sceneFov = fov;
        std::fprintf(stderr, "[widescreen] aspect=%s xscale=%.3f fov=%d\n", dw5xl::widescreen::kAspectNames[aspect], kFactors[aspect], fov);
    }
    originalScene(ram, ctx, runtime);
    if (g_trace) {
        std::fprintf(stderr, "[widescreen:picture] stage=%d characters=%d angle=%.4f built=%.4f volume=%.4f yscale=%.6f const=%08X,%08X\n", g_stageObjects, g_characters,
                     g_gameAngle, g_builtAngle, g_volumeAngle, g_yScale, readWord(ram, kMathConstants[0]), readWord(ram, kMathConstants[1]));
        g_stageObjects = g_characters = 0;               // characters are chosen before the picture is drawn
    }
}

void stageObjectCheck(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    originalStageObjectCheck(ram, ctx, runtime);
    if (getRegU32(ctx, 2)) ++g_stageObjects;
}

void drawCharacter(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    ++g_characters;
    originalDrawCharacter(ram, ctx, runtime);
}

// Calls one of the game's heap functions with a private register context: the real caller sees nothing of it.
uint32_t callHeap(PS2Runtime::RecompiledFunction function, uint32_t a0, uint32_t a1, uint8_t *ram, const R5900Context *ctx, PS2Runtime *runtime)
{
    auto heapContext = *ctx;
    SET_GPR_U32(&heapContext, 4, a0);
    SET_GPR_U32(&heapContext, 5, a1);
    function(ram, &heapContext, runtime);
    return getRegU32(&heapContext, 2);
}

void findLoadedCopies(uint8_t *ram)
{
    g_searchCopies = false;
    uint32_t header = readWord(ram, kHeapRoot);
    for (int i = 0; header && header + 16 <= PS2_RAM_SIZE && i < 1000000; ++i, header = readWord(ram, header + 4)) {
        const uint32_t block = readWord(ram, header + 8);
        if (block && block + sizeof(kCopiesMark) <= PS2_RAM_SIZE && std::memcmp(ram + block, kCopiesMark, sizeof(kCopiesMark)) == 0) {
            g_copies = block;
            return;
        }
    }
}

void start2d(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, 31) == k2dStartReturn && g_sceneAspect != dw5xl::widescreen::Aspect4x3) {
        g_hud = true;
        g_used = 0;
        if (g_searchCopies) findLoadedCopies(ram);
        if (!g_copies) {
            g_copies = callHeap(allocate, 64, kCopiesHeader + kCopyBytes * 2, ram, ctx, runtime);
            if (g_copies) std::memcpy(ram + g_copies, kCopiesMark, sizeof(kCopiesMark));
            else if (!g_loggedMemory) {
                g_loggedMemory = true;
                std::fprintf(stderr, "[widescreen] no memory for the HUD: not squeezed\n");
            }
        }
    }
    original2dStart(ram, ctx, runtime);
}

void fades(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, 31) == kFadesReturn) g_hud = false;
    originalFades(ram, ctx, runtime);
}

// Paused: a full-screen 2D screen with its own map camera, not the battle view. The battle's next picture brings that back.
void pause(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_scene = g_hud = false;
    if (g_trace) std::fprintf(stderr, "[widescreen] pause menu\n");   // DW5XL-DEBUG (issue 058's test)
    originalPause(ram, ctx, runtime);
}

void battleEnd(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, 31) == kBattleEndReturn) {
        g_scene = g_hud = false;
        if (g_searchCopies) findLoadedCopies(ram);
        if (g_copies) {                                    // the heap stays whole between battles
            std::memset(ram + g_copies, 0, sizeof(kCopiesMark));
            callHeap(release, g_copies, 0, ram, ctx, runtime);
            g_copies = 0;
        }
    }
    originalBattleEnd(ram, ctx, runtime);
}

void queue(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    afterStateLoad(runtime);                             // every picture queues packets, in menus too
    if (g_hud && g_copies) {
        const uint32_t from = getRegU32(ctx, 5) & 0x1FFFFFFFu, qwc = getRegU32(ctx, 6) & 0xFFFFu;
        const uint32_t bytes = qwc * 16u;
        const uint32_t to = g_copies + kCopiesHeader + (runtime->memory().read32(kBufferIndex) & 1u) * kCopyBytes + g_used;
        if (bytes && from + bytes <= PS2_RAM_SIZE) {
            if (g_used + bytes <= kCopyBytes) {
                std::memcpy(ram + to, ram + from, bytes);
                if (dw5xl::gifsqueeze::squeeze(ram + to, qwc, kFactors[g_sceneAspect])) {
                    g_used += bytes;
                    SET_GPR_U32(ctx, 5, to);
                }
            } else if (!g_loggedFull) {
                g_loggedFull = true;
                std::fprintf(stderr, "[widescreen] HUD copies full: the rest of this picture's HUD is not squeezed\n");
            }
        }
    }
    originalQueue(ram, ctx, runtime);
}

void install(PS2Runtime &runtime)
{
    const struct { uint32_t address, word; } expected[] = {
        {kCamera, 0x27BDFE40u}, {kViewAngle, 0x3C030047u}, {kScene, 0x27BDFFF0u}, {k2dStart, 0x27BDFFA0u}, {kFades, 0x27BDFFD0u},
        {kPause, 0x27BDFFE0u}, {kBattleEnd, 0x27BDFFD0u}, {kQueue, 0x27BDFF90u}, {kAllocate, 0x27BDFFE0u}, {kFree, 0x10800045u},
        {k2dStartReturn - 8, 0x0C04FB5Cu}, {kFadesReturn - 8, 0x0C0722DCu}, {kBattleEndReturn - 8, 0x0C050664u}};
    for (const auto &e : expected)
        if (runtime.memory().read32(e.address) != e.word) {
            std::fprintf(stderr, "[widescreen] declined: unexpected code at 0x%X\n", e.address);
            return;
        }
    originalCamera = runtime.lookupFunction(kCamera);
    originalViewAngle = runtime.lookupFunction(kViewAngle);
    originalScene = runtime.lookupFunction(kScene);
    original2dStart = runtime.lookupFunction(k2dStart);
    originalFades = runtime.lookupFunction(kFades);
    originalPause = runtime.lookupFunction(kPause);
    originalBattleEnd = runtime.lookupFunction(kBattleEnd);
    originalQueue = runtime.lookupFunction(kQueue);
    allocate = runtime.lookupFunction(kAllocate);
    release = runtime.lookupFunction(kFree);
    runtime.replaceFunction(kCamera, &camera);
    runtime.replaceFunction(kViewAngle, &viewAngle);
    runtime.replaceFunction(kScene, &scene);
    runtime.replaceFunction(k2dStart, &start2d);
    runtime.replaceFunction(kFades, &fades);
    runtime.replaceFunction(kPause, &pause);
    runtime.replaceFunction(kBattleEnd, &battleEnd);
    runtime.replaceFunction(kQueue, &queue);
    if (std::getenv("PS2X_WIDESCREEN_TRACE")) {          // DW5XL-DEBUG
        g_trace = true;
        originalStageObjectCheck = runtime.lookupFunction(kStageObjectCheck);
        originalDrawCharacter = runtime.lookupFunction(kDrawCharacter);
        runtime.replaceFunction(kStageObjectCheck, &stageObjectCheck);
        runtime.replaceFunction(kDrawCharacter, &drawCharacter);
    }
    g_aspect.store(dw5xl::widescreen::savedAspect());
    g_fov.store(dw5xl::widescreen::savedFov());
    g_runtime = &runtime;
    std::string text;
    if (ps2x::settings::loadText("sharpen", text)) runtime.setPresentSharpen(text == "on");
    runtime.setPictureShape(kShapes[g_aspect.load()]);
}
} // namespace

namespace dw5xl::widescreen {
Aspect aspect() { return Aspect(g_aspect.load()); }
float shape() { return kShapes[g_aspect.load()]; }
void setAspect(Aspect aspect)
{
    g_aspect.store(aspect);
    if (g_runtime) g_runtime->setPictureShape(kShapes[aspect]);
    saveAspect(aspect);
}
int fov() { return g_fov.load(); }
void setFov(int percent)
{
    g_fov.store(percent);
    saveFov(percent);
}
} // namespace dw5xl::widescreen

PS2_REGISTER_GAME_OVERRIDE("DW5XL widescreen", "SLES_538.60", 0u, 0u, &install)
