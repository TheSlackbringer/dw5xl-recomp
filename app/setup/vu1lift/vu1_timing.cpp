#include "vu1_timing.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace dw5xl::vu1lift {
namespace {
constexpr uint32_t FMAC_LATENCY = 4, ACC_FORWARD_LATENCY = 1;

uint32_t laneForComponent(uint32_t component) { return 1u << (3 - component); }
uint32_t dest(uint32_t w) { return (w >> 21) & 0xF; }
uint32_t ft(uint32_t w) { return (w >> 16) & 0x1F; }
uint32_t fs(uint32_t w) { return (w >> 11) & 0x1F; }
uint32_t fd(uint32_t w) { return (w >> 6) & 0x1F; }

void addVfRead(Usage &u, uint32_t reg, uint32_t lanes)
{
    if (lanes == 0)
        return;
    for (auto &entry : u.vfRead)
        if (entry.first == reg)
        {
            entry.second |= lanes;
            return;
        }
    if (u.vfRead.size() < 2)
        u.vfRead.push_back({reg, lanes});
}

void addVfWrite(Usage &u, uint32_t reg, uint32_t lanes)
{
    if (reg == 0 || lanes == 0)
        return;
    if (u.vfWrite[0] == 0)
    {
        u.vfWrite[0] = reg;
        u.vfWrite[1] = lanes;
    }
    else if (u.vfWrite[0] == reg)
        u.vfWrite[1] |= lanes;
}

void readVi(Usage &u, uint32_t reg)
{
    if (reg != 0)
        u.viRead |= 1u << reg;
}

void writeVi(Usage &u, uint32_t reg)
{
    if (reg != 0)
        u.viWrite |= 1u << reg;
}

[[noreturn]] void reserved(const char *which, uint32_t w)
{
    char text[64];
    std::snprintf(text, sizeof(text), "reserved %s instruction %08x", which, w);
    throw std::runtime_error(text);
}

bool in(uint32_t v, std::initializer_list<uint32_t> set)
{
    return std::find(set.begin(), set.end(), v) != set.end();
}

uint32_t vfReadLanes(const Usage &usage, uint32_t reg)
{
    for (const auto &[r, lanes] : usage.vfRead)
        if (r == reg)
            return lanes;
    return 0;
}
} // namespace

Usage upperUsage(uint32_t upper)
{
    Usage u;
    u.pipeline = PIPE_FMAC;
    u.latency = FMAC_LATENCY;
    const uint32_t op = upper & 0x3F;
    const uint32_t d = dest(upper), s = fs(upper), t = ft(upper), f = fd(upper);
    if (op <= 0x2F)
    {
        addVfRead(u, s, d);
        addVfWrite(u, f, d);
        if (op <= 0x1B)
            addVfRead(u, t, laneForComponent(op & 3));
        else if (op >= 0x28)
            addVfRead(u, t, op == 0x2E ? 0xE : d);
        if (in(op, {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x21, 0x23, 0x25, 0x27, 0x29, 0x2D, 0x2E}))
            u.accRead = d;
        return u;
    }
    if (op >= 0x3C)
    {
        const uint32_t special = (upper & 3) | ((upper >> 4) & 0x7C);
        const bool writesAcc = special <= 0x0F || (0x18 <= special && special <= 0x1C) || special == 0x1E ||
                               (0x20 <= special && special <= 0x2A) || (0x2C <= special && special <= 0x2E);
        if (writesAcc)
        {
            addVfRead(u, s, d);
            if (special <= 0x1B)
                addVfRead(u, t, laneForComponent(special & 3));
            else if (0x28 <= special && special <= 0x2E)
                addVfRead(u, t, special == 0x2E ? 0xE : d);
            u.accWrite = d;
            if ((0x08 <= special && special <= 0x0F) || in(special, {0x21, 0x23, 0x25, 0x27, 0x29, 0x2D}))
                u.accRead = d;
        }
        else if ((0x10 <= special && special <= 0x17) || special == 0x1D)
        {
            addVfRead(u, s, d);
            addVfWrite(u, t, d);
        }
        else if (special == 0x1F)
        {
            addVfRead(u, s, 0xE);
            addVfRead(u, t, 0x1);
        }
        else if (!in(special, {0x2F, 0x30}))
            reserved("upper", upper);
        return u;
    }
    reserved("upper", upper);
}

