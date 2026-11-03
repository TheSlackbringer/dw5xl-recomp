// Issue 009 v4.5: IOP module code -> C++. Every instruction does exactly what IopCpuCore::executeInstruction does
// (ps2xIOP/src/emulator/core/iop_cpu.cpp), including the load delay and the branch rules:
// - operands are read before the previous load is committed; the commit skips the register the instruction wrote;
// - a taken branch runs its delay slot as a delay slot (pc = target afterwards); after a branch that is not taken
//   the next instruction is an ordinary instruction (the interpreter's branchPending stays false);
// - branchTarget is 0 after an ordinary instruction and the target after a delay slot.
// An instruction without a native form, or one that would fault, touch anything but IOP RAM or not fit the budget,
// ends the run before it (the interpreter carries it out).
#include "iop_translator.h"

#include "ps2x/iop/iop_native.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <sstream>

namespace dw5xl::ioplift
{
    namespace
    {
        constexpr uint32_t kChunkWords = 512u;

        std::string hex(uint32_t value)
        {
            char text[16];
            std::snprintf(text, sizeof(text), "0x%08Xu", value);
            return text;
        }

        std::string reg(uint32_t r) { return r == 0u ? std::string("0u") : "R[" + std::to_string(r) + "]"; }

        // `R[r] = expr;` (nothing for $zero), then the commit for written register r.
        std::string write(uint32_t r, const std::string &expr)
        {
            if (r == 0u)
                return "IOPN_COMMIT(0u);";
            return "R[" + std::to_string(r) + "] = " + expr + "; IOPN_COMMIT(" + std::to_string(r) + "u);";
        }

        std::string load(uint32_t r, const std::string &value)
        {
            if (r == 0u)
                return "IOPN_COMMIT(0u);";
            return "{ const uint32_t lv_ = " + value + "; IOPN_COMMIT(0u); pl = true; plr = " + std::to_string(r) +
                   "u; plv = lv_; }";
        }

        bool isImportStub(const std::vector<uint32_t> &words, size_t i)
        {
            return words[i] == 0x03E00008u && i + 1u < words.size() && (words[i + 1u] & 0xFFFF0000u) == 0x24000000u;
        }

        struct Branch
        {
            bool conditional = false;
            std::string condition = "true";
            bool staticTarget = true;
            uint32_t target = 0u;
            uint32_t linkReg = 0u;       // 0: no link
            uint32_t targetReg = 0u;     // JR/JALR
        };

        std::optional<Branch> decodeBranch(uint32_t address, uint32_t w)
        {
            const uint32_t op = w >> 26u, rs = (w >> 21u) & 31u, rt = (w >> 16u) & 31u, rd = (w >> 11u) & 31u;
            const uint32_t fn = w & 63u;
            const int32_t simm = static_cast<int16_t>(w & 0xFFFFu);
            const uint32_t relTarget = address + 4u + (static_cast<uint32_t>(simm) << 2u);
            Branch b;
            switch (op)
            {
            case 0x00:
                if (fn == 0x08 || fn == 0x09)
                {
                    b.staticTarget = false;
                    b.targetReg = rs;
                    if (fn == 0x09)
                        b.linkReg = rd ? rd : 31u; // as the interpreter: rd 0 links $ra
                    return b;
                }
                return std::nullopt;
            case 0x01:
            {
                const std::string s = "static_cast<int32_t>(" + reg(rs) + ")";
                if (rt == 0x00 || rt == 0x10)
                    b.condition = s + " < 0";
                else if (rt == 0x01 || rt == 0x11)
                    b.condition = s + " >= 0";
                else
                    return std::nullopt;
                b.conditional = true;
                b.target = relTarget;
                if (rt == 0x10 || rt == 0x11)
                    b.linkReg = 31u;
                return b;
            }
            case 0x02:
            case 0x03:
                b.target = ((address + 4u) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2u);
                if (op == 0x03)
                    b.linkReg = 31u;
                return b;
            case 0x04:
                b.conditional = true;
                b.condition = reg(rs) + " == " + reg(rt);
                b.target = relTarget;
                return b;
            case 0x05:
                b.conditional = true;
                b.condition = reg(rs) + " != " + reg(rt);
                b.target = relTarget;
                return b;
            case 0x06:
                b.conditional = true;
                b.condition = "static_cast<int32_t>(" + reg(rs) + ") <= 0";
                b.target = relTarget;
                return b;
            case 0x07:
                b.conditional = true;
                b.condition = "static_cast<int32_t>(" + reg(rs) + ") > 0";
                b.target = relTarget;
                return b;
            default:
                return std::nullopt;
            }
        }

