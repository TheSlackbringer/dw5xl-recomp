// Issue 049 Part 2: in-between pictures from the game's finished command list.
// DW5XL-DEBUG (no setting yet; MOTION SMOOTHING comes with P2.5): PS2X_SMOOTH_PROTO=<n>:rollback[:read] switches
// them on, at most n per game frame. (":rollback" is the format the tests pass. Nothing is rolled back: the pictures
// are not drawn on the game's GS.) Logs "[smooth] ..." every 250 frames.
//
// What a picture is made of. The display thread sends a new frame's list with sceDmaSend (called from FUN_00142860
// at 0x1428DC). The frame start is not in that list. Before it the display thread's FUN_001429e0 sends, on the GIF
// channel:
//   - with sceDmaSendN, the copy of the finished picture from the drawing buffer (page 64) to the display buffer
//     (page 0): 8 or 16 sprites, with their GS setup (the deflicker packet);
//   - through FUN_00111910 -> FUN_00111590 (which starts the GIF DMA itself), the field's draw environment: FRAME page
//     64, ZBUF, XYOFFSET for the field's parity, ..., and the sprite that clears the drawing buffer and its Z buffer.
// A picture is the draw environment with the clear, a copy of the list with in-between values (the chain walked as
// the DMAC walks it, dw5xl_smooth_list.h) and the game's own deflicker packet into the display buffer.
// FUN_001429e0 alternates two deflicker packets with the field, and the one that copies drawing N is sent at frame
// N+1's start, the same as frame N-1's.
//
// Where it is drawn: on a GS memory of the pictures' own (PS2Runtime::pictureGs: the same renderer and graphics
// device as the game's GS). Before a frame's first picture the pages that changed are copied there; the pictures'
// GIF packets go there and are read from there. Nothing is drawn, kept or put back on the game's GS, so the game's
// own pictures must be IDENTICAL to a run without the switch. PS2X_SMOOTH_GS2_CHECK=<n> (criterion 6c): after every
// n-th copy the two memories are compared page by page ("[smooth:check]").
//
// When it is made (amendment v3.6): when the game has finished the frame (frameEnd: the wait in FUN_001425d0, the
// battle's frame end), one field before it sends the list. The frame's instant (the start of the field in which the
// list will be sent) is known in advance from the field clock, and so are the draw environment of that field and the
// deflicker packet (makePictures). The send then checks what was taken as known ("[smooth] known in advance ...").
// When the frame end's code is not the expected one, no in-between pictures are made.
//
// What runs it: a VIF1/VU1 device of the pictures' own (ps2x::PictureDevice, a thread of its own; amendment v3.7).
// It is the pictures' one path (owner 2026-11-13). At the frame end one job goes into the game's device queue:
// there the display registers for the pictures' reads are taken and the game's VIF1/VU1 state is copied as the
// pictures' start; the pictures' VIF1/VU1 work begins at once. From there a call in the game's GS stream drops what
// the GS side still holds of the frame before's pictures, copies the changed pages into the picture GS and lets
// this frame's packets be read: the GS thread reads them one at a time between the game's own packets, the game's
// first, while the pictures' VU1 goes on. A picture's read follows its last packet there.
// "[smooth:device2] ..." every 250 frames.
// Every picture the time line asks for is started; nothing is foretold (amendment v3.8). A picture not ready at its
// instant is left out by the window; one still being drawn when the next frame's pictures need the device is cut
// off there ("[smooth:cut]" with PS2X_SMOOTH_TIMELINE=1) and makes its frame one that "did not fit" for the rate
// choice. The head start follows what pictures needed in two separate moments or in ten frames (HeadStartMeter).
// The game does not wait for the graphics card on a picture's behalf (amendment v3.9). A picture is cut off when
// the three pictures before it are still unfinished on the card: asked on the GS thread before its first packet is
// read (GifArbiter::setSideStartFn) and again at its read (GS::submitPictureRead says false); nothing waits at
// either place. It counts as cut off for the rate choice.
//
// ":read": each picture is also read out as an in-between picture (GS::submitPictureRead, right after its deflicker
// packet), tagged with the field its frame's list was sent in and its number; it belongs to the next field, whose
// own picture is the game's picture of that frame. The window collects the pictures ("[smooth:queue]");
// PS2X_SMOOTH_DUMP=<dir> saves them (inb_<field>_<i>of<n>.png). An unchanged copy must be IDENTICAL to the game's
// picture of the same frame (criterion 8a).
//
// More switches:
//   PS2X_SMOOTH_LIST_HASH=1 (criterion 6b): the list is hashed at the frame end and at the send ("[smooth:end] ...
//   hash=", "[smooth:list] ... hash=" with PS2X_SMOOTH_TIMELINE=1).
//   PS2X_SMOOTH_EVERY=<n>: without timed pictures, pictures only for every n-th frame (proofs at a scale where one
//   before every game frame does not fit).
//   PS2X_SMOOTH_TIMELINE=1 also logs "[smooth:level] frame= level= of=": the rate choice's level from that frame on
//   (0: off).
//   PS2X_HOLD_TRACE=1: "[smooth:hold]", one line per game frame: what the game's send waited for.
//   PS2X_SMOOTH_DEVICE2_CHECK=1 (criterion 6d, with unchanged copies, PS2X_SMOOTH_FIXED_T=1): the PATH1/PATH2 bytes
//   the game's device makes from a frame's list (between two marks in device order around the game's own send) and
//   those the pictures' device makes from the copy's list part are hashed: "[smooth:device] frame= field= bytes=
//   game= picture= same=".
//   PS2X_SMOOTH_DEVICE2=0 and PS2X_SMOOTH_AT_SEND=1 asked for ways that no longer exist (the pictures through the
//   game's device; pictures made at the send): a line says so, and the pictures are made as described above.
#include "dw5xl_smooth_list.h"
#include "dw5xl_smooth_time.h"
#include "runtime/ps2_hold_trace.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_device_queue.h"
#include "runtime/ps2_display_clock.h"
#include "runtime/ps2_picture_device.h"
#include "runtime/gs/gs_frontend.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kDmaSend = 0x00112FE8u;          // sceDmaSend (runtime HLE stub)
constexpr uint32_t kSendNewFrame = 0x001428E4u;     // its return address in the display thread, new frame
constexpr uint32_t kDmaSendN = 0x00113050u;         // sceDmaSendN (runtime HLE stub)
constexpr uint32_t kStartSendA = 0x00142A9Cu, kStartSendB = 0x00142ABCu; // its return addresses in FUN_001429e0
constexpr uint32_t kSwapEnv = 0x00111910u;          // puts the draw environment of a field's parity
constexpr uint32_t kSwapEnvFromStart = 0x00142B74u; // its return address in FUN_001429e0
constexpr uint32_t kSwapEnvPutOdd = 0x00111944u, kSwapEnvPutEven = 0x00111954u; // its "addiu a0, s1, <packet>"
constexpr uint32_t kEnvOdd = 0x140u, kEnvEven = 0x50u;                           // the two packets in the buffer set
constexpr uint32_t kVif1Chcr = 0x10009000u;
// P2.4: the list builders that write a block with a matrix in it, and where the list being built ends.
constexpr uint32_t kMatrixBlock = 0x0019CED0u;      // (matrix, flags, ...): 8 quadwords at the current builder's end
constexpr uint32_t kLitBlock = 0x0019CDD0u;         // the same for lit objects: 15 quadwords
constexpr uint32_t kCameraBlock = 0x0019C550u;      // (builder, view, mode): 24 quadwords at that builder's end
constexpr uint32_t kBufferIndex = 0x70003F80u;      // which of the two lists the game is writing
constexpr uint32_t kBuilders = 0x0044C600u;         // their builders, 0x20 apart; the first word is the list's end
// Each camera's matrices, 0x40 apart per view (written by FUN_001469d0 before a pass group's camera block).
constexpr uint32_t kWorldToView = 0x004746E0u, kViewToWorld = 0x00474260u, kScreenProjView = 0x00474560u;
constexpr uint32_t kClip = 0x00473F60u, kProjView = 0x004743E0u, kWideClip = 0x00473DE0u;
constexpr uint32_t kViews = 6u;
// The 44 character slots (FUN_0018c130): two pairs of matrix buffers each, written in turn frame by frame.
constexpr uint32_t kSlots = 0x00493570u, kSlotBytes = 0x58u, kSlotCount = 44u;
constexpr uint32_t kSlotActive = 0x30u;             // a signed byte, -1 = free
constexpr uint32_t kSlotBuffers[2] = {0x10u, 0x48u}; // each pair: two pointers
// Amendment v3.6 (P2.8): the battle's frame end, FUN_001425d0. It runs the draw step, closes the list
// (FUN_00113510, FUN_001133c8), calls FlushCache, flags the list as ready for the display thread (which sends it at
// the next field) and then waits for that thread. Our hook is that wait (WaitSema, told by its return address): the
// list is closed and flagged, and the game thread has nothing to do until its field. A wait that was paused goes on
// at kWaitSemaResume: not a new call.
constexpr uint32_t kWaitSema = 0x0011F5A0u, kWaitSemaResume = 0x0011F5A8u;
constexpr uint32_t kFrameEndCall = 0x00142620u, kFrameEndWait = 0x00142628u;
PS2Runtime::RecompiledFunction originalWaitSema = nullptr;
PS2Runtime::RecompiledFunction originalMatrixBlock = nullptr, originalLitBlock = nullptr, originalCameraBlock = nullptr;
PS2Runtime::RecompiledFunction originalSend = nullptr, originalSendN = nullptr, originalSwapEnv = nullptr;
unsigned g_copies = 0;
bool g_read = false;
std::vector<uint8_t> g_envPrevious;                 // the draw environment of the frame start before (the other field's)
std::vector<uint8_t> g_startRaw;                    // the latest frame start as the game sends it (VIF DIRECT)
std::vector<uint8_t> g_startRawPrevious;            // the one before it
uint64_t g_startHash = 0, g_startChanges = 0;
bool g_battleStart = false, g_battleStartPrevious = false; // the last two frame starts were the battle's kind
// DW5XL-DEBUG PS2X_SMOOTH_TRACE=1: every frame start's packet and every frame's in-between pass ("[smooth:trace]"),
// with the GS side's lines for every read (GS::submitFieldScanout / submitPictureRead): which deflicker packet and
// which line-up a picture got.
bool g_trace = false;
std::vector<uint8_t> g_env;                         // the latest draw environment with the clear
bool g_envOk = false;
uint64_t g_frames = 0, g_failed = 0, g_noStart = 0;
double g_copyMs = 0.0, g_valuesMs = 0.0;            // per frame: copying the list, reading its values
uint64_t g_copied = 0;

