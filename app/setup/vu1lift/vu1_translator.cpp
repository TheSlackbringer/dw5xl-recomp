// Port of tools/vu1_translator.py (disc mode), tools/native_call_analysis.py and tools/disc_vu_inputs.py.
// Function and variable names follow the Python so the two can be read side by side; the output must stay
// byte-identical (vu1lift test).
#include "vu1_translator.h"
#include "../sha256.h"
#include "vu1_inputs.h"
#include "vu1_timing.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace dw5xl::vu1lift {
namespace {

std::string F(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char small[512];
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(small, sizeof(small), format, copy);
    va_end(copy);
    std::string out;
    if (n >= 0 && size_t(n) < sizeof(small))
        out.assign(small, size_t(n));
    else if (n >= 0)
    {
        out.resize(size_t(n) + 1);
        std::vsnprintf(out.data(), out.size(), format, args);
        out.resize(size_t(n));
    }
    va_end(args);
    return out;
}

std::string S(long long v) { return std::to_string(v); }

[[noreturn]] void fail(const std::string &message) { throw std::runtime_error(message); }

// Target unit (issue 025, as UNIT / set_unit in the Python): VU1 (16 KB micro memory) or VU0 (4 KB, the
// microprograms the EE starts with VCALLMS). VU0 has no PATH1/XGKICK, no EFU, no XTOP/XITOP and no WAITP/MFP.
bool g_vu0 = false;

uint32_t pcMask() { return g_vu0 ? 0x0fffu : 0x3fffu; }

std::string memLq(const std::string &addr)
{
    return (g_vu0 ? "vl::lq0(mem, " : "vl::lq(mem, ") + addr + ")";
}

std::string memSq(uint32_t lm, const std::string &addr, const std::string &val)
{
    if (g_vu0)
        return F("vl::sq0<0x%x>(c, mem, ", lm) + addr + ", " + val + ");";
    return F("vl::sq<0x%x, false>(c, mem, ", lm) + addr + ", " + val + ", 0u);";
}

std::string memIlw(const std::string &addr, int comp)
{
    return (g_vu0 ? "vl::ilw0(mem, " : "vl::ilw(mem, ") + addr + F(", %d)", comp);
}

std::string memIsw(uint32_t lm, const std::string &addr, const std::string &val)
{
    if (g_vu0)
        return F("vl::isw0<0x%x>(c, mem, ", lm) + addr + ", " + val + ");";
    return F("vl::isw<0x%x, false>(c, mem, ", lm) + addr + ", " + val + ", 0u);";
}

void vu1Only(const char *name, uint32_t pc)
{
    if (g_vu0)
        fail(F("%s at pc %04x is not a VU0 instruction", name, pc));
}

std::pair<uint32_t, uint32_t> unpackPair(const std::vector<uint8_t> &code, uint32_t pc)
{
    if (size_t(pc) + 8 > code.size())
        fail(F("VU code read outside the image at %04x", pc));
    uint32_t lo, up;
    std::memcpy(&lo, &code[pc], 4);
    std::memcpy(&up, &code[pc + 4], 4);
    return {lo, up};
}

const char *const kBC[] = {"x", "y", "z", "w"};

std::string destName(uint32_t d)
{
    std::string s;
    for (int i = 0; i < 4; ++i)
        if (d & (8u >> i))
            s += "xyzw"[i];
    return s;
}

uint32_t destMask(uint32_t d) { return ((d & 8) >> 3) | ((d & 4) >> 1) | ((d & 2) << 1) | ((d & 1) << 3); }
int s11(uint32_t x) { return (x & 0x400) ? int(x) - 0x800 : int(x); }

float f32(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// Python f"{x:g}" of a float32 widened to double (C's %g, except NaN prints "nan" whatever its sign).
std::string formatG(float value)
{
    const double v = value;
    if (std::isnan(v))
        return "nan";
    return F("%g", v);
}

const std::map<uint32_t, const char *> kUP = {
    {0x00, "ADDx"}, {0x01, "ADDy"}, {0x02, "ADDz"}, {0x03, "ADDw"}, {0x04, "SUBx"}, {0x05, "SUBy"},
    {0x06, "SUBz"}, {0x07, "SUBw"}, {0x08, "MADDx"}, {0x09, "MADDy"}, {0x0a, "MADDz"}, {0x0b, "MADDw"},
    {0x0c, "MSUBx"}, {0x0d, "MSUBy"}, {0x0e, "MSUBz"}, {0x0f, "MSUBw"}, {0x10, "MAXx"}, {0x11, "MAXy"},
    {0x12, "MAXz"}, {0x13, "MAXw"}, {0x14, "MINIx"}, {0x15, "MINIy"}, {0x16, "MINIz"}, {0x17, "MINIw"},
    {0x18, "MULx"}, {0x19, "MULy"}, {0x1a, "MULz"}, {0x1b, "MULw"}, {0x1c, "MULq"}, {0x1d, "MAXi"},
    {0x1e, "MULi"}, {0x1f, "MINIi"}, {0x20, "ADDq"}, {0x21, "MADDq"}, {0x22, "ADDi"}, {0x23, "MADDi"},
    {0x24, "SUBq"}, {0x25, "MSUBq"}, {0x26, "SUBi"}, {0x27, "MSUBi"}, {0x28, "ADD"}, {0x29, "MADD"},
    {0x2a, "MUL"}, {0x2b, "MAX"}, {0x2c, "SUB"}, {0x2d, "MSUB"}, {0x2e, "OPMSUB"}, {0x2f, "MINI"}};

const std::map<uint32_t, const char *> kUPS = {
    {0x00, "ADDAx"}, {0x01, "ADDAy"}, {0x02, "ADDAz"}, {0x03, "ADDAw"}, {0x04, "SUBAx"}, {0x05, "SUBAy"},
    {0x06, "SUBAz"}, {0x07, "SUBAw"}, {0x08, "MADDAx"}, {0x09, "MADDAy"}, {0x0a, "MADDAz"}, {0x0b, "MADDAw"},
    {0x0c, "MSUBAx"}, {0x0d, "MSUBAy"}, {0x0e, "MSUBAz"}, {0x0f, "MSUBAw"}, {0x10, "ITOF0"}, {0x11, "ITOF4"},
    {0x12, "ITOF12"}, {0x13, "ITOF15"}, {0x14, "FTOI0"}, {0x15, "FTOI4"}, {0x16, "FTOI12"}, {0x17, "FTOI15"},
    {0x18, "MULAx"}, {0x19, "MULAy"}, {0x1a, "MULAz"}, {0x1b, "MULAw"}, {0x1c, "MULAq"}, {0x1d, "ABS"},
    {0x1e, "MULAi"}, {0x1f, "CLIP"}, {0x20, "ADDAq"}, {0x21, "MADDAq"}, {0x22, "ADDAi"}, {0x23, "MADDAi"},
    {0x24, "SUBAq"}, {0x25, "MSUBAq"}, {0x26, "SUBAi"}, {0x27, "MSUBAi"}, {0x28, "ADDA"}, {0x29, "MADDA"},
    {0x2a, "MULA"}, {0x2c, "SUBA"}, {0x2d, "MSUBA"}, {0x2e, "OPMULA"}, {0x2f, "NOP"}};

// ---- decode_upper / decode_lower ----
struct Upper
{
    bool special = false; // 'UPS'
    std::string name;
    uint32_t op = 0, sp = 0, d = 0, ft = 0, fs = 0, fd = 0;
};

Upper decodeUpper(uint32_t w)
{
    Upper u;
    u.d = (w >> 21) & 15;
    u.ft = (w >> 16) & 31;
    u.fs = (w >> 11) & 31;
    u.fd = (w >> 6) & 31;
    u.op = w & 63;
    if (u.op < 0x3c)
    {
        const auto it = kUP.find(u.op);
        u.name = it != kUP.end() ? it->second : F("?up%x", u.op);
        return u;
    }
    u.special = true;
    u.sp = (((w >> 6) & 31) << 2) | (w & 3);
    const auto it = kUPS.find(u.sp);
    u.name = it != kUPS.end() ? it->second : F("?ups%x", u.sp);
    return u;
}

enum class LType { INT, SPEC, NORM };

struct Lower
{
    LType type = LType::NORM;
    std::string name; // INT
    uint32_t op = 0;  // NORM
    uint32_t sp = 0;  // SPEC
    uint32_t dd = 0, ft = 0, fs = 0, fd = 0, fsf = 0, ftf = 0, tgt = 0;
    int imm = 0;
    int getOp() const { return type == LType::NORM ? int(op) : -1; } // l.get('op', -1)
};

Lower decodeLower(uint32_t w, uint32_t pc)
{
    Lower l;
    const uint32_t op = w >> 25;
    l.dd = (w >> 21) & 15;
    l.ft = (w >> 16) & 31;
    l.fs = (w >> 11) & 31;
    l.fd = (w >> 6) & 31;
    const int imm = s11(w & 0x7ff);
    if (op == 0x40)
    {
        const uint32_t fn = w & 63;
        if (fn == 0x30 || fn == 0x31 || fn == 0x32 || fn == 0x34 || fn == 0x35)
        {
            l.type = LType::INT;
            l.name = fn == 0x30 ? "IADD" : fn == 0x31 ? "ISUB" : fn == 0x32 ? "IADDI" : fn == 0x34 ? "IAND" : "IOR";
            int i5 = int((w >> 6) & 31);
            l.imm = (i5 & 16) ? i5 - 32 : i5;
            return l;
        }
        if (fn >= 0x3c)
        {
            l.type = LType::SPEC;
            l.sp = (((w >> 6) & 31) << 2) | (w & 3);
            l.fsf = (w >> 21) & 3;
            l.ftf = (w >> 23) & 3;
            return l;
        }
    }
    l.type = LType::NORM;
    l.op = op;
    l.imm = imm;
    l.tgt = uint32_t(int(pc) + 8 + imm * 8) & pcMask();
    return l;
}

std::string blendWrap(const std::string &reg, const std::string &val, uint32_t d)
{
    const uint32_t lm = destMask(d);
    if (lm == 0xf)
        return reg + " = " + val + ";";
    if (lm == 0)
        return "";
    return reg + F(" = blend<0x%x>(", lm) + reg + ", " + val + ");";
}

// Issue 025 v10 V1 (port of NORM_KNOWN / nv): lanes known to hold zero or a normal float per VF register (bits as
// in dest: x=8 y=4 z=2 w=1) for the pair being translated; nullptr = nothing known. norm() is the identity on such
// lanes, so it is left out where every lane an operand is read in is known (see normKnownStates).
const std::map<uint32_t, uint32_t> *g_normKnown = nullptr;

std::string nv(uint32_t r, uint32_t lanes)
{
    if (g_normKnown && r != 0)
    {
        const auto it = g_normKnown->find(r);
        if (((it == g_normKnown->end() ? 0u : it->second) & lanes) == lanes)
            return F("vf[%u]", r);
    }
    return F("norm(vf[%u])", r);
}

// ---- translate_upper ----
std::string translateUpper(const Upper &u)
{
    const std::string &name = u.name;
    const uint32_t d = u.d, fd = u.fd, fs = u.fs, ft = u.ft;
    const std::string vfd = "vf[" + S(fd) + "]";
    if (name == "NOP")
        return "";
    const std::map<std::string, const char *> fmacMap = {{"ADD", "Add"},  {"SUB", "Sub"},   {"MUL", "Mul"},
                                                         {"MADD", "MAdd"}, {"MSUB", "MSub"}, {"ADDA", "Add"},
                                                         {"SUBA", "Sub"},  {"MULA", "Mul"},  {"MADDA", "MAdd"},
                                                         {"MSUBA", "MSub"}};
    const char *bases[] = {"ADD", "SUB", "MUL", "MADD", "MSUB"};
    for (const char *base : bases)
        for (int i = 0; i < 4; ++i)
        {
            const std::string val = F("fmac<%s, %u>(", fmacMap.at(base), d) + nv(fs, d) + F(", bc<%d>(", i) +
                                    nv(ft, 8u >> i) + "), acc, fl)";
            if (name == std::string(base) + kBC[i])
                return blendWrap(vfd, val, d);
            if (name == std::string(base) + "A" + kBC[i])
                return blendWrap("acc", val, d);
        }
    for (const char *base : bases)
    {
        const std::string val = F("fmac<%s, %u>(", fmacMap.at(base), d) + nv(fs, d) + ", " + nv(ft, d) + ", acc, fl)";
        if (name == base)
            return blendWrap(vfd, val, d);
        if (name == std::string(base) + "A")
            return blendWrap("acc", val, d);
    }
    for (const char *base : bases)
    {
        const std::string val = F("fmac<%s, %u>(", fmacMap.at(base), d) + nv(fs, d) + ", splat(c.st.q), acc, fl)";
        if (name == std::string(base) + "q")
            return blendWrap(vfd, val, d);
        if (name == std::string(base) + "Aq")
            return blendWrap("acc", val, d);
    }
    for (const char *base : bases)
    {
        const std::string val = F("fmac<%s, %u>(", fmacMap.at(base), d) + nv(fs, d) + ", splat(i_reg), acc, fl)";
        if (name == std::string(base) + "i")
            return blendWrap(vfd, val, d);
        if (name == std::string(base) + "Ai")
            return blendWrap("acc", val, d);
    }
    if (name == "OPMULA" || name == "OPMSUB")
    {
        const std::string a = "_mm_shuffle_ps(" + nv(fs, 15) + ", " + nv(fs, 15) + ", _MM_SHUFFLE(3, 0, 2, 1))";
        const std::string b = "_mm_shuffle_ps(" + nv(ft, 15) + ", " + nv(ft, 15) + ", _MM_SHUFFLE(3, 1, 0, 2))";
        if (name == "OPMULA")
            return blendWrap("acc", F("fmac<Mul, %u>(", d) + a + ", " + b + ", acc, fl)", d);
        return blendWrap(vfd, F("fmac<MSub, %u>(", d) + a + ", " + b + ", acc, fl)", d);
    }
    if (name == "MAX")
        return blendWrap(vfd, F("vuMinMaxV(vf[%u], vf[%u], true)", fs, ft), d);
    if (name == "MAXi")
        return blendWrap(vfd, F("vuMinMaxV(vf[%u], splat(i_reg), true)", fs), d);
    for (int i = 0; i < 4; ++i)
        if (name == std::string("MAX") + kBC[i])
            return blendWrap(vfd, F("vuMinMaxV(vf[%u], bc<%d>(vf[%u]), true)", fs, i, ft), d);
    if (name == "MINI")
        return blendWrap(vfd, F("vuMinMaxV(vf[%u], vf[%u], false)", fs, ft), d);
    if (name == "MINIi")
        return blendWrap(vfd, F("vuMinMaxV(vf[%u], splat(i_reg), false)", fs), d);
    for (int i = 0; i < 4; ++i)
        if (name == std::string("MINI") + kBC[i])
            return blendWrap(vfd, F("vuMinMaxV(vf[%u], bc<%d>(vf[%u]), false)", fs, i, ft), d);
    if (name == "CLIP")
        return F("vl::LiftAccess::clipAt(c, cyc, vl::clipFlags(vf[%u], bc<3>(vf[%u])) & 0x3F);", fs, ft);
    const std::string vft = "vf[" + S(ft) + "]";
    if (name == "FTOI0")
        return blendWrap(vft, "vuFtoiV(" + nv(fs, d) + ", 1.0f)", d);
    if (name == "FTOI4")
        return blendWrap(vft, "vuFtoiV(" + nv(fs, d) + ", 16.0f)", d);
    if (name == "ITOF0")
        return blendWrap(vft, F("vuItofV(vf[%u], 1.0f)", fs), d);
    if (name == "ITOF4")
        return blendWrap(vft, F("vuItofV(vf[%u], 16.0f)", fs), d);
    if (name == "ABS")
        return blendWrap(vft, F("vabs(vf[%u])", fs), d);
    fail("Unknown upper op: " + name);
}

// ---- translate_lower ----
std::string translateLower(const Lower &l, uint32_t lo, bool iBit, uint32_t pc)
{
    if (iBit)
        return F("i_reg = vl::normf(f32(0x%08xu)); // ", lo) + formatG(f32(lo));
    if (l.type == LType::INT)
    {
        const uint32_t fd = l.fd, fs = l.fs, ft = l.ft;
        const int imm = l.imm;
        if (l.name == "IADD")
            return F("if (%u != 0) vi[%u] = vl::i16(vi[%u] + vi[%u]);", fd, fd, fs, ft);
        if (l.name == "ISUB")
            return F("if (%u != 0) vi[%u] = vl::i16(vi[%u] - vi[%u]);", fd, fd, fs, ft);
        if (l.name == "IADDI")
            return F("if (%u != 0) vi[%u] = vl::i16(vi[%u] + (%d));", ft, ft, fs, imm);
        if (l.name == "IAND")
            return F("if (%u != 0) vi[%u] = vi[%u] & vi[%u];", fd, fd, fs, ft);
        if (l.name == "IOR")
            return F("if (%u != 0) vi[%u] = vi[%u] | vi[%u];", fd, fd, fs, ft);
    }
    if (l.type == LType::SPEC)
    {
        const uint32_t sp = l.sp, dd = l.dd, lm = destMask(dd), ft = l.ft, fs = l.fs;
        const std::string vft = "vf[" + S(ft) + "]";
        if (sp == 0x30)
            return blendWrap(vft, F("vf[%u]", fs), dd);
        if (sp == 0x31)
            return blendWrap(vft, F("vl::mr32(vf[%u])", fs), dd);
        if (sp == 0x34)
        {
            std::string out;
            const std::string bw = blendWrap(vft, memLq(F("vi[%u]", fs)), dd);
            if (!bw.empty())
                out = bw + " ";
            return out + F("if (%u != 0) vi[%u] = vl::i16(vi[%u] + 1);", fs, fs, fs);
        }
        if (sp == 0x35)
            return memSq(lm, F("vi[%u]", ft), F("vf[%u]", fs)) + " " +
                   F("if (%u != 0) vi[%u] = vl::i16(vi[%u] + 1);", ft, ft, ft);
        if (sp == 0x36)
        {
            std::string out = F("if (%u != 0) vi[%u] = vl::i16(vi[%u] - 1);", fs, fs, fs);
            const std::string bw = blendWrap(vft, memLq(F("vi[%u]", fs)), dd);
            if (!bw.empty())
                out += " " + bw;
            return out;
        }
        if (sp == 0x37)
            return F("if (%u != 0) vi[%u] = vl::i16(vi[%u] - 1);", ft, ft, ft) + " " +
                   memSq(lm, F("vi[%u]", ft), F("vf[%u]", fs));
        if (sp == 0x38)
            return F("{ uint32_t di = 0; const float qv = vl::div(vl::lane<%u>(vf[%u]), vl::lane<%u>(vf[%u]), di); "
                     "vl::LiftAccess::queueQAt(c, cyc, qv, 7u, di); }",
                     l.fsf, fs, l.ftf, ft);
        if (sp == 0x3b)
            return "";
        if (sp == 0x3c)
            return F("if (%u != 0) vi[%u] = static_cast<int32_t>(static_cast<int16_t>(vl::bits<%u>(vf[%u]) & 0xFFFF));",
                     ft, ft, l.fsf, fs);
        if (sp == 0x3d)
            return blendWrap(vft,
                             F("_mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(static_cast<int16_t>(vi[%u] & "
                               "0xFFFF))))",
                               fs),
                             dd);
        if (sp == 0x3e)
        {
            const int comp = (dd & 8) ? 0 : (dd & 4) ? 1 : (dd & 2) ? 2 : 3;
            return F("if (%u != 0) vi[%u] = ", ft, ft) + memIlw(F("vi[%u]", fs), comp) + ";";
        }
        if (sp == 0x3f)
            return memIsw(destMask(dd), F("vi[%u]", fs), F("vi[%u]", ft));
        if (sp == 0x68)
        {
            vu1Only("XTOP", pc);
            return F("if (%u != 0) vi[%u] = static_cast<int32_t>(c.st.top & 0x3FFu);", ft, ft);
        }
        if (sp == 0x69)
        {
            vu1Only("XITOP", pc);
            return F("if (%u != 0) vi[%u] = static_cast<int32_t>(c.st.itop & 0x3FFu);", ft, ft);
        }
        if (sp == 0x6c)
        {
            vu1Only("XGKICK", pc);
            return F("vl::LiftAccess::xgkickAt(c, cyc, vi[%u] & 0x3FF);", fs);
        }
        if (sp == 0x7b)
        {
            vu1Only("WAITP", pc);
            return "";
        }
    }
    if (l.type == LType::NORM)
    {
        const uint32_t op = l.op, dd = l.dd, lm = destMask(dd), ft = l.ft, fs = l.fs;
        const int imm = l.imm;
        const std::string vft = "vf[" + S(ft) + "]";
        if (op == 0x00)
            return blendWrap(vft, memLq(F("vi[%u] + (%d)", fs, imm)), dd);
        if (op == 0x01)
            return memSq(lm, F("vi[%u] + (%d)", ft, imm), F("vf[%u]", fs));
        if (op == 0x04)
        {
            const int comp = (dd & 8) ? 0 : (dd & 4) ? 1 : (dd & 2) ? 2 : 3;
            return F("if (%u != 0) vi[%u] = ", ft, ft) + memIlw(F("vi[%u] + (%d)", fs, imm), comp) + ";";
        }
        if (op == 0x05)
            return memIsw(lm, F("vi[%u] + (%d)", fs, imm), F("vi[%u]", ft));
        if (op == 0x08 || op == 0x09)
        {
            const uint32_t imm15 = (lo & 0x7FF) | ((lo >> 10) & 0x7800);
            return F("if (%u != 0) vi[%u] = vl::i16(vi[%u] %c (%u));", ft, ft, fs, op == 0x08 ? '+' : '-', imm15);
        }
        if (op == 0x1c)
        {
            const uint32_t viT = ft & 15;
            return F("if (%u != 0) vi[%u] = static_cast<int32_t>(c.st.clip & 0x0FFFu);", viT, viT);
        }
        if (op == 0x11)
            return F("vl::LiftAccess::fcsetAt(c, cyc, 0x%06xu);", lo & 0xFFFFFF);
        if (op == 0x12)
            return F("vi[1] = ((c.st.clip & 0x%06xu) != 0) ? 1 : 0;", lo & 0xFFFFFF);
        if (op == 0x13)
            return F("vi[1] = ((c.st.clip | 0x%06xu) == 0xFFFFFFu) ? 1 : 0;", lo & 0xFFFFFF);
        if (op == 0x16) // FSAND (issue 025, VU0 program 0x0030)
        {
            const uint32_t viT = ft & 15;
            const uint32_t imm12 = (((lo >> 21) & 1) << 11) | (lo & 0x7FF);
            return F("vl::LiftAccess::flagsSync(c, fq, cyc); if (%u != 0) vi[%u] = static_cast<int32_t>((c.st.status & "
                     "0xFFFu) & 0x%03xu);",
                     viT, viT, imm12);
        }
        if (op == 0x1a)
            return F("vl::LiftAccess::flagsSync(c, fq, cyc); if (%u != 0) vi[%u] = static_cast<int32_t>(c.st.mac & "
                     "(vi[%u] & 0xFFFF));",
                     ft, ft, fs);
    }
    fail(F("Unknown lower op at pc %04x: lo=%08x", pc, lo));
}

// ---- legacy_find_reachable ----
std::vector<uint32_t> legacyFindReachable(const std::vector<uint8_t> &code, uint32_t entryPc)
{
    std::set<uint32_t> visited;
    std::vector<uint32_t> worklist{entryPc};
    while (!worklist.empty())
    {
        uint32_t pc = worklist.back();
        worklist.pop_back();
        while (!visited.count(pc))
        {
            visited.insert(pc);
            const auto [lo, up] = unpackPair(code, pc);
            const uint32_t eBit = (up >> 30) & 1;
            const Lower l = decodeLower(lo, pc);
            if (!(up >> 31) && l.type == LType::NORM &&
                (l.op == 0x20 || l.op == 0x28 || l.op == 0x29 || l.op == 0x2c || l.op == 0x2d || l.op == 0x2e ||
                 l.op == 0x2f))
            {
                const uint32_t dPc = pc + 8;
                visited.insert(dPc);
                worklist.push_back(l.tgt);
                if (l.op == 0x20)
                    break;
                pc = dPc + 8;
                continue;
            }
            if (eBit)
            {
                visited.insert(pc + 8);
                break;
            }
            pc += 8;
        }
    }
    return {visited.begin(), visited.end()};
}

// ---- native_call_analysis.analyze ----
constexpr size_t kMaxValues = 64;
using Value = std::optional<std::set<int>>; // None = unknown
using Regs = std::array<Value, 16>;
using State = std::pair<Regs, Regs>; // (vals, branch_vals)

int signed16(int x)
{
    x &= 0xffff;
    return (x & 0x8000) ? x - 0x10000 : x;
}

Value joinValue(const Value &a, const Value &b)
{
    if (!a || !b)
        return std::nullopt;
    std::set<int> u = *a;
    u.insert(b->begin(), b->end());
    if (u.size() > kMaxValues)
        return std::nullopt;
    return u;
}

template <class Fn> Value unary(const Value &a, Fn fn)
{
    if (!a)
        return std::nullopt;
    std::set<int> out;
    for (int x : *a)
        out.insert(fn(x));
    return out;
}

template <class Fn> Value binary(const Value &a, const Value &b, Fn fn)
{
    if (!a || !b || a->size() * b->size() > kMaxValues)
        return std::nullopt;
    std::set<int> out;
    for (int x : *a)
        for (int y : *b)
            out.insert(fn(x, y));
    return out;
}

const std::set<int> kBranches = {0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f};

struct NativeCallInfo
{
    std::vector<uint32_t> reachable;
    std::map<uint32_t, std::set<uint32_t>> jrTargets;
};

NativeCallInfo analyzeNativeCalls(const std::vector<uint8_t> &code, uint32_t entry)
{
    if (entry % 8 || size_t(entry) + 8 > code.size())
        fail("invalid VU entry");
    struct PairInfo
    {
        uint32_t lo, up;
        Lower l;
        int op;
    };
    auto pairAt = [&](int64_t pc) {
        if (pc < 0 || pc % 8 || size_t(pc) + 8 > code.size())
            fail(F("VU flow outside image at %04llx", (long long)pc));
        const auto [lo, up] = unpackPair(code, uint32_t(pc));
        PairInfo p{lo, up, decodeLower(lo, uint32_t(pc)), -1};
        p.op = (up >> 31) ? -1 : p.l.getOp();
        return p;
    };
    Regs initialRegs;
    initialRegs[0] = std::set<int>{0};
    std::map<uint32_t, State> states{{entry, {initialRegs, initialRegs}}};
    std::deque<uint32_t> queue{entry};
    std::set<uint32_t> seen, unresolved, delaySlots;
    std::map<uint32_t, std::set<uint32_t>> jrTargets;

    auto enqueue = [&](uint32_t pc, const State &state) {
        pairAt(pc);
        const auto it = states.find(pc);
        if (it == states.end())
        {
            states[pc] = state;
            queue.push_back(pc);
            return;
        }
        State merged;
        for (int r = 0; r < 16; ++r)
        {
            merged.first[r] = joinValue(it->second.first[r], state.first[r]);
            merged.second[r] = joinValue(it->second.second[r], state.second[r]);
        }
        if (merged != it->second)
        {
            it->second = merged;
            queue.push_back(pc);
        }
    };
    auto transfer = [&](uint32_t pc, const State &state) -> State {
        Regs vals = state.first;
        const Regs prior = vals;
        const PairInfo p = pairAt(pc);
        if (p.up >> 31)
            return {vals, vals};
        const Usage usage = lowerUsage(p.lo);
        if ((usage.viRead | usage.viWrite) >> 16)
            fail(F("noncanonical VI operand unsupported in native-call graph at %04x", pc));
        for (int reg = 1; reg < 16; ++reg)
            if (usage.viWrite & (1u << reg))
                vals[reg] = std::nullopt;
        const uint32_t fs = p.l.fs & 15, ft = p.l.ft & 15, fd = p.l.fd & 15;
        if (p.op == 0x21)
        {
            if (ft)
                vals[ft] = std::set<int>{signed16(int((pc + 16) / 8))};
        }
        else if (p.op == 0x08 || p.op == 0x09)
        {
            const int imm = int((p.lo & 0x7ff) | ((p.lo >> 10) & 0x7800));
            const int delta = p.op == 0x08 ? imm : -imm;
            if (ft)
                vals[ft] = unary(prior[fs], [&](int x) { return signed16(x + delta); });
        }
        else if (p.l.type == LType::INT)
        {
            const std::string &name = p.l.name;
            const uint32_t target = name == "IADDI" ? ft : fd;
            if (target)
            {
                if (name == "IADDI")
                    vals[target] = unary(prior[fs], [&](int x) { return signed16(x + p.l.imm); });
                else if (name == "IADD")
                    vals[target] = binary(prior[fs], prior[ft], [](int x, int y) { return signed16(x + y); });
                else if (name == "ISUB")
                    vals[target] = binary(prior[fs], prior[ft], [](int x, int y) { return signed16(x - y); });
                else if (name == "IAND")
                    vals[target] = binary(prior[fs], prior[ft], [](int x, int y) { return signed16(x & y); });
                else if (name == "IOR")
                    vals[target] = binary(prior[fs], prior[ft], [](int x, int y) { return signed16(x | y); });
            }
        }
        vals[0] = std::set<int>{0};
        Regs nextBranch = vals;
        const PairTiming timing = pairTiming(p.lo, p.up);
        if (timing.firstVi && (timing.flags & FLAG_BRANCH_DELAY))
            nextBranch[timing.firstVi] = prior[timing.firstVi];
        return {vals, nextBranch};
    };

    while (!queue.empty())
    {
        const uint32_t pc = queue.front();
        queue.pop_front();
        const State state = states[pc];
        seen.insert(pc);
        const PairInfo p = pairAt(pc);
        if (p.op == 0x25)
            fail(F("JALR not supported at %04x", pc));
        const bool eBit = (p.up >> 30) & 1;
        if (kBranches.count(p.op) || eBit)
        {
            const PairInfo d = pairAt(pc + 8);
            if (kBranches.count(d.op) || ((d.up >> 30) & 1))
                fail(F("branch/E-bit in delay slot %04x", pc + 8));
            if (kBranches.count(p.op) && eBit)
                fail(F("branch with E-bit at %04x", pc));
            seen.insert(pc + 8);
            delaySlots.insert(pc + 8);
            const State out = transfer(pc + 8, transfer(pc, state));
            if (eBit)
                continue;
            if (p.op == 0x20 || p.op == 0x21)
                enqueue(p.l.tgt, out);
            else if (p.op == 0x24)
            {
                const Value &values = state.second[p.l.fs & 15];
                if (!values)
                {
                    unresolved.insert(pc);
                    continue;
                }
                std::set<uint32_t> dests;
                for (int x : *values)
                    dests.insert((uint32_t(x & 0xffff) * 8) & pcMask());
                jrTargets[pc].insert(dests.begin(), dests.end());
                for (uint32_t dst : dests)
                    enqueue(dst, out);
            }
            else
            {
                enqueue(p.l.tgt, out);
                enqueue(pc + 16, out);
            }
        }
        else
            enqueue(pc + 8, transfer(pc, state));
    }
    for (const auto &[pc, targets] : jrTargets)
    {
        const PairInfo p = pairAt(pc);
        if (!states[pc].second[p.l.fs & 15])
            unresolved.insert(pc);
    }
    if (!unresolved.empty())
    {
        std::string list;
        for (uint32_t pc : unresolved)
            list += (list.empty() ? "" : ",") + F("%04x", pc);
        fail("Unproven JR target(s): " + list);
    }
    for (uint32_t slot : delaySlots)
        if (states.count(slot))
            fail("independent delay-slot entry is not supported");
    return {{seen.begin(), seen.end()}, jrTargets};
}

// ---- find_reachable ----
struct Reachable
{
    std::vector<uint32_t> pcs;
    std::optional<NativeCallInfo> calls;
};

Reachable findReachableWithCalls(const std::vector<uint8_t> &code, uint32_t entryPc)
{
    std::vector<uint32_t> legacy = legacyFindReachable(code, entryPc);
    bool needsCalls = false;
    for (uint32_t pc : legacy)
    {
        const auto [lo, up] = unpackPair(code, pc);
        const Lower l = decodeLower(lo, pc);
        if (!(up >> 31) && l.type == LType::NORM && (l.op == 0x21 || l.op == 0x24 || l.op == 0x25))
            needsCalls = true;
    }
    if (!needsCalls)
        return {legacy, std::nullopt};
    NativeCallInfo info = analyzeNativeCalls(code, entryPc);
    std::vector<uint32_t> pcs = info.reachable;
    return {pcs, std::move(info)};
}

// ---- upper_writes_vf / lower_reads_vf / lower_writes_vf (0 = None) ----
bool isAccOp(const std::string &name)
{
    for (const char *a : {"ADDA", "SUBA", "MULA", "MADDA", "MSUBA", "OPMULA", "OPMSUB", "CLIP", "NOP"})
        if (name.rfind(a, 0) == 0)
            return true;
    return false;
}

uint32_t upperWritesVf(const Upper &u)
{
    if (isAccOp(u.name) || u.d == 0)
        return 0;
    if (u.special)
    {
        if (u.name.rfind("ITOF", 0) == 0 || u.name.rfind("FTOI", 0) == 0 || u.name.rfind("ABS", 0) == 0)
            return u.ft;
        return 0;
    }
    return u.fd;
}

std::vector<uint32_t> lowerReadsVf(const Lower &l)
{
    if (l.type == LType::SPEC)
    {
        if (l.sp == 0x30 || l.sp == 0x31 || l.sp == 0x35 || l.sp == 0x37 || l.sp == 0x3c)
            return {l.fs};
        if (l.sp == 0x38 || l.sp == 0x3a)
            return {l.fs, l.ft};
        if (l.sp == 0x39)
            return {l.ft};
    }
    else if (l.type == LType::NORM && l.op == 0x01)
        return {l.fs};
    return {};
}

uint32_t lowerWritesVf(const Lower &l)
{
    if (l.type == LType::SPEC)
    {
        if ((l.sp == 0x30 || l.sp == 0x31 || l.sp == 0x34 || l.sp == 0x36 || l.sp == 0x3d) && l.dd != 0)
            return l.ft;
    }
    else if (l.type == LType::NORM && l.op == 0x00 && l.dd != 0)
        return l.ft;
    return 0;
}

const std::set<uint32_t> kMacStatusReaders = {0x14, 0x15, 0x16, 0x17, 0x18, 0x1a, 0x1b};

void replaceAll(std::string &text, const std::string &from, const std::string &to)
{
    for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size()))
        text.replace(pos, from.size(), to);
}