        struct Plain
        {
            std::string code; // statements; `EXIT` stands for the run's exit before this instruction
            bool store = false;
        };

        // A non-branch instruction with a native form; nullopt: the interpreter carries it out.
        std::optional<Plain> decodePlain(uint32_t w)
        {
            const uint32_t op = w >> 26u, rs = (w >> 21u) & 31u, rt = (w >> 16u) & 31u, rd = (w >> 11u) & 31u;
            const uint32_t sa = (w >> 6u) & 31u, fn = w & 63u, imm = w & 0xFFFFu;
            const int32_t simm = static_cast<int16_t>(imm);
            const std::string S = reg(rs), T = reg(rt);
            const std::string address = "const uint32_t a_ = " + S + " + " + hex(static_cast<uint32_t>(simm)) + ";";
            const std::string phys = "const uint32_t p_ = a_ & 0x1FFFFFFFu;";
            auto ram = [](uint32_t size) { return "if (p_ > IOPN_RAM - " + std::to_string(size) + "u) EXIT;"; };
            auto storeRoom = [] { return std::string("if (f.undo && f.undoCount == f.undoCapacity) EXIT;"); };
            // Outside IOP RAM: the runtime's hardware access at this instruction, then the run ends (IOPN_DONE).
            auto hwLoad = [](uint32_t size, const std::string &loadCode)
            {
                return "if (p_ > IOPN_RAM - " + std::to_string(size) + "u) { if (!f.hwRead) EXIT; const uint32_t h_ = f.hwRead(f.host, a_, " +
                       std::to_string(size) + "u, f.executed + n); " + loadCode + " IOPN_DONE }";
            };
            auto hwStore = [](uint32_t size, const std::string &value)
            {
                return "if (p_ > IOPN_RAM - " + std::to_string(size) + "u) { if (!f.hwWrite) EXIT; f.hwWrite(f.host, a_, " +
                       std::to_string(size) + "u, " + value + ", f.executed + n); IOPN_COMMIT(0u); IOPN_DONE }";
            };
            Plain p;
            switch (op)
            {
            case 0x00:
                switch (fn)
                {
                case 0x00: p.code = write(rd, T + " << " + std::to_string(sa) + "u"); return p;
                case 0x02: p.code = write(rd, T + " >> " + std::to_string(sa) + "u"); return p;
                case 0x03:
                    p.code = write(rd, "static_cast<uint32_t>(static_cast<int32_t>(" + T + ") >> " + std::to_string(sa) + ")");
                    return p;
                case 0x04: p.code = write(rd, T + " << (" + S + " & 31u)"); return p;
                case 0x06: p.code = write(rd, T + " >> (" + S + " & 31u)"); return p;
                case 0x07:
                    p.code = write(rd, "static_cast<uint32_t>(static_cast<int32_t>(" + T + ") >> (" + S + " & 31u))");
                    return p;
                case 0x10: p.code = write(rd, "hi"); return p;
                case 0x11: p.code = "hi = " + S + "; IOPN_COMMIT(0u);"; return p;
                case 0x12: p.code = write(rd, "lo"); return p;
                case 0x13: p.code = "lo = " + S + "; IOPN_COMMIT(0u);"; return p;
                case 0x18:
                    p.code = "{ const int64_t m_ = static_cast<int64_t>(static_cast<int32_t>(" + S +
                             ")) * static_cast<int64_t>(static_cast<int32_t>(" + T +
                             ")); lo = static_cast<uint32_t>(m_); hi = static_cast<uint32_t>(static_cast<uint64_t>(m_) >> 32u); } IOPN_COMMIT(0u);";
                    return p;
                case 0x19:
                    p.code = "{ const uint64_t m_ = static_cast<uint64_t>(" + S + ") * " + T +
                             "; lo = static_cast<uint32_t>(m_); hi = static_cast<uint32_t>(m_ >> 32u); } IOPN_COMMIT(0u);";
                    return p;
                case 0x1A:
                    p.code = "{ const int32_t x_ = static_cast<int32_t>(" + S + "), y_ = static_cast<int32_t>(" + T +
                             "); if (y_ == 0) { lo = x_ >= 0 ? 0xFFFFFFFFu : 1u; hi = static_cast<uint32_t>(x_); } "
                             "else if (x_ == INT32_MIN && y_ == -1) { lo = static_cast<uint32_t>(x_); hi = 0u; } "
                             "else { lo = static_cast<uint32_t>(x_ / y_); hi = static_cast<uint32_t>(x_ % y_); } } IOPN_COMMIT(0u);";
                    return p;
                case 0x1B:
                    p.code = "{ const uint32_t x_ = " + S + ", y_ = " + T +
                             "; if (y_ == 0u) { lo = 0xFFFFFFFFu; hi = x_; } else { lo = x_ / y_; hi = x_ % y_; } } IOPN_COMMIT(0u);";
                    return p;
                case 0x20:
                case 0x22:
                    p.code = "{ const int64_t r_ = static_cast<int64_t>(static_cast<int32_t>(" + S + ")) " +
                             (fn == 0x20 ? "+" : "-") + " static_cast<int64_t>(static_cast<int32_t>(" + T +
                             ")); if (r_ > INT32_MAX || r_ < INT32_MIN) EXIT; " +
                             write(rd, "static_cast<uint32_t>(static_cast<int32_t>(r_))") + " }";
                    return p;
                case 0x21: p.code = write(rd, S + " + " + T); return p;
                case 0x23: p.code = write(rd, S + " - " + T); return p;
                case 0x24: p.code = write(rd, S + " & " + T); return p;
                case 0x25: p.code = write(rd, S + " | " + T); return p;
                case 0x26: p.code = write(rd, S + " ^ " + T); return p;
                case 0x27: p.code = write(rd, "~(" + S + " | " + T + ")"); return p;
                case 0x2A:
                    p.code = write(rd, "(static_cast<int32_t>(" + S + ") < static_cast<int32_t>(" + T + ") ? 1u : 0u)");
                    return p;
                case 0x2B: p.code = write(rd, "(" + S + " < " + T + " ? 1u : 0u)"); return p;
                default: return std::nullopt; // syscall, break, reserved
                }
            case 0x08:
                p.code = "{ const int64_t r_ = static_cast<int64_t>(static_cast<int32_t>(" + S + ")) + " +
                         std::to_string(simm) + "; if (r_ > INT32_MAX || r_ < INT32_MIN) EXIT; " +
                         write(rt, "static_cast<uint32_t>(static_cast<int32_t>(r_))") + " }";
                return p;
            case 0x09: p.code = write(rt, S + " + " + hex(static_cast<uint32_t>(simm))); return p;
            case 0x0A:
                p.code = write(rt, "(static_cast<int32_t>(" + S + ") < " + std::to_string(simm) + " ? 1u : 0u)");
                return p;
            case 0x0B: p.code = write(rt, "(" + S + " < " + hex(static_cast<uint32_t>(simm)) + " ? 1u : 0u)"); return p;
            case 0x0C: p.code = write(rt, S + " & " + hex(imm)); return p;
            case 0x0D: p.code = write(rt, S + " | " + hex(imm)); return p;
            case 0x0E: p.code = write(rt, S + " ^ " + hex(imm)); return p;
            case 0x0F: p.code = write(rt, hex(imm << 16u)); return p;
            case 0x20:
            case 0x24:
            {
                auto value = [&](const std::string &byte)
                {
                    return op == 0x20 ? "static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(" + byte + ")))"
                                      : "static_cast<uint32_t>(static_cast<uint8_t>(" + byte + "))";
                };
                p.code = "{ " + address + " " + phys + " " + hwLoad(1, load(rt, value("h_"))) + " " +
                         load(rt, value("ram[p_]")) + " }";
                return p;
            }
            case 0x21:
            case 0x25:
            {
                auto value = [&](const std::string &half)
                {
                    return op == 0x21 ? "static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(" + half + ")))"
                                      : "static_cast<uint32_t>(static_cast<uint16_t>(" + half + "))";
                };
                p.code = "{ " + address + " if (a_ & 1u) EXIT; " + phys + " " + hwLoad(2, load(rt, value("h_"))) +
                         " uint16_t v_; std::memcpy(&v_, ram + p_, 2); " + load(rt, value("v_")) + " }";
                return p;
            }
            case 0x22:
            case 0x26:
            {
                const char *merge = op == 0x22
                    ? "(o_ & kLwlMask[a_ & 3u]) | (m_ << kLwlShift[a_ & 3u])"
                    : "(o_ & kLwrMask[a_ & 3u]) | (m_ >> kLwrShift[a_ & 3u])";
                p.code = "{ " + address + " const uint32_t p_ = (a_ & ~3u) & 0x1FFFFFFFu; " + ram(4) +
                         " uint32_t m_; std::memcpy(&m_, ram + p_, 4); const uint32_t o_ = " + T + "; " +
                         load(rt, merge) + " }";
                return p;
            }
            case 0x23:
                p.code = "{ " + address + " if (a_ & 3u) EXIT; " + phys + " " + hwLoad(4, load(rt, "h_")) +
                         " uint32_t v_; std::memcpy(&v_, ram + p_, 4); " + load(rt, "v_") + " }";
                return p;
            case 0x28:
                p.store = true;
                p.code = "{ " + address + " " + phys + " " + hwStore(1, "(" + T + " & 0xFFu)") + " " + storeRoom() +
                         " const uint8_t v_ = static_cast<uint8_t>(" + T + "); iopnStore(f, p_, &v_, 1u); } IOPN_COMMIT(0u);";
                return p;
            case 0x29:
                p.store = true;
                p.code = "{ " + address + " if (a_ & 1u) EXIT; " + phys + " " + hwStore(2, "(" + T + " & 0xFFFFu)") + " " +
                         storeRoom() + " const uint16_t v_ = static_cast<uint16_t>(" + T + "); iopnStore(f, p_, &v_, 2u); } IOPN_COMMIT(0u);";
                return p;
            case 0x2A:
            case 0x2E:
            {
                const char *merge = op == 0x2A ? "iopnSwl(o_, " : "iopnSwr(o_, ";
                p.store = true;
                p.code = "{ " + address + " const uint32_t p_ = (a_ & ~3u) & 0x1FFFFFFFu; " + ram(4) + " " +
                         storeRoom() + " uint32_t o_; std::memcpy(&o_, ram + p_, 4); const uint32_t v_ = " + merge + T +
                         ", a_ & 3u); iopnStore(f, p_, &v_, 4u); } IOPN_COMMIT(0u);";
                return p;
            }
            case 0x2B:
                p.store = true;
                p.code = "{ " + address + " if (a_ & 3u) EXIT; " + phys + " " + hwStore(4, T) + " " + storeRoom() +
                         " const uint32_t v_ = " + T + "; iopnStore(f, p_, &v_, 4u); } IOPN_COMMIT(0u);";
                return p;
            default:
                return std::nullopt; // COP0 (MFC0/MTC0/RFE), COP1-3, LWC/SWC, reserved
            }
        }

