#pragma once
// Issue 009 v4.5: the disc's IOP modules (iop_inputs.h) as C++, for dw5xl_ioplift and the setup pipeline.

#include <filesystem>
#include <functional>
#include <map>
#include <string>

namespace dw5xl::ioplift
{
    // Each module of iop_inputs.h from `modulesDir` (the disc's MODULES folder): checked against its size and hash,
    // relocated at its load address with the IOP's own module loader, its .text translated. `files`: file name -> C++
    // (iop_<name>.cpp per module and iop_registry.cpp). `progress` gets one line per module.
    [[nodiscard]] bool generateIopModules(const std::filesystem::path &modulesDir, std::map<std::string, std::string> &files,
                                          std::string &error, const std::function<void(const std::string &)> &progress = {});
}
