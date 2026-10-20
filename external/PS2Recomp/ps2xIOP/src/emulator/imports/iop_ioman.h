#pragma once
#include "ps2x/state_archive.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopGuestExecutor;
    class IopMemory;

    class IopIoman
    {
    public:
        // DW5XL (issue 027): save-state support (open files are reopened by path on load).
        void serializeState(ps2x::StateArchive &archive);
        explicit IopIoman(IopMemory &memory) noexcept;

        void reset();
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor);

    private:
        struct Device
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            std::string name;
        };

        IopMemory &m_memory;
        std::vector<Device> m_devices;
        // Host-backed files for ioman open/read/... on disc (cdrom0:) and
        // host: paths, resolved under the runtime's working directory.
        std::map<int32_t, std::FILE *> m_files;
        // DW5XL (issue 027): host path and writability of each open file, so a loaded state can reopen them.
        std::map<int32_t, std::pair<std::string, bool>> m_filePaths;
        int32_t m_nextFd = 3;
    };
}