bool contains(const std::string &text, const char *needle) { return text.find(needle) != std::string::npos; }

std::string join(const std::vector<std::string> &items, const std::string &sep)
{
    std::string out;
    for (size_t i = 0; i < items.size(); ++i)
        out += (i ? sep : "") + items[i];
    return out;
}

// Issue 025 (port of find_dead_pushes): the destination mask of the first "fmac<Op, Dest>" in an upper's code, or
// -1 if there is none.
int fmacDest(const std::string &upCode)
{
    for (size_t pos = upCode.find("fmac<"); pos != std::string::npos; pos = upCode.find("fmac<", pos + 1))
    {
        size_t p = pos + 5, q = p;
        while (q < upCode.size() && (std::isalnum(static_cast<unsigned char>(upCode[q])) || upCode[q] == '_'))
            ++q;
        if (q == p || upCode.compare(q, 2, ", ") != 0)
            continue;
        size_t d = q + 2, e = d;
        while (e < upCode.size() && std::isdigit(static_cast<unsigned char>(upCode[e])))
            ++e;
        if (e == d || e >= upCode.size() || upCode[e] != '>')
            continue;
        return std::stoi(upCode.substr(d, e - d));
    }
    return -1;
}

const std::set<uint32_t> kBranchOps = {0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f};
constexpr uint32_t kPathEnd = 0xFFFFFFFFu;     // program end (Python _END)
constexpr uint32_t kPathUnknown = 0xFFFFFFFEu; // no proven successor (Python None)

