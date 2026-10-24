#pragma once
#include <array>
#include <cstdint>
namespace boundary_fixture {
enum Case { Jump, Call, RegisterCall, RegisterCall5, NonLinkingJalr, RegisterJump, Return,
    Bltzal, Bgezal, Bltzall, Bgezall, BlezWide, BgtzWide, BltzWide, BgezWide,
    Bc0f, Bc0t, Bc0fl, Bc0tl, Bc1f, Bc1t, Bc1fl, Bc1tl, Bc2f, Bc2t, Bc2fl, Bc2tl,
    BreakSlot, StoreSlot, DirectSlotBreak, LikelyBreakSlot, Count };
constexpr uint32_t base=0x180000, stride=0x40;
// Authored copied-code fixture: JAL's high PC bits must use the execution address.
constexpr uint32_t copySource=0x193100, copyBase=0x80070000, secondCopyBase=0x80071000;
constexpr std::array<uint32_t,16> copiedWords{{
    0x03e0502d,0x0c01c008,0x24840003,0x0140f82d,0x03e00008,0,
    0,0,0x27e30000,0x2482000b,0x03e00008,0x24840001,0x12345678,0,0,0
}};
constexpr uint32_t overlap=0x190000, clipped=0x191000, shared=0x193000;
struct Words { uint32_t branch, slot; };
// Independently specified instruction words; no decoder/code-generator expectation helpers.
constexpr std::array<Words,Count> words{{
    {0x08000000,0x24840004},{0x0c000000,0x27e40001},
    {0x0020f809,0x24210004},{0x00202809,0x24a40001},
    {0x00200009,0x24210004},{0x00200008,0x24210004},{0x03e00008,0x27ff0004},
    {0x04300007,0x27e40001},{0x04310007,0x27e40001},
    {0x04320007,0x27e40001},{0x04330007,0x27e40001},
    {0x18200007,0x24840004},{0x1c200007,0x24840004},
    {0x04200007,0x24840004},{0x04210007,0x24840004},
    {0x41000007,0x24840004},{0x41010007,0x24840004},
    {0x41020007,0x24840004},{0x41030007,0x24840004},
    {0x45000007,0x24840004},{0x45010007,0x24840004},
    {0x45020007,0x24840004},{0x45030007,0x24840004},
    {0x49000007,0x24840004},{0x49010007,0x24840004},
    {0x49020007,0x24840004},{0x49030007,0x24840004},
    {0x10200007,0x0000000d},{0x10200007,0xad040000},{0x10200007,0x0000000d},{0x50200007,0x0000000d}
}};
constexpr uint32_t address(Case c) { return base+static_cast<uint32_t>(c)*stride; }
}
