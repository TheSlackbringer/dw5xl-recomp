// Issue 049 Part 2 (amendments v3.1 / v3.2, P2.4): a game frame's finished command list as data of our own.
//  - flatten(): the VIF1 stream the DMAC makes of the list's DMA chain, with a map of where each run of bytes came
//    from in game memory.
//  - Tags: where a known matrix block landed in the list, recorded while the game builds it (dw5xl_smoothing.cpp),
//    resolved to stream offsets here.
// No runtime or game access in this file's functions: they work on memory handed in, so they are unit-tested without
// a game (app/tests/smooth_list_test.cpp).
#ifndef DW5XL_SMOOTH_LIST_H
#define DW5XL_SMOOTH_LIST_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dw5xl::smooth {

// A run of stream bytes copied from game memory: `bytes` at `source` went to `offset` in the stream.
struct Span
{
    uint32_t source = 0; // game address of the first byte
    uint32_t offset = 0; // its place in the stream
    uint32_t bytes = 0;
    uint32_t tag = 0;    // game address of the DMA tag this run belongs to
    uint8_t id = 0;      // the tag's kind: 0 refe, 1 cnt, 2 next, 3 ref, 4 refs, 5 call, 6 ret, 7 end
};

// The blocks the game's list builders write with a matrix in them (their first quadword is their own "cnt" DMA tag).
enum class TagKind : uint8_t
{
    Matrix = 0,    // FUN_0019ced0: 8 quadwords; a 4x4 world matrix at +0x20, a colour scale at +0x60
    LitMatrix = 1, // FUN_0019cdd0: 15 quadwords; the world matrix at +0x20, light data made from it behind it
    Camera = 2,    // FUN_0019c550: 24 quadwords; four camera matrices at +0x20, +0x60, +0xA0, +0xE0
};

struct Tag
{
    TagKind kind = TagKind::Matrix;
    uint32_t block = 0;  // game address of the block (its DMA tag)
    uint32_t key = 0;    // what the matrix belongs to; the same thing has the same key in the next frame
    uint32_t model = 0;  // Matrix / LitMatrix: the model index beside the matrix (a changed model is another thing)
    uint32_t aux = 0;    // Matrix / LitMatrix: the builder's flags; Camera: its mode
    uint32_t offset = 0; // resolve(): where the block's first quadword after the DMA tag is in the stream
    uint64_t serial = 0; // order of recording
};

// A 4x4 matrix as the game stores it: four vectors of four floats, each a column of the matrix (libvu0); a
// transform's position is the fourth vector.
using Matrix = std::array<float, 16>;

// One camera's matrices in a frame (the game keeps them per view, 0x40 apart). The list holds copies of them: in the
// camera blocks and in other packets (the skinned characters' setup, effect packets).
struct Camera
{
    uint32_t view = 0;
    Matrix worldToView{};    // 0x4746E0: rotation and position only
    Matrix viewToWorld{};    // 0x474260: its inverse as the game keeps it (the camera's position is its fourth vector)
    Matrix screenProjView{}; // 0x474560
    Matrix clip{};           // 0x473F60
    Matrix projView{};       // 0x4743E0
    Matrix wideClip{};       // 0x473DE0
};

struct List
{
    uint32_t start = 0;            // game address of the chain's first tag
    bool tagHalves = false;        // the stream holds each tag's upper half before its data (CHCR.TTE)
    std::vector<uint8_t> stream;   // what VIF1 receives
    std::vector<Span> spans;       // in stream order
    std::vector<Tag> tags;         // resolved, in stream order
    std::vector<Camera> cameras;   // the cameras of this frame's camera blocks (a view can have more than one)
};

// A character's matrix buffer the list sends by reference (a ready VIF packet: UNPACK codes and bone matrices). The
// game keeps two per character and kind and writes them in turn, so the same bone has another address every frame.
struct BoneBuffer
{
    uint32_t base = 0;  // game address the reference points to
    uint16_t slot = 0;  // character slot
    uint8_t pair = 0;   // which of the slot's buffer pairs
};

// What a frame's picture is made from, by key: the same thing has the same key in the next frame.
struct Values
{
    struct Item
    {
        uint64_t key = 0;
        uint32_t offset = 0; // of the matrix in the frame's stream
        uint32_t model = 0;  // objects: a changed model is another thing
        Matrix matrix{};
        // Objects: the numbers behind the matrix in its block, blended number by number: the colour scale (1
        // quadword), and for a lit object its light directions and colours (8 quadwords).
        uint32_t extraQuadwords = 0;
        std::array<float, 32> extra{};
    };
    std::vector<Item> items; // sorted by key
    std::vector<Camera> cameras;
};

struct BuildStats
{
    uint32_t objects = 0;      // object matrices given an in-between value
    uint32_t bones = 0;        // bone matrices given an in-between value
    uint32_t cameraCopies = 0; // camera matrices and positions replaced in the stream
    uint32_t unmatched = 0;    // matrices without a partner in the frame before (left at their newest value)
    uint32_t jumped = 0;       // matrices that moved too far in one frame to be the same motion (left at newest)
    bool cut = false;          // the camera jumped: the whole picture is left at the newest frame
};

inline constexpr uint32_t kRamBytes = 32u * 1024u * 1024u;
inline constexpr uint32_t kBlockQuadwords[3] = {8u, 15u, 24u};