// The frame start's packet (the copy of the finished picture into the display buffer) as the game sends it, kept as
// a VIF1 stream: three NOPs and DIRECT, so the packet goes to the GIF through PATH2 and starts on a quadword.
void captureStart(const uint8_t *ram, uint32_t addr, uint32_t qwc)
{
    if ((addr & 0x70000000u) == 0x70000000u) return;                         // scratchpad: not copied here
    addr &= 0x01FFFFF0u;
    if (qwc == 0u || qwc > 0xFFFFu || addr + qwc * 16u > PS2_RAM_SIZE) return;
    const std::vector<uint8_t> packet(ram + addr, ram + addr + qwc * 16u);
    uint64_t h = 1469598103934665603ull;                                      // FNV-1a of the packet as sent
    for (const uint8_t b : packet) h = (h ^ b) * 1099511628211ull;
    if (h != g_startHash && g_startChanges++ < 4u)
        std::fprintf(stderr, "[smooth] frame start packet %u qw at 0x%X, checksum %016llx\n", qwc, addr,
                     static_cast<unsigned long long>(h));
    g_startHash = h;
    g_startRawPrevious.swap(g_startRaw);
    g_startRaw.assign(16u, 0u);
    const uint32_t direct = 0x50000000u | qwc;
    std::memcpy(&g_startRaw[12], &direct, 4);
    g_startRaw.insert(g_startRaw.end(), packet.begin(), packet.end());
}

void captureEnv(const uint8_t *ram, uint32_t addr)
{
    g_envOk = false;
    if ((addr & 0x70000000u) == 0x70000000u) return;                         // scratchpad: not copied here
    addr &= 0x01FFFFF0u;
    if (addr + 16u > PS2_RAM_SIZE) return;
    uint64_t tag;
    std::memcpy(&tag, ram + addr, 8);
    const uint32_t qwc = static_cast<uint32_t>(tag & 0x7FFFu) + 1u;          // as FUN_00111590 sets the DMA's QWC
    if (addr + qwc * 16u > PS2_RAM_SIZE) return;
    g_envPrevious.swap(g_env);
    g_env.assign(16u, 0u);
    const uint32_t direct = 0x50000000u | qwc;
    std::memcpy(&g_env[12], &direct, 4);
    g_env.insert(g_env.end(), ram + addr, ram + addr + qwc * 16u);
    g_envOk = true;
}

// P2.4: where the game's builders put a matrix, recorded when each builder is entered, from its arguments and the
// end of the list at that moment (the block starts there). Nothing is read after the original has run: a game thread
// paused inside does not come back through the hook (P2.3b, fault 2).
std::mutex g_tagMutex;
std::vector<dw5xl::smooth::Tag> g_recorded;
struct RecordedCamera { uint64_t serial; dw5xl::smooth::Camera camera; };
std::vector<RecordedCamera> g_recordedCameras;      // the cameras of the recorded camera blocks, by the tag's serial
uint64_t g_tagSerial = 0;
dw5xl::smooth::List g_list;                         // the newest frame's list
dw5xl::smooth::List g_listBefore;                   // the list of the frame before (PS2X_SMOOTH_BLENDED_ONLY)
// PS2X_SMOOTH_BLENDED_ONLY=1 (the spec's second test switch): two pictures per frame, both holding only what this
// frame's list and the frame before's share (dw5xl::smooth::sharedOnly): picture 1 the list of the frame before,
// picture 2 this frame's list with the frame before's values (t = 0). Both are drawn with the frame before's draw
// environment, so for the blended content they must be the same picture. Needs ":read"; a frame after a camera cut
// or without a frame before gets no pictures.
bool g_blendedOnly = false;
struct SharedTotals {
    uint64_t frames = 0, cuts = 0, partsNow = 0, partsBefore = 0, shared = 0, bytesNow = 0, sharedBytes = 0;
    uint64_t objectBlocks = 0, cameraBlocks = 0, bonePackets = 0, otherInline = 0, otherReferences = 0;
    uint64_t otherInlineBytes = 0, otherReferenceBytes = 0;
    dw5xl::smooth::SharedStats::Why objects, cameras, bones;
    uint64_t examples = 0;
    double ms = 0.0;
};
SharedTotals g_shared;
dw5xl::smooth::Values g_now, g_before;              // its keyed values, and those of the frame before
uint64_t g_newFrames = 0, g_beforeFrame = ~0ull;    // g_before is of new frame number g_beforeFrame
uint64_t g_tagged = 0, g_tagsMissing = 0;
float g_fixedT = -1.0f;                             // PS2X_SMOOTH_FIXED_T: every picture at this t (test switch)
// Amendment v3.5 (P2.5c), with PS2X_SMOOTH_SHOW=1 and a display published by the window: the pictures of a frame are
// those the time line asks for, each made for one instant on screen (dw5xl_smooth_time.h). Until P2.5d chooses them
// from measurements, test switches set the head start (PS2X_SMOOTH_HEAD_START, ms), the step on a fixed-rate display
// (PS2X_SMOOTH_STEP: a picture every n-th refresh) and the pictures per frame on a following one
// (PS2X_SMOOTH_PER_FRAME). The prototype's count (PS2X_SMOOTH_PROTO=<n>) is the most it makes per frame.
// P2.5d: without those switches the head start is the measured one (HeadStartMeter: the worst of the last five
// seconds) and the step or the pictures per frame come from the rate choice ("never slower", RateChoice), both fed
// by what the window reports about every timed picture.
bool g_show = false;
bool g_headFixed = false;                           // PS2X_SMOOTH_HEAD_START is set ...
double g_headStart = 0.0;                           // ... to this fixed head start (ms; may be below zero, v3.6)
// The pictures of a frame are made when the game has finished it (frameEnd), one field before it sends the list.
// PS2X_SMOOTH_LIST_HASH=1 (criterion 6b): the list is hashed at the frame end and at the send ("hash=" in the
// time-line log); it costs a second copy of the list at each point, so it is not for timing runs.
bool g_hashLists = false;
// The pictures run on a VIF1/VU1 device of their own. PS2X_SMOOTH_DEVICE2_CHECK=1: criterion 6d (see the top of the
// file).
bool g_device2Check = false;
struct DeviceFrame { GSPresentationRequest captured{}; };   // what a frame's pictures share on the pictures' device
ps2x::GifStreamHash g_tapHash;                      // the game's device's PATH1/PATH2 packets of one list (device order)
bool g_tapOn = false;
struct DeviceCheck { uint64_t frame = 0, field = 0, game = 0, gameBytes = 0, picture = 0, pictureBytes = 0; bool haveGame = false, havePicture = false; };
std::mutex g_checkMutex;
std::deque<DeviceCheck> g_checks;
ps2x::PictureDevice::Totals g_device2Before;        // the totals at the last "[smooth:device2]" line
GifArbiter::SideTotals g_sideBefore;                // and the GS side's
uint64_t g_sideNs = 0;                              // the GS side's time at the last frame (a picture's cost)

// Criterion 6d: one side's hash of a frame's list; the line is printed when both sides are known.
void deviceCheck(uint64_t frame, bool game, uint64_t field, uint64_t hash, uint64_t bytes)
{
    std::lock_guard<std::mutex> lock(g_checkMutex);
    auto it = std::find_if(g_checks.begin(), g_checks.end(), [frame](const DeviceCheck &c) { return c.frame == frame; });
    if (it == g_checks.end()) {
        if (g_checks.size() >= 64u) g_checks.pop_front();   // frames one side never reported
        g_checks.emplace_back();
        it = g_checks.end() - 1;
        it->frame = frame;
    }
    if (game) {
        it->haveGame = true;
        it->field = field;
        it->game = hash;
        it->gameBytes = bytes;
    } else if (!it->havePicture) {
        it->havePicture = true;
        it->picture = hash;
        it->pictureBytes = bytes;
    }
    if (!it->haveGame || !it->havePicture) return;
    std::fprintf(stderr, "[smooth:device] frame=%llu field=%llu bytes=%llu game=%016llx picture=%016llx same=%d\n",
                 static_cast<unsigned long long>(frame), static_cast<unsigned long long>(it->field),
                 static_cast<unsigned long long>(it->gameBytes), static_cast<unsigned long long>(it->game),
                 static_cast<unsigned long long>(it->picture),
                 it->game == it->picture && it->gameBytes == it->pictureBytes ? 1 : 0);
    g_checks.erase(it);
}

// The pictures are drawn on a GS memory of their own (PS2Runtime::pictureGs: the same renderer and graphics device).
// Before a frame's first picture the pages that changed are copied there; the pictures' GIF packets go there and
// are read from there; nothing is drawn, kept or put back on the game's GS. PS2X_SMOOTH_GS2_CHECK=<n> (criterion
// 6c): after every n-th copy the two memories are compared page by page (slow: both are read back) and
// "[smooth:check]" is logged.
unsigned g_gs2Check = 0;
struct Gs2 { uint64_t syncs = 0, pages = 0, refused = 0, checks = 0, differ = 0; double ms = 0.0, longest = 0.0; };
Gs2 g_gs2Stats;
uint64_t g_gs2LastFrame = 0;                        // the frame of the last copy (a break in the frames: all pages)
dw5xl::smooth::FinishedFrames g_finished;
// What a frame end takes as known in advance, checked at the send: the field, the list, the draw environment.
struct Advance { uint64_t frames = 0, wrongField = 0, wrongList = 0, wrongEnv = 0, wrongMode = 0, noEnd = 0, hashed = 0; };
Advance g_advance;
std::vector<uint8_t> g_usedEnv;                     // the draw environment the last frame end took for the coming field
bool g_usedEnvValid = false, g_endTte = false;
double g_endFirstTagMs = 0.0, g_endLastTagMs = 0.0; // the tag times of the frame finished last
int g_step = 0, g_perFrame = 0, g_loggedStep = 0, g_loggedLevel = -2;   // > 0: PS2X_SMOOTH_STEP / PS2X_SMOOTH_PER_FRAME (a fixed rate)
dw5xl::smooth::TimeLine g_line;
dw5xl::smooth::HeadStartMeter g_meter;
dw5xl::smooth::RateChoice g_rate;
struct Asked { uint64_t frames = 0, pictures = 0, own = 0, capped = 0, moved = 0, none = 0; };
Asked g_asked;
// A game frame as the rate choice needs it: complete once all its pictures are reported and the next list has come.
struct FrameNote {
    uint64_t frame = 0;
    double instantMs = 0.0, listMs = 0.0;
    unsigned asked = 0, reported = 0;
    unsigned cutOff = 0;                            // v3.8: pictures cut off (still being drawn when the next frame's needed the device)
    double firstStarted = 0.0, lastStarted = 0.0, firstQueued = 0.0;
    double spareMs = 0.0;                           // the game's spare time when it sent this frame's list
};
std::deque<FrameNote> g_notes;
struct Auto { uint64_t frames = 0, heldUp = 0, late = 0, shown = 0, past = 0, cutOff = 0, cutFrames = 0; double leastMargin = 1e18, marginSum = 0.0, costSum = 0.0, leastSpare = 1e18; uint64_t costs = 0; };
Auto g_auto;
struct Built { uint64_t pictures = 0, objects = 0, bones = 0, cameraCopies = 0, unmatched = 0, jumped = 0, cuts = 0; double ms = 0.0; };
Built g_built;
// When the game built the first and the last matrix block of the list it sends next (steady clock, ms): how long a
// finished list waits for the field in which the game sends it. That wait is the game's spare time in that frame.
// "Never slower" (P2.5d) counts the game as held up when it finished a frame with less than a quarter field to
// spare, or missed its field: the reserve that keeps a heavier moment from costing a field.
double g_firstTagMs = 0.0, g_lastTagMs = 0.0;
uint64_t g_judgeFrom = 0, g_lastTimedFrame = 0;     // the rate choice judges frames from here on (not a line's first second)
// Amendment v3.7 (P2.6a): a break in the game's own clock between two frames: the game stood still (the MENU, a save,
// a load) or a state was loaded. Read from the PS2 field clock (dw5xl::smooth::fieldClockBroke) and the runtime's
// count of state loads. After a break the time line starts again (measured 2026-11-11 without this: a quick save
// of 82 ms switched the pictures off for 20 seconds, and the MENU left a head start of 660 ms to be reached), the
// picture GS gets every page, and after a load the frame before is not used for in-between values.
struct ClockWatch {
    bool have = false, pending = false, allPages = false;
    uint64_t field = 0, loads = 0, stops = 0, jumps = 0;
    double instantMs = 0.0;
};
ClockWatch g_clock;
uint64_t g_lineFrom = 0;                            // the time line started (again) with this frame: older pictures are not measured
uint64_t g_measureFrom = 0;                         // ... and the first frame with pictures after it is not either (cold caches)
uint64_t g_deviceNs = 0, g_deviceCount = 0;         // the pictures' device's totals at the last frame (time, pictures)
double g_pictureCost = 0.0;                         // the device time of one picture, the last frames' average (ms; 0: none yet)

