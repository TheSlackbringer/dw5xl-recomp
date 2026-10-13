#pragma once
#include "dw5xl_overlay_validation.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

namespace dw5xl::discs {
enum class Validation { valid, missing, unsupported, unreadable };
inline Validation validateFile(const std::filesystem::path& path,
                               const std::array<uint8_t,32>& expected) {
    std::error_code ec;
    if(!std::filesystem::exists(path,ec)) return ec?Validation::unreadable:Validation::missing;
    if(!std::filesystem::is_regular_file(path,ec) || ec) return Validation::unreadable;
    std::ifstream file(path,std::ios::binary);
    if(!file) return Validation::unreadable;
    auto sha=std::unique_ptr<AVSHA,decltype(&av_free)>(av_sha_alloc(),&av_free);
    if(!sha || av_sha_init(sha.get(),256)<0) return Validation::unreadable;
    std::array<uint8_t,65536> bytes{};
    while(file) {
        file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
        const auto count=file.gcount();
        if(count>0) av_sha_update(sha.get(),bytes.data(),static_cast<size_t>(count));
    }
    if(!file.eof()) return Validation::unreadable;
    std::array<uint8_t,32> digest{};
    av_sha_final(sha.get(),digest.data());
    return digest==expected?Validation::valid:Validation::unsupported;
}
inline constexpr std::array<uint8_t,32> originalHash={
  0x73,0xb6,0xa2,0x02,0x94,0xe5,0xc3,0x03,0xb9,0x62,0x4a,0xab,0x48,0x08,0x5b,0xe5,
  0x2d,0x37,0xae,0xd0,0x55,0x7a,0x05,0x3e,0x36,0xb3,0xa9,0x35,0x2c,0x89,0xcc,0x0f};
inline constexpr std::array<uint8_t,32> xlHash={
  0xf8,0xf9,0xf9,0x98,0x76,0x07,0x71,0x3c,0x80,0x9f,0xb3,0xae,0x9d,0x04,0xf2,0x13,
  0xff,0x88,0x32,0x6c,0x4a,0xa0,0xc3,0x38,0xab,0x99,0xc4,0xac,0x33,0x65,0x5d,0x1f};
inline Validation validateImportedDisc(const PS2Runtime::IoPaths& paths,uint32_t which) {
    // Runtime ELF directory is the extracted XL root; cwd is deliberately irrelevant.
    const auto xl=validateFile(paths.elfPath,xlHash);
    if(which==1u) return xl;
    if(which!=0u) return Validation::unsupported;
    if(xl!=Validation::valid) return xl;
    return validateFile(paths.elfDirectory.parent_path()/"dw5"/"SLES_533.39",originalHash);
}
// App-owned metadata keeps one function substitution confined to its module.
// Construct before registration and keep this object alive for the runtime.
struct ModuleWithOverride {
    std::vector<RecompiledModuleEntry> entries;
    RecompiledModule descriptor{};
    bool initialize(const RecompiledModule& original,uint32_t pc,
                    PS2Runtime::RecompiledFunction replacement) {
        if(!original.entries || !replacement) return false;
        entries.assign(original.entries,original.entries+original.entryCount);
        auto entry=std::lower_bound(entries.begin(),entries.end(),pc,
            [](const auto& row,uint32_t address){return row.address<address;});
        if(entry==entries.end() || entry->address!=pc) { entries.clear();return false; }
        entry->function=replacement;
        descriptor=original;descriptor.entries=entries.data();
        return true;
    }
};
}