        std::string replaceDone(std::string code, const std::string &doneCode)
        {
            for (size_t at = code.find("IOPN_DONE"); at != std::string::npos; at = code.find("IOPN_DONE", at + doneCode.size()))
                code.replace(at, 9, doneCode);
            return code;
        }

        std::string replaceExit(std::string code, const std::string &exitCode)
        {
            for (size_t at = code.find("EXIT"); at != std::string::npos; at = code.find("EXIT", at + exitCode.size()))
                code.replace(at, 4, exitCode);
            return code;
        }

        class ChunkEmitter
        {
        public:
            ChunkEmitter(const ModuleText &module, const std::vector<uint32_t> &words, uint32_t chunk, std::ostringstream &out,
                         TranslationStats &stats)
                : m_module(module), m_words(words), m_out(out), m_stats(stats)
            {
                m_first = chunk * kChunkWords;
                m_last = std::min<size_t>(m_first + kChunkWords, words.size());
                m_base = module.base + static_cast<uint32_t>(m_first) * 4u;
                m_end = module.base + static_cast<uint32_t>(m_last) * 4u;
            }

            void emit(const std::string &functionName)
            {
                m_out << "static uint32_t " << functionName << "(F &f)\n{\n"
                      << "    IOPN_PROLOGUE\n"
                      << "dispatch:\n"
                      << "    if (pc - " << hex(m_base) << " >= " << hex(m_end - m_base) << " || (pc & 3u)) goto out;\n"
                      << "    switch ((pc - " << hex(m_base) << ") >> 2u)\n    {\n";
                for (size_t i = m_first; i < m_last; ++i)
                    m_out << "    case " << (i - m_first) << "u: goto L_" << std::hex << address(i) << std::dec << ";\n";
                m_out << "    default: goto out;\n    }\n";
                for (size_t i = m_first; i < m_last; ++i)
                    instruction(i);
                m_out << "    pc = " << hex(m_end) << "; goto out;\n"
                      << "out:\n    IOPN_EPILOGUE\n}\n\n";
            }