uint64_t record(dw5xl::smooth::TagKind kind, uint32_t block, uint32_t key, uint32_t model, uint32_t aux,
                const dw5xl::smooth::Camera *camera = nullptr)
{
    g_lastTagMs = ps2x::display_clock::nowMs();
    if (g_firstTagMs == 0.0) g_firstTagMs = g_lastTagMs;
    std::lock_guard<std::mutex> lock(g_tagMutex);
    if (g_recorded.size() >= 8192u) {               // lists that were never sent as a new frame
        g_recorded.erase(g_recorded.begin(), g_recorded.begin() + 4096);
        const uint64_t oldest = g_recorded.front().serial;
        g_recordedCameras.erase(std::remove_if(g_recordedCameras.begin(), g_recordedCameras.end(),
                                               [oldest](const RecordedCamera &c) { return c.serial < oldest; }),
                                g_recordedCameras.end());
    }
    dw5xl::smooth::Tag tag;
    tag.kind = kind;
    tag.block = block & 0x01FFFFF0u;
    tag.key = key;
    tag.model = model;
    tag.aux = aux;
    tag.serial = ++g_tagSerial;
    g_recorded.push_back(tag);
    if (camera) g_recordedCameras.push_back(RecordedCamera{tag.serial, *camera});
    return tag.serial;
}

void objectMatrix(dw5xl::smooth::TagKind kind, const uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t matrix = getRegU32(ctx, 4) & 0x01FFFFFFu;
    if (matrix == 0u || matrix + 0x70u > PS2_RAM_SIZE) return;                 // 0: the builder writes a unit matrix
    const uint32_t builder = kBuilders + (runtime->memory().read32(kBufferIndex) & 1u) * 0x20u;
    uint32_t model;
    std::memcpy(&model, ram + matrix + 0x64u, 4);                              // the model index beside the matrix
    record(kind, runtime->memory().read32(builder), matrix, model, getRegU32(ctx, 5));
}

void matrixBlock(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    objectMatrix(dw5xl::smooth::TagKind::Matrix, ram, ctx, runtime);
    originalMatrixBlock(ram, ctx, runtime);
}

void litBlock(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    objectMatrix(dw5xl::smooth::TagKind::LitMatrix, ram, ctx, runtime);
    originalLitBlock(ram, ctx, runtime);
}

void cameraBlock(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t builder = getRegU32(ctx, 4) & 0x01FFFFFCu, view = getRegU32(ctx, 5);
    if (builder + 4u <= PS2_RAM_SIZE && view < kViews) {
        // The view's matrices as they are now: the block gets copies of them, and other packets of this pass too.
        dw5xl::smooth::Camera camera;
        camera.view = view;
        const auto read = [ram, view](uint32_t address, dw5xl::smooth::Matrix &m) {
            std::memcpy(m.data(), ram + address + view * 0x40u, 64);
        };
        read(kWorldToView, camera.worldToView);
        read(kViewToWorld, camera.viewToWorld);
        read(kScreenProjView, camera.screenProjView);
        read(kClip, camera.clip);
        read(kProjView, camera.projView);
        read(kWideClip, camera.wideClip);
        record(dw5xl::smooth::TagKind::Camera, runtime->memory().read32(builder), view, 0u, getRegU32(ctx, 6), &camera);
    }
    originalCameraBlock(ram, ctx, runtime);
}

// The newest frame's list as our own data: stream, map and the tags that lie in it.
bool captureList(const uint8_t *ram, uint32_t list, bool tte)
{
    if (!dw5xl::smooth::flatten(ram, list, tte, g_list)) return false;
    std::lock_guard<std::mutex> lock(g_tagMutex);
    g_tagsMissing += dw5xl::smooth::resolve(g_recorded, g_list);
    g_tagged += g_list.tags.size();
    // The cameras of this list's camera blocks, each different one once.
    for (const dw5xl::smooth::Tag &tag : g_list.tags) {
        if (tag.kind != dw5xl::smooth::TagKind::Camera) continue;
        for (const RecordedCamera &recorded : g_recordedCameras) {
            if (recorded.serial != tag.serial) continue;
            const bool known = std::any_of(g_list.cameras.begin(), g_list.cameras.end(),
                                           [&](const dw5xl::smooth::Camera &c) {
                                               return c.view == recorded.camera.view &&
                                                      c.screenProjView == recorded.camera.screenProjView &&
                                                      c.projView == recorded.camera.projView &&
                                                      c.worldToView == recorded.camera.worldToView;
                                           });
            if (!known) g_list.cameras.push_back(recorded.camera);
            break;
        }
    }
    // Recordings up to the newest one found belong to this list or to older ones.
    uint64_t newest = 0;
    for (const dw5xl::smooth::Tag &tag : g_list.tags) newest = std::max(newest, tag.serial);
    g_recorded.erase(std::remove_if(g_recorded.begin(), g_recorded.end(),
                                    [newest](const dw5xl::smooth::Tag &tag) { return tag.serial <= newest; }),
                     g_recorded.end());
    g_recordedCameras.erase(std::remove_if(g_recordedCameras.begin(), g_recordedCameras.end(),
                                           [newest](const RecordedCamera &c) { return c.serial <= newest; }),
                            g_recordedCameras.end());
    return true;
}

// The matrix buffers of the characters in use: what a reference to a bone packet points to.
std::vector<dw5xl::smooth::BoneBuffer> boneBuffers(const uint8_t *ram)
{
    std::vector<dw5xl::smooth::BoneBuffer> buffers;
    for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
        const uint32_t at = kSlots + slot * kSlotBytes;
        if (static_cast<int8_t>(ram[at + kSlotActive]) == -1) continue;
        for (uint32_t pair = 0; pair < 2u; ++pair) {
            for (uint32_t turn = 0; turn < 2u; ++turn) {
                uint32_t base;
                std::memcpy(&base, ram + at + kSlotBuffers[pair] + turn * 4u, 4);
                base &= 0x01FFFFFFu;
                if (base == 0u) continue;
                dw5xl::smooth::BoneBuffer buffer;
                buffer.base = base;
                buffer.slot = static_cast<uint16_t>(slot);
                buffer.pair = static_cast<uint8_t>(pair);
                buffers.push_back(buffer);
            }
        }
    }
    return buffers;
}

// DW5XL-DEBUG PS2X_SMOOTH_LISTDUMP=<dir> with PS2X_SMOOTH_LISTDUMP_AT=<field>[:<frames>]: for the first new frames sent
// from that field on (2 unless given), writes list_<field>.bin (the stream), list_<field>.txt (its map and tags) and
// ram_<field>.bin (the 32 MB of game memory at that moment). Game data: for local analysis only.
void dumpList(const uint8_t *ram, uint64_t field)
{
    static const char *dir = std::getenv("PS2X_SMOOTH_LISTDUMP");
    static uint64_t from = ~0ull, left = 0;
    static bool parsed = false;
    if (!dir || !*dir) return;
    if (!parsed) {
        parsed = true;
        if (const char *at = std::getenv("PS2X_SMOOTH_LISTDUMP_AT")) {
            char *rest = nullptr;
            from = std::strtoull(at, &rest, 10);
            left = (rest && *rest == ':') ? std::strtoull(rest + 1, nullptr, 10) : 2u;
        }
    }
    if (field < from || left == 0u) return;
    --left;
    const std::string base = std::string(dir) + "/list_" + std::to_string(field);
    if (FILE *f = std::fopen((base + ".bin").c_str(), "wb")) {
        std::fwrite(g_list.stream.data(), 1, g_list.stream.size(), f);
        std::fclose(f);
    }
    if (FILE *f = std::fopen((base + ".txt").c_str(), "w")) {
        std::fprintf(f, "start %x bytes %zu\n", g_list.start, g_list.stream.size());
        for (const dw5xl::smooth::Span &s : g_list.spans)
            std::fprintf(f, "span %x %x %x %x %u\n", s.source, s.offset, s.bytes, s.tag, unsigned(s.id));
        for (const dw5xl::smooth::Tag &t : g_list.tags)
            std::fprintf(f, "tag %u %x %x %x %x %x %llu\n", unsigned(t.kind), t.block, t.key, t.model, t.aux, t.offset,
                         static_cast<unsigned long long>(t.serial));
        std::fclose(f);
    }
    if (FILE *f = std::fopen((std::string(dir) + "/ram_" + std::to_string(field) + ".bin").c_str(), "wb")) {
        std::fwrite(ram, 1, PS2_RAM_SIZE, f);
        std::fclose(f);
    }
    // The frame start's draw environment packet and the one of the frame start before (as VIF1 streams).
    const std::vector<uint8_t> *const packets[2] = {&g_env, &g_envPrevious};
    const char *const names[2] = {"/env_", "/envprev_"};
    for (int i = 0; i < 2; ++i) {
        if (FILE *f = std::fopen((std::string(dir) + names[i] + std::to_string(field) + ".bin").c_str(), "wb")) {
            std::fwrite(packets[i]->data(), 1, packets[i]->size(), f);
            std::fclose(f);
        }
    }
    std::fprintf(stderr, "[smooth] list of the frame sent in field %llu saved: %zu bytes, %zu map entries, %zu tags\n",
                 static_cast<unsigned long long>(field), g_list.stream.size(), g_list.spans.size(), g_list.tags.size());
}

// A frame whose pictures are to be made: taken at the game's frame end, before its list is sent (v3.6).
struct FrameAt {
    uint64_t number = 0;                            // the frame's number (FinishedFrames)
    bool haveInstant = false;
    double instantMs = 0.0;                         // its instant: the start of the field in which its list is sent
    double fieldMs = 20.0;                          // the length of a PS2 field
    uint64_t sendField = 0;                         // that field
    double takenMs = 0.0;                           // when the list was taken (the game's frame end)
    double spareMs = 0.0;                           // the game's spare time in this frame
};

