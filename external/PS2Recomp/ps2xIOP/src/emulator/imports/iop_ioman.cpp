#include "iop_ioman.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "../services/iop_rpc.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace ps2x::iop::detail
{
    namespace
    {
        bool traceIoOperation()
        {
            static const bool enabled = []
            {
                const char *value = std::getenv("PS2X_IO_TRACE");
                return value && *value && std::strcmp(value, "0") != 0;
            }();
            if (!enabled) return false;
            static const uint64_t limit = []
            {
                const char *value = std::getenv("PS2X_IO_TRACE_LIMIT");
                if (!value || !*value) return uint64_t{4096u};
                char *end = nullptr;
                const uint64_t parsed = std::strtoull(value, &end, 10);
                return end != value && *end == '\0' && parsed != 0u
                           ? std::min<uint64_t>(parsed, 1000000u) : uint64_t{4096u};
            }();
            static uint64_t count = 0u;
            return count++ < limit;
        }

        // "cdrom0:\\DATA\\KF4.DAT;1" -> "DATA/KF4.DAT" (relative to the working
        // directory, which holds the unpacked disc). Components are matched
        // case-insensitively because ISO9660 names are upper case.
        std::string resolveHostPath(const std::string &guestPath)
        {
            std::string path = guestPath;
            const size_t colon = path.find(':');
            if (colon != std::string::npos)
                path = path.substr(colon + 1u);
            const size_t semi = path.rfind(';');
            if (semi != std::string::npos)
                path = path.substr(0u, semi);
            std::replace(path.begin(), path.end(), '\\', '/');
            while (!path.empty() && path.front() == '/')
                path.erase(path.begin());

            std::filesystem::path resolved = ".";
            size_t start = 0u;
            while (start <= path.size())
            {
                const size_t slash = path.find('/', start);
                const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                start = (slash == std::string::npos) ? path.size() + 1u : slash + 1u;
                if (part.empty())
                    continue;
                std::filesystem::path next = resolved / part;
                std::error_code ec;
                if (!std::filesystem::exists(next, ec) && std::filesystem::is_directory(resolved, ec))
                {
                    for (const auto &entry : std::filesystem::directory_iterator(resolved, ec))
                    {
                        const std::string name = entry.path().filename().string();
                        if (name.size() == part.size() &&
                            std::equal(name.begin(), name.end(), part.begin(),
                                       [](char a, char b)
                                       { return std::toupper(static_cast<unsigned char>(a)) ==
                                                std::toupper(static_cast<unsigned char>(b)); }))
                        {
                            next = entry.path();
                            break;
                        }
                    }
                }
                resolved = next;
            }
            return resolved.string();
        }
    }

    IopIoman::IopIoman(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    void IopIoman::reset()
    {
        m_devices.clear();
        for (auto &file : m_files)
            std::fclose(file.second);
        m_files.clear();
        m_nextFd = 3;
    }

    bool IopIoman::dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor)
    {
        constexpr size_t kMaxDevices = 16u;
        const uint32_t a0 = cpu.gpr[4];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const auto fileFor = [&](uint32_t fd) -> std::FILE *
        {
            const auto it = m_files.find(static_cast<int32_t>(fd));
            return it == m_files.end() ? nullptr : it->second;
        };
        constexpr uint32_t kEBADF = static_cast<uint32_t>(-9);
        constexpr uint32_t kENOENT = static_cast<uint32_t>(-2);

        switch (ordinal)
        {
        case 4: // open(name, flags, mode)
        {
            const std::string guestPath = m_memory.readString(a0, 256u);
            const std::string hostPath = resolveHostPath(guestPath);
            // IOP O_* flags: RDONLY 1, WRONLY 2, RDWR 3, APPEND 0x100, CREAT 0x200, TRUNC 0x400
            const uint32_t access = a1 & 3u;
            const char *mode = "rb";
            if (access == 2u || access == 3u)
                mode = (a1 & 0x200u) ? ((a1 & 0x400u) || access == 2u ? "w+b" : "r+b") : "r+b";
            std::FILE *file = std::fopen(hostPath.c_str(), mode);
            if (!file && (a1 & 0x200u))
                file = std::fopen(hostPath.c_str(), "w+b");
            if (!file)
            {
                std::fprintf(stderr, "[iop:ioman] open(\"%s\") -> %s: not found\n", guestPath.c_str(), hostPath.c_str());
                setV0(kENOENT);
                return true;
            }
            const int32_t fd = m_nextFd++;
            m_files[fd] = file;
            if (traceIoOperation())
                std::fprintf(stderr, "[io:ioman] pc=%08x open fd=%d flags=%08x guest=\"%s\" path=\"%s\"\n",
                             cpu.pc, fd, a1, guestPath.c_str(), hostPath.c_str());
            setV0(static_cast<uint32_t>(fd));
            return true;
        }
        case 5: // close(fd)
        {
            const auto it = m_files.find(static_cast<int32_t>(a0));
            if (it == m_files.end())
            {
                setV0(kEBADF);
                return true;
            }
            std::fclose(it->second);
            m_files.erase(it);
            setV0(0u);
            return true;
        }
        case 6: // read(fd, buf, size)
        case 7: // write(fd, buf, size)
        {
            std::FILE *file = fileFor(a0);
            if (!file)
            {
                setV0(kEBADF);
                return true;
            }
            std::vector<uint8_t> buffer(a2);
            size_t done = 0u;
            const bool trace = traceIoOperation();
            const long before = trace ? std::ftell(file) : 0;
            if (ordinal == 6u)
            {
                done = a2 ? std::fread(buffer.data(), 1u, a2, file) : 0u;
                if (done && !m_memory.writeRam(a1, buffer.data(), done))
                    done = 0u;
            }
            else
            {
                if (a2 && m_memory.readRam(a1, buffer.data(), a2))
                    done = std::fwrite(buffer.data(), 1u, a2, file);
            }
            if (trace)
                std::fprintf(stderr, "[io:ioman] pc=%08x %s fd=%d offset=%ld dst=%08x requested=%u actual=%zu error=%d\n",
                             cpu.pc, ordinal == 6u ? "read" : "write", static_cast<int32_t>(a0),
                             before, a1, a2, done, std::ferror(file));
            setV0(static_cast<uint32_t>(done));
            return true;
        }
        case 8: // lseek(fd, offset, whence)
        {
            std::FILE *file = fileFor(a0);
            if (!file)
            {
                setV0(kEBADF);
                return true;
            }
            const int whence = a2 == 1u ? SEEK_CUR : (a2 == 2u ? SEEK_END : SEEK_SET);
            const int result = std::fseek(file, static_cast<long>(static_cast<int32_t>(a1)), whence);
            if (traceIoOperation())
                std::fprintf(stderr, "[io:ioman] pc=%08x seek fd=%d offset=%d whence=%u result=%d position=%ld\n",
                             cpu.pc, static_cast<int32_t>(a0), static_cast<int32_t>(a1), a2, result, std::ftell(file));
            if (result != 0)
            {
                setV0(static_cast<uint32_t>(-22));
                return true;
            }
            setV0(static_cast<uint32_t>(std::ftell(file)));
            return true;
        }
        case 9: // ioctl(fd, cmd, arg)
        {
            if (!fileFor(a0))
            {
                setV0(kEBADF);
                return true;
            }
            // Disc drivers opened with O_NOWAIT use command 1 to query the
            // pending operation. Host-backed open/read/lseek complete before
            // returning, so report that completion through the output word.
            if (a1 == 1u)
            {
                const bool ramSegment = a2 < IopMemory::RamSize ||
                                        (a2 >= 0x80000000u && a2 < 0x80200000u) ||
                                        (a2 >= 0xA0000000u && a2 < 0xA0200000u);
                const uint32_t ready = 0u;
                if (a2 == 0u || !ramSegment ||
                    !m_memory.ownsRamRange(a2, sizeof(ready)) ||
                    !m_memory.writeRam(a2, &ready, sizeof(ready)))
                {
                    setV0(static_cast<uint32_t>(-14)); // EFAULT
                    return true;
                }
            }
            setV0(0u);
            return true;
        }
        case 20: // AddDrv
        {
            if (a0 == 0u || m_devices.size() >= kMaxDevices)
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t nameAddress = m_memory.read32(a0);
            const uint32_t operations = m_memory.read32(a0 + 16u);
            const std::string name = m_memory.readString(nameAddress, 64u);
            if (nameAddress == 0u || operations == 0u || name.empty())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            m_devices.push_back({a0, cpu.gpr[28], name});
            const uint32_t init = m_memory.read32(operations);
            if (init != 0u)
            {
                const int32_t result = static_cast<int32_t>(
                    executor.executeGuestFunction(init, a0, 0u, 0u, 0u, cpu.gpr[28]));
                if (result < 0)
                {
                    m_devices.pop_back();
                    setV0(0xFFFFFFFFu);
                    return true;
                }
            }

            setV0(0u);
            return true;
        }
        case 21: // DelDrv
        {
            const std::string name = m_memory.readString(a0, 64u);
            const auto device = std::find_if(
                m_devices.begin(), m_devices.end(),
                [&](const Device &candidate)
                { return candidate.name == name; });
            if (device == m_devices.end())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t operations = m_memory.read32(device->address + 16u);
            const uint32_t deinit = operations != 0u ? m_memory.read32(operations + 4u) : 0u;
            if (deinit != 0u)
                (void)executor.executeGuestFunction(deinit, device->address, 0u, 0u, 0u, device->gp);
            m_devices.erase(device);
            setV0(0u);
            return true;
        }
        default:
            return false;
        }
    }
}