// Issue 025 v10 V2 (port of pair_successors): the pairs that can run after each pair. A branch's delay slot runs
// next, then its target and (conditional) the fall-through; JR goes to its proven targets; an E bit ends the
// program after its delay slot; a jump straight into a delay slot continues after it.
std::map<uint32_t, std::set<uint32_t>> pairSuccessors(const std::vector<uint8_t> &code,
                                                      const std::vector<uint32_t> &reachable,
                                                      const NativeCallInfo *calls)
{
    const std::set<uint32_t> reach(reachable.begin(), reachable.end());
    auto jrOf = [&](uint32_t pc) -> const std::set<uint32_t> * {
        if (!calls)
            return nullptr;
        const auto it = calls->jrTargets.find(pc);
        return it == calls->jrTargets.end() ? nullptr : &it->second;
    };
    std::set<uint32_t> jumpedTo;
    for (uint32_t pc : reachable)
    {
        const auto [lo, up] = unpackPair(code, pc);
        const Lower l = decodeLower(lo, pc);
        if (!((up >> 31) & 1) && l.type == LType::NORM && kBranchOps.count(l.op))
        {
            if (l.op == 0x24)
            {
                if (const auto *jr = jrOf(pc))
                    jumpedTo.insert(jr->begin(), jr->end());
            }
            else if (l.op != 0x25)
                jumpedTo.insert(l.tgt);
        }
    }
    std::map<uint32_t, std::set<uint32_t>> succ;
    for (uint32_t pc : reachable)
    {
        const auto [lo, up] = unpackPair(code, pc);
        if (!succ.count(pc))
            succ[pc] = {pc + 8};
        const Lower l = decodeLower(lo, pc);
        const uint32_t dPc = pc + 8;
        if ((up >> 30) & 1)
        {
            succ[pc] = {dPc};
            succ[dPc] = {kPathEnd};
        }
        else if (!((up >> 31) & 1) && l.type == LType::NORM && kBranchOps.count(l.op))
        {
            succ[pc] = {dPc};
            if (l.op == 0x24)
            {
                const auto *jr = jrOf(pc);
                succ[dPc] = (jr && !jr->empty()) ? *jr : std::set<uint32_t>{kPathUnknown};
            }
            else if (l.op == 0x25)
                succ[dPc] = {kPathUnknown};
            else if (l.op == 0x20 || l.op == 0x21)
                succ[dPc] = {l.tgt};
            else
                succ[dPc] = {l.tgt, pc + 16};
            if (jumpedTo.count(dPc))
                succ[dPc].insert(pc + 16);
        }
    }
    for (auto &[pc, next] : succ)
    {
        std::set<uint32_t> mapped;
        for (uint32_t q : next)
            mapped.insert(q == kPathEnd || q == kPathUnknown || reach.count(q) ? q : kPathUnknown);
        next = mapped;
    }
    return succ;
}