// DW5XL-DEBUG PS2X_SMOOTH_MEASURE_ONLY=1: the hooks and the log lines, but no pictures at all (the base line of what
// the game does by itself).
bool measureOnly()
{
    static const bool on = [] { const char *v = std::getenv("PS2X_SMOOTH_MEASURE_ONLY"); return v && *v && *v != '0'; }();
    return on;
}

// The list as the DMAC would send it, hashed (criterion 6b). 0: it could not be copied.
uint64_t listHash(const uint8_t *ram, uint32_t list, bool tte)
{
    static dw5xl::smooth::List copy;
    if (!dw5xl::smooth::flatten(ram, list, tte, copy)) return 0u;
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8u <= copy.stream.size(); i += 8u) {
        uint64_t word;
        std::memcpy(&word, &copy.stream[i], 8);
        h = (h ^ word) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < copy.stream.size(); ++i) h = (h ^ copy.stream[i]) * 1099511628211ull;
    h ^= copy.stream.size();
    return h ? h : 1u;
}

// A frame's pictures, drawn on the picture GS. They are made when the game has finished the frame: one field before
// the game sends the frame's list.
void makePictures(uint8_t *ram, PS2Runtime *runtime, uint32_t list, bool tte, const FrameAt &at)
{
    using Clock = std::chrono::steady_clock;
    namespace dc = ps2x::display_clock;
    g_newFrames = at.number;
    const bool haveInstant = at.haveInstant;
    const double instantMs = at.instantMs, listMs = at.takenMs, spareMs = at.spareMs;
    {
        // A break in the game's own clock since the frame before (g_clock, above).
        const uint64_t loads = runtime->stateLoadCount();
        const bool loaded = loads != g_clock.loads;
        const bool stood = haveInstant && g_clock.have &&
                           dw5xl::smooth::fieldClockBroke(g_clock.field, g_clock.instantMs, at.sendField, instantMs, at.fieldMs);
        if (loaded || stood) {
            const double apart = g_clock.have ? instantMs - g_clock.instantMs : 0.0;
            std::fprintf(stderr, "[smooth:break] frame=%llu field=%llu before=%llu apart=%.1f kind=%s\n",
                         static_cast<unsigned long long>(at.number), static_cast<unsigned long long>(at.sendField),
                         static_cast<unsigned long long>(g_clock.field), apart, loaded ? "load" : "stood");
            g_clock.pending = true;                 // the time line starts again with this frame
            g_clock.allPages = true;                // the picture GS gets every page with its next copy
            ++(loaded ? g_clock.jumps : g_clock.stops);
            if (loaded) g_beforeFrame = ~0ull;      // the frame before is not this frame's in game time
        }
        g_clock.loads = loads;
        if (haveInstant) {
            g_clock.have = true;
            g_clock.field = at.sendField;
            g_clock.instantMs = instantMs;
        }
    }
    // The latest frame start's deflicker packet stands in for the one that will copy this frame's drawing (two frame
    // starts on). That holds only while the scene kind is steady: the first frame after a change (menu to battle, a
    // state load) has a frame start of another kind before it (another packet size) and gets no pictures.
    if (!g_envOk || g_startRawPrevious.empty() || g_startRawPrevious.size() != g_startRaw.size()) {
        ++g_noStart;
        return;
    }
    // In-between values are for battles (the spec): menus and other scenes get no pictures. PS2X_SMOOTH_ALL_SCENES=1
    // (DW5XL-DEBUG) lifts that, as the prototype was before P2.5.
    static const bool allScenes = [] { const char *v = std::getenv("PS2X_SMOOTH_ALL_SCENES"); return v && *v && *v != '0'; }();
    if (!allScenes && !(g_battleStart && g_battleStartPrevious)) {
        ++g_noStart;
        return;
    }
    // The frame start before may be another scene's (menu to battle, a state load): its packet then sets another
    // buffer layout, and a picture drawn with it would clear and draw where this scene keeps other things (found
    // 2026-11-10: the battle's textures were cleared). It is used only when it differs from the latest one in
    // nothing but the offset (the packet's quadword 4, XYOFFSET_1).
    const auto sameLayout = [] {
        if (g_envPrevious.size() != g_env.size() || g_env.size() < 0x60u) return false;
        return std::memcmp(g_envPrevious.data(), g_env.data(), 0x40u) == 0 &&
               std::memcmp(g_envPrevious.data() + 0x50u, g_env.data() + 0x50u, g_env.size() - 0x50u) == 0;
    };
    // Which packets are this frame's. At the frame end (v3.6) this frame's start has not come yet: the game puts one
    // of two draw environments by the field's parity (FUN_001429e0 reads the field bit at every VBlank), so the
    // coming field's is the one of two frame starts ago, the latest is the frame before's, and the deflicker packet
    // is the latest one. That needs both starts to be the battle's (checked here); the send checks afterwards that
    // the game did put that environment ("known in advance").
    if (!sameLayout()) {
        ++g_noStart;
        return;
    }
    const std::vector<uint8_t> &envThis = g_envPrevious, &envBefore = g_env;
    const std::vector<uint8_t> &deflicker = g_startRaw;
    // Every picture: the draw environment with the clear, the list (with in-between values), the deflicker packet.
    // The game draws each frame for one field, half a line apart from the frame before (the draw environment's
    // offset). An in-between picture is drawn as the nearer of its two game frames is: up to half way with the frame
    // before's draw environment, then with this frame's. So at t = 0 it is the frame before's picture and at t = 1
    // this frame's, as the game drew them.
    const auto headFor = [&](float t) -> std::vector<uint8_t> {
        return (t < 0.5f && sameLayout()) ? envBefore : envThis;
    };
    {
        const auto t0 = Clock::now();
        if (!captureList(ram, list, tte)) {
            ++g_failed;
            return;
        }
        const auto t1 = Clock::now();
        dumpList(ram, runtime->eeScheduler().currentVSyncTick());
        dw5xl::smooth::extract(g_list, boneBuffers(ram), g_now);
        if (g_beforeFrame + 1u != g_newFrames) g_before = dw5xl::smooth::Values{};   // not the frame before this one
        g_copyMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
        g_valuesMs += std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
        ++g_copied;
    }
    PS2Memory &mem = runtime->memory();
    // The frame's pictures: streams of our own, built here (the game thread is waiting for its field) and handed to
    // their device one by one as each is built: the first is on its way while the next is being built (v3.6: nothing
    // finished waits). Before the first one the changed pages are copied into the picture GS.
    // The frame's list is sent in this field; its pictures belong to the next one (shown before the game's own).
    const uint64_t frameField = at.sendField;
    unsigned count = 0, queued = 0;                 // the pictures this frame gets (set before the first), and those handed over
    std::shared_ptr<DeviceFrame> deviceFrame;       // what this frame's pictures share on their device
    const auto finished = [&](float t, const std::vector<uint8_t> &body, const dc::Timed *info = nullptr) {
        if (queued == 0u) {
            g_usedEnv = envThis;                    // checked at the send against the environment the game puts
            g_usedEnvValid = true;
            // All pages when the frames did not follow one another (another scene in between) or the game's
            // clock broke (a state load, the MENU with a change of settings; g_clock). The GS says "all pages"
            // itself after a load or a change of scale (GS::takeWrittenPages); this does not rely on it.
            const bool everything = g_gs2LastFrame + 1u != g_newFrames || g_clock.allPages;
            g_clock.allPages = false;
            g_gs2LastFrame = g_newFrames;
            std::function<void()> copyPages = [runtime, everything, frameField] {
                const auto t0 = Clock::now();
                uint32_t pages = 0, differ = 0;
                const bool check = g_gs2Check != 0u && (g_gs2Stats.syncs + 1u) % g_gs2Check == 0u;
                const bool ok = runtime->syncPictureGs(everything, &pages, check ? &differ : nullptr);
                const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                Gs2 &s = g_gs2Stats;
                if (!ok) ++s.refused;
                ++s.syncs;
                s.pages += pages;
                if (!check) {                   // a check reads both memories back: not the copy's own time
                    s.ms += ms;
                    s.longest = std::max(s.longest, ms);
                } else {
                    ++s.checks;
                    s.differ += differ;
                    // Criterion 6c (amendment v3.7) reads this line: one per comparison.
                    std::fprintf(stderr, "[smooth:check] copy=%llu field=%llu pages=%u differ=%u\n",
                                 static_cast<unsigned long long>(s.syncs),
                                 static_cast<unsigned long long>(frameField), pages, ok ? differ : 512u);
                }
                if (s.syncs % 250u == 0u)
                    std::fprintf(stderr, "[smooth:gs2] %llu copies into the picture GS: %.1f pages each, %.3f ms average "
                                 "(%.3f longest); %llu refused; compared %llu times, %llu pages different\n",
                                 static_cast<unsigned long long>(s.syncs), static_cast<double>(s.pages) / 250.0,
                                 s.ms / 250.0, s.longest, static_cast<unsigned long long>(s.refused),
                                 static_cast<unsigned long long>(s.checks), static_cast<unsigned long long>(s.differ)),
                    s.pages = 0, s.ms = 0.0, s.longest = 0.0;
            };
            // One job in the game's device queue (addition 2): the display registers are read in the order of the
            // game's writes, and the game's VIF1/VU1 state as it is there is the pictures' start (their VIF1/VU1
            // work begins). From there, in the game's GS stream: the page copy, after which the GS side reads this
            // frame's packets.
            deviceFrame = std::make_shared<DeviceFrame>();
            const uint64_t frame = g_newFrames;
            auto job = [runtime, frame, frameField, shared = deviceFrame, copyPages] {
                shared->captured = runtime->gs().captureFieldScanoutRequest(frameField + 1u);
                runtime->pictureDevice().takeStart(frame, runtime->memory().m_vif1, runtime->vu1());
                runtime->gifArbiter().submitCall([runtime, frame, copyPages] {
                    runtime->pictureDevice().open(frame, copyPages);
                });
            };
            if (!ps2x::device::submitGsCall(mem, job)) job();
            if (g_trace)
                std::fprintf(stderr, "[smooth:trace] pictures field %llu deflicker packet %016llx\n",
                             static_cast<unsigned long long>(frameField),
                             static_cast<unsigned long long>(g_startHash));
        }
        const unsigned index = queued++;
        const uint64_t tag = g_read ? gsPictureTag(frameField, index + 1u, count) : 0u;
        if (g_read && info) {
            dc::Timed timed = *info;
            timed.queuedMs = dc::nowMs();
            dc::announce(tag, timed);               // before the picture can be read
        }
        ps2x::PictureDevice::Picture picture;
        picture.frame = g_newFrames;
        picture.head = headFor(t);
        picture.body = body;
        picture.tail = deflicker;
        picture.hashBody = g_device2Check && index == 0u;
        picture.done = [runtime, shared = deviceFrame, tag, frame = g_newFrames, read = g_read,
                        check = picture.hashBody](const ps2x::PictureDevice::Result &result) {
            if (check && result.drawn) deviceCheck(frame, false, 0u, result.bodyHash, result.bodyBytes);
            if (!read) return;
            // Amendment v3.9: the read is refused when the three reads before it are unfinished on the graphics
            // card (nothing waits for the card on a picture's behalf): the picture is cut off, as below.
            if (result.drawn && runtime->pictureGs().submitPictureRead(shared->captured, tag))
                return;
            // Given up or dropped (the next frame's end or its page copy came first), cut off before its
            // drawing or at its read (no place for its read): it is reported as not shown, so its frame's
            // account closes.
            dc::Timed timed;
            if (!dc::take(tag, timed)) return;
            if (dc::logTimeline())
                std::fprintf(stderr, "[smooth:cut] refresh=%lld at=%.3f frame=%u t=%.5f\n",
                             static_cast<long long>(timed.refresh), timed.atMs, timed.frame, timed.t);
            dc::Outcome outcome;
            outcome.timed = timed;
            outcome.startedMs = outcome.readyMs = dc::nowMs();
            outcome.givenUp = true;
            dc::report(outcome);
        };
        runtime->pictureDevice().submit(std::move(picture));
    };
    if (g_blendedOnly) {
        const bool haveBefore = g_beforeFrame + 1u == g_newFrames && !g_listBefore.stream.empty();
        dw5xl::smooth::BuildStats stats;
        std::vector<uint8_t> atZero, beforeOnly;
        if (haveBefore) dw5xl::smooth::build(g_list, g_now, g_before, 0.0f, atZero, &stats);
        if (haveBefore && !stats.cut) {
            dw5xl::smooth::SharedStats shared;
            const auto t0 = Clock::now();
            dw5xl::smooth::sharedOnly(g_listBefore, g_list, atZero, beforeOnly, &shared, &g_now, &g_before);
            const auto add = [](dw5xl::smooth::SharedStats::Why &to, const dw5xl::smooth::SharedStats::Why &from) {
                to.fresh += from.fresh;
                to.jumped += from.jumped;
                to.outOfPlace += from.outOfPlace;
                to.program += from.program;
                to.different += from.different;
            };
            add(g_shared.objects, shared.objects);
            add(g_shared.cameras, shared.cameras);
            add(g_shared.bones, shared.bones);
            if (shared.hasExample && g_shared.examples++ < 6u) {
                static const char *const kinds[3] = {"object block", "camera block", "bone packet"};
                std::fprintf(stderr, "[smooth] blended content only: a %s of the frame sent in field %llu is not the frame "
                             "before's at t = 0: %u words differ, the first is word %u (%08x, in the list before %08x)\n",
                             kinds[shared.exampleKind % 3u],
                             static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), shared.exampleWords,
                             shared.exampleWord, shared.exampleNow, shared.exampleBefore);
            }
            g_shared.ms += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            count = 2u;
            finished(0.0f, beforeOnly);
            finished(0.0f, atZero);
            ++g_shared.frames;
            g_shared.objectBlocks += shared.objectBlocks;
            g_shared.cameraBlocks += shared.cameraBlocks;
            g_shared.bonePackets += shared.bonePackets;
            g_shared.otherInline += shared.otherInline;
            g_shared.otherReferences += shared.otherReferences;
            g_shared.otherInlineBytes += shared.otherInlineBytes;
            g_shared.otherReferenceBytes += shared.otherReferenceBytes;
            g_shared.partsNow += shared.partsNow;
            g_shared.partsBefore += shared.partsBefore;
            g_shared.shared += shared.shared;
            g_shared.bytesNow += shared.bytesNow;
            g_shared.sharedBytes += shared.sharedBytes;
        } else if (haveBefore) {
            ++g_shared.cuts;
        }
    } else {
        const auto make = [&](float t, const dc::Timed *info = nullptr) {
            std::vector<uint8_t> built;
            const auto t0 = Clock::now();
            dw5xl::smooth::BuildStats stats;
            dw5xl::smooth::build(g_list, g_now, g_before, t, built, &stats);
            g_built.ms += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            ++g_built.pictures;
            g_built.objects += stats.objects;
            g_built.bones += stats.bones;
            g_built.cameraCopies += stats.cameraCopies;
            g_built.unmatched += stats.unmatched;
            g_built.jumped += stats.jumped;
            g_built.cuts += stats.cut ? 1u : 0u;
            finished(t, built, info);
        };
        dc::Grid grid;
        if (g_show && g_fixedT < 0.0f && g_read && haveInstant && dc::published(grid)) {
            // Amendment v3.5: the pictures the time line asks for, each for one instant on screen.
            dw5xl::smooth::Display display;
            display.follows = grid.follows;
            display.period = grid.periodMs;
            display.origin = grid.originMs;
            const double fieldMs = at.fieldMs;
            // What the window reported since the last frame: every picture feeds the head start's measure and its
            // frame's note.
            // The line starts (again) with this frame: the frames did not follow one another (another scene in
            // between), or the game's clock broke (g_clock). Pictures announced before are void, and what they and
            // the first frame with pictures afterwards report is not measured: they were made across the break, or
            // with caches that had to be filled first (measured 2026-11-11: 13.3 ms for the first picture after a
            // change of render scale, 6.5 ms for those after it).
            const bool restart = g_lastTimedFrame + 1u != g_newFrames || g_clock.pending;
            g_clock.pending = false;
            if (restart) {
                dc::voidFrom(0.0);
                g_line.reset();
                g_lineFrom = g_newFrames;
                g_measureFrom = g_newFrames + 2u;   // this frame gets no pictures (the line's first), the next the first ones
            }
            {
                std::vector<dc::Outcome> outcomes;
                dc::takeOutcomes(outcomes);
                for (const dc::Outcome &o : outcomes) {
                    if (o.timed.frame < g_lineFrom) continue;      // made before the line started again
                    const bool measure = o.timed.frame >= g_measureFrom && !o.givenUp;
                    // The list is taken at the frame end, before its instant: a frame finished late in its field needs
                    // a larger head start, which is the meter's matter.
                    if (measure)
                        g_meter.add(listMs, o.timed.frame, o.timed.headStartMs, o.timed.atMs - (o.readyMs + o.handMs));
                    g_auto.shown += o.shown ? 1u : 0u;
                    g_auto.late += o.shown || o.givenUp ? 0u : 1u;
                    g_auto.cutOff += o.givenUp ? 1u : 0u;
                    for (FrameNote &note : g_notes) {
                        if (note.frame != o.timed.frame) continue;
                        if (note.reported == 0u || o.startedMs < note.firstStarted) note.firstStarted = o.startedMs;
                        if (note.reported == 0u || o.startedMs > note.lastStarted) note.lastStarted = o.startedMs;
                        if (note.reported == 0u || o.timed.queuedMs < note.firstQueued) note.firstQueued = o.timed.queuedMs;
                        note.cutOff += o.givenUp ? 1u : 0u;
                        ++note.reported;
                        break;
                    }
                }
            }
            // Frames whose pictures are all reported and whose next list has come (this one, for the newest note).
            g_rate.configure(grid.follows, grid.periodMs, fieldMs, static_cast<int>(g_copies));
            if (restart) {                          // the line starts (again): its first seconds are not judged
                g_notes.clear();
                g_judgeFrom = g_newFrames + 250u;    // measured 2026-11-11: the first seconds after a state load are rough
            }
            g_lastTimedFrame = g_newFrames;
            const double reserve = fieldMs / 4.0;
            {
                // What one picture costs on its device, from the pictures done since the last frame. Its VIF1/VU1
                // work and the GS's reading of its packets run side by side: the longer one.
                const ps2x::PictureDevice::Totals totals = runtime->pictureDevice().totals();
                const uint64_t ns = totals.workNs, count = totals.pictures;
                const uint64_t sideNs = runtime->gifArbiter().sideTotals().ns;
                if (count > g_deviceCount) {
                    const double cost = static_cast<double>(std::max(ns - g_deviceNs, sideNs - g_sideNs)) / 1e6 /
                                        static_cast<double>(count - g_deviceCount);
                    g_pictureCost = g_pictureCost > 0.0 ? g_pictureCost + (cost - g_pictureCost) * 0.1 : cost;
                }
                g_deviceNs = ns;
                g_deviceCount = count;
                g_sideNs = sideNs;
            }
            while (!g_notes.empty()) {
                const FrameNote &note = g_notes.front();
                const bool last = g_notes.size() == 1u;
                if (note.reported < note.asked && g_notes.size() <= 50u) break;   // still waiting (void pictures never come)
                const double nextInstant = last ? instantMs : g_notes[1].instantMs, nextList = last ? listMs : g_notes[1].listMs;
                const double nextSpare = last ? spareMs : g_notes[1].spareMs;
                const bool follows = (last ? g_newFrames : g_notes[1].frame) == note.frame + 1u;
                if (note.frame < g_judgeFrom || !follows) {  // the line's first frames, or a break in the frames: no verdict
                    g_notes.pop_front();
                    if (last) break;
                    continue;
                }
                dw5xl::smooth::RateChoice::Frame f;
                // The frame the game worked on while this frame's pictures were drawn is the next one: did it keep its reserve?
                // A frame whose pictures did not all fit counts as well: the rate asks for more than this PC draws in
                // that time. That is a frame with a picture that was cut off (v3.8).
                const bool held = nextSpare < reserve || nextInstant - note.instantMs > 1.5 * fieldMs;
                f.heldUp = held;
                f.cutOff = note.cutOff != 0u;
                f.spare = nextSpare;
                f.reserve = reserve;
                g_auto.leastSpare = std::min(g_auto.leastSpare, nextSpare);
                // Room: how long before the next frame's end (nextList) the frame's last picture left the pictures'
                // device. A frame's pictures have until then (v3.7): the next frame's start is taken there, and its
                // pages are copied soon after.
                f.room = note.reported == 0u ? fieldMs : nextList - note.lastStarted;
                f.cost = g_pictureCost;
                g_rate.frame(f);
                ++g_auto.frames;
                g_auto.heldUp += held ? 1u : 0u;
                g_auto.cutFrames += note.cutOff != 0u ? 1u : 0u;
                if (note.reported != 0u) {
                    g_auto.leastMargin = std::min(g_auto.leastMargin, f.room);
                    g_auto.marginSum += f.room;
                    g_auto.past += f.room < 0.0 ? 1u : 0u;
                    g_auto.costSum += f.cost;
                    ++g_auto.costs;
                }
                g_notes.pop_front();
                if (last) break;
            }
            const bool rateOn = g_step > 0 || g_perFrame > 0 || g_rate.on();
            if (g_loggedLevel != g_rate.level()) {  // criterion 7h reads these: the level from this frame on (0: off)
                g_loggedLevel = g_rate.level();
                if (dc::logTimeline())
                    std::fprintf(stderr, "[smooth:level] frame=%llu level=%d of=%d\n",
                                 static_cast<unsigned long long>(g_newFrames), g_rate.level() + 1, g_rate.levels());
            }
            g_line.setHeadStart(g_headFixed ? g_headStart : g_meter.wanted(), !g_headFixed && g_meter.warmingUp());
            g_line.setStep(g_step > 0 ? g_step : g_rate.step());
            g_line.setPerFrame(g_perFrame > 0 ? g_perFrame : g_rate.perFrame());
            std::vector<dw5xl::smooth::Picture> asked;
            const double moved = g_line.frame(static_cast<uint32_t>(g_newFrames), instantMs, fieldMs, display, asked);
            if (!rateOn) asked.clear();             // "never slower": not even the fewest pictures fit; the window shows fields
            if (moved >= 0.0) {
                dc::voidFrom(moved);                // the line moved on: earlier pictures for later instants are void
                ++g_asked.moved;
            }
            if (asked.size() > g_copies) {
                g_asked.capped += asked.size() - g_copies;
                asked.resize(g_copies);
            }
            // On their own device the pictures are in nobody's way (v3.8, change 1): every picture the time line
            // asks for is started and nothing is foretold. One that is not ready at its instant is left out by the
            // window and tells the head start's measure what it needed; one still being drawn when the next
            // frame's pictures need the device is cut off there (PictureDevice) and counts for the rate choice.
            ++g_asked.frames;
            g_asked.none += asked.empty() ? 1u : 0u;
            if (!asked.empty() && g_loggedStep != (grid.follows ? -g_line.perFrame() : g_line.step())) {
                g_loggedStep = grid.follows ? -g_line.perFrame() : g_line.step();
                if (dc::logTimeline())
                    std::fprintf(stderr, "[smooth:rate] refresh=%lld step=%d\n", static_cast<long long>(asked.front().refresh),
                                 grid.follows ? 1 : g_line.step());
            }
            {
                FrameNote note;
                note.frame = g_newFrames;
                note.instantMs = instantMs;
                note.listMs = listMs;
                note.asked = static_cast<unsigned>(asked.size());
                note.spareMs = spareMs;
                g_notes.push_back(note);
            }
            count = static_cast<unsigned>(asked.size());
            for (const dw5xl::smooth::Picture &picture : asked) {
                dc::Timed info;
                info.atMs = picture.at;
                info.refresh = picture.refresh;
                info.frame = picture.frame;
                info.t = picture.t;
                info.own = picture.own;
                info.instantMs = instantMs;
                info.listMs = listMs;
                info.headStartMs = g_line.headStart();
                make(static_cast<float>(picture.t), &info);
                ++g_asked.pictures;
                g_asked.own += picture.own ? 1u : 0u;
            }
            if (g_asked.frames % 250u == 0u) {
                std::fprintf(stderr, "[smooth:line] %llu frames: %llu pictures asked for (%llu exactly on a game frame), %llu "
                             "more than the prototype's count, %llu frames without a picture, the line moved on %llu times; "
                             "head start %.2f ms\n",
                             static_cast<unsigned long long>(g_asked.frames), static_cast<unsigned long long>(g_asked.pictures),
                             static_cast<unsigned long long>(g_asked.own), static_cast<unsigned long long>(g_asked.capped),
                             static_cast<unsigned long long>(g_asked.none), static_cast<unsigned long long>(g_asked.moved),
                             g_line.headStart());
                g_asked = Asked{};
            }
            if (g_auto.frames >= 250u) {
                const double with = static_cast<double>(std::max<uint64_t>(g_auto.costs, 1u));
                // "0 not started for lack of room": the count stays in the line for the scripts that read it. No
                // picture is kept from starting, so it is always 0.
                std::fprintf(stderr, "[smooth:auto] %llu frames: level %d of %d (%s%d), head start %.2f ms (wanted %.2f, one "
                             "picture needed %.2f at most%s); "
                             "pictures shown %llu, too late %llu, 0 not started for lack of room, %llu cut off in "
                             "%llu frames; the game held up in "
                             "%llu frames; a frame's last picture left "
                             "the device %.2f ms before the next field began on average (%.2f at least, %llu frames past it); a "
                             "picture costs %.2f ms on the device; the game finished its frames with %.2f ms to spare at least "
                             "(its reserve: %.2f)\n",
                             static_cast<unsigned long long>(g_auto.frames), g_rate.level() + 1, g_rate.levels(),
                             !rateOn ? "off, step " : grid.follows ? "pictures per frame " : "step ",
                             grid.follows ? g_line.perFrame() : g_line.step(), g_line.headStart(), g_meter.wanted(),
                             g_meter.largest(),
                             g_headFixed ? ", fixed by a switch" : "", static_cast<unsigned long long>(g_auto.shown),
                             static_cast<unsigned long long>(g_auto.late),
                             static_cast<unsigned long long>(g_auto.cutOff), static_cast<unsigned long long>(g_auto.cutFrames),
                             static_cast<unsigned long long>(g_auto.heldUp),
                             g_auto.marginSum / with, g_auto.costs ? g_auto.leastMargin : 0.0,
                             static_cast<unsigned long long>(g_auto.past), g_auto.costSum / with, g_auto.leastSpare, reserve);
                g_auto = Auto{};
            }
        } else {
            // DW5XL-DEBUG PS2X_SMOOTH_EVERY=<n>: pictures only for every n-th frame (for the proofs at a scale where
            // a picture before every game frame does not fit and the game would miss fields).
            static const unsigned every = [] {
                const char *v = std::getenv("PS2X_SMOOTH_EVERY");
                return v && *v ? static_cast<unsigned>(std::max(1, std::atoi(v))) : 1u;
            }();
            const unsigned made = g_newFrames % every == 0u ? g_copies : 0u;
            count = made;
            for (unsigned i = 0; i < made; ++i) {
                // Picture i of n between the frame before (t = 0) and this frame (t = 1).
                make(g_fixedT >= 0.0f ? g_fixedT : static_cast<float>(i + 1u) / static_cast<float>(g_copies + 1u));
            }
        }
    }
    const auto keepForNextFrame = [] {
        g_before = std::move(g_now);
        g_beforeFrame = g_newFrames;
        if (g_blendedOnly) g_listBefore = std::move(g_list);
    };
    if (queued == 0u) {                             // nothing was drawn
        keepForNextFrame();
        return;
    }
    if ((g_frames + 1u) % 250u == 0u) {
        const ps2x::PictureDevice::Totals now = runtime->pictureDevice().totals();
        const ps2x::PictureDevice::Totals &was = g_device2Before;
        const GifArbiter::SideTotals side = runtime->gifArbiter().sideTotals();
        const double n = static_cast<double>(std::max<uint64_t>(now.pictures - was.pictures, 1u));
        std::fprintf(stderr, "[smooth:device2] %llu pictures on their own device, %llu given up, %llu dropped on the GS "
                     "side; per picture: VIF1 and VU1 %.3f ms, the GS's reading of its packets %.3f ms (%.0f packets; the "
                     "game went first %.2f times), %.2f MB of packets\n",
                     static_cast<unsigned long long>(now.pictures - was.pictures),
                     static_cast<unsigned long long>(now.givenUp - was.givenUp),
                     static_cast<unsigned long long>(now.dropped - was.dropped),
                     static_cast<double>(now.workNs - was.workNs) / 1e6 / n,
                     static_cast<double>(side.ns - g_sideBefore.ns) / 1e6 / n,
                     static_cast<double>(side.packets - g_sideBefore.packets) / n,
                     static_cast<double>(side.gaveWay - g_sideBefore.gaveWay) / n,
                     static_cast<double>(now.bytes - was.bytes) / 1048576.0 / n);
        g_device2Before = now;
        g_sideBefore = side;
    }
    if (++g_frames % 250u == 0u) {
        std::fprintf(stderr, "[smooth] %llu frames with %u pictures in place (the list), %llu not copied, %llu without "
                     "a frame start, field %llu; matrix tags per frame %.1f, recordings outside the list %.1f\n",
                     static_cast<unsigned long long>(g_frames), g_copies, static_cast<unsigned long long>(g_failed),
                     static_cast<unsigned long long>(g_noStart),
                     static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), g_tagged / 250.0,
                     g_tagsMissing / 250.0);
        g_tagged = g_tagsMissing = 0;
        if (g_built.pictures != 0u) {
            const double n = static_cast<double>(g_built.pictures);
            std::fprintf(stderr, "[smooth] in-between values per picture: %.1f object matrices, %.1f bone matrices, %.1f "
                         "camera copies; left at the newest value: %.1f without a partner, %.1f that jumped; camera cuts "
                         "%llu of %llu pictures; build %.3f ms per picture\n", g_built.objects / n, g_built.bones / n,
                         g_built.cameraCopies / n, g_built.unmatched / n, g_built.jumped / n,
                         static_cast<unsigned long long>(g_built.cuts), static_cast<unsigned long long>(g_built.pictures),
                         g_built.ms / n);
            g_built = Built{};
            if (g_copied != 0u)
                std::fprintf(stderr, "[smooth] per frame: copying the list %.3f ms, reading its values %.3f ms\n",
                             g_copyMs / static_cast<double>(g_copied), g_valuesMs / static_cast<double>(g_copied));
            g_copyMs = g_valuesMs = 0.0;
            g_copied = 0;
        }
        if (g_shared.frames != 0u || g_shared.cuts != 0u) {
            const double n = static_cast<double>(std::max<uint64_t>(g_shared.frames, 1u));
            std::fprintf(stderr, "[smooth] blended content only: %llu frames; per frame %.1f parts in the list, %.1f in the "
                         "list before, %.1f shared (%.1f %% of the list's bytes); %llu frames left out after a camera "
                         "cut\n", static_cast<unsigned long long>(g_shared.frames), g_shared.partsNow / n,
                         g_shared.partsBefore / n, g_shared.shared / n,
                         g_shared.bytesNow ? 100.0 * static_cast<double>(g_shared.sharedBytes) / static_cast<double>(g_shared.bytesNow) : 0.0,
                         static_cast<unsigned long long>(g_shared.cuts));
            std::fprintf(stderr, "[smooth] blended content only, not shared in %llu frames: %llu object blocks, %llu camera "
                         "blocks, %llu bone packets; per frame %.1f other inline blocks (%.0f bytes), %.1f other references "
                         "(%.0f bytes); %.2f ms per frame to find them\n",
                         static_cast<unsigned long long>(g_shared.frames), static_cast<unsigned long long>(g_shared.objectBlocks),
                         static_cast<unsigned long long>(g_shared.cameraBlocks),
                         static_cast<unsigned long long>(g_shared.bonePackets), g_shared.otherInline / n,
                         g_shared.otherInlineBytes / n, g_shared.otherReferences / n, g_shared.otherReferenceBytes / n,
                         g_shared.ms / n);
            const auto why = [](const char *name, const dw5xl::smooth::SharedStats::Why &w) {
                std::fprintf(stderr, "[smooth] blended content only, %s not shared: %u new, %u jumped, %u out of place, %u "
                             "with another draw program, %u different\n", name, w.fresh, w.jumped, w.outOfPlace, w.program,
                             w.different);
            };
            why("object blocks", g_shared.objects);
            why("camera blocks", g_shared.cameras);
            why("bone packets", g_shared.bones);
            const uint64_t examples = g_shared.examples;
            g_shared = SharedTotals{};
            g_shared.examples = examples;
        }
    }
    keepForNextFrame();
}

