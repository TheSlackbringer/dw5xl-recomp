#pragma once
#include <filesystem>

// Called only by installed boot. Developer ELF runs never start a check or install the hook.
void dw5xlStartOriginalCheck(const std::filesystem::path &data);