// The VIF1 stream of the source chain at `start` (refe / cnt / next / ref / refs / call / ret / end), as the DMAC
// sends it: with CHCR.TTE the upper half of each tag first, then its data. `ram` is the 32 MB of game memory. Returns
// false for a chain it cannot copy whole (scratchpad data, out of range, too long): the list is then empty.
bool flatten(const uint8_t *ram, uint32_t start, bool tte, List &out);

// Where game address `address` is in the stream (the first span holding it); false when the list did not send it.
bool offsetOf(const List &list, uint32_t address, uint32_t &offset);

// Puts into list.tags the recorded tags whose block lies in this list and still looks like its kind (DMA tag and
// UNPACK code as the game's builder writes them), with their stream offsets, in stream order. Later recordings win
// over earlier ones at the same block. Returns how many recorded tags were not in the list.
size_t resolve(const std::vector<Tag> &recorded, List &list);

// A value between two transforms, t from 0 (a) to 1 (b): the rotation turns the short way at an even rate, position
// and scale move straight. A matrix that is no rotation with scale (sheared, flattened, mirrored in only one of the
// two) is blended number by number. t <= 0 gives a and t >= 1 gives b, bit for bit.
Matrix blend(const Matrix &a, const Matrix &b, float t);

// The camera between two frames' cameras of one view: the pose (worldToView) blended as a transform, every other
// matrix rebuilt from its own projection part (matrix x inverse view, blended number by number) and the blended
// view. t <= 0 gives `before` and t >= 1 gives `now`, bit for bit.
Camera blend(const Camera &before, const Camera &now, float t);

// True when two transforms are too far apart to be one motion over a single game frame (a cut, a respawn).
bool jumped(const Matrix &a, const Matrix &b);

// The frame's keyed values: object matrices from the matrix tags, bone matrices from the references into `buffers`
// (their VIF packets read as UNPACK V4-32 runs of whole matrices), the cameras.
void extract(const List &list, const std::vector<BoneBuffer> &buffers, Values &out);

// The stream of an in-between picture at t between the frame before (t = 0) and this frame (t = 1): `list`'s stream
// with every keyed matrix that has a partner in `before` at its blended value, and every copy of a camera matrix or
// camera position in the inline data replaced by the blended camera's. At t >= 1 it is the list's stream unchanged.
void build(const List &list, const Values &now, const Values &before, float t, std::vector<uint8_t> &stream,
           BuildStats *stats = nullptr);

struct SharedStats
{
    uint32_t partsNow = 0, partsBefore = 0, shared = 0; // parts of each list, and how many the two share
    uint64_t bytesNow = 0, bytesBefore = 0, sharedBytes = 0;
    // The newer list's parts that are not shared, by kind. The first three are what the builder gives in-between
    // values.
    uint32_t objectBlocks = 0, cameraBlocks = 0, bonePackets = 0, otherInline = 0, otherReferences = 0;
    uint64_t otherInlineBytes = 0, otherReferenceBytes = 0;
    // Why a part the builder gives values is not shared (with both frames' values given to sharedOnly):
    //   fresh      the thing has no partner in the frame before (it is new, or its model changed);
    //   jumped     it moved too far to be blended;
    //   outOfPlace the same bytes are a part of the list before, but not in this place (the draw order changed);
    //   program    an object block that differs from its partner's block in one word only, the VU1 program it starts
    //              (the game drew the thing another way in the two frames); its values are the frame before's;
    //   different  it has a partner, and yet its bytes at t = 0 are in no part of the list before: something else
    //              in the block is not the frame before's.
    struct Why { uint32_t fresh = 0, jumped = 0, outOfPlace = 0, program = 0, different = 0; };
    Why objects, cameras, bones;
    // The first "different" part: which kind (0 object, 1 camera, 2 bones), where it first differs from its
    // partner's part (a word index behind the DMA tag's upper half) and the two words.
    bool hasExample = false;
    uint32_t exampleKind = 0, exampleWord = 0, exampleNow = 0, exampleBefore = 0, exampleWords = 0;
};

// "Blended content only" (the spec's test switch): what two consecutive frames' lists share. A part is one DMA tag's
// upper half and data; in this game's lists every part stands alone as VIF data. `nowStream` comes in as the newer
// list's stream with the frame before's values (build() at t = 0; its parts lie where `now.spans` say). The shared
// parts are the longest run of parts that both lists hold with the same bytes and in the same order (a longest
// common subsequence): a part counts only in its place, so a model packet stays with the bone packet before it. On
// return `nowStream` and `beforeStream` (the list before's stream) have every other part turned into VIF NOPs. Both
// are then the same VIF data apart from NOPs: drawn the same way they must give the same picture, whatever else the
// two frames held.
// `nowValues` and `beforeValues` (optional) are the two lists' keyed values: with them a reference that holds bone
// matrices is counted as a bone packet, and the reasons in SharedStats are filled in.
void sharedOnly(const List &before, const List &now, std::vector<uint8_t> &nowStream, std::vector<uint8_t> &beforeStream,
                SharedStats *stats = nullptr, const Values *nowValues = nullptr, const Values *beforeValues = nullptr);

} // namespace dw5xl::smooth

#endif
