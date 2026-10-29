#pragma once
// 042: physical batches retain indexed, byte-exact legacy files and ordered compilation groups.
#include "stage_timing.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace ps2recomp {
inline uint64_t outputChecksum(const std::string &bytes, uint64_t h = 14695981039346656037ull)
{
    for (unsigned char c : bytes) h = (h ^ c) * 1099511628211ull;
    return h; // FNV-1a integrity checksum; provenance/cache and acceptance use SHA-256 separately.
}
inline std::string checksumHex(uint64_t h)
{
    std::ostringstream out; out << std::hex << std::setfill('0') << std::setw(16) << h; return out.str();
}
inline std::string legacyDiskBytes(const std::string &text)
{
#ifdef _WIN32
    std::string bytes;
    bytes.reserve(text.size() + text.size() / 16);
    for (char c : text) { if (c == '\n') bytes += '\r'; bytes += c; }
    return bytes;
#else
    return text;
#endif
}

class OutputBatchWriter
{
public:
    OutputBatchWriter(std::filesystem::path root, size_t size) : m_root(std::move(root)), m_size(size)
    {
        if (!size || size > 4096) throw std::runtime_error("Invalid output batch size");
    }
    void append(const std::string &name, const std::string &text)
    {
        if (std::filesystem::path(name).filename() != name || name.empty() || !m_names.insert(name).second)
            throw std::runtime_error("Invalid/duplicate virtual output filename");
        if (!m_last.empty() && name <= m_last) throw std::runtime_error("Virtual outputs must be lexically ordered");
        m_last = name;
        if (!m_file.is_open() || m_count == m_size)
        {
            closeBatch();
            std::ostringstream file; file << "ps2_batch_" << std::setw(6) << std::setfill('0') << m_batches.size() << ".cpp";
            m_current = file.str(); m_count = 0; m_offset = 0; m_hash = 14695981039346656037ull;
            m_file.open(m_root / m_current, std::ios::binary | std::ios::trunc);
            if (!m_file) throw std::runtime_error("Cannot open generated batch");
        }
        // Logical attribution survives scratch-to-installed moves; never bake private/temporary paths into output.
        write(legacyDiskBytes("\n#line 1 " + timing::quote(name) + "\n"));
        const std::string bytes = legacyDiskBytes(text);
        m_entries.push_back({name, m_current, m_offset, bytes.size(), checksumHex(outputChecksum(bytes))});
        write(bytes);
        if (bytes.empty() || bytes.back() != '\n') write(legacyDiskBytes("\n"));
        ++m_count;
    }
    void finish()
    {
        closeBatch();
        std::ostringstream batches;
        batches << '[';
        for (size_t i = 0; i < m_batches.size(); ++i)
        {
            if (i) batches << ',';
            const auto &b = m_batches[i];
            batches << "{\"file\":" << timing::quote(b.name) << ",\"count\":" << b.count
                    << ",\"bytes\":" << b.bytes << ",\"fnv1a64\":" << timing::quote(b.hash) << '}';
        }
        batches << ']';
        const std::string header = "{\"schema\":1,\"batch_size\":" + std::to_string(m_size) +
            ",\"legacy_cpp_count\":" + std::to_string(m_entries.size()) + ",\"batches\":" + batches.str();
        std::ostringstream index;
        index << header << ",\"files\":[";
        for (size_t i = 0; i < m_entries.size(); ++i)
        {
            if (i) index << ',';
            const auto &e = m_entries[i];
            index << "{\"name\":" << timing::quote(e.name) << ",\"file\":" << timing::quote(e.file)
                  << ",\"offset\":" << e.offset << ",\"size\":" << e.bytes
                  << ",\"fnv1a64\":" << timing::quote(e.hash) << '}';
        }
        index << "]}\n";
        writeMetadata("ps2_generated_index.json", index.str());
        writeMetadata("ps2_generated_sources.json", header + "}\n");
    }
    size_t batchCount() const { return m_batches.size(); }
private:
    struct Entry { std::string name, file; uint64_t offset, bytes; std::string hash; };
    struct Batch { std::string name; uint64_t count, bytes; std::string hash; };
    void write(const std::string &bytes)
    {
        m_file.write(bytes.data(), std::streamsize(bytes.size()));
        if (!m_file) throw std::runtime_error("Cannot write generated batch");
        m_hash = outputChecksum(bytes, m_hash); m_offset += bytes.size();
    }
    void closeBatch()
    {
        if (!m_file.is_open()) return;
        m_file.flush(); m_file.close();
        if (!m_file) throw std::runtime_error("Cannot finish generated batch");
        m_batches.push_back({m_current, m_count, m_offset, checksumHex(m_hash)});
    }
    void writeMetadata(const char *name, const std::string &text)
    {
        std::ofstream out(m_root / name, std::ios::binary | std::ios::trunc);
        out.write(text.data(), std::streamsize(text.size())); out.flush(); out.close();
        if (!out) throw std::runtime_error("Cannot finish generated output index");
    }
    std::filesystem::path m_root;
    size_t m_size;
    std::ofstream m_file;
    std::string m_current, m_last;
    uint64_t m_count = 0, m_offset = 0, m_hash = 0;
    std::set<std::string> m_names;
    std::vector<Entry> m_entries;
    std::vector<Batch> m_batches;
};
} // namespace ps2recomp
