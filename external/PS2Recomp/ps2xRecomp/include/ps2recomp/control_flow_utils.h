#ifndef PS2RECOMP_CONTROL_FLOW_UTILS_H
#define PS2RECOMP_CONTROL_FLOW_UTILS_H

#include "ps2recomp/instructions.h"

#include <cstdint>

namespace ps2recomp
{
    inline uint32_t buildAbsoluteJumpTarget(uint32_t address, uint32_t target) noexcept
    {
        return ((address + 4u) & 0xF0000000u) | (target << 2);
    }

    inline bool isGuestLikelyBranch(const Instruction &inst) noexcept
    {
        return (inst.opcode == OPCODE_BEQL || inst.opcode == OPCODE_BNEL ||
                inst.opcode == OPCODE_BLEZL || inst.opcode == OPCODE_BGTZL ||
                (inst.opcode == OPCODE_REGIMM &&
                 (inst.rt == REGIMM_BLTZL || inst.rt == REGIMM_BGEZL ||
                  inst.rt == REGIMM_BLTZALL || inst.rt == REGIMM_BGEZALL)) ||
                (inst.opcode == OPCODE_COP0 && inst.rs == COP0_BC &&
                 (inst.rt == COP0_BC_BCFL || inst.rt == COP0_BC_BCTL)) ||
                (inst.opcode == OPCODE_COP1 && inst.rs == COP1_BC &&
                 (inst.rt == COP1_BC_BCFL || inst.rt == COP1_BC_BCTL)) ||
                (inst.opcode == OPCODE_COP2 && inst.rs == COP2_BC &&
                 (inst.rt == COP2_BC_BCFL || inst.rt == COP2_BC_BCTL)));
    }

    inline bool isGuestNop(const Instruction &inst) noexcept
    {
        const bool sllZero =
            inst.opcode == OPCODE_SPECIAL &&
            inst.function == SPECIAL_SLL &&
            inst.rd == 0u &&
            inst.rt == 0u &&
            inst.sa == 0u;

        // Some tests and decoded streams represent a no-op as addiu $zero, $zero, 0.
        const bool addiuZero =
            inst.opcode == OPCODE_ADDIU &&
            inst.rs == 0u &&
            inst.rt == 0u &&
            static_cast<int16_t>(inst.simmediate) == 0;

        return sllZero || addiuZero;
    }
}

#endif // PS2RECOMP_CONTROL_FLOW_UTILS_H