Usage lowerUsage(uint32_t lower)
{
    Usage u;
    if (lower == 0 || lower == 0x8000033C)
        return u;
    const uint32_t opHi = (lower >> 25) & 0x7F;
    const uint32_t d = dest(lower), vfT = ft(lower), vfS = fs(lower);
    const uint32_t viT = ft(lower), viS = fs(lower), viD = fd(lower);
    auto set = [&](Pipe p, uint32_t latency) {
        u.pipeline = p;
        u.latency = latency;
    };
    switch (opHi)
    {
    case 0x00: set(PIPE_LSU, 4); readVi(u, viS); addVfWrite(u, vfT, d); return u;
    case 0x01: set(PIPE_LSU, 1); readVi(u, viT); addVfRead(u, vfS, d); return u;
    case 0x04: set(PIPE_LSU, 4); readVi(u, viS); writeVi(u, viT); return u;
    case 0x05: set(PIPE_LSU, 1); readVi(u, viS); readVi(u, viT); return u;
    case 0x08:
    case 0x09: set(PIPE_IALU, 1); u.delaysBranch = true; readVi(u, viS); writeVi(u, viT); return u;
    case 0x10:
    case 0x12:
    case 0x13: set(PIPE_IALU, 1); writeVi(u, 1); return u;
    case 0x11: set(PIPE_FMAC, FMAC_LATENCY); return u;
    case 0x14:
    case 0x16:
    case 0x17: set(PIPE_IALU, 1); writeVi(u, viT); return u;
    case 0x15: set(PIPE_FMAC, FMAC_LATENCY); return u;
    case 0x18:
    case 0x1A:
    case 0x1B: set(PIPE_IALU, 1); readVi(u, viS); writeVi(u, viT); return u;
    case 0x1C: set(PIPE_IALU, 1); writeVi(u, viT & 15); return u;
    case 0x20: u.pipeline = PIPE_BRANCH; return u;
    case 0x21: set(PIPE_BRANCH, 1); writeVi(u, viT & 15); return u;
    case 0x24: u.pipeline = PIPE_BRANCH; readVi(u, viS & 15); return u;
    case 0x25: set(PIPE_BRANCH, 1); readVi(u, viS); writeVi(u, viT); return u;
    case 0x28:
    case 0x29: u.pipeline = PIPE_BRANCH; readVi(u, viS); readVi(u, viT); return u;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F: u.pipeline = PIPE_BRANCH; readVi(u, viS); return u;
    default: break;
    }
    if (opHi != 0x40)
        reserved("lower", lower);

    const uint32_t direct = lower & 0x3F;
    if (in(direct, {0x30, 0x31, 0x34, 0x35}))
    {
        set(PIPE_IALU, 1);
        u.delaysBranch = true;
        readVi(u, viS);
        readVi(u, viT);
        writeVi(u, viD);
        return u;
    }
    if (direct == 0x32)
    {
        set(PIPE_IALU, 1);
        u.delaysBranch = true;
        readVi(u, viS);
        writeVi(u, viT);
        return u;
    }
    if (direct < 0x3C)
        reserved("lower", lower);

    const uint32_t special = (lower & 3) | ((lower >> 4) & 0x7C);
    const uint32_t fsfLane = laneForComponent((lower >> 21) & 3);
    const uint32_t ftfLane = laneForComponent((lower >> 23) & 3);
    if (in(special, {0x30, 0x31}))
    {
        set(PIPE_FMAC, 4);
        addVfRead(u, vfS, special == 0x31 ? 0xF : d);
        addVfWrite(u, vfT, d);
    }
    else if (in(special, {0x34, 0x36}))
    {
        set(PIPE_LSU, 4);
        u.viLatency = 1;
        u.delaysBranch = true;
        readVi(u, viS);
        writeVi(u, viS);
        addVfWrite(u, vfT, d);
    }
    else if (in(special, {0x35, 0x37}))
    {
        set(PIPE_LSU, 1);
        u.delaysBranch = true;
        readVi(u, viT);
        writeVi(u, viT);
        addVfRead(u, vfS, d);
    }
    else if (special == 0x38)
    {
        set(PIPE_FDIV, 7);
        addVfRead(u, vfS, fsfLane);
        addVfRead(u, vfT, ftfLane);
    }
    else if (special == 0x39)
    {
        set(PIPE_FDIV, 7);
        addVfRead(u, vfT, ftfLane);
    }
    else if (special == 0x3A)
    {
        set(PIPE_FDIV, 13);
        addVfRead(u, vfS, fsfLane);
        addVfRead(u, vfT, ftfLane);
    }
    else if (special == 0x3B)
    {
        u.pipeline = PIPE_FDIV;
        u.waitQ = true;
    }
    else if (special == 0x3C)
    {
        set(PIPE_IALU, 1);
        u.delaysBranch = true;
        addVfRead(u, vfS, fsfLane);
        writeVi(u, viT);
    }
    else if (special == 0x3D)
    {
        set(PIPE_FMAC, 4);
        readVi(u, viS);
        addVfWrite(u, vfT, d);
    }
    else if (special == 0x3E)
    {
        set(PIPE_LSU, 4);
        readVi(u, viS);
        writeVi(u, viT);
    }
    else if (special == 0x3F)
    {
        set(PIPE_LSU, 1);
        readVi(u, viS);
        readVi(u, viT);
    }
    else if (in(special, {0x40, 0x41}))
    {
        set(PIPE_FMAC, 4);
        addVfWrite(u, vfT, d);
    }
    else if (in(special, {0x42, 0x43}))
    {
        set(PIPE_IALU, 1);
        addVfRead(u, vfS, fsfLane);
    }
    else if (special == 0x64)
    {
        set(PIPE_FMAC, 4);
        addVfWrite(u, vfT, d);
    }
    else if (in(special, {0x68, 0x69}))
    {
        set(PIPE_IALU, 1);
        writeVi(u, viT);
    }
    else if (special == 0x6C)
    {
        set(PIPE_XGKICK, 2);
        readVi(u, viS);
    }
    else if (in(special, {0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x78, 0x79, 0x7A, 0x7C, 0x7D, 0x7E}))
    {
        u.pipeline = PIPE_EFU;
        switch (special)
        {
        case 0x70: u.latency = 11; break;
        case 0x71: case 0x72: case 0x79: u.latency = 18; break;
        case 0x73: u.latency = 24; break;
        case 0x74: case 0x75: case 0x7D: u.latency = 54; break;
        case 0x76: case 0x78: case 0x7A: u.latency = 12; break;
        case 0x7C: u.latency = 29; break;
        default: u.latency = 44; break; // 0x7E
        }
        if (0x70 <= special && special <= 0x73)
            addVfRead(u, vfS, 0xE);
        else if (special == 0x74)
            addVfRead(u, vfS, 0xC);
        else if (special == 0x75)
            addVfRead(u, vfS, 0xA);
        else if (special == 0x76)
            addVfRead(u, vfS, 0xF);
        else
            addVfRead(u, vfS, fsfLane);
    }
    else if (special == 0x7B)
    {
        u.pipeline = PIPE_EFU;
        u.waitP = true;
    }
    else
        reserved("lower", lower);
    return u;
}

