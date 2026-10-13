#include <cstring>
#include <cstdio>
#include "iop_stdio.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <string>

namespace ps2x::iop::detail
{
    IopStdio::IopStdio(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    bool IopStdio::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        const auto logString = [&](std::string_view prefix, uint32_t address, uint32_t resultBias = 0u)
        {
            const std::string text = m_memory.readString(address, 2048u);
            m_host.log(LogLevel::Info, std::string(prefix) + text);
            setV0(static_cast<uint32_t>(text.size()) + resultBias);
        };

        switch (ordinal)
        {
        case 4: // printf(fmt, ...): a1-a3 then the stack (o32: 5th arg at sp+16)
        {
            const std::string format = m_memory.readString(a0, 2048u);
            uint32_t argIndex = 0u;
            const auto nextArg = [&]() -> uint32_t
            {
                const uint32_t i = argIndex++;
                if (i < 3u)
                    return cpu.gpr[5u + i];
                return m_memory.read32(cpu.gpr[29] + 16u + (i - 3u) * 4u);
            };
            std::string out;
            for (size_t i = 0; i < format.size(); ++i)
            {
                if (format[i] != '%')
                {
                    out.push_back(format[i]);
                    continue;
                }
                std::string spec = "%";
                size_t j = i + 1u;
                while (j < format.size() && std::strchr("-+ #0123456789.lhz", format[j]))
                {
                    if (format[j] != 'l' && format[j] != 'h' && format[j] != 'z')
                        spec.push_back(format[j]);
                    ++j;
                }
                if (j >= format.size())
                    break;
                const char conv = format[j];
                i = j;
                char buf[512];
                switch (conv)
                {
                case 'd': case 'i':
                    std::snprintf(buf, sizeof(buf), (spec + "d").c_str(), static_cast<int32_t>(nextArg()));
                    out += buf;
                    break;
                case 'u': case 'x': case 'X': case 'o':
                    std::snprintf(buf, sizeof(buf), (spec + conv).c_str(), nextArg());
                    out += buf;
                    break;
                case 'p':
                    std::snprintf(buf, sizeof(buf), "0x%08x", nextArg());
                    out += buf;
                    break;
                case 'c':
                    out.push_back(static_cast<char>(nextArg()));
                    break;
                case 's':
                {
                    const std::string str = m_memory.readString(nextArg(), 400u);
                    std::snprintf(buf, sizeof(buf), (spec + "s").c_str(), str.c_str());
                    out += buf;
                    break;
                }
                case '%':
                    out.push_back('%');
                    break;
                default:
                    out += spec;
                    out.push_back(conv);
                    break;
                }
            }
            m_host.log(LogLevel::Info, "[IOP printf] " + out);
            setV0(static_cast<uint32_t>(out.size()));
            return true;
        }
        case 5: // getchar
        case 10:
            setV0(0xFFFFFFFFu);
            return true;
        case 6: // putchar
            m_host.log(LogLevel::Info, std::string("[IOP putchar] ") + static_cast<char>(a0 & 0xFFu));
            setV0(a0 & 0xFFu);
            return true;
        case 7: // puts
            logString("[IOP puts] ", a0, 1u);
            return true;
        case 8: // gets
        case 13:
            setV0(0u);
            return true;
        case 9: // fdprintf
            logString("[IOP fdprintf] ", a1);
            return true;
        case 11:
            setV0(a0 & 0xFFu);
            return true;
        case 12: // fdputs
            logString("[IOP fdputs] ", a0);
            return true;
        case 14: // vfdprintf
            logString("[IOP vfdprintf] ", a1);
            return true;
        default:
            return false;
        }
    }
}