        private:
            uint32_t address(size_t index) const { return m_module.base + static_cast<uint32_t>(index) * 4u; }
            bool inText(uint32_t a) const
            {
                return a >= m_module.base && a < m_module.base + m_words.size() * 4u && (a & 3u) == 0u;
            }
            bool inChunk(uint32_t a) const { return a >= m_base && a < m_end && (a & 3u) == 0u; }
            std::string jumpTo(uint32_t target) const
            {
                if (inChunk(target))
                {
                    char label[32];
                    std::snprintf(label, sizeof(label), "goto L_%x;", target);
                    return "pc = " + hex(target) + "; " + label;
                }
                return "pc = " + hex(target) + "; goto out;";
            }

            void instruction(size_t i)
            {
                const uint32_t a = address(i);
                const uint32_t w = m_words[i];
                m_out << "L_" << std::hex << a << std::dec << ":\n";
                ++m_stats.words;
                const std::string exitHere = "{ pc = " + hex(a) + "; goto out; }";
                if (isImportStub(m_words, i))
                {
                    // The runtime's import call (as the interpreter's step() makes it), then the run ends.
                    ++m_stats.importStubs;
                    m_out << "    if (n == budget || !f.importCall) " << exitHere << "\n"
                          << "    pc = " << hex(a) << "; IOPN_STORE\n"
                          << "    if (!f.importCall(f.host, f, f.executed + n)) goto out;\n"
                          << "    f.ended = true; return n + 1u;\n";
                    return;
                }
                if (const auto branch = decodeBranch(a, w))
                {
                    ++m_stats.nativeWords;
                    emitBranch(i, a, *branch);
                    return;
                }
                const auto plain = decodePlain(w);
                if (!plain)
                {
                    m_out << "    " << exitHere << "\n";
                    return;
                }
                ++m_stats.nativeWords;
                const std::string done = "{ bt = 0u; ++n; f.ended = true; pc = " + hex(a + 4u) + "; goto out; }";
                m_out << "    if (n == budget) " << exitHere << "\n"
                      << "    " << replaceDone(replaceExit(plain->code, exitHere), done) << " bt = 0u; ++n;\n";
                if (plain->store)
                    m_out << "    if (f.codeWritten) { pc = " << hex(a + 4u) << "; goto out; }\n";
            }

