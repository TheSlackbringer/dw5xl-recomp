#pragma once

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
        int32_t m_nextFd = 3;
    };
}
