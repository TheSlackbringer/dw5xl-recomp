// Generate only synthetic unit fixtures. Compilation makes emitted C++ part of the test.
#include "ps2recomp/code_generator.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/types.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace ps2recomp;
int main(int argc, char **argv)
{
    try {
        if (argc != 2) throw std::runtime_error("expected output path");
        std::ofstream out(argv[1], std::ios::trunc);
        if (!out) throw std::runtime_error("cannot open generated fixture");
        out << "#include \"vu0_test_fixtures.h\"\n#include \"ps2_vu0_macro.h\"\nnamespace synthetic_vu0 {\n";
        CodeGenerator gen({}, {});
        auto emit = [&](const char *name, const std::string &code) {
            out << "void " << name << "(R5900Context *ctx) {\n" << code << "\n}\n";
        };
        Instruction inst{};
        inst.opcode = OPCODE_COP2;
        inst.rs = COP2_CO | 0xB;
        inst.rt = 7; inst.rd = 11; inst.sa = 3;
        inst.function = VU0_S1_VADD; inst.vectorInfo.vectorField = 0xF;
        emit("s1_add", gen.translateInstruction(inst));
        inst.rs = COP2_CO | 9;
        inst.rt = 5; inst.rd = 13; inst.sa = 4;
        inst.vectorInfo.vectorField = 9;
        inst.function = VU0_S1_VADDq;
        emit("s1_addq", gen.translateInstruction(inst));
        inst.function = VU0_S1_VADDi;
        emit("s1_addi", gen.translateInstruction(inst));
        inst.rt = 7; inst.rd = 11; inst.sa = 3;
        inst.function = 0; inst.vectorInfo.vectorField = 0xE;
        emit("madd_field", gen.translateVU_VMADD_Field(inst));
        emit("madd", gen.translateVU_VMADD(inst));
        emit("maddq", gen.translateVU_VMADDq(inst));
        emit("maddi", gen.translateVU_VMADDi(inst));
        emit("msub_field", gen.translateVU_VMSUB_Field(inst));
        emit("msub", gen.translateVU_VMSUB(inst));
        emit("msubq", gen.translateVU_VMSUBq(inst));
        emit("msubi", gen.translateVU_VMSUBi(inst));
        emit("opmsub", gen.translateVU_VOPMSUB(inst));
        emit("madda", gen.translateVU_VMADDA(inst));
        emit("opmula", gen.translateVU_VOPMULA(inst));
        out << "}\n";
        out.close();
        if (!out) throw std::runtime_error("cannot write generated fixture");
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