            void emitBranch(size_t i, uint32_t a, const Branch &b)
            {
                const std::string exitHere = "{ pc = " + hex(a) + "; goto out; }";
                const uint32_t delay = a + 4u;
                m_out << "    if (budget - n < 2u) " << exitHere << "\n    {\n";
                // The branch itself, in the interpreter's order: the link register first, then the condition and the
                // target from the registers (so BLTZAL/BGEZAL on $ra and JALR with rd == rs see the link value).
                if (b.linkReg)
                    m_out << "        R[" << b.linkReg << "] = " << hex(a + 8u) << ";\n";
                if (b.conditional)
                    m_out << "        const bool tk_ = " << b.condition << ";\n";
                if (!b.staticTarget)
                    m_out << "        const uint32_t tg_ = " << reg(b.targetReg) << ";\n";
                m_out << "        IOPN_COMMIT(" << b.linkReg << "u); ++n;\n";
                if (b.conditional)
                    m_out << "        if (!tk_) { bt = 0u; " << jumpTo(delay) << " }\n";
                // Taken: the delay slot runs as a delay slot, then pc = target.
                const std::string target = b.staticTarget ? hex(b.target) : std::string("tg_");
                const std::string exitDelay = "{ pc = " + hex(delay) + "; bp = true; bt = " + target + "; goto out; }";
                const size_t d = i + 1u;
                std::optional<Plain> slot;
                if (d < m_words.size() && !isImportStub(m_words, d) && !decodeBranch(delay, m_words[d]))
                    slot = decodePlain(m_words[d]);
                if (!slot)
                    m_out << "        " << exitDelay << "\n";
                else
                {
                    const std::string done = "{ ++n; f.ended = true; bt = " + target + "; pc = " + target + "; goto out; }";
                    m_out << "        " << replaceDone(replaceExit(slot->code, exitDelay), done) << " ++n;\n"
                          << "        bt = " << target << ";\n";
                    if (slot->store)
                        m_out << "        if (f.codeWritten) { pc = " << target << "; goto out; }\n";
                    if (b.staticTarget)
                        m_out << "        " << jumpTo(b.target) << "\n";
                    else
                        m_out << "        pc = tg_; goto dispatch;\n";
                }
                m_out << "    }\n";
            }

