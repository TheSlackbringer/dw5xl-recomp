#pragma once
#include <filesystem>
#include <cstdint>
class PS2Runtime;

// Called only by installed boot. Developer ELF runs never start a check or install these hooks.
void dw5xlStartOriginalCheck(const std::filesystem::path &data);
// A completed original overlay check, scoped to its runtime and EE thread.
void dw5xlOriginalCheckCompleted(PS2Runtime *runtime, int thread, uint32_t which, bool success);
void dw5xlOriginalCheckReset();