// Amendment v3.6: the game has finished a frame (the wait in FUN_001425d0). The list is closed and flagged for the
// display thread, which sends it at the next field boundary: that field's start is the frame's instant, known now.
void frameEnd(uint8_t *ram, PS2Runtime *runtime)
{
    namespace dc = ps2x::display_clock;
    const double doneMs = dc::nowMs();
    PS2Memory &mem = runtime->memory();
    // As the display thread takes it (FUN_00142860): the builder being written, its list's start, nothing for an
    // empty list.
    const uint32_t builder = kBuilders + (mem.read32(kBufferIndex) & 1u) * 0x20u;
    const uint32_t list = mem.read32(builder + 4u) & 0x0FFFFFFFu;
    if ((list & 0x01FFFFFFu) + 2u > PS2_RAM_SIZE) return;
    uint16_t first;
    std::memcpy(&first, ram + (list & 0x01FFFFFFu), 2);
    if (first == 0u) return;
    std::chrono::steady_clock::time_point fieldStart{};
    uint64_t fieldNumber = 0;
    std::chrono::nanoseconds fieldLength{};
    if (!runtime->eeScheduler().lastFieldStart(fieldStart, fieldNumber, fieldLength)) return;
    const double fieldMs = std::chrono::duration<double, std::milli>(fieldLength).count();
    const bool tte = (mem.readIORegister(kVif1Chcr) & 0x40u) != 0u;   // as the last send; checked at this list's send
    const uint64_t hash = g_hashLists ? listHash(ram, list, tte) : 0u;
    const dw5xl::smooth::FinishedFrames::Coming coming = g_finished.finished(list, hash, fieldNumber, dc::toMs(fieldStart), fieldMs);
    if (coming.again) return;
    if (dc::logTimeline())
        std::fprintf(stderr, "[smooth:end] frame=%u field=%llu at=%.3f done=%.3f hash=%016llx\n", coming.number,
                     static_cast<unsigned long long>(coming.field), coming.instant, doneMs,
                     static_cast<unsigned long long>(hash));
    g_endTte = tte;
    g_endFirstTagMs = g_firstTagMs;
    g_endLastTagMs = g_lastTagMs;
    g_firstTagMs = 0.0;
    g_usedEnvValid = false;
    if (measureOnly()) return;
    FrameAt at;
    at.number = coming.number;
    at.haveInstant = true;
    at.instantMs = coming.instant;
    at.fieldMs = fieldMs;
    at.sendField = coming.field;
    at.takenMs = doneMs;
    at.spareMs = std::max(0.0, coming.instant - doneMs);   // the game's spare time: from here to its field
    makePictures(ram, runtime, list, tte, at);
}