            const ModuleText &m_module;
            const std::vector<uint32_t> &m_words;
            std::ostringstream &m_out;
            TranslationStats &m_stats;
            size_t m_first = 0, m_last = 0;
            uint32_t m_base = 0, m_end = 0;
        };

        std::vector<uint32_t> wordsOf(const ModuleText &module)
        {
            std::vector<uint32_t> words(module.bytes.size() / 4u);
            for (size_t i = 0; i < words.size(); ++i)
                words[i] = static_cast<uint32_t>(module.bytes[i * 4u]) | (static_cast<uint32_t>(module.bytes[i * 4u + 1u]) << 8u) |
                           (static_cast<uint32_t>(module.bytes[i * 4u + 2u]) << 16u) |
                           (static_cast<uint32_t>(module.bytes[i * 4u + 3u]) << 24u);
            return words;
        }

        const char *kPrelude = R"(// Generated by dw5xl_ioplift (issue 009 v4.5) from the game's own IOP module: game-derived, never commit.
#include "ps2x/iop/iop_native.h"

#include <cstdint>
#include <cstring>

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-label"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-label"
#endif

namespace
{
using F = ps2x::iop::IopNativeFrame;
constexpr uint32_t IOPN_RAM = ps2x::iop::kIopNativeRamSize;
constexpr uint32_t kLwlMask[4] = {0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0x00000000u};
constexpr uint32_t kLwlShift[4] = {24u, 16u, 8u, 0u};
constexpr uint32_t kLwrMask[4] = {0x00000000u, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u};
constexpr uint32_t kLwrShift[4] = {0u, 8u, 16u, 24u};

inline uint32_t iopnSwl(uint32_t old, uint32_t value, uint32_t shift)
{
    switch (shift)
    {
    case 0u: return (old & 0xFFFFFF00u) | (value >> 24u);
    case 1u: return (old & 0xFFFF0000u) | (value >> 16u);
    case 2u: return (old & 0xFF000000u) | (value >> 8u);
    default: return value;
    }
}

inline uint32_t iopnSwr(uint32_t old, uint32_t value, uint32_t shift)
{
    switch (shift)
    {
    case 0u: return value;
    case 1u: return (old & 0x000000FFu) | (value << 8u);
    case 2u: return (old & 0x0000FFFFu) | (value << 16u);
    default: return (old & 0x00FFFFFFu) | (value << 24u);
    }
}

// An IOP RAM store as IopMemory::write8/16/32 do it (bytes marked written by the IOP), with the check-mode undo
// record and the native-code check.
inline void iopnStore(F &f, uint32_t p, const void *value, uint32_t size)
{
    if (f.undo)
    {
        F::UndoEntry &u = f.undo[f.undoCount++];
        u.address = p;
        u.size = size;
        std::memcpy(u.oldBytes, f.ram + p, size);
        std::memcpy(u.oldOwned, f.owned + p, size);
    }
    std::memcpy(f.ram + p, value, size);
    std::memset(f.owned + p, 1, size);
    if (ps2x::iop::iopNativeCodeWord(f.codeWords, p) || ps2x::iop::iopNativeCodeWord(f.codeWords, p + size - 1u))
        f.codeWritten = true;
}
}

// The previous load's value goes to its register unless this instruction wrote that register (R3000A load delay).
#define IOPN_COMMIT(W) do { if (pl) { if (plr != (W)) R[plr] = plv; pl = false; } } while (0)
#define IOPN_PROLOGUE \
    uint32_t R[32]; std::memcpy(R, f.gpr, sizeof(R)); R[0] = 0u; \
    uint32_t hi = f.hi, lo = f.lo, pc = f.pc, plr = f.pendingLoadReg, plv = f.pendingLoadValue, bt = f.branchTarget; \
    bool pl = f.pendingLoad, bp = false; \
    uint8_t *const ram = f.ram; (void)ram; \
    const uint32_t budget = f.budget; uint32_t n = 0u;
#define IOPN_STORE \
    std::memcpy(f.gpr, R, sizeof(R)); f.hi = hi; f.lo = lo; f.pc = pc; \
    f.pendingLoad = pl; f.pendingLoadReg = plr; f.pendingLoadValue = plv; f.branchPending = bp; f.branchTarget = bt;
#define IOPN_EPILOGUE IOPN_STORE return n;

)";
    }

    std::string moduleIdentifier(const std::string &name)
    {
        std::string id;
        for (const char c : name)
            id += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
        return id.empty() ? std::string("module") : id;
    }

    std::string translateModule(const ModuleText &module, TranslationStats *statsOut)
    {
        TranslationStats stats;
        const std::vector<uint32_t> words = wordsOf(module);
        const std::string id = moduleIdentifier(module.name);
        std::ostringstream out;
        out << kPrelude;
        const uint32_t chunks = static_cast<uint32_t>((words.size() + kChunkWords - 1u) / kChunkWords);
        for (uint32_t chunk = 0; chunk < chunks; ++chunk)
        {
            ChunkEmitter emitter(module, words, chunk, out, stats);
            emitter.emit("c" + std::to_string(chunk));
        }
        out << "static constexpr uint32_t (*kChunks[])(F &) = {";
        for (uint32_t chunk = 0; chunk < chunks; ++chunk)
            out << (chunk ? ", " : "") << "c" << chunk;
        out << "};\n\n"
            << "uint32_t iopn_run_" << id << "(F &f)\n{\n"
            << "    // Chunk to chunk inside the module until the budget is used up or an instruction needs the\n"
            << "    // interpreter (a chunk that returns 0 instructions, or stops between a branch and its delay slot).\n"
            << "    uint32_t total = 0u;\n"
            << "    while (f.budget != 0u && !f.branchPending && !f.codeWritten && !f.ended)\n    {\n"
            << "        const uint32_t offset = f.pc - " << hex(module.base) << ";\n"
            << "        if (offset >= " << hex(static_cast<uint32_t>(words.size() * 4u)) << " || (offset & 3u)) break;\n"
            << "        f.executed = total;\n"
            << "        const uint32_t done = kChunks[offset / " << (kChunkWords * 4u) << "u](f);\n"
            << "        if (done == 0u) break;\n"
            << "        total += done;\n"
            << "        f.budget -= done;\n"
            << "    }\n"
            << "    return total;\n}\n";
        if (statsOut)
            *statsOut = stats;
        return out.str();
    }

    std::string registrySource(const std::vector<ModuleText> &modules)
    {
        std::ostringstream out;
        out << "// Generated by dw5xl_ioplift (issue 009 v4.5): the translated IOP modules. Game-derived, never commit.\n"
            << "#include \"ps2x/iop/iop_native.h\"\n\n#include <cstddef>\n\n";
        for (const ModuleText &module : modules)
            out << "uint32_t iopn_run_" << moduleIdentifier(module.name) << "(ps2x::iop::IopNativeFrame &);\n";
        out << "\nnamespace\n{\nconst ps2x::iop::IopNativeModule kModules[] = {\n";
        for (const ModuleText &module : modules)
        {
            char hash[32];
            std::snprintf(hash, sizeof(hash), "0x%016llXull",
                          static_cast<unsigned long long>(ps2x::iop::iopNativeHash(module.bytes.data(), module.bytes.size())));
            out << "    {\"" << module.name << "\", " << hex(module.base) << ", " << hex(static_cast<uint32_t>(module.bytes.size()))
                << ", " << hash << ", &iopn_run_" << moduleIdentifier(module.name) << "},\n";
        }
        out << "};\nconst ps2x::iop::IopNativeRegistrar kRegistrar(kModules, sizeof(kModules) / sizeof(kModules[0]));\n}\n";
        return out.str();
    }
}