// FMAC flag results nothing can observe (same rule as tools/vu1_translator.py find_dead_pushes): on every path from
// the pair after it, the next flag-producing FMAC comes before any flag reader, and no reader, end of program, D/T
// bit or unproven jump comes in the three pairs after that FMAC.
std::set<uint32_t> findDeadPushes(const std::vector<uint8_t> &code, const std::vector<uint32_t> &reachable,
                                  const NativeCallInfo *calls)
{
    struct Info { bool push, reader, stop; };
    std::map<uint32_t, Info> info;
    for (uint32_t pc : reachable)
    {
        const auto [lo, up] = unpackPair(code, pc);
        const bool iBit = (up >> 31) & 1;
        const Lower l = decodeLower(lo, pc);
        const int dest = fmacDest(translateUpper(decodeUpper(up & 0x07ffffff)));
        const bool lowerNorm = !iBit && l.type == LType::NORM;
        info[pc] = {dest > 0, lowerNorm && kMacStatusReaders.count(l.op) != 0, ((up >> 27) & 0x3) != 0};
    }
    const auto succ = pairSuccessors(code, reachable, calls);
    std::set<uint32_t> dead;
    for (uint32_t pc : reachable)
    {
        const Info &p = info.at(pc);
        if (!p.push || p.stop)
            continue;
        // States: (pair, n) with n = 0 before the next push, n = 1..3 the pairs still checked after it.
        std::vector<std::pair<uint32_t, int>> todo;
        for (uint32_t q : succ.at(pc))
            todo.push_back({q, 0});
        std::set<std::pair<uint32_t, int>> seen;
        bool ok = true;
        while (!todo.empty() && ok)
        {
            const auto [q, n] = todo.back();
            todo.pop_back();
            if (!seen.insert({q, n}).second)
                continue;
            if (q == kPathUnknown || q == kPathEnd || info.at(q).reader || info.at(q).stop)
            {
                ok = false;
                break;
            }
            int nxt;
            if (n == 0)
                nxt = info.at(q).push ? 1 : 0;
            else
                nxt = n < 3 ? n + 1 : -1;
            if (nxt >= 0)
                for (uint32_t r : succ.at(q))
                    todo.push_back({r, nxt});
        }
        if (ok)
            dead.insert(pc);
    }
    return dead;
}

// Issue 025 v10 V1 (port of norm_known_states): which VF lanes hold zero or a normal float before each pair. The
// destination lanes of ADD, SUB and MUL (not MADD/MSUB) are zero or normal on every path of vl::fmac; every other VF
// write makes its lanes unknown. Straight-line code only: empty at each block, each label and after a program end.
bool normFmacName(const std::string &name)
{
    for (const char *base : {"ADD", "SUB", "MUL"})
        if (name == base || (name.size() == 4 && name.compare(0, 3, base) == 0 &&
                             std::string("xyzwqi").find(name[3]) != std::string::npos))
            return true;
    return false;
}

