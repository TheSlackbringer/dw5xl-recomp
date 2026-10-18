#include "ps2recomp/code_generator.h"
#include "ps2recomp/codegen_helpers.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/types.h"
#include <fmt/format.h>
#include <sstream>
#include <cmath>

namespace ps2recomp
{
    namespace
    {
        // VU0 macro FMAC instructions call one runtime helper each (ps2_vu0_macro.h); fd < 0 targets ACC.
        std::string fmacCall(const char *op, uint8_t dest, int fd, const std::string &a, const std::string &b)
        {
            if (fd < 0)
                return fmt::format("ps2_vu0::toAcc<ps2_vu0::{}, {}>(ctx, {}, {});", op, dest, a, b);
            return fmt::format("ps2_vu0::toVf<ps2_vu0::{}, {}>(ctx, {}, {}, {});", op, dest, fd, a, b);
        }

        std::string vf(uint8_t reg) { return fmt::format("ctx->vu0_vf[{}]", reg); }

        // The bc forms broadcast one field of VF[ft].
        std::string bcField(uint8_t reg, uint8_t field)
        {
            return fmt::format("_mm_shuffle_ps(ctx->vu0_vf[{0}], ctx->vu0_vf[{0}], _MM_SHUFFLE({1},{1},{1},{1}))", reg, field);
        }

        // OPMULA/OPMSUB: fs.yzx * ft.zxy.
        std::string opFs(uint8_t reg) { return fmt::format("_mm_shuffle_ps(ctx->vu0_vf[{0}], ctx->vu0_vf[{0}], _MM_SHUFFLE(3,0,2,1))", reg); }
        std::string opFt(uint8_t reg) { return fmt::format("_mm_shuffle_ps(ctx->vu0_vf[{0}], ctx->vu0_vf[{0}], _MM_SHUFFLE(3,1,0,2))", reg); }

        // One field of VF[reg] as a float (DIV/SQRT/RSQRT operands).
        std::string fieldExpr(uint8_t reg, uint8_t field)
        {
            return fmt::format("_mm_cvtss_f32(_mm_shuffle_ps(ctx->vu0_vf[{0}], ctx->vu0_vf[{0}], _MM_SHUFFLE(0,0,0,{1})))", reg, field);
        }

