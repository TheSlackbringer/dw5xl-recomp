#include "gs_record.h"

#include "runtime/gs/gs_cpu_backend.h"
#ifdef PS2X_HAS_VULKAN_GS
#include "runtime/gs/gs_vulkan_backend.h"
#endif

#include <atomic>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <cstring>
#include <stdexcept>

namespace
{
    using GSRecord::Op;

    class RecordingBackend final : public GSRasterBackend
    {
    public:
        RecordingBackend(std::unique_ptr<GSRasterBackend> inner, std::FILE *file, uint64_t fromTick, uint64_t toTick)
            : m_inner(std::move(inner)), m_file(file), m_fromTick(fromTick), m_toTick(toTick),
              m_active(fromTick == 0u)
        {
            std::fwrite(GSRecord::kMagic, 1u, sizeof(GSRecord::kMagic), m_file);
            const uint32_t header[2] = {GSRecord::kVersion, 0u};
            std::fwrite(header, sizeof(header), 1u, m_file);
        }

        ~RecordingBackend() override
        {
            std::fclose(m_file);
        }

        void Initialize(uint8_t *vram, uint32_t vramSize) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (begin(Op::Initialize))
            {
                const uint32_t size = vram ? vramSize : 0u;
                put(size);
                put(GSRecord::fnv1a(vram, size));
                m_writer.bytes(vram, size);
                end();
            }
            m_inner->Initialize(vram, vramSize);
        }

        void Reset() override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (begin(Op::Reset))
                end();
            m_inner->Reset();
        }

        void Submit(const GSPrimitiveBatch &batch) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (begin(Op::Submit))
            {
                put(static_cast<int32_t>(std::fegetround()));
                put(batch);
                end();
            }
            m_inner->Submit(batch);
        }

        void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (begin(Op::LoadClut))
            {
                put(tex0);
                put(texclut);
                end();
            }
            m_inner->LoadClut(tex0, texclut);
        }

        void BeginTransfer(const GSTransferCommand &command) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (begin(Op::BeginTransfer))
            {
                put(command);
                end();
            }
            m_inner->BeginTransfer(command);
        }

        void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (begin(Op::UploadImage))
            {
                const uint32_t size = data ? sizeBytes : 0u;
                put(size);
                m_writer.bytes(data, size);
                end();
            }
            m_inner->UploadImage(data, sizeBytes);
        }

        void Flush() override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (begin(Op::Flush))
                end();
            m_inner->Flush();
        }

        void TextureFlush() override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (begin(Op::TextureFlush))
                end();
            m_inner->TextureFlush();
        }

        void Sync(GSSyncReason reason) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (begin(Op::Sync))
            {
                put(static_cast<uint8_t>(reason));
                end();
            }
            m_inner->Sync(reason);
        }

        void recordPresent(const GSPresentationRequest &request)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // The PS2X_GS_RECORD_FROM/TO window opens and closes on presentation ticks.
            if (!m_active && !m_closed && request.vsyncTick >= m_fromTick)
            {
                m_active = true;
                // A windowed recording starts from the current VRAM so it can be replayed.
                if (m_fromTick != 0u)
                {
                    std::vector<uint8_t> vram;
                    m_inner->SnapshotVram(vram);
                    if (begin(Op::Initialize))
                    {
                        const uint32_t size = static_cast<uint32_t>(vram.size());
                        put(size);
                        put(GSRecord::fnv1a(vram.data(), size));
                        m_writer.bytes(vram.data(), size);
                        end();
                    }
                }
            }
            if (begin(Op::Present))
            {
                put(request);
                end();
                std::fflush(m_file); // keep the file usable if the process exits abruptly
            }
            if (m_active && request.vsyncTick >= m_toTick)
            {
                m_active = false;
                m_closed = true;
                std::fflush(m_file);
            }
        }

        PresentationFrame Present(const GSPresentationRequest &request) override
        {
            recordPresent(request);
            return m_inner->Present(request);
        }

        bool PresentInto(const GSPresentationRequest &request, PresentationFrame &out) override
        {
            recordPresent(request);
            return m_inner->PresentInto(request, out);
        }

        bool SubmitFieldScanout(const GSPresentationRequest &request) override
        {
            if (!m_inner->SubmitFieldScanout(request))
                return false;
            recordPresent(request);
            return true;
        }

        bool CollectFieldScanouts(PresentationFrame &out, uint64_t waitForField) override
        {
            return m_inner->CollectFieldScanouts(out, waitForField);
        }

        bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (begin(Op::ClearFramebuffer))
            {
                put(context);
                put(rgba);
                end();
            }
            return m_inner->ClearFramebuffer(context, rgba);
        }

        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (begin(Op::ConsumeLocalToHost))
            {
                put(maxBytes);
                end();
            }
            return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
        }

        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (begin(Op::ReadVram))
            {
                for (uint32_t v : {psm, base, bw, x, y})
                    put(v);
                end();
            }
            return m_inner->ReadVram(psm, base, bw, x, y);
        }

        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (begin(Op::WriteVram))
            {
                for (uint32_t v : {psm, base, bw, x, y, value})
                    put(v);
                end();
            }
            m_inner->WriteVram(psm, base, bw, x, y, value);
        }

        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (begin(Op::SnapshotVram))
                end();
            m_inner->SnapshotVram(out);
        }

        GSTransferSnapshot GetTransferSnapshot() const override
        {
            return m_inner->GetTransferSnapshot();
        }

        void SetResolutionScale(uint32_t scale) override
        {
            m_inner->SetResolutionScale(scale);
        }

        void SetAASamples(uint32_t samples) override { m_inner->SetAASamples(samples); }
        uint32_t GetAASamples() const override { return m_inner->GetAASamples(); }

        uint32_t GetResolutionScale() const override
        {
            return m_inner->GetResolutionScale();
        }

    private:
        // Records are assembled in m_buffer, then written as op | u32 size | payload.
        bool begin(Op value) const
        {
            if (!m_active)
                return false;
            m_op = static_cast<uint8_t>(value);
            m_buffer.clear();
            return true;
        }

        void end() const
        {
            const uint32_t size = static_cast<uint32_t>(m_buffer.size());
            std::fwrite(&m_op, 1u, 1u, m_file);
            std::fwrite(&size, sizeof(size), 1u, m_file);
            if (size != 0u)
                std::fwrite(m_buffer.data(), 1u, size, m_file);
        }

        template <typename T>
        void put(const T &value) const
        {
            m_writer.put(value);
        }

        std::unique_ptr<GSRasterBackend> m_inner;
        std::FILE *m_file = nullptr;
        uint64_t m_fromTick = 0;
        uint64_t m_toTick = UINT64_MAX;
        bool m_active = true;
        bool m_closed = false;
        mutable uint8_t m_op = 0;
        mutable std::vector<uint8_t> m_buffer;
        mutable GSRecord::Writer m_writer{m_buffer};
        mutable std::mutex m_mutex;
    };

    // DW5XL-TEST: accepts every call and draws nothing (offline replay tools).
    class NullBackend final : public GSRasterBackend
    {
    public:
        void Initialize(uint8_t *vram, uint32_t vramSize) override
        {
            m_vram = vram;
            m_vramSize = vramSize;
        }
        void Reset() override {}
        void Submit(const GSPrimitiveBatch &) override {}
        void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override {}
        void BeginTransfer(const GSTransferCommand &) override {}
        void UploadImage(const uint8_t *, uint32_t) override {}
        void Flush() override {}
        void TextureFlush() override {}
        void Sync(GSSyncReason) override {}
        PresentationFrame Present(const GSPresentationRequest &) override { return {}; }
        bool ClearFramebuffer(const GSContext &, uint32_t) override { return true; }
        uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override { return 0u; }
        uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override { return 0u; }
        void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            if (m_vram)
                out.assign(m_vram, m_vram + m_vramSize);
            else
                out.clear();
        }
        GSTransferSnapshot GetTransferSnapshot() const override { return {}; }

    private:
        const uint8_t *m_vram = nullptr;
        uint32_t m_vramSize = 0u;
    };

    uint64_t envTick(const char *name, uint64_t fallback)
    {
        const char *value = std::getenv(name);
        return (value && *value) ? std::strtoull(value, nullptr, 0) : fallback;
    }

    std::FILE *openRecordFile(const char *path)
    {
        std::FILE *file = std::fopen(path, "wb");
        if (file)
            std::setvbuf(file, nullptr, _IOFBF, 1u << 20u);
        return file;
    }
}

