#pragma once
#include <array>
#include <cstdint>
namespace boundary_fixture {
enum Case { Jump, Call, RegisterCall, RegisterCall5, NonLinkingJalr, RegisterJump, Return,
    Bltzal, Bgezal, Bltzall, Bgezall, BlezWide, BgtzWide, BltzWide, BgezWide,
    Bc0f, Bc0t, Bc0fl, Bc0tl, Bc1f, Bc1t, Bc1fl, Bc1tl, Bc2f, Bc2t, Bc2fl, Bc2tl,
    BreakSlot, StoreSlot, Count };
constexpr uint32_t base=0x180000, stride=0x40;
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
    {0x10200007,0x0000000d},{0x10200007,0xad040000}
}};
constexpr uint32_t address(Case c) { return base+static_cast<uint32_t>(c)*stride; }
}
