#pragma once
#include "ps2_runtime.h"
// Compiled output of CodeGenerator for synthetic instructions, never game code.
namespace synthetic_vu0 {
void s1_add(R5900Context *ctx);
void s1_addq(R5900Context *ctx);
void s1_addi(R5900Context *ctx);
void madd_field(R5900Context *ctx);
void madd(R5900Context *ctx);
void maddq(R5900Context *ctx);
void maddi(R5900Context *ctx);
void msub_field(R5900Context *ctx);
void msub(R5900Context *ctx);
void msubq(R5900Context *ctx);
void msubi(R5900Context *ctx);
void opmsub(R5900Context *ctx);
void madda(R5900Context *ctx);
void opmula(R5900Context *ctx);
}