std::unique_ptr<GSRasterBackend> makeGsNullBackend()
{
    return std::make_unique<NullBackend>();
}

std::unique_ptr<GSRasterBackend> makeGsRecordingBackend(std::unique_ptr<GSRasterBackend> inner, const char *path,
                                                        uint64_t fromTick, uint64_t toTick)
{
    if (!inner || !path || !*path)
        return nullptr;
    std::FILE *file = openRecordFile(path);
    if (!file)
        return nullptr;
    return std::make_unique<RecordingBackend>(std::move(inner), file, fromTick, toTick);
}

std::unique_ptr<GSRasterBackend> makeGsRasterBackend()
{
    std::unique_ptr<GSRasterBackend> backend;
    const char *choice = std::getenv("PS2X_GS_BACKEND");
    const bool cpu = choice && std::strcmp(choice, "cpu") == 0;
    const bool vulkan = choice && std::strcmp(choice, "vulkan") == 0;
    if (choice && *choice && !cpu && !vulkan && std::strcmp(choice, "auto") != 0)
        throw std::invalid_argument("PS2X_GS_BACKEND must be auto, cpu or vulkan");
#ifdef PS2X_HAS_VULKAN_GS
    if (!cpu)
    {
        try { backend = std::make_unique<GSVulkanBackend>(); }
        catch (const std::exception &error)
        {
            if (vulkan) throw;
            std::fprintf(stderr, "[gs] Vulkan unavailable (%s); using CPU renderer\n", error.what());
        }
    }
#else
    if (vulkan) throw std::runtime_error("This build does not include the Vulkan GS renderer");
#endif
    if (!backend) backend = std::make_unique<GSCpuBackend>();
    static std::atomic<bool> s_recorded{false};
    const char *path = std::getenv("PS2X_GS_RECORD");
    if (path && *path && !s_recorded.exchange(true))
    {
        if (std::FILE *file = openRecordFile(path))
        {
            const uint64_t fromTick = envTick("PS2X_GS_RECORD_FROM", 0u);
            const uint64_t toTick = envTick("PS2X_GS_RECORD_TO", UINT64_MAX);
            std::fprintf(stderr, "[gs] recording backend calls to %s (format v%u, ticks %llu..%llu)\n", path,
                         GSRecord::kVersion, static_cast<unsigned long long>(fromTick),
                         static_cast<unsigned long long>(toTick));
            return std::make_unique<RecordingBackend>(std::move(backend), file, fromTick, toTick);
        }
        std::fprintf(stderr, "[gs] cannot open PS2X_GS_RECORD file %s\n", path);
    }
    return backend;
}