        const std::string kVu0Q = "_mm_set1_ps(ctx->vu0_q)";
        const std::string kVu0I = "_mm_set1_ps(ctx->vu0_i)";
    }
    std::string CodeGenerator::translateVU_VADD_Field(const Instruction &inst)
    {
        return fmacCall("Add", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), bcField(inst.rt, inst.function & 0x3));
    }

    std::string CodeGenerator::translateVU_VSUB_Field(const Instruction &inst)
    {
        return fmacCall("Sub", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), bcField(inst.rt, inst.function & 0x3));
    }

    std::string CodeGenerator::translateVU_VMUL_Field(const Instruction &inst)
    {
        return fmacCall("Mul", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), bcField(inst.rt, inst.function & 0x3));
    }

    std::string CodeGenerator::translateVU_VADD(const Instruction &inst)
    {
        return fmacCall("Add", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), vf(inst.rt));
    }

    std::string CodeGenerator::translateVU_VSUB(const Instruction &inst)
    {
        return fmacCall("Sub", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), vf(inst.rt));
    }

    std::string CodeGenerator::translateVU_VMUL(const Instruction &inst)
    {
        return fmacCall("Mul", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), vf(inst.rt));
    }

    std::string CodeGenerator::translateVU_VDIV(const Instruction &inst)
    {
        return fmt::format("ps2_vu0::div(ctx, {}, {});", fieldExpr(inst.rd, inst.vectorInfo.fsf), fieldExpr(inst.rt, inst.vectorInfo.ftf));
    }

    std::string CodeGenerator::translateVU_VSQRT(const Instruction &inst)
    {
        return fmt::format("ps2_vu0::sqrt(ctx, {});", fieldExpr(inst.rt, inst.vectorInfo.ftf));
    }

    std::string CodeGenerator::translateVU_VRSQRT(const Instruction &inst)
    {
        return fmt::format("ps2_vu0::rsqrt(ctx, {}, {});", fieldExpr(inst.rd, inst.vectorInfo.fsf), fieldExpr(inst.rt, inst.vectorInfo.ftf));
    }

    std::string CodeGenerator::translateVU_VMTIR(const Instruction &inst)
    {
        uint8_t fsf = inst.vectorInfo.fsf;
        return fmt::format("{{ uint32_t bits; float src = _mm_cvtss_f32(_mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(0,0,0,{}))); std::memcpy(&bits, &src, sizeof(bits)); ctx->vi[{}] = (uint16_t)(bits & 0xFFFF); }}", inst.rd, inst.rd, fsf, inst.rt);
    }

    std::string CodeGenerator::translateVU_VMFIR(const Instruction &inst)
    {
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ uint32_t tmp = (uint32_t)(int32_t)(int16_t)ctx->vi[{}]; float val; std::memcpy(&val, &tmp, sizeof(val)); "
                           "__m128 res = _mm_set1_ps(val); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           inst.rd,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           inst.rt, inst.rt);
    }

    std::string CodeGenerator::translateVU_VILWR(const Instruction &inst)
    {
        return fmt::format("{{ uint32_t addr = (uint32_t)(ctx->vi[{}] << 2) & 0x3FFC; ctx->vi[{}] = static_cast<uint16_t>(READ32(addr)); }}", inst.rd, inst.rt); // VILWR.<f> vit, (vis)
    }

    std::string CodeGenerator::translateVU_VISWR(const Instruction &inst)
    {
        return fmt::format("{{ uint32_t addr = (uint32_t)(ctx->vi[{}] << 2) & 0x3FFC; WRITE32(addr, (uint32_t)ctx->vi[{}]); }}", inst.rd, inst.rt); // VISWR.<f> vit, (vis)
    }

    std::string CodeGenerator::translateVU_VIADD(const Instruction &inst)
    {
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] + ctx->vi[{}];", inst.sa, inst.rd, inst.rt); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VISUB(const Instruction &inst)
    {
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] - ctx->vi[{}];", inst.sa, inst.rd, inst.rt); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VIADDI(const Instruction &inst)
    {
        int32_t imm5 = (inst.sa & 0x10) ? static_cast<int32_t>(inst.sa | ~0x1F) : static_cast<int32_t>(inst.sa);
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] + {};", inst.rt, inst.rd, imm5); // vit, vis, imm5
    }

    std::string CodeGenerator::translateVU_VIAND(const Instruction &inst)
    {
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] & ctx->vi[{}];", inst.sa, inst.rd, inst.rt); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VIOR(const Instruction &inst)
    {
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] | ctx->vi[{}];", inst.sa, inst.rd, inst.rt); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VCALLMS(const Instruction &inst)
    {
        // VCALLMS calls a VU0 microprogram at the specified immediate address.
        // VU0 micro memory is 4KB = 512 instructions (8 bytes each). Index is 0-511.
        uint16_t instr_index = static_cast<uint16_t>((inst.raw >> 6) & 0x1FF); // imm15[8:0]
        uint32_t target_byte_addr = static_cast<uint32_t>(instr_index) << 3;   // Convert instruction index to byte address

        return fmt::format(
            "{{ "
            "    ctx->vu0_tpc = 0x{:X}; " // Set target program counter
            "    runtime->executeVU0Microprogram(rdram, ctx, 0x{:X}); "
            "}}",
            target_byte_addr, target_byte_addr);
    }

    std::string CodeGenerator::translateVU_VCALLMSR(const Instruction &inst)
    {
        // VCALLMSR calls a VU0 microprogram at address stored in integer register
        uint8_t vis_reg_idx = inst.rd; // Source integer register (vis)

        return fmt::format(
            "{{ "
            "    uint16_t instr_index = ctx->vi[{}] & 0x1FF; "             // Get instruction index from VI[IS], mask to 9 bits
            "    uint32_t target_byte_addr = (uint32_t)instr_index << 3; " // Convert to byte address
            "    ctx->vu0_pc = target_byte_addr; "
            "    runtime->vu0StartMicroProgram(rdram, ctx, target_byte_addr); "
            "}}",
            vis_reg_idx);
    }

    std::string CodeGenerator::translateVU_VRNEXT(const Instruction &inst)
    {
        return fmt::format(
            "{{\n"
            "    uint32_t r_vals[4];\n"
            "    _mm_storeu_si128((__m128i*)r_vals, _mm_castps_si128(ctx->vu0_r));\n"
            "\n"
            "    // Simple LFSR-based random number generation (PS2-like behavior)\n"
            "    uint32_t feedback = r_vals[0] ^ (r_vals[0] << 13) ^ (r_vals[1] >> 19) ^ (r_vals[2] << 7);\n"
            "    r_vals[0] = r_vals[1];\n"
            "    r_vals[1] = r_vals[2];\n"
            "    r_vals[2] = r_vals[3];\n"
            "    r_vals[3] = feedback;\n"
            "\n"
            "    ctx->vu0_r = _mm_castsi128_ps(_mm_loadu_si128((__m128i*)r_vals));\n"
            "}}");
    }

    std::string CodeGenerator::translateVU_VMADD_Field(const Instruction &inst)
    {
        return fmacCall("MAdd", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), bcField(inst.rt, inst.function & 0x3));
    }

    std::string CodeGenerator::translateVU_VMSUB_Field(const Instruction &inst)
    {
        return fmacCall("MSub", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), bcField(inst.rt, inst.function & 0x3));
    }

    std::string CodeGenerator::translateVU_VMINI_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;

        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 res = _mm_min_ps(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft, vft, shuffle_pattern,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMAX_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;

        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 res = _mm_max_ps(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft, vft, shuffle_pattern,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMADD(const Instruction &inst)
    {
        return fmacCall("MAdd", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), vf(inst.rt));
    }

    std::string CodeGenerator::translateVU_VMADDq(const Instruction &inst)
    {
        return fmacCall("MAdd", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), kVu0Q);
    }

    std::string CodeGenerator::translateVU_VMADDi(const Instruction &inst)
    {
        return fmacCall("MAdd", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), kVu0I);
    }

    std::string CodeGenerator::translateVU_VMAX(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = _mm_max_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMAXi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = _mm_max_ps(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMINIi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = _mm_min_ps(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMULi(const Instruction &inst)
    {
        return fmacCall("Mul", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), kVu0I);
    }

    std::string CodeGenerator::translateVU_VMULq(const Instruction &inst)
    {
        return fmacCall("Mul", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), kVu0Q);
    }

    std::string CodeGenerator::translateVU_VOPMSUB(const Instruction &inst)
    {
        return fmacCall("MSub", inst.vectorInfo.vectorField, inst.sa, opFs(inst.rd), opFt(inst.rt));
    }

    std::string CodeGenerator::translateVU_VADDq(const Instruction &inst)
    {
        return fmacCall("Add", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), kVu0Q);
    }

    std::string CodeGenerator::translateVU_VADDi(const Instruction &inst)
    {
        return fmacCall("Add", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), kVu0I);
    }

    std::string CodeGenerator::translateVU_VMSUB(const Instruction &inst)
    {
        return fmacCall("MSub", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), vf(inst.rt));
    }

    std::string CodeGenerator::translateVU_VMINI(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = _mm_min_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VSUBi(const Instruction &inst)
    {
        return fmacCall("Sub", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), kVu0I);
    }

    std::string CodeGenerator::translateVU_VSUBq(const Instruction &inst)
    {
        return fmacCall("Sub", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), kVu0Q);
    }

    std::string CodeGenerator::translateVU_VMSUBq(const Instruction &inst)
    {
        return fmacCall("MSub", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), kVu0Q);
    }

    std::string CodeGenerator::translateVU_VMSUBi(const Instruction &inst)
    {
        return fmacCall("MSub", inst.vectorInfo.vectorField, inst.sa, vf(inst.rd), kVu0I);
    }

    std::string CodeGenerator::translateVU_VADDA_Field(const Instruction &inst)
    {
        return fmacCall("Add", inst.vectorInfo.vectorField, -1, vf(inst.rd), bcField(inst.rt, inst.function & 0x3));
    }

    std::string CodeGenerator::translateVU_VSUBA_Field(const Instruction &inst)
    {
        return fmacCall("Sub", inst.vectorInfo.vectorField, -1, vf(inst.rd), bcField(inst.rt, inst.function & 0x3));
    }

    std::string CodeGenerator::translateVU_VMADDA_Field(const Instruction &inst)
    {
        return fmacCall("MAdd", inst.vectorInfo.vectorField, -1, vf(inst.rd), bcField(inst.rt, inst.function & 0x3));
    }

    std::string CodeGenerator::translateVU_VMSUBA_Field(const Instruction &inst)
    {
        return fmacCall("MSub", inst.vectorInfo.vectorField, -1, vf(inst.rd), bcField(inst.rt, inst.function & 0x3));
    }

    std::string CodeGenerator::translateVU_VMULA_Field(const Instruction &inst)
    {
        return fmacCall("Mul", inst.vectorInfo.vectorField, -1, vf(inst.rd), bcField(inst.rt, inst.function & 0x3));
    }

    std::string CodeGenerator::translateVU_VADDA(const Instruction &inst)
    {
        return fmacCall("Add", inst.vectorInfo.vectorField, -1, vf(inst.rd), vf(inst.rt));
    }

    std::string CodeGenerator::translateVU_VADDAq(const Instruction &inst)
    {
        return fmacCall("Add", inst.vectorInfo.vectorField, -1, vf(inst.rd), kVu0Q);
    }

    std::string CodeGenerator::translateVU_VADDAi(const Instruction &inst)
    {
        return fmacCall("Add", inst.vectorInfo.vectorField, -1, vf(inst.rd), kVu0I);
    }

    std::string CodeGenerator::translateVU_VSUBA(const Instruction &inst)
    {
        return fmacCall("Sub", inst.vectorInfo.vectorField, -1, vf(inst.rd), vf(inst.rt));
    }

    std::string CodeGenerator::translateVU_VSUBAq(const Instruction &inst)
    {
        return fmacCall("Sub", inst.vectorInfo.vectorField, -1, vf(inst.rd), kVu0Q);
    }

    std::string CodeGenerator::translateVU_VSUBAi(const Instruction &inst)
    {
        return fmacCall("Sub", inst.vectorInfo.vectorField, -1, vf(inst.rd), kVu0I);
    }

    std::string CodeGenerator::translateVU_VMADDA(const Instruction &inst)
    {
        return fmacCall("MAdd", inst.vectorInfo.vectorField, -1, vf(inst.rd), vf(inst.rt));
    }

    std::string CodeGenerator::translateVU_VMADDAq(const Instruction &inst)
    {
        return fmacCall("MAdd", inst.vectorInfo.vectorField, -1, vf(inst.rd), kVu0Q);
    }

    std::string CodeGenerator::translateVU_VMADDAi(const Instruction &inst)
    {
        return fmacCall("MAdd", inst.vectorInfo.vectorField, -1, vf(inst.rd), kVu0I);
    }

    std::string CodeGenerator::translateVU_VMSUBA(const Instruction &inst)
    {
        return fmacCall("MSub", inst.vectorInfo.vectorField, -1, vf(inst.rd), vf(inst.rt));
    }

    std::string CodeGenerator::translateVU_VMSUBAq(const Instruction &inst)
    {
        return fmacCall("MSub", inst.vectorInfo.vectorField, -1, vf(inst.rd), kVu0Q);
    }

    std::string CodeGenerator::translateVU_VMSUBAi(const Instruction &inst)
    {
        return fmacCall("MSub", inst.vectorInfo.vectorField, -1, vf(inst.rd), kVu0I);
    }

    std::string CodeGenerator::translateVU_VMULA(const Instruction &inst)
    {
        return fmacCall("Mul", inst.vectorInfo.vectorField, -1, vf(inst.rd), vf(inst.rt));
    }

    std::string CodeGenerator::translateVU_VMULAq(const Instruction &inst)
    {
        return fmacCall("Mul", inst.vectorInfo.vectorField, -1, vf(inst.rd), kVu0Q);
    }

    std::string CodeGenerator::translateVU_VMULAi(const Instruction &inst)
    {
        return fmacCall("Mul", inst.vectorInfo.vectorField, -1, vf(inst.rd), kVu0I);
    }

    std::string CodeGenerator::translateVU_VOPMULA(const Instruction &inst)
    {
        return fmacCall("Mul", inst.vectorInfo.vectorField, -1, opFs(inst.rd), opFt(inst.rt));
    }

    std::string CodeGenerator::translateVU_VITOF(const Instruction &inst, int shift)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        float scale = (shift == 0) ? 1.0f : (1.0f / static_cast<float>(1 << shift));

        return fmt::format("{{ __m128i src = _mm_castps_si128(ctx->vu0_vf[{}]); "
                           "__m128 res = _mm_cvtepi32_ps(src); "
                           "res = _mm_mul_ps(res, _mm_set1_ps({})); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, codegen::formatFloatLiteral(scale),
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           inst.rt, inst.rt);
    }

    std::string CodeGenerator::translateVU_VFTOI(const Instruction &inst, int shift)
    {
        return fmt::format("ps2_vu0::ftoi<{}, {}>(ctx, {}, ctx->vu0_vf[{}]);", shift, inst.vectorInfo.vectorField, inst.rt, inst.rd);
    }

    std::string CodeGenerator::translateVU_VLQI(const Instruction &inst)
    {
        uint8_t vis = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ uint32_t addr = ((uint32_t)(ctx->vi[{}] & 0x3FF)) << 4; "
                           "__m128 res = _mm_castsi128_ps(READ128(addr)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); "
                           "ctx->vi[{}] = (ctx->vi[{}] + 1) & 0x3FF; }}",
                           vis,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           inst.rt, inst.rt,
                           vis, vis);
    }

    std::string CodeGenerator::translateVU_VSQI(const Instruction &inst)
    {
        uint8_t vis = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ uint32_t addr = ((uint32_t)(ctx->vi[{}] & 0x3FF)) << 4; "
                           "__m128i old_val = READ128(addr); "
                           "__m128 res = _mm_blendv_ps(_mm_castsi128_ps(old_val), ctx->vu0_vf[{}], _mm_castsi128_ps(_mm_set_epi32({}, {}, {}, {}))); "
                           "WRITE128(addr, _mm_castps_si128(res)); "
                           "ctx->vi[{}] = (ctx->vi[{}] + 1) & 0x3FF; }}",
                           vis,
                           inst.rt,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vis, vis);
    }

    std::string CodeGenerator::translateVU_VLQD(const Instruction &inst)
    {
        uint8_t vis = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ ctx->vi[{}] = (ctx->vi[{}] - 1) & 0x3FF; "
                           "uint32_t addr = ((uint32_t)(ctx->vi[{}] & 0x3FF)) << 4; "
                           "__m128 res = _mm_castsi128_ps(READ128(addr)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vis, vis,
                           vis,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           inst.rt, inst.rt);
    }

    std::string CodeGenerator::translateVU_VSQD(const Instruction &inst)
    {
        uint8_t vis = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ ctx->vi[{}] = (ctx->vi[{}] - 1) & 0x3FF; "
                           "uint32_t addr = ((uint32_t)(ctx->vi[{}] & 0x3FF)) << 4; "
                           "__m128i old_val = READ128(addr); "
                           "__m128 res = _mm_blendv_ps(_mm_castsi128_ps(old_val), ctx->vu0_vf[{}], _mm_castsi128_ps(_mm_set_epi32({}, {}, {}, {}))); "
                           "WRITE128(addr, _mm_castps_si128(res)); }}",
                           vis, vis,
                           vis,
                           inst.rt,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0);
    }

    std::string CodeGenerator::translateVU_VRGET(const Instruction &inst)
    {
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t ft_reg = inst.rt;
        return fmt::format("{{ __m128 res = ctx->vu0_r; __m128i mask = _mm_set_epi32({}, {}, {}, {}); ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}", (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, ft_reg, ft_reg);
    }

    std::string CodeGenerator::translateVU_VRINIT(const Instruction &inst)
    {
        uint8_t fs_reg = inst.rd;
        uint8_t fsf = inst.vectorInfo.fsf;

        return fmt::format(
            "{{\n"
            "    float src = _mm_cvtss_f32(_mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(0,0,0,{})));\n"
            "    uint32_t seed; std::memcpy(&seed, &src, sizeof(seed));\n"
            "\n"
            "    // PS2 uses a specific LFSR initialization pattern\n"
            "    if (seed == 0) seed = 1;\n"
            "\n"
            "    uint32_t r0 = seed;\n"
            "    uint32_t r1 = seed * 0x41C64E6D + 0x3039;\n"
            "    uint32_t r2 = r1 * 0x41C64E6D + 0x3039;\n"
            "    uint32_t r3 = r2 * 0x41C64E6D + 0x3039;\n"
            "\n"
            "    ctx->vu0_r = _mm_castsi128_ps(_mm_set_epi32(r3, r2, r1, r0));\n"
            "}}",
            fs_reg, fs_reg, fsf);
    }

    std::string CodeGenerator::translateVU_VRXOR(const Instruction &inst)
    {
        uint8_t fs_reg = inst.rd;
        uint8_t fsf = inst.vectorInfo.fsf;

        return fmt::format(
            "{{\n"
            "    float src = _mm_cvtss_f32(_mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(0,0,0,{})));\n"
            "    uint32_t src_bits; std::memcpy(&src_bits, &src, sizeof(src_bits));\n"
            "    __m128i r_current = _mm_castps_si128(ctx->vu0_r);\n"
            "    __m128i fs_data = _mm_set1_epi32((int)src_bits);\n"
            "\n"
            "    // XOR the current random value with the data from the VU vector register\n"
            "    __m128i xored = _mm_xor_si128(r_current, fs_data);\n"
            "\n"
            "    // Apply a simple mixing function similar to PS2's LFSR\n"
            "    __m128i mixed = _mm_xor_si128(xored, _mm_slli_epi32(xored, 7));\n"
            "    mixed = _mm_xor_si128(mixed, _mm_srli_epi32(mixed, 9));\n"
            "\n"
            "    ctx->vu0_r = _mm_castsi128_ps(mixed);\n"
            "}}",
            fs_reg, fs_reg, fsf);
    }

}