// The game sends a new frame's list (the display thread, at a field's start).
// DW5XL-DEBUG PS2X_HOLD_TRACE=1 (fact-finding 2026-11-12: what holds the game up at 4x): one line per game frame at
// the game's send, "[smooth:hold]". Times in ms; a/b/c is total / how many / the longest single one, all since the
// line before.
//   late       the send, after its field began            start      the frame start (its first packet), after the same
//   pre_dev, pre_gs   this thread's waits in drains (for its device's worker, for the GS thread) from the send
//                     before to the frame start;   dev, gs: from the frame start to the send
//   game, call, pic   the GS thread: the game's packets, calls in the game's GS order, the pictures' packets
//   lw_*, lh_*        the GS backend's device lock: waited for it / held it, by what the thread was doing (other: the
//                     game's threads; game, pic, call: the GS thread at such a packet or call; present: the window)
//   copy, scanout, read   the page copy into the picture GS, a field's scanout, a picture's read (start to end)
//   rflush, rvsync        inside a picture's read: the flush before it, paraLLEl-GS's vsync,
//   rslots, rbuffer, rsubmit, rwait   the ring's slots, the read's buffer, the submit, the wait at its end
//   ask_*                 questions "is the card done with this read?" (a read's fence, no timeout), by who asked:
//                         as lw_* (pic: the GS thread at a picture's packet or its start; call: at a call, as a read)
double g_holdStartMs = 0.0;
uint64_t g_holdStartDevice = 0, g_holdStartGs = 0, g_holdSentDevice = 0, g_holdSentGs = 0;
void holdLine(uint32_t frame, uint64_t field, double instantMs, double sentMs)
{
    namespace hold = ps2x::hold;
    constexpr size_t kCount = 4u * hold::Roles + 11u;
    static std::array<hold::Value, kCount> before{};
    hold::Counters &c = hold::counters();
    std::array<hold::Value, kCount> now{};
    size_t n = 0;
    for (auto *group : {&c.packets, &c.lockWait, &c.lockHeld})
        for (hold::Counter &counter : *group) now[n++] = hold::take(counter);
    now[n++] = hold::take(c.copyPages);
    now[n++] = hold::take(c.fieldScanout);
    now[n++] = hold::take(c.pictureRead);
    now[n++] = hold::take(c.readFlush);
    now[n++] = hold::take(c.readVsync);
    now[n++] = hold::take(c.readSlots);
    now[n++] = hold::take(c.readBuffer);
    now[n++] = hold::take(c.readSubmit);
    now[n++] = hold::take(c.readWait);
    for (hold::Counter &counter : c.fenceAsk) now[n++] = hold::take(counter);
    now[n++] = hold::take(c.contextMoved);
    now[n++] = hold::take(c.contextLeft);
    const auto ms = [](uint64_t ns) { return static_cast<double>(ns) / 1e6; };
    char parts[kCount][40];
    for (size_t i = 0; i < kCount; ++i)
        std::snprintf(parts[i], sizeof(parts[i]), "%.2f/%llu/%.2f", ms(now[i].ns - before[i].ns),
                      static_cast<unsigned long long>(now[i].count - before[i].count), ms(now[i].maxNs));
    before = now;
    const size_t P = 0u, W = hold::Roles, H = 2u * hold::Roles, F = 3u * hold::Roles, A = F + 9u;
    std::fprintf(stderr, "[smooth:hold] frame=%u field=%llu late=%.2f start=%.2f pre_dev=%.2f pre_gs=%.2f dev=%.2f gs=%.2f "
                 "game=%s call=%s pic=%s lw_other=%s lw_game=%s lw_pic=%s lw_present=%s lw_call=%s lh_other=%s lh_game=%s "
                 "lh_pic=%s lh_present=%s lh_call=%s copy=%s scanout=%s read=%s rflush=%s rvsync=%s rslots=%s rbuffer=%s "
                 "rsubmit=%s rwait=%s ask_other=%s ask_game=%s ask_pic=%s ask_present=%s ask_call=%s ctx_moved=%s ctx_left=%s\n",
                 frame, static_cast<unsigned long long>(field), sentMs - instantMs, g_holdStartMs - instantMs,
                 ms(g_holdStartDevice - g_holdSentDevice), ms(g_holdStartGs - g_holdSentGs),
                 ms(hold::t_deviceWaitNs - g_holdStartDevice), ms(hold::t_gsWaitNs - g_holdStartGs),
                 parts[P + hold::GameGs], parts[P + hold::GsCall], parts[P + hold::PictureGs],
                 parts[W + hold::Other], parts[W + hold::GameGs], parts[W + hold::PictureGs], parts[W + hold::Present],
                 parts[W + hold::GsCall], parts[H + hold::Other], parts[H + hold::GameGs], parts[H + hold::PictureGs],
                 parts[H + hold::Present], parts[H + hold::GsCall], parts[F], parts[F + 1u], parts[F + 2u], parts[F + 3u],
                 parts[F + 4u], parts[F + 5u], parts[F + 6u], parts[F + 7u], parts[F + 8u],
                 parts[A + hold::Other], parts[A + hold::GameGs], parts[A + hold::PictureGs], parts[A + hold::Present],
                 parts[A + hold::GsCall], parts[A + hold::Roles], parts[A + hold::Roles + 1u]);
    g_holdSentDevice = hold::t_deviceWaitNs;
    g_holdSentGs = hold::t_gsWaitNs;
}

