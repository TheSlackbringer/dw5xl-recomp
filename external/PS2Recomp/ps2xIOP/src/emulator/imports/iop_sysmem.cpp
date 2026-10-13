#include <cstdio>
#include "iop_sysmem.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <string>

namespace ps2x::iop::detail
{
    IopSysmem::IopSysmem(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    bool IopSysmem::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        switch (ordinal)
        {
        case 4: // AllocSysMemory
        {
            const uint32_t address = a0 == 2u
                                         ? m_memory.allocate(a1, 16u, a2)
                                         : m_memory.allocate(a1, 16u);
            setV0(address);
            if (a1 >= 0x10000u) // DW5XL-DEBUG (temporary)
                std::fprintf(stderr, "[dw5xl:iopmem] AllocSysMemory(type=%u, size=0x%x) -> 0x%x ra=0x%x\n",
                             a0, a1, address, cpu.gpr[31]);
            return true;
        }
        case 5: // FreeSysMemory
        {
            const bool freed = m_memory.freeAllocation(a0);
            std::fprintf(stderr, "[dw5xl:iopmem] FreeSysMemory(0x%x) -> %s ra=0x%x\n", a0, freed ? "ok" : "FAILED",
                         cpu.gpr[31]); // DW5XL-DEBUG (temporary)
            setV0(freed ? 0u : 0xFFFFFFFFu);
            return true;
        }
        case 6: // QueryMemSize
            setV0(IopMemory::RamSize);
            return true;
        case 7: // QueryMaxFreeMemSize
        case 8: // QueryTotalFreeMemSize
            setV0(m_memory.maxFreeMemory());
            return true;
        case 9: // QueryBlockTopAddress
            if (const auto block = m_memory.allocationContaining(a0))
                setV0(block->address);
            else
                setV0(0u);
            return true;
        case 10: // QueryBlockSize
            if (const auto block = m_memory.allocationContaining(a0))
                setV0(block->size);
            else
                setV0(0xFFFFFFFFu);
            return true;
        case 14: // Kprintf
        {
            const std::string format = m_memory.readString(a0, 512u);
            m_host.log(LogLevel::Info, std::string("[IOP Kprintf] ") + format);
            setV0(static_cast<uint32_t>(format.size()));
            return true;
        }
        case 15:
            setV0(0u);
            return true;
        default:
            return false;
        }
    }
}
