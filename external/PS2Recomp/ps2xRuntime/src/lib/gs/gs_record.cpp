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
    class RecordingBackend final : public GSRasterBackend
    {
    public:
        RecordingBackend(std::unique_ptr<GSRasterBackend> inner, std::FILE *file)
            : m_inner(std::move(inner)), m_file(file)
        {
        }

        ~RecordingBackend() override
        {
            std::fclose(m_file);
        }

        void Initialize(uint8_t *vram, uint32_t vramSize) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::Initialize);
            put(vramSize);
            if (vram && vramSize != 0u)
                std::fwrite(vram, 1u, vramSize, m_file);
            m_inner->Initialize(vram, vramSize);
        }

        void Reset() override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::Reset);
            m_inner->Reset();
        }

        void Submit(const GSPrimitiveBatch &batch) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::Submit);
            put(static_cast<int32_t>(std::fegetround()));
            put(batch);
            m_inner->Submit(batch);
        }

        void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::LoadClut);
            put(tex0);
            put(texclut);
            m_inner->LoadClut(tex0, texclut);
        }

        void BeginTransfer(const GSTransferCommand &command) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::BeginTransfer);
            put(command);
            m_inner->BeginTransfer(command);
        }

        void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::UploadImage);
            put(sizeBytes);
            if (data && sizeBytes != 0u)
                std::fwrite(data, 1u, sizeBytes, m_file);
            m_inner->UploadImage(data, sizeBytes);
        }

        void Flush() override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::Flush);
            m_inner->Flush();
        }

        void TextureFlush() override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::TextureFlush);
            m_inner->TextureFlush();
        }

        void Sync(GSSyncReason reason) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::Sync);
            put(static_cast<uint8_t>(reason));
            m_inner->Sync(reason);
        }

        PresentationFrame Present(const GSPresentationRequest &request) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::Present);
            put(request);
            return m_inner->Present(request);
        }

        bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::ClearFramebuffer);
            put(context);
            put(rgba);
            return m_inner->ClearFramebuffer(context, rgba);
        }

        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::ConsumeLocalToHost);
            put(maxBytes);
            return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
        }

        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::ReadVram);
            for (uint32_t v : {psm, base, bw, x, y})
                put(v);
            return m_inner->ReadVram(psm, base, bw, x, y);
        }

        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::WriteVram);
            for (uint32_t v : {psm, base, bw, x, y, value})
                put(v);
            m_inner->WriteVram(psm, base, bw, x, y, value);
        }

        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            op(GSRecord::Op::SnapshotVram);
            m_inner->SnapshotVram(out);
        }

        GSTransferSnapshot GetTransferSnapshot() const override
        {
            return m_inner->GetTransferSnapshot();
        }

    private:
        void op(GSRecord::Op value) const { put(static_cast<uint8_t>(value)); }

        template <typename T>
        void put(const T &value) const
        {
            std::fwrite(&value, sizeof(T), 1u, m_file);
        }

        std::unique_ptr<GSRasterBackend> m_inner;
        std::FILE *m_file = nullptr;
        mutable std::mutex m_mutex;
    };
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
        if (std::FILE *file = std::fopen(path, "wb"))
        {
            std::setvbuf(file, nullptr, _IOFBF, 1u << 20u);
            std::fprintf(stderr, "[gs] recording backend calls to %s\n", path);
            return std::make_unique<RecordingBackend>(std::move(backend), file);
        }
        std::fprintf(stderr, "[gs] cannot open PS2X_GS_RECORD file %s\n", path);
    }
    return backend;
}