void atSend(uint8_t *ram, PS2Runtime *runtime, uint32_t list)
{
    namespace dc = ps2x::display_clock;
    const bool tte = (runtime->memory().readIORegister(kVif1Chcr) & 0x40u) != 0u;
    // Amendment v3.5: this frame's instant on the game's time line is the start of the field its list is sent in
    // (the PS2 field clock). PS2X_SMOOTH_TIMELINE=1 logs it for the 049 test.
    std::chrono::steady_clock::time_point fieldStart{};
    uint64_t fieldNumber = 0;
    std::chrono::nanoseconds fieldLength{};
    const bool haveInstant = runtime->eeScheduler().lastFieldStart(fieldStart, fieldNumber, fieldLength);
    const double instantMs = haveInstant ? dc::toMs(fieldStart) : 0.0, listMs = dc::nowMs();
    const uint64_t hash = g_hashLists ? listHash(ram, list, tte) : 0u;
    const dw5xl::smooth::FinishedFrames::Sent sent = g_finished.sent(list & 0x0FFFFFFFu, hash, fieldNumber);
    const bool ended = sent.finished;                   // the frame came through the frame end
    if (g_device2Check) {
        // Criterion 6d: the game's own list ends here in device order; its PATH1/PATH2 packets are hashed up to here.
        auto mark = [frame = static_cast<uint64_t>(sent.number), fieldNumber, ended] {
            g_tapOn = false;
            if (ended) deviceCheck(frame, true, fieldNumber, g_tapHash.hash, g_tapHash.bytes);
        };
        if (!ps2x::device::submitGsCall(runtime->memory(), mark)) mark();
    }
    const double firstTag = ended ? g_endFirstTagMs : g_firstTagMs, lastTag = ended ? g_endLastTagMs : g_lastTagMs;
    if (haveInstant && dc::logTimeline())
        std::fprintf(stderr, "[smooth:list] frame=%u field=%llu at=%.3f sent=%.3f first=%.3f last=%.3f hash=%016llx\n",
                     sent.number, static_cast<unsigned long long>(fieldNumber), instantMs, listMs, firstTag, lastTag,
                     static_cast<unsigned long long>(hash));
    if (haveInstant && ps2x::hold::enabled())
        holdLine(sent.number, fieldNumber, instantMs, listMs);
    // What the frame end took as known in advance: the field, the list, the transfer mode, the draw environment.
    ++g_advance.frames;
    g_advance.noEnd += sent.finished ? 0u : 1u;
    g_advance.wrongField += sent.finished && !sent.sameField ? 1u : 0u;
    g_advance.wrongList += sent.finished && !sent.sameList ? 1u : 0u;
    g_advance.hashed += sent.finished && hash != 0u ? 1u : 0u;
    g_advance.wrongMode += sent.finished && tte != g_endTte ? 1u : 0u;
    if (sent.finished && g_usedEnvValid) g_advance.wrongEnv += g_env != g_usedEnv ? 1u : 0u;
    g_usedEnvValid = false;
    if (!sent.finished) g_firstTagMs = 0.0;
    if (g_advance.frames % 250u == 0u) {
        std::fprintf(stderr, "[smooth] known in advance, %llu frames: %llu sent in another field, %llu with another draw "
                     "environment, %llu with another transfer mode, %llu lists changed after the frame end (of %llu "
                     "compared), %llu sent without a frame end\n",
                     static_cast<unsigned long long>(g_advance.frames), static_cast<unsigned long long>(g_advance.wrongField),
                     static_cast<unsigned long long>(g_advance.wrongEnv), static_cast<unsigned long long>(g_advance.wrongMode),
                     static_cast<unsigned long long>(g_advance.wrongList), static_cast<unsigned long long>(g_advance.hashed),
                     static_cast<unsigned long long>(g_advance.noEnd));
        g_advance = Advance{};
    }
}