std::map<uint32_t, std::map<uint32_t, uint32_t>> normKnownStates(const std::vector<uint8_t> &code,
                                                                 const std::vector<std::vector<uint32_t>> &blocks,
                                                                 const std::set<uint32_t> &targets)
{
    std::map<uint32_t, std::map<uint32_t, uint32_t>> states;
    for (const auto &block : blocks)
    {
        std::map<uint32_t, uint32_t> state;
        std::optional<uint32_t> stopAfter;
        for (uint32_t pc : block)
        {
            if (targets.count(pc))
                state.clear();
            states[pc] = state;
            const auto [lo, up] = unpackPair(code, pc);
            const Upper u = decodeUpper(up & 0x07ffffff);
            if (!((up >> 31) & 1))
            {
                const Lower l = decodeLower(lo, pc);
                if (const uint32_t w = lowerWritesVf(l))
                    state[w] &= ~l.dd;
            }
            if (const uint32_t w = upperWritesVf(u))
            {
                if (!u.special && normFmacName(u.name))
                    state[w] |= u.d;
                else
                    state[w] &= ~u.d;
            }
            if ((up >> 30) & 1)
                stopAfter = pc + 8;
            else if (stopAfter && pc == *stopAfter)
                state.clear();
        }
    }
    return states;
}

// Issue 025 v10 V3 (port of ready_known_states): ready-table slots that cannot delay a pair (their last writer lies
// at least its latency in pairs earlier in the same straight-line code). Empty at each block, label and program end.
std::map<uint32_t, std::set<int>> readyKnownStates(const std::vector<uint8_t> &code,
                                                   const std::vector<std::vector<uint32_t>> &blocks,
                                                   const std::set<uint32_t> &targets)
{
    std::map<uint32_t, std::set<int>> states;
    for (const auto &block : blocks)
    {
        std::map<int, std::pair<int, uint32_t>> last; // slot -> (index of its writer, latency)
        int index = 0;
        std::optional<uint32_t> stopAfter;
        for (uint32_t pc : block)
        {
            if (targets.count(pc))
                last.clear();
            const auto [lo, up] = unpackPair(code, pc);
            std::set<int> &ready = states[pc];
            for (const auto &[slot, at] : last)
                if (index - at.first >= int(at.second))
                    ready.insert(slot);
            for (const auto &[slot, lat] : pairTiming(lo, up).writes)
                last[slot] = {index, lat};
            ++index;
            if ((up >> 30) & 1)
                stopAfter = pc + 8;
            else if (stopAfter && pc == *stopAfter)
                last.clear();
        }
    }
    return states;
}

struct Segment
{
    const std::vector<uint8_t> &code;
    bool queueFmacFlags = true;
    std::set<uint32_t> deadPushes;
    std::vector<std::pair<uint32_t, PairTiming>> timings;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> normStates; // V1
    std::map<uint32_t, std::set<int>> readyStates;               // V3
    std::set<std::pair<uint32_t, int>> deadWrites;               // V3: (pc, slot)
    std::set<uint32_t> deadHorizon;                              // V3

    // Sets g_normKnown for pc while it lives (the emitted pair's operands).
    struct NormScope
    {
        explicit NormScope(const Segment &s, uint32_t pc)
        {
            const auto it = s.normStates.find(pc);
            g_normKnown = it == s.normStates.end() ? nullptr : &it->second;
        }
        ~NormScope() { g_normKnown = nullptr; }
    };

    std::string fmacFlagCode(uint32_t pc) const
    {
        if (queueFmacFlags && !deadPushes.empty() && deadPushes.count(pc))
            return "    if (fl.pending) { fl.pending = false; stk |= fl.cur | fl.extra; }";
        if (queueFmacFlags && !deadPushes.empty())
            return "    if (fl.pending) { fl.pending = false; vl::LiftAccess::flagsPush(c, fq, cyc, fl.mac, fl.cur, "
                   "fl.extra | stk); stk = 0; }";
        if (queueFmacFlags)
            return "    if (fl.pending) { fl.pending = false; vl::LiftAccess::flagsPush(c, fq, cyc, fl.mac, fl.cur, "
                   "fl.extra); }";
        return "    if (fl.pending) { fl.pending = false; lastMac = fl.mac; lastCur = fl.cur; sticky6 |= fl.cur | "
               "fl.extra; anyFmac = true; }";
    }

    // ---- emit_pair ----
    void emitPair(const Upper &u, const Lower &l, uint32_t lo, bool iBit, uint32_t pc, std::vector<std::string> &out)
    {
        uint32_t shadow = 0;
        const uint32_t w = upperWritesVf(u);
        if (w != 0)
        {
            const auto reads = lowerReadsVf(l);
            if (std::find(reads.begin(), reads.end(), w) != reads.end() || w == lowerWritesVf(l))
            {
                shadow = w;
                out.push_back(F("    shadow_vf = vf[%u];", shadow));
            }
        }
        std::string upCode;
        {
            const NormScope scope(*this, pc);
            upCode = translateUpper(u);
        }
        std::string loCode = translateLower(l, lo, iBit, pc);
        if (shadow)
            replaceAll(loCode, F("vf[%u]", shadow), "shadow_vf");
        if (!upCode.empty())
        {
            out.push_back("    " + upCode);
            if (contains(upCode, "fmac<"))
                out.push_back(fmacFlagCode(pc));
        }
        if (!loCode.empty())
            out.push_back("    " + loCode);
    }

    void emitOne(uint32_t pc, std::vector<std::string> &body)
    {
        const auto [lo, up] = unpackPair(code, pc);
        emitPair(decodeUpper(up & 0x07ffffff), decodeLower(lo, pc), lo, (up >> 31) & 1, pc, body);
    }

    // ---- timing_begin / timing_end ----
    std::vector<std::string> timingBegin(uint32_t pc)
    {
        const auto [lo, up] = unpackPair(code, pc);
        PairTiming t = pairTiming(lo, up);
        timings.push_back({pc, t});
        std::vector<std::string> lines;
        const auto ready = readyStates.find(pc);
        std::vector<std::string> checks;
        for (int slot : t.reads)
            if (ready == readyStates.end() || !ready->second.count(slot))
                checks.push_back(F("r_ = rdy[%d] > r_ ? rdy[%d] : r_;", slot, slot));
        if (!checks.empty())
            lines.push_back("    { uint64_t r_ = cyc; " + join(checks, " ") +
                            " if (r_ > cyc) vl::LiftAccess::stall(c, cyc, r_); }");
        if (t.flags & (FLAG_FDIV | FLAG_WAITQ))
            lines.push_back("    vl::LiftAccess::fdivWait(c, cyc);");
        if (t.flags & (FLAG_EFU | FLAG_WAITP))
            fail(F("EFU timing at %04x is not supported by the lifter", pc));
        if (t.flags & FLAG_XGKICK)
            lines.push_back("    vl::LiftAccess::kickWait(c, cyc);");
        if (t.firstVi && (t.flags & FLAG_BRANCH_DELAY))
            lines.push_back(F("    ov = vi[%u];", t.firstVi));
        return lines;
    }

    std::vector<std::string> timingEnd(uint32_t pc) const
    {
        const auto [lo, up] = unpackPair(code, pc);
        const PairTiming t = pairTiming(lo, up);
        std::vector<std::string> lines;
        std::vector<std::string> w;
        for (const auto &[slot, lat] : t.writes)
            if (!deadWrites.count({pc, slot}))
                w.push_back(F("rdy[%d] = cyc + %u;", slot, lat));
        if (!w.empty())
            lines.push_back("    " + join(w, " "));
        if (!t.writes.empty() && !deadHorizon.count(pc))
            lines.push_back(F("    if (cyc + %u > hz) hz = cyc + %u;", t.maxLat, t.maxLat));
        if (t.firstVi && (t.flags & FLAG_BRANCH_DELAY))
            lines.push_back(F("    bbValid = true; bbReg = %u; bbVal = ov;", t.firstVi));
        else
            lines.push_back("    bbValid = false;");
        lines.push_back("    vl::LiftAccess::tick(c, cyc);");
        return lines;
    }

    // ---- find_dead_timing (issue 025 v10 V3) ----
    // A ready-table write of a slot is left out when on every path from the next pair the slot is written again (or
    // the program ends) before a stall check or region guard reads it; a horizon update when on every path a later
    // update is at least as far before the end (see tools/vu1_translator.py find_dead_timing).
    void findDeadTiming(const std::vector<uint32_t> &reachable,
                        const std::map<uint32_t, std::vector<uint32_t>> &regionStart, const NativeCallInfo *calls)
    {
        const auto succ = pairSuccessors(code, reachable, calls);
        std::map<uint32_t, std::set<int>> reads, writes;
        std::map<uint32_t, std::optional<uint32_t>> hzl;
        for (uint32_t pc : reachable)
        {
            const auto [lo, up] = unpackPair(code, pc);
            const PairTiming t = pairTiming(lo, up);
            const auto ready = readyStates.find(pc);
            auto isReady = [&](int slot) { return ready != readyStates.end() && ready->second.count(slot); };
            std::set<int> &r = reads[pc];
            for (int slot : t.reads)
                if (!isReady(slot))
                    r.insert(slot);
            const auto region = regionStart.find(pc);
            if (region != regionStart.end())
                for (const auto &[slot, k] : simulateRegion(region->second).guards)
                    if (!isReady(slot))
                        r.insert(slot);
            std::set<int> &wr = writes[pc];
            for (const auto &[slot, lat] : t.writes)
                wr.insert(slot);
            hzl[pc] = t.writes.empty() ? std::nullopt : std::optional<uint32_t>(t.maxLat);
        }
        for (uint32_t pc : reachable)
        {
            for (int slot : writes.at(pc))
            {
                std::vector<uint32_t> todo(succ.at(pc).begin(), succ.at(pc).end());
                std::set<uint32_t> seen;
                bool live = false;
                while (!todo.empty() && !live)
                {
                    const uint32_t q = todo.back();
                    todo.pop_back();
                    if (seen.count(q) || q == kPathEnd)
                        continue;
                    seen.insert(q);
                    if (q == kPathUnknown || reads.at(q).count(slot))
                        live = true;
                    else if (!writes.at(q).count(slot))
                        todo.insert(todo.end(), succ.at(q).begin(), succ.at(q).end());
                }
                if (!live)
                    deadWrites.insert({pc, slot});
            }
            const std::optional<uint32_t> lat = hzl.at(pc);
            if (!lat)
                continue;
            std::vector<std::pair<uint32_t, uint32_t>> todo;
            for (uint32_t q : succ.at(pc))
                todo.push_back({q, 1u});
            std::set<std::pair<uint32_t, uint32_t>> seen;
            bool live = false;
            while (!todo.empty() && !live)
            {
                const auto [q, dist] = todo.back();
                todo.pop_back();
                if (!seen.insert({q, dist}).second)
                    continue;
                if (q == kPathUnknown || q == kPathEnd)
                    live = true;
                else if (!hzl.at(q) || dist + *hzl.at(q) < *lat)
                    for (uint32_t r : succ.at(q))
                        todo.push_back({r, std::min(dist + 1u, *lat)});
            }
            if (!live)
                deadHorizon.insert(pc);
        }
    }

