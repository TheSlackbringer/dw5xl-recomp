// Issue 049 Part 2 (P2.4): a game frame's command list as data of our own (app/src/dw5xl_smooth_list.h).
// No game data: a small DMA chain is written into an empty 32 MB memory image by hand.
//  - flatten(): the stream is what the DMAC sends (tag halves with TTE, inline and referenced data in chain order),
//    and every byte's source is found again through the map;
//  - a chain that leaves memory is refused whole;
//  - resolve(): recorded matrix blocks get their stream offsets; a recording whose block is not in the list, or
//    where the game has written something else since, is left out; the later recording wins.
#include "dw5xl_smooth_list.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dw5xl::smooth;

namespace {
int failures = 0;
void check(bool ok, const std::string &what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

void put32(std::vector<uint8_t> &ram, uint32_t at, uint32_t value) { std::memcpy(ram.data() + at, &value, 4); }
uint32_t get32(const std::vector<uint8_t> &bytes, size_t at)
{
    uint32_t v;
    std::memcpy(&v, bytes.data() + at, 4);
    return v;
}

// A DMA tag: id, quadword count, address, and the two VIF words in its upper half.
void tag(std::vector<uint8_t> &ram, uint32_t at, uint32_t id, uint32_t qwc, uint32_t addr, uint32_t vif0, uint32_t vif1)
{
    put32(ram, at, (id << 28) | qwc);
    put32(ram, at + 4u, addr);
    put32(ram, at + 8u, vif0);
    put32(ram, at + 12u, vif1);
}

constexpr uint32_t kMatrixBlock = 0x100000u, kRef = 0x100080u, kCameraBlock = 0x100090u, kEnd = 0x100210u;
constexpr uint32_t kModel = 0x200000u;

std::vector<uint8_t> scene()
{
    std::vector<uint8_t> ram(kRamBytes, 0u);
    // A matrix block as FUN_0019ced0 writes it: cnt of 7, UNPACK V4-32 x 5, the matrix at +0x20.
    tag(ram, kMatrixBlock, 1u, 7u, 0u, 0u, 0u);
    put32(ram, kMatrixBlock + 0x1Cu, 0x6C058000u);
    for (uint32_t i = 0; i < 16u; ++i)
        put32(ram, kMatrixBlock + 0x20u + i * 4u, 0x3F800000u + i);
    // A model packet by reference: 3 quadwords, its tag carries two VIF codes.
    tag(ram, kRef, 3u, 3u, kModel, 0x01000404u, 0x11000000u);
    for (uint32_t i = 0; i < 12u; ++i)
        put32(ram, kModel + i * 4u, 0xA0000000u + i);
    // A camera block as FUN_0019c550 writes it: cnt of 0x17, UNPACK V4-32 x 0x15.
    tag(ram, kCameraBlock, 1u, 0x17u, 0u, 0u, 0u);
    put32(ram, kCameraBlock + 0x1Cu, 0x6C150000u);
    for (uint32_t i = 0; i < 16u; ++i)
        put32(ram, kCameraBlock + 0x20u + i * 4u, 0x40000000u + i);
    tag(ram, kEnd, 7u, 0u, 0u, 0u, 0u);
    return ram;
}
} // namespace

int main()
{
    std::vector<uint8_t> ram = scene();

    List list;
    check(flatten(ram.data(), kMatrixBlock, true, list), "a chain of cnt, ref, cnt, end is copied");
    const size_t expected = 8u + 7u * 16u + 8u + 3u * 16u + 8u + 0x17u * 16u + 8u;
    check(list.stream.size() == expected, "with TTE the stream is every tag half and all data, in order");
    check(list.spans.size() == 3u && list.spans[0].id == 1u && list.spans[1].id == 3u && list.spans[2].id == 1u,
          "one map entry per tag with data");
    check(get32(list.stream, 8u + 7u * 16u) == 0x01000404u && get32(list.stream, 8u + 7u * 16u + 4u) == 0x11000000u,
          "the reference's two VIF codes come before its data");
    uint32_t at = 0;
    check(offsetOf(list, kMatrixBlock + 0x20u, at) && at == 8u + 0x10u && get32(list.stream, at) == 0x3F800000u,
          "inline data is found in the stream by its game address");
    check(offsetOf(list, kModel + 0x10u, at) && at == 8u + 7u * 16u + 8u + 0x10u && get32(list.stream, at) == 0xA0000004u,
          "referenced data is found in the stream by its game address");
    check(!offsetOf(list, 0x300000u, at), "an address the list did not send is not found");

    List plain;
    check(flatten(ram.data(), kMatrixBlock, false, plain) && plain.stream.size() == expected - 4u * 8u,
          "without TTE the tag halves are left out");

    // A reference that leaves memory: nothing is copied.
    std::vector<uint8_t> broken = ram;
    tag(broken, kRef, 3u, 0x4000u, kRamBytes - 0x1000u, 0u, 0u);
    List none;
    check(!flatten(broken.data(), kMatrixBlock, true, none) && none.stream.empty() && none.spans.empty(),
          "a chain that leaves memory is refused whole");

    // Recorded tags, as the builders' hooks make them.
    std::vector<Tag> recorded;
    Tag t;
    t.kind = TagKind::Matrix; t.block = kMatrixBlock; t.key = 0x4A0020u; t.serial = 1u;
    recorded.push_back(t);
    t.kind = TagKind::Matrix; t.block = kCameraBlock; t.key = 0x4A0090u; t.serial = 0u; // an older list had a matrix here
    recorded.push_back(t);
    t.kind = TagKind::Camera; t.block = kCameraBlock; t.key = 0u; t.serial = 2u;
    recorded.push_back(t);
    t.kind = TagKind::Matrix; t.block = 0x180000u; t.key = 0x4A0100u; t.serial = 3u;     // built for another list
    recorded.push_back(t);
    t.kind = TagKind::Matrix; t.block = kMatrixBlock; t.key = 0x4A0770u; t.serial = 5u;  // the same block, written later
    recorded.push_back(t);

    const size_t missing = resolve(recorded, list);
    check(missing == 2u, "a recording outside the list and one the game has overwritten are left out");
    check(list.tags.size() == 2u && list.tags[0].kind == TagKind::Matrix && list.tags[1].kind == TagKind::Camera &&
              list.tags[0].offset < list.tags[1].offset,
          "the tags come out in stream order");
    check(!list.tags.empty() && list.tags[0].key == 0x4A0770u, "the later recording of a block wins");
    check(list.tags.size() == 2u && list.tags[0].offset == 8u && get32(list.stream, list.tags[0].offset + 0x10u) == 0x3F800000u,
          "a matrix block's matrix is 16 bytes behind its offset");
    check(list.tags.size() == 2u && get32(list.stream, list.tags[1].offset + 0x10u) == 0x40000000u,
          "the camera block's first matrix is 16 bytes behind its offset");

    std::printf("%s\n", failures == 0 ? "smooth_list: PASS" : "smooth_list: FAIL");
    return failures == 0 ? 0 : 1;
}