void waitSema(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    // Before the original: the game thread is paused inside it and does not come back through this hook.
    if (ctx->pc != kWaitSemaResume && getRegU32(ctx, 31) == kFrameEndWait) frameEnd(ram, runtime);
    originalWaitSema(ram, ctx, runtime);
}

void send(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31), list = getRegU32(ctx, 5);
    const bool newFrame = ra == kSendNewFrame;
    if (newFrame && g_device2Check) {
        // Criterion 6d: the game's own list starts here in device order (its end: atSend).
        auto mark = [] {
            g_tapHash = ps2x::GifStreamHash{};
            g_tapOn = true;
        };
        if (!ps2x::device::submitGsCall(runtime->memory(), mark)) mark();
    }
    originalSend(ram, ctx, runtime);
    if (newFrame) atSend(ram, runtime, list);
}

void sendN(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31);
    if (ra == kStartSendA || ra == kStartSendB) {
        if (ps2x::hold::enabled()) {                // DW5XL-DEBUG PS2X_HOLD_TRACE: the frame start begins here
            g_holdStartMs = ps2x::display_clock::nowMs();
            g_holdStartDevice = ps2x::hold::t_deviceWaitNs;
            g_holdStartGs = ps2x::hold::t_gsWaitNs;
        }
        // The battle's frame start (FUN_001429e0 with the half-line offset set per frame) sends from kStartSendB.
        g_battleStartPrevious = g_battleStart;
        g_battleStart = ra == kStartSendB;
        captureStart(ram, getRegU32(ctx, 5), getRegU32(ctx, 6));
        if (g_trace)
            std::fprintf(stderr, "[smooth:trace] start field %llu packet %016llx at 0x%X\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()),
                         static_cast<unsigned long long>(g_startHash), getRegU32(ctx, 5) & 0x01FFFFF0u);
        // The picture GS is made at the first frame start of the run (its set-up takes about 0.3 s once), not at the
        // first frame that gets pictures.
        static bool gs2Made = false;
        if (!gs2Made) {
            gs2Made = true;
            ps2x::device::submitGsSideCall(runtime->memory(), [runtime] { runtime->syncPictureGs(true); });
        }
    }
    originalSendN(ram, ctx, runtime);
}

void swapEnv(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    // The field's draw environment packet: FUN_00111910(db, index) puts db + 0x50 (even index) or db + 0x140 (odd), and
    // FUN_001429e0 has finished writing it (the half-line offset included) before this call. It is read here, in this
    // one hook. The game thread can be paused inside the original function (FUN_00111590 waits for the GIF channel)
    // and continues there without coming back through this hook, so nothing may depend on state kept across the call:
    // a flag set here and read in a hook on FUN_00111590 was lost in about 1 frame in 1,600 (more when the GS was
    // behind), and that frame's pictures were drawn with the previous frame's environment, half a line off.
    if (getRegU32(ctx, 31) == kSwapEnvFromStart)
        captureEnv(ram, getRegU32(ctx, 4) + ((getRegU32(ctx, 5) & 1u) ? kEnvOdd : kEnvEven));
    originalSwapEnv(ram, ctx, runtime);
}

void install(PS2Runtime &runtime)
{
    const char *proto = std::getenv("PS2X_SMOOTH_PROTO");
    if (!proto || !*proto) return;
    char *rest = nullptr;
    g_copies = static_cast<unsigned>(std::strtoul(proto, &rest, 10));
    std::string mode(rest ? rest : "");
    const auto takeSuffix = [&mode](const char *suffix) {
        const size_t n = std::strlen(suffix);
        if (mode.size() < n || mode.compare(mode.size() - n, n, suffix) != 0) return false;
        mode.resize(mode.size() - n);
        return true;
    };
    g_read = takeSuffix(":read");
    if (const char *t = std::getenv("PS2X_SMOOTH_TRACE")) g_trace = *t && *t != '0';
    // PS2X_SMOOTH_FIXED_T=<t> (the spec's test switch): every in-between picture at this t; 1 = an unchanged copy of
    // the frame, 0 = the frame with the frame before's values. Without it picture i of n is at t = i / (n + 1).
    if (const char *b = std::getenv("PS2X_SMOOTH_BLENDED_ONLY")) g_blendedOnly = *b && *b != '0';
    if (const char *v = std::getenv("PS2X_SMOOTH_SHOW")) g_show = *v && *v != '0';
    if (const char *v = std::getenv("PS2X_SMOOTH_HEAD_START"); v && *v) {
        g_headFixed = true;
        g_headStart = std::strtod(v, nullptr);
    }
    if (const char *v = std::getenv("PS2X_SMOOTH_LIST_HASH")) g_hashLists = *v && *v != '0';
    if (const char *v = std::getenv("PS2X_SMOOTH_GS2_CHECK"); v && *v) g_gs2Check = static_cast<unsigned>(std::max(0, std::atoi(v)));
    if (const char *v = std::getenv("PS2X_SMOOTH_STEP"); v && *v) g_step = std::max(1, std::atoi(v));
    if (const char *v = std::getenv("PS2X_SMOOTH_PER_FRAME"); v && *v) g_perFrame = std::max(1, std::atoi(v));
    if (const char *t = std::getenv("PS2X_SMOOTH_FIXED_T")) {
        if (*t) g_fixedT = std::min(1.0f, std::max(0.0f, std::strtof(t, nullptr)));
    }
    if (g_copies == 0u) return;
    // The switch's format is "<n>:rollback[:read]" (what the tests pass). The name is an old one: nothing is rolled
    // back. The other modes it had no longer exist: "<n>" alone and ":after" (plain copies of the list, drawn on
    // the game's GS), ":rollback:empty" and ":fullclear".
    if (mode != ":rollback") {
        std::fprintf(stderr, "[smooth] PS2X_SMOOTH_PROTO=%s: this mode no longer exists (the format is "
                     "<n>:rollback[:read]); no in-between pictures\n", proto);
        return;
    }
    if (const char *v = std::getenv("PS2X_SMOOTH_GS2"); v && (!*v || *v == '0'))
        std::fprintf(stderr, "[smooth] PS2X_SMOOTH_GS2=0: the rollback on the game's GS no longer exists; the pictures "
                     "are drawn on their own GS memory\n");
    if (runtime.memory().read32(kDmaSend) != 0x27BDFFD0u || runtime.memory().read32(kDmaSendN) != 0x27BDFFC0u ||
        runtime.memory().read32(kSwapEnv) != 0x27BDFFD0u ||
        runtime.memory().read32(kSwapEnvPutOdd) != (0x26240000u | kEnvOdd) ||
        runtime.memory().read32(kSwapEnvPutEven) != (0x26240000u | kEnvEven)) {
        std::fprintf(stderr, "[smooth] declined: unexpected code at a replaced function\n");
        return;
    }
    // The game's frame end, where the pictures are made: "addiu v1, zero, 0x44" (WaitSema's syscall number) and
    // "jal WaitSema" in FUN_001425d0. Checked before any function is replaced: without the frame end there are no
    // pictures, and then nothing of this file is installed.
    if (runtime.memory().read32(kWaitSema) != 0x24030044u || runtime.memory().read32(kFrameEndCall) != 0x0C047D68u) {
        std::fprintf(stderr, "[smooth] the frame end declined: its code is not the expected one; no in-between "
                     "pictures are made\n");
        return;
    }
    originalSend = runtime.lookupFunction(kDmaSend);
    runtime.replaceFunction(kDmaSend, &send);
    originalSendN = runtime.lookupFunction(kDmaSendN);
    runtime.replaceFunction(kDmaSendN, &sendN);
    originalSwapEnv = runtime.lookupFunction(kSwapEnv);
    runtime.replaceFunction(kSwapEnv, &swapEnv);
    originalWaitSema = runtime.lookupFunction(kWaitSema);
    runtime.replaceFunction(kWaitSema, &waitSema);
    if (runtime.memory().read32(kMatrixBlock) != 0x27BDFFB0u || runtime.memory().read32(kLitBlock) != 0x27BDFF70u ||
        runtime.memory().read32(kCameraBlock) != 0x27BDFFB0u) {
        std::fprintf(stderr, "[smooth] matrix tags declined: unexpected code at a list builder\n");
    } else {
        originalMatrixBlock = runtime.lookupFunction(kMatrixBlock);
        runtime.replaceFunction(kMatrixBlock, &matrixBlock);
        originalLitBlock = runtime.lookupFunction(kLitBlock);
        runtime.replaceFunction(kLitBlock, &litBlock);
        originalCameraBlock = runtime.lookupFunction(kCameraBlock);
        runtime.replaceFunction(kCameraBlock, &cameraBlock);
    }
    // PS2X_SMOOTH_AT_SEND=1 asked for pictures made at the send, right after the game's list: that order no longer
    // exists.
    if (const char *v = std::getenv("PS2X_SMOOTH_AT_SEND"); v && *v && *v != '0')
        std::fprintf(stderr, "[smooth] PS2X_SMOOTH_AT_SEND=1: pictures made at the send no longer exist; the pictures "
                     "are made at the game's frame end\n");
    std::fprintf(stderr, "[smooth] prototype: %u copies of each new frame's list, on the picture GS%s, made at the "
                 "game's frame end\n", g_copies, g_read ? ", each read out as an in-between picture" : "");
    std::fprintf(stderr, "[smooth] the pictures are drawn on a GS memory of their own (no rollback on the game's GS)\n");
    {
        // The pictures run on a VIF1/VU1 device of their own: their one path. PS2X_SMOOTH_DEVICE2=0 (or empty) asked
        // for the path through the game's device, which no longer exists; any other value changes nothing.
        if (const char *v = std::getenv("PS2X_SMOOTH_DEVICE2"); v && (!*v || *v == '0'))
            std::fprintf(stderr, "[smooth] PS2X_SMOOTH_DEVICE2=0: the pictures' path through the game's device no "
                         "longer exists; the pictures run on their own device\n");
        runtime.pictureDevice();                    // made here, before the game runs
        const char *c = std::getenv("PS2X_SMOOTH_DEVICE2_CHECK");
        g_device2Check = c && *c && *c != '0';
        if (g_device2Check)
            runtime.memory().m_gifTap = [](GifPathId path, const uint8_t *data, uint32_t size) {
                if (g_tapOn) g_tapHash.add(path, data, size);
            };
        std::fprintf(stderr, "[smooth] the pictures run on a VIF1/VU1 device of their own%s\n",
                     g_device2Check ? "; the GIF packets of every frame's list are compared with the game's device's" : "");
    }
    if (g_blendedOnly) {
        std::fprintf(stderr, "[smooth] blended content only: two pictures per frame, the list before and this list at t = 0, "
                     "each with only what the two lists share\n");
    } else if (g_fixedT >= 0.0f) {
        std::fprintf(stderr, "[smooth] every picture at t = %.3f between the frame before and the frame\n", g_fixedT);
    } else {
        std::fprintf(stderr, "[smooth] picture i of %u at t = i / %u between the frame before and the frame\n", g_copies,
                     g_copies + 1u);
    }
}
} // namespace

PS2_REGISTER_GAME_OVERRIDE("DW5XL in-between pictures (issue 049, P2.3 prototype)", "SLES_538.60", 0u, 0u, &install)