    // ---- static_regions ----
    std::vector<std::vector<uint32_t>> staticRegions(const std::vector<std::vector<uint32_t>> &blocks,
                                                     const std::set<uint32_t> &targets,
                                                     const std::map<uint32_t, std::pair<uint32_t, std::string>> &branchInfo,
                                                     const std::set<uint32_t> &branchDelaySlots) const
    {
        constexpr uint32_t kStaticSpecialFlags = FLAG_FDIV | FLAG_EFU | FLAG_WAITQ | FLAG_WAITP | FLAG_XGKICK;
        std::set<uint32_t> special(branchDelaySlots);
        for (const auto &[pc, info] : branchInfo)
            special.insert(pc);
        for (const auto &block : blocks)
            for (size_t i = 0; i < block.size(); ++i)
            {
                const uint32_t pc = block[i];
                const auto [lo, up] = unpackPair(code, pc);
                if ((up >> 30) & 1)
                {
                    special.insert(pc);
                    if (i + 1 < block.size())
                        special.insert(block[i + 1]);
                }
                if (pairTiming(lo, up).flags & kStaticSpecialFlags)
                    special.insert(pc);
            }
        std::vector<std::vector<uint32_t>> regions;
        for (const auto &block : blocks)
        {
            std::vector<uint32_t> cur;
            for (uint32_t pc : block)
            {
                if (special.count(pc) || (targets.count(pc) && !cur.empty()))
                {
                    if (cur.size() >= 2)
                        regions.push_back(cur);
                    cur.clear();
                }
                if (!special.count(pc))
                    cur.push_back(pc);
            }
            if (cur.size() >= 2)
                regions.push_back(cur);
        }
        return regions;
    }

    // ---- simulate_region ----
    struct Simulation
    {
        std::vector<int> issue;
        std::map<int, int> guards, local;
        std::optional<int> hz;
        int length = 0;
    };

    Simulation simulateRegion(const std::vector<uint32_t> &region) const
    {
        Simulation s;
        int t = -1;
        for (uint32_t pc : region)
        {
            const auto [lo, up] = unpackPair(code, pc);
            const PairTiming tm = pairTiming(lo, up);
            int k = t + 1;
            for (int slot : tm.reads)
                if (s.local.count(slot))
                    k = std::max(k, s.local[slot]);
            for (int slot : tm.reads)
                if (!s.local.count(slot) && !s.guards.count(slot))
                    s.guards[slot] = k;
            for (const auto &[slot, lat] : tm.writes)
                s.local[slot] = k + int(lat);
            if (!tm.writes.empty())
                s.hz = std::max(s.hz.value_or(0), k + int(tm.maxLat));
            s.issue.push_back(k);
            t = k;
        }
        s.length = t + 1;
        return s;
    }

    // ---- emit_static_region ----
    void emitStaticRegion(const std::vector<uint32_t> &region, std::vector<std::string> &out)
    {
        static const char *const kObservers[] = {"c.st.q", "c.st.p",  "c.st.clip", "c.st.mac",   "c.st.status",
                                                 "LiftAccess::", "vl::sq", "vl::isw", "flagsPush", "flagsSync"};
        static const char *const kQuietObservers[] = {"vl::sq",    "vl::isw", "clipAt", "fcsetAt",
                                                      "c.st.clip", "c.st.q",  "c.st.p", "c.st.status"};
        const uint32_t start = region.front();
        const Simulation sim = simulateRegion(region);
        std::vector<std::vector<std::string>> bodies;
        for (uint32_t pc : region)
        {
            std::vector<std::string> body;
            emitOne(pc, body);
            bodies.push_back(body);
        }
        // calls = set(re.findall(r'LiftAccess::(\w+)', text)); quiet = calls <= {...}
        bool quiet = true;
        for (const auto &body : bodies)
            for (const auto &line : body)
                for (size_t pos = line.find("LiftAccess::"); pos != std::string::npos;
                     pos = line.find("LiftAccess::", pos + 1))
                {
                    size_t e = pos + 12;
                    while (e < line.size() && (std::isalnum(static_cast<unsigned char>(line[e])) || line[e] == '_'))
                        ++e;
                    const std::string call = line.substr(pos + 12, e - pos - 12);
                    if (call.empty())
                        continue;
                    if (call != "flagsPush" && call != "flagsSync" && call != "clipAt" && call != "fcsetAt")
                        quiet = false;
                }
        std::vector<int> pushAt;
        for (size_t i = 0; i < bodies.size(); ++i)
            for (const auto &line : bodies[i])
                if (contains(line, "flagsPush("))
                {
                    pushAt.push_back(sim.issue[i]);
                    break;
                }
        int window = 0;
        for (int k : pushAt)
        {
            int n = 0;
            for (int q : pushAt)
                if (k - 4 < q && q <= k)
                    ++n;
            window = std::max(window, n);
        }
        std::vector<std::pair<int, int>> guards; // (k, slot) sorted
        const auto ready = readyStates.find(start);
        for (const auto &[slot, k] : sim.guards)
            if (ready == readyStates.end() || !ready->second.count(slot))
                guards.push_back({k, slot});
        std::sort(guards.begin(), guards.end());
        std::vector<std::string> conds;
        for (const auto &[k, slot] : guards)
            conds.push_back(F("rdy[%d] <= t0 + %d", slot, k));
        if (quiet)
        {
            conds.push_back(F("vl::LiftAccess::quietUntil(c, t0 + %d)", sim.length));
            if (window)
                conds.push_back(F("vl::LiftAccess::flagRoom(fq, %du)", window));
        }
        out.push_back(F("    // static region %04x-%04x: %zu pairs, %d cycles", start, region.back(), region.size(),
                        sim.length) +
                      (quiet ? ", quiet" : ""));
        out.push_back("    { const uint64_t t0 = cyc;");
        out.push_back("    if (" + (conds.empty() ? std::string("true") : join(conds, " && ")) + ") {");
        out.push_back("    uint64_t synced = t0;");
        for (size_t i = 0; i < region.size(); ++i)
        {
            const uint32_t pc = region[i];
            const int k = sim.issue[i];
            const auto [lo, up] = unpackPair(code, pc);
            const PairTiming t = pairTiming(lo, up);
            out.push_back(F("    // %04x @+%d", pc, k));
            out.push_back(F("    cyc = t0 + %d;", k));
            bool observed = false;
            for (const auto &line : bodies[i])
            {
                if (quiet)
                {
                    for (const char *o : kQuietObservers)
                        observed = observed || contains(line, o);
                }
                else
                {
                    for (const char *o : kObservers)
                        observed = observed || contains(line, o);
                }
            }
            if (observed)
                out.push_back("    vl::LiftAccess::catchUp(c, synced, cyc);");
            const bool bd = t.firstVi && (t.flags & FLAG_BRANCH_DELAY);
            if (bd)
                out.push_back(F("    ov = vi[%u];", t.firstVi));
            out.insert(out.end(), bodies[i].begin(), bodies[i].end());
            if (bd)
                out.push_back(F("    bbValid = true; bbReg = %u; bbVal = ov;", t.firstVi));
            else
                out.push_back("    bbValid = false;");
        }
        out.push_back(F("    cyc = t0 + %d;", sim.length));
        if (!sim.local.empty())
        {
            std::vector<std::string> w;
            for (const auto &[slot, k] : sim.local)
                w.push_back(F("rdy[%d] = t0 + %d;", slot, k));
            out.push_back("    " + join(w, " "));
        }
        if (sim.hz)
            out.push_back(F("    if (t0 + %d > hz) hz = t0 + %d;", *sim.hz, *sim.hz));
        out.push_back("    vl::LiftAccess::catchUp(c, synced, cyc);");
        out.push_back(F("    goto rend_%04x;", start));
        out.push_back("    } }");
    }
};

} // namespace

std::vector<uint32_t> findReachable(const std::vector<uint8_t> &code, uint32_t entryPc)
{
    return findReachableWithCalls(code, entryPc).pcs;
}