PairTiming pairTiming(uint32_t lower, uint32_t upper)
{
    const Usage up = upperUsage(upper);
    const Usage lo = (upper & 0x80000000u) ? Usage{} : lowerUsage(lower);
    uint32_t suppressed = 0;
    const uint32_t w = up.vfWrite[0];
    if (w != 0 && (vfReadLanes(lo, w) != 0 || lo.vfWrite[0] == w))
        if (lo.vfWrite[0] == w)
            suppressed = w;

    PairTiming t;
    size_t groups = 0;
    for (const Usage *usage : {&up, &lo})
        for (const auto &[r, l] : usage->vfRead)
            if (r != 0 && l != 0 && groups < 4)
            {
                t.vfReg[groups] = r;
                t.vfLanes[groups] = l;
                ++groups;
            }

    if (lo.vfWrite[0] != 0 && suppressed != lo.vfWrite[0])
    {
        const uint32_t lat = lo.vfLatency ? lo.vfLatency : lo.latency;
        t.wReg[0] = lo.vfWrite[0];
        t.wLanes[0] = lo.vfWrite[1];
        t.wLat[0] = lat;
        for (uint32_t c = 0; c < 4; ++c)
            if (lo.vfWrite[1] & laneForComponent(c))
                t.writes.push_back({int(lo.vfWrite[0] * 4 + c), lat});
    }
    if (up.vfWrite[0] != 0)
    {
        const uint32_t lat = up.vfLatency ? up.vfLatency : up.latency;
        t.wReg[1] = up.vfWrite[0];
        t.wLanes[1] = up.vfWrite[1];
        t.wLat[1] = lat;
        for (uint32_t c = 0; c < 4; ++c)
            if (up.vfWrite[1] & laneForComponent(c))
                t.writes.push_back({int(up.vfWrite[0] * 4 + c), lat});
    }
    t.viLat = lo.viLatency ? lo.viLatency : lo.latency;
    t.viWrite = lo.viWrite & 0xFFFE;
    for (int i = 0; i < 16; ++i)
        if (t.viWrite & (1u << i))
            t.writes.push_back({READY_VI_BASE + i, t.viLat});
    for (uint32_t c = 0; c < 4; ++c)
        if (up.accWrite & laneForComponent(c))
            t.writes.push_back({READY_ACC_BASE + int(c), ACC_FORWARD_LATENCY});
    for (const auto &[slot, lat] : t.writes)
        t.maxLat = std::max(t.maxLat, lat);
    if (t.viWrite)
    {
        uint32_t lowest = 0;
        while (!(t.viWrite & (1u << lowest)))
            ++lowest;
        t.firstVi = lowest;
    }
    t.flags = (lo.pipeline == PIPE_FDIV ? FLAG_FDIV : 0) | (lo.pipeline == PIPE_EFU ? FLAG_EFU : 0) |
              (lo.waitQ ? FLAG_WAITQ : 0) | (lo.waitP ? FLAG_WAITP : 0) |
              (lo.pipeline == PIPE_XGKICK ? FLAG_XGKICK : 0) | (lo.delaysBranch ? FLAG_BRANCH_DELAY : 0);

    std::set<int> reads;
    for (const Usage *usage : {&up, &lo})
    {
        for (const auto &[r, l] : usage->vfRead)
            if (r != 0)
                for (uint32_t c = 0; c < 4; ++c)
                    if (l & laneForComponent(c))
                        reads.insert(int(r * 4 + c));
        for (int i = 1; i < 16; ++i)
            if (usage->viRead & (1u << i))
                reads.insert(READY_VI_BASE + i);
        for (uint32_t c = 0; c < 4; ++c)
            if (usage->accRead & laneForComponent(c))
                reads.insert(READY_ACC_BASE + int(c));
    }
    t.reads.assign(reads.begin(), reads.end());
    t.viRead = (up.viRead | lo.viRead) & 0xFFFE;
    t.accRead = up.accRead | lo.accRead;
    t.accWrite = up.accWrite;
    return t;
}

std::string timingInitializer(const PairTiming &t)
{
    auto list = [](const auto &xs) {
        std::string s = "{";
        for (size_t i = 0; i < xs.size(); ++i)
            s += (i ? ", " : "") + std::to_string(xs[i]);
        return s + "}";
    };
    char hex[32];
    std::string s = "{" + list(t.vfReg) + ", " + list(t.vfLanes) + ", ";
    std::snprintf(hex, sizeof(hex), "0x%04x, 0x%04x, ", t.viRead, t.viWrite);
    s += hex;
    s += std::to_string(t.accRead) + ", " + std::to_string(t.accWrite) + ", " + list(t.wReg) + ", " + list(t.wLanes) +
         ", " + list(t.wLat) + ", " + std::to_string(t.viLat) + ", " + std::to_string(t.maxLat) + ", " +
         std::to_string(t.firstVi) + ", " + std::to_string(t.flags) + "}";
    return s;
}

} // namespace dw5xl::vu1lift