// ---- translate_segment ----
std::string translateSegment(const std::vector<uint8_t> &code, uint32_t entryPc, const std::string &fnName,
                             bool staticTiming)
{
    const Reachable reach = findReachableWithCalls(code, entryPc);
    const std::vector<uint32_t> &reachable = reach.pcs;
    Segment seg{code};
    seg.queueFmacFlags = false;
    for (uint32_t rpc : reachable)
    {
        const auto [rlo, rup] = unpackPair(code, rpc);
        if ((rup >> 31) & 1)
            continue;
        const Lower rl = decodeLower(rlo, rpc);
        if (rl.type == LType::NORM && kMacStatusReaders.count(rl.op))
            seg.queueFmacFlags = true;
    }

    std::map<uint32_t, std::pair<uint32_t, std::string>> branchInfo;
    std::map<uint32_t, std::array<uint32_t, 3>> callInfo; // op, ft & 15, fs & 15
    std::set<uint32_t> branchDelaySlots, targets;
    for (uint32_t pc : reachable)
    {
        const auto [lo, up] = unpackPair(code, pc);
        const Lower l = decodeLower(lo, pc);
        if ((up >> 31) || l.type != LType::NORM)
            continue;
        const uint32_t op = l.op;
        if (!(op == 0x20 || op == 0x21 || op == 0x24 || op == 0x28 || op == 0x29 || op == 0x2c || op == 0x2d ||
              op == 0x2e || op == 0x2f))
            continue;
        const uint32_t tgt = l.tgt, ft = l.ft, fs = l.fs;
        std::string cond;
        if (op == 0x20 || op == 0x21 || op == 0x24)
            cond = "true";
        else if (op == 0x28)
            cond = F("(brvi(%u) == brvi(%u))", ft, fs);
        else if (op == 0x29)
            cond = F("(brvi(%u) != brvi(%u))", ft, fs);
        else if (op == 0x2c)
            cond = F("(brvi(%u) < 0)", fs);
        else if (op == 0x2d)
            cond = F("(brvi(%u) > 0)", fs);
        else if (op == 0x2e)
            cond = F("(brvi(%u) <= 0)", fs);
        else
            cond = F("(brvi(%u) >= 0)", fs);
        branchInfo[pc] = {tgt, cond};
        branchDelaySlots.insert(pc + 8);
        if (op == 0x24)
        {
            if (!reach.calls || !reach.calls->jrTargets.count(pc))
                fail(F("unproven JR at %04x", pc));
            const auto &jr = reach.calls->jrTargets.at(pc);
            targets.insert(jr.begin(), jr.end());
        }
        else
            targets.insert(tgt);
        if (op == 0x21 || op == 0x24)
            callInfo[pc] = {op, ft & 15, fs & 15};
    }
    if (!callInfo.empty() || entryPc != reachable.front())
        targets.insert(entryPc);

    std::vector<std::vector<uint32_t>> blocks;
    std::vector<uint32_t> currBlock;
    for (uint32_t pc : reachable)
    {
        if (currBlock.empty() || pc == currBlock.back() + 8)
            currBlock.push_back(pc);
        else
        {
            blocks.push_back(currBlock);
            currBlock = {pc};
        }
    }
    if (!currBlock.empty())
        blocks.push_back(currBlock);
    seg.readyStates = readyKnownStates(code, blocks, targets);
    seg.normStates = normKnownStates(code, blocks, targets);

    const bool queue = seg.queueFmacFlags;
    std::vector<std::string> out;
    out.push_back("void " + fnName + "(vl::Run &c)");
    out.push_back("{");
    out.push_back("    using namespace vl;");
    out.push_back("@@TIMING_TABLE@@");
    out.push_back("    vl::checkReset(c);");
    out.push_back("    alignas(16) V vf[32];");
    out.push_back("    for (int i = 0; i < 32; ++i) vf[i] = _mm_loadu_ps(c.st.vf[i]);");
    out.push_back("    int32_t vi[16];");
    out.push_back("    for (int i = 0; i < 16; ++i) vi[i] = c.st.vi[i];");
    out.push_back("    V acc = _mm_loadu_ps(c.st.acc);");
    out.push_back("    V shadow_vf = _mm_setzero_ps();");
    out.push_back("    float i_reg = c.st.i;");
    out.push_back("    Flags fl{ c.st.status & 0xFu, c.st.mac, (c.st.status >> 6) & 0x3Fu };");
    if (!queue)
        out.push_back("    uint32_t lastMac = 0, lastCur = 0, sticky6 = 0; bool anyFmac = false;");
    out.push_back("    uint8_t *mem = c.mem;");
    out.push_back("    uint64_t *rdy = vl::LiftAccess::readyTable(c);");
    out.push_back("    uint64_t cyc = vl::LiftAccess::cycle(c), hz = 0;");
    if (queue)
        seg.deadPushes = findDeadPushes(code, reachable, reach.calls ? &*reach.calls : nullptr);
    if (queue)
        out.push_back("    vl::LiftAccess::FlagQueue fq; vl::LiftAccess::flagsEnter(c, fq);");
    if (!seg.deadPushes.empty())
        out.push_back("    uint32_t stk = 0; // sticky bits of FMAC results nothing can observe (issue 025)");
    out.push_back("    bool bbValid = false; uint8_t bbReg = 0; int32_t bbVal = 0, ov = 0;");
    out.push_back("    auto brvi = [&](int r) -> int32_t { return r == 0 ? 0 : ((bbValid && bbReg == r) ? bbVal : "
                  "vi[r]); };");
    for (const auto &[bpc, info] : branchInfo)
    {
        out.push_back(F("    bool taken_%04x = false;", bpc));
        const auto call = callInfo.find(bpc);
        if (call != callInfo.end() && call->second[0] == 0x24)
            out.push_back(F("    uint32_t jump_%04x = 0;", bpc));
    }
    out.push_back("    auto checkpoint = [&] { if (!vl::resetRequested()) return;");
    out.push_back("        for (int n = 1; n < 32; ++n) _mm_storeu_ps(c.st.vf[n], vf[n]);");
    out.push_back("        for (int n = 1; n < 16; ++n) c.st.vi[n] = vi[n];");
    out.push_back("        _mm_storeu_ps(c.st.acc, acc);");
    if (!queue)
        out.push_back("        if (anyFmac) { c.st.mac = lastMac; c.st.status = (c.st.status & 0xFF0u) | lastCur | "
                      "(sticky6 << 6); }");
    if (queue)
        out.push_back("        vl::LiftAccess::flagsLeave(c, fq, cyc);");
    out.push_back("        c.st.i = i_reg; vl::LiftAccess::leave(c, cyc, hz, bbValid, bbReg, bbVal); throw "
                  "vl::ResetCanceled{}; };");
    out.push_back("    bool resetCanceled = false;");
    out.push_back("    try {");
    if (!callInfo.empty() || entryPc != reachable.front())
        out.push_back(F("    goto loc_%04x;", entryPc));
    out.push_back("");

    std::map<uint32_t, std::vector<uint32_t>> regionStart;
    std::map<uint32_t, uint32_t> regionEnd;
    if (staticTiming)
        for (const auto &r : seg.staticRegions(blocks, targets, branchInfo, branchDelaySlots))
        {
            regionStart[r.front()] = r;
            regionEnd[r.back()] = r.front();
        }
    seg.findDeadTiming(reachable, regionStart, reach.calls ? &*reach.calls : nullptr);

    auto label = [&](uint32_t pc) {
        if (targets.count(pc))
        {
            out.push_back(F("loc_%04x:;", pc));
            out.push_back("    checkpoint();");
        }
    };
    auto append = [&](const std::vector<std::string> &lines) { out.insert(out.end(), lines.begin(), lines.end()); };

    for (const auto &block : blocks)
    {
        size_t i = 0;
        while (i < block.size())
        {
            const uint32_t pc = block[i];
            const auto [lo, up] = unpackPair(code, pc);
            const bool eBit = (up >> 30) & 1;
            const bool iBit = (up >> 31) & 1;
            const Upper u = decodeUpper(up & 0x07ffffff);
            const Lower l = decodeLower(lo, pc);
            label(pc);
            if (regionStart.count(pc))
                seg.emitStaticRegion(regionStart[pc], out);
            if (branchInfo.count(pc))
            {
                const auto &[tgt, cond] = branchInfo[pc];
                out.push_back(F("    // %04x: branch to %04x", pc, tgt));
                append(seg.timingBegin(pc));
                std::string upCode;
                {
                    const Segment::NormScope scope(seg, pc);
                    upCode = translateUpper(u);
                }
                if (!upCode.empty())
                {
                    out.push_back("    " + upCode);
                    if (contains(upCode, "fmac<"))
                        out.push_back(seg.fmacFlagCode(pc));
                }
                out.push_back(F("    taken_%04x = ", pc) + cond + ";");
                const auto call = callInfo.find(pc);
                if (call != callInfo.end())
                {
                    const auto [op, ft, fs] = call->second;
                    if (op == 0x24)
                        out.push_back(F("    jump_%04x = (static_cast<uint32_t>(static_cast<uint16_t>(brvi(%u))) * "
                                        "8u) & 0x%04Xu;",
                                        pc, fs, pcMask()));
                    else if (ft)
                        out.push_back(F("    vi[%u] = %u;", ft, (pc + 16) / 8));
                }
                append(seg.timingEnd(pc));
                ++i;
                const uint32_t dPc = block.at(i);
                const auto [dLo, dUp] = unpackPair(code, dPc);
                label(dPc);
                out.push_back(F("    // %04x: delay slot for %04x", dPc, pc));
                append(seg.timingBegin(dPc));
                seg.emitPair(decodeUpper(dUp & 0x07ffffff), decodeLower(dLo, dPc), dLo, (dUp >> 31) & 1, dPc, out);
                append(seg.timingEnd(dPc));
                if (call != callInfo.end() && call->second[0] == 0x24)
                {
                    out.push_back(F("    switch (jump_%04x) {", pc));
                    for (uint32_t target : reach.calls->jrTargets.at(pc))
                        out.push_back(F("    case 0x%04xu: goto loc_%04x;", target, target));
                    out.push_back("    default: throw std::logic_error(\"verified VU JR target proof violated\");");
                    out.push_back("    }");
                }
                else
                    out.push_back(F("    if (taken_%04x) { taken_%04x = false; goto loc_%04x; }", pc, pc, tgt));
                ++i;
                continue;
            }
            out.push_back(F("    // %04x", pc));
            append(seg.timingBegin(pc));
            seg.emitPair(u, l, lo, iBit, pc, out);
            append(seg.timingEnd(pc));
            if (regionEnd.count(pc))
                out.push_back(F("rend_%04x:;", regionEnd[pc]));
            if (eBit)
            {
                ++i;
                const uint32_t dPc = block.at(i);
                const auto [dLo, dUp] = unpackPair(code, dPc);
                label(dPc);
                out.push_back(F("    // %04x: delay slot for e-bit at %04x", dPc, pc));
                append(seg.timingBegin(dPc));
                seg.emitPair(decodeUpper(dUp & 0x07ffffff), decodeLower(dLo, dPc), dLo, (dUp >> 31) & 1, dPc, out);
                append(seg.timingEnd(dPc));
                out.push_back(F("    c.st.pc = 0x%04x;", dPc + 8));
                out.push_back("    goto done;");
                ++i;
                continue;
            }
            ++i;
        }
    }

    out.push_back("done:;");
    out.push_back("    } catch (const vl::ResetCanceled &) { resetCanceled = true; }");
    if (queue)
        out.push_back("    vl::LiftAccess::flagsLeave(c, fq, cyc);");
    out.push_back("    vl::LiftAccess::leave(c, cyc, hz, bbValid, bbReg, bbVal);");
    out.push_back("    for (int i = 1; i < 32; ++i) _mm_storeu_ps(c.st.vf[i], vf[i]);");
    out.push_back("    c.st.vf[0][0] = 0.0f; c.st.vf[0][1] = 0.0f; c.st.vf[0][2] = 0.0f; c.st.vf[0][3] = 1.0f;");
    out.push_back("    for (int i = 1; i < 16; ++i) c.st.vi[i] = vi[i];");
    out.push_back("    c.st.vi[0] = 0;");
    out.push_back("    _mm_storeu_ps(c.st.acc, acc);");
    out.push_back("    c.st.i = i_reg;");
    if (!queue)
        out.push_back("    if (anyFmac) { c.st.mac = lastMac; c.st.status = (c.st.status & 0xFF0u) | lastCur | (sticky6 "
                      "<< 6); }");
    out.push_back("    if (resetCanceled) throw vl::ResetCanceled{};");
    out.push_back("}");

    std::vector<std::string> pcs, inits;
    for (const auto &[p, t] : seg.timings)
    {
        pcs.push_back(F("0x%04x", p));
        inits.push_back("        " + timingInitializer(t));
    }
    const std::vector<std::string> table = {
        "    static const uint16_t kPcs[] = {" + join(pcs, ", ") + "};",
        "    static const VuLiftTiming kTiming[] = {",
        join(inits, ",\n"),
        "    };",
        "    static std::atomic<int> s_timingState{0};",
        F("    if (!vl::LiftAccess::validate(c, kTiming, kPcs, %zuu, s_timingState, \"", seg.timings.size()) + fnName +
            "\")) { c.declined = true; return; }"};
    const auto at = std::find(out.begin(), out.end(), "@@TIMING_TABLE@@");
    const size_t index = size_t(at - out.begin());
    out.erase(at);
    out.insert(out.begin() + std::ptrdiff_t(index), table.begin(), table.end());
    // Every program uses the inlined FMAC fast path (issue 025 v10 V4b; see tools/vu1_translator.py).
    return join(out, "\n");
}

// ---- __main__ (disc mode) ----
namespace {
uint64_t fnv1a64(const uint8_t *data, size_t size)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i)
        h = (h ^ data[i]) * 1099511628211ull;
    return h;
}

std::vector<uint8_t> readFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        fail("cannot read " + path.string());
    return {std::istreambuf_iterator<char>(in), {}};
}

struct DiscBins
{
    std::map<std::string, std::vector<uint8_t>> bins, covered;
};

// disc_vu_inputs._load_bins: load_disc_bins (VU1, 16 KB images) and load_vu0_bins (VU0, 4 KB, each range's SHA256
// over its own chunks)
DiscBins loadBins(const std::filesystem::path &elfPath, const Range *ranges, size_t rangeCount, size_t imageSize,
                  bool ownDigest)
{
    const std::vector<uint8_t> elf = readFile(elfPath);
    const std::string actual = dw5xl::setup::sha256Hex(elf.data(), elf.size());
    if (actual != kElfSha256)
        fail("Unsupported PAL executable SHA256: " + actual);
    std::map<std::string, std::vector<uint8_t>> sources;
    auto source = [&](const char *name) -> const std::vector<uint8_t> & {
        if (!name)
            return elf;
        auto it = sources.find(name);
        if (it == sources.end())
        {
            std::vector<uint8_t> blob = readFile(elfPath.parent_path() / name);
            const char *expected = nullptr;
            for (const auto &s : kSourceFiles)
                if (std::strcmp(s.name, name) == 0)
                    expected = s.sha256;
            if (!expected || dw5xl::setup::sha256Hex(blob.data(), blob.size()) != expected)
                fail(std::string("Unsupported disc file SHA256: ") + name);
            it = sources.emplace(name, std::move(blob)).first;
        }
        return it->second;
    };
    DiscBins out;
    for (size_t r = 0; r < rangeCount; ++r)
    {
        const Range &range = ranges[r];
        auto &image = out.bins.try_emplace(range.key, std::vector<uint8_t>(imageSize, 0)).first->second;
        auto &occupied = out.covered.try_emplace(range.key, std::vector<uint8_t>(imageSize, 0)).first->second;
        std::vector<uint8_t> own(imageSize, 0);
        for (uint32_t c = 0; c < range.chunkCount; ++c)
        {
            const Chunk &chunk = range.chunks[c];
            const std::vector<uint8_t> &src = source(chunk.source);
            if (size_t(chunk.offset) + chunk.size > src.size() || size_t(chunk.pc) + chunk.size > image.size())
                fail(std::string("Out-of-bounds VU input: ") + range.name);
            for (uint32_t i = 0; i < chunk.size; ++i)
            {
                const uint8_t byte = src[chunk.offset + i];
                if (occupied[chunk.pc + i] && image[chunk.pc + i] != byte)
                    fail(std::string("Overlapping VU inputs disagree: ") + range.name);
                image[chunk.pc + i] = byte;
                occupied[chunk.pc + i] = 1;
                own[chunk.pc + i] = byte;
            }
        }
        const std::vector<uint8_t> &checked = ownDigest ? own : image;
        if (size_t(range.end) + 8 > checked.size() ||
            dw5xl::setup::sha256Hex(&checked[range.start], range.end + 8 - range.start) != range.sha256)
            fail(std::string("VU instruction range mismatch: ") + range.name);
    }
    return out;
}

DiscBins loadDiscBins(const std::filesystem::path &elfPath)
{
    return loadBins(elfPath, kRanges, std::size(kRanges), 16384, false);
}

DiscBins loadVu0Bins(const std::filesystem::path &elfPath)
{
    return loadBins(elfPath, kVu0Ranges, std::size(kVu0Ranges), 4096, true);
}

// Restores the VU1 target when a VU0 translation ends (also on errors).
struct Vu0Target
{
    Vu0Target() { g_vu0 = true; }
    ~Vu0Target() { g_vu0 = false; }
};
} // namespace

bool generateLiftedVu1(const std::filesystem::path &elf, std::string &text, std::string &error,
                       const std::function<void(const std::string &)> &log,
                       const std::function<bool(const std::string &)> &staticTiming)
{
    try
    {
        auto say = [&](const std::string &line) {
            if (log)
                log(line);
        };
        const DiscBins disc = loadDiscBins(elf);
        std::vector<std::string> out{kPrologue};
        std::vector<std::string> regs{"    // Preserved observed full-image registrations (metadata only):"};
        for (const ImageRegistration &r : kImageRegistrations)
            regs.push_back(F("    { 0x%016llxull, 0x%04x, ", (unsigned long long)r.hash, r.pc) + r.name + ", 0u },");
        // sub_map (only feeds the capture-mode census): reports missing images like the Python
        for (const Target &t : kTargets)
            if (!disc.bins.count(t.key))
                say(std::string("Skipping ") + t.name + ": no captured microcode for " + t.key);
        if (!disc.bins.count("baa8e61743cea7a2"))
            fail("missing microcode image baa8e61743cea7a2 (flush functions)");
        regs.push_back("");
        regs.push_back("    // Subroutine-level footprint registrations (matches across all dynamic images):");
        for (const Target &t : kTargets)
        {
            const auto it = disc.bins.find(t.key);
            if (it == disc.bins.end())
            {
                say(std::string("Skipping ") + t.name + ": no captured microcode for " + t.key);
                continue;
            }
            const std::vector<uint8_t> &code = it->second;
            const std::vector<uint32_t> reach = findReachable(code, t.pc);
            const uint32_t subStart = std::min(*std::min_element(reach.begin(), reach.end()), t.pc);
            const size_t subSize = size_t(t.end) + 8 - subStart;
            const uint64_t subHash = fnv1a64(&code[subStart], subSize);
            say(F("Translating %s (PC=0x%04x, len=%zuB)...", t.name, t.pc, subSize));
            const std::vector<uint8_t> &cov = disc.covered.at(t.key);
            std::string missing;
            for (uint32_t p : reach)
            {
                bool all = size_t(p) + 8 <= cov.size();
                for (uint32_t b = 0; all && b < 8; ++b)
                    all = cov[p + b] != 0;
                if (!all)
                    missing += (missing.empty() ? "" : ", ") + std::to_string(p);
            }
            if (!missing.empty())
                fail(std::string("Unextracted reachable instructions in ") + t.name + ": [" + missing + "]");
            out.push_back(translateSegment(code, t.pc, t.name, !staticTiming || staticTiming(t.name)));
            const std::string startField = subStart != t.pc ? F(", 0x%04xu", subStart) : std::string();
            regs.push_back(F("    { 0x%016llxull, 0x%04x, ", (unsigned long long)subHash, t.pc) + t.name +
                           F(", %zuu", subSize) + startField + " },");
            // Issue 025: when the footprint has gaps, also register the reachable code alone (same as the Python).
            std::vector<std::pair<uint32_t, uint32_t>> runs;
            for (uint32_t p : reach)
            {
                if (!runs.empty() && runs.back().first + runs.back().second == p)
                    runs.back().second += 8;
                else
                    runs.emplace_back(p, 8u);
            }
            size_t runBytes = 0;
            for (const auto &r : runs)
                runBytes += r.second;
            if (runBytes != subSize)
            {
                std::vector<uint8_t> joined;
                std::string list;
                for (const auto &[start, size] : runs)
                {
                    joined.insert(joined.end(), code.begin() + start, code.begin() + start + size);
                    list += (list.empty() ? "" : ", ") + F("0x%04xu, %uu", start, size);
                }
                const uint64_t runHash = fnv1a64(joined.data(), joined.size());
                out.push_back(std::string("static const uint32_t kRuns_") + t.name + "[] = { " + list + " };");
                regs.push_back(F("    { 0x%016llxull, 0x%04x, ", (unsigned long long)runHash, t.pc) + t.name +
                               ", 0u, ~0u, false, kRuns_" + t.name + F(", %zuu },", runs.size()));
            }
        }
        out.push_back(kFlushFunctions);
        regs.push_back("    { 0x5344f2e1f3bd36fbull, 0x0220, lift_flush_0220, 16u },");
        regs.push_back("    { 0x5344f2e1f3bd36fbull, 0x0360, lift_flush_0360, 16u },");
        out.push_back("static const vl::Registration s_battle_registrations[] = {\n" + join(regs, "\n") + "\n};\n" +
                      "static vl::Registrar s_battle_registrar(s_battle_registrations, sizeof(s_battle_registrations) / "
                      "sizeof(s_battle_registrations[0]));\n");
        text = join(out, "\n\n");
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
}

// Port of tools/vu0_translator.py (issue 025).
bool generateLiftedVu0(const std::filesystem::path &elf, std::string &text, std::string &error,
                       const std::function<void(const std::string &)> &log,
                       const std::function<bool(const std::string &)> &staticTiming)
{
    try
    {
        const Vu0Target target;
        const DiscBins disc = loadVu0Bins(elf);
        std::vector<std::string> out{kPrologue};
        std::vector<std::string> regs;
        for (const Range &range : kVu0Ranges)
        {
            const std::vector<uint8_t> &code = disc.bins.at(range.key);
            const std::vector<uint8_t> &cov = disc.covered.at(range.key);
            const std::vector<uint32_t> reach = findReachable(code, range.start);
            std::string missing;
            for (uint32_t p : reach)
            {
                bool all = size_t(p) + 8 <= cov.size();
                for (uint32_t b = 0; all && b < 8; ++b)
                    all = cov[p + b] != 0;
                if (!all)
                    missing += (missing.empty() ? "" : ", ") + std::to_string(p);
            }
            if (!missing.empty())
                fail(std::string("Unextracted reachable VU0 instructions in ") + range.name + ": [" + missing + "]");
            if (reach.back() != range.end)
                fail(F("%s: last reachable pair 0x%04x, VU0_RANGES says 0x%04x", range.name, reach.back(), range.end));
            if (log)
                log(F("Translating %s (VU0 pc=0x%04x, %zu pairs)...", range.name, range.start, reach.size()));
            out.push_back(translateSegment(code, range.start, range.name, !staticTiming || staticTiming(range.name)));
            regs.push_back(std::string("    { 0x") + range.key + F("ull, 0x%04x, ", range.start) + range.name +
                           ", 0u, ~0u, true },");
        }
        out.push_back("static const vl::Registration s_vu0_registrations[] = {\n" + join(regs, "\n") + "\n};\n" +
                      "static vl::Registrar s_vu0_registrar(s_vu0_registrations, "
                      "sizeof(s_vu0_registrations) / sizeof(s_vu0_registrations[0]));\n");
        text = join(out, "\n\n");
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
}

bool loadVu1Images(const std::filesystem::path &elf, std::map<std::string, std::vector<uint8_t>> &images,
                   std::string &error)
{
    try
    {
        images = loadDiscBins(elf).bins;
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
}

} // namespace dw5xl::vu1lift
