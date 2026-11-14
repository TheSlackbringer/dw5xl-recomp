// SPDX-License-Identifier: GPL-3.0-only
// DW5XL issue 066: the file of the game's own compiled shader programs (runtime/gs/gs_shader_store.h).
#include "runtime/gs/gs_shader_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>

namespace ps2x::gs::shader_store
{
    namespace
    {
        // The head: eight letters, the format's number, the shaders' mark, the content's size and its check sum.
        // Every number least significant byte first, whatever the machine.
        const char kLetters[8] = {'D', 'W', '5', 'X', 'L', 'S', 'H', 'S'};
        const uint32_t kFormat = 1u;
        const size_t kHead = 8u + 4u + 4u + 8u + 8u + 8u;

        void put(uint8_t *at, uint64_t value, size_t bytes)
        {
            for (size_t i = 0; i < bytes; ++i)
                at[i] = static_cast<uint8_t>(value >> (8u * i));
        }

        uint64_t get(const uint8_t *at, size_t bytes)
        {
            uint64_t value = 0u;
            for (size_t i = 0; i < bytes; ++i)
                value |= static_cast<uint64_t>(at[i]) << (8u * i);
            return value;
        }
    }

    namespace
    {
        std::mutex g_mutex;
        std::string g_path;
        void (*g_writer)(bool) = nullptr;
        void (*g_named)() = nullptr;
    }

    void setPath(const std::string &path)
    {
        void (*named)() = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_path = path;
            named = g_named;
        }
        if (named)
            named();
    }

    void setNamed(void (*named)())
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_named = named;
    }

    std::string path()
    {
        if (const char *named = std::getenv("PS2X_SHADER_STORE"); named && *named)
            return std::strcmp(named, "off") == 0 || std::strcmp(named, "0") == 0 ? std::string() : std::string(named);
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_path;
    }

    void setWriter(void (*writer)(bool always))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_writer = writer;
    }

    void writeNow(bool always)
    {
        void (*writer)(bool) = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            writer = g_writer;
        }
        if (writer)
            writer(always);
    }

    uint64_t markOf(const void *bytes, size_t size, uint64_t from)
    {
        const uint8_t *at = static_cast<const uint8_t *>(bytes);
        uint64_t mark = from;
        for (size_t i = 0; i < size; ++i)
        {
            mark ^= at[i];
            mark *= 1099511628211ull;
        }
        return mark;
    }

    const char *said(Read result)
    {
        switch (result)
        {
        case Read::Taken: return "read";
        case Read::NoFile: return "there is none yet";
        case Read::NotOurs: return "the file is not a store of this format; it is left out";
        case Read::OtherShaders: return "it was written by a program with other shaders; it is left out";
        case Read::Damaged: return "the file is damaged; it is left out";
        }
        return "";
    }

    Read read(const std::string &path, uint64_t mark, std::vector<uint8_t> &content)
    {
        content.clear();
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return Read::NoFile;
        uint8_t head[kHead] = {};
        in.read(reinterpret_cast<char *>(head), static_cast<std::streamsize>(kHead));
        if (static_cast<size_t>(in.gcount()) != kHead || std::memcmp(head, kLetters, sizeof(kLetters)) != 0 || get(head + 8, 4) != kFormat)
            return Read::NotOurs;
        if (get(head + 16, 8) != mark)
            return Read::OtherShaders;
        const uint64_t size = get(head + 24, 8), sum = get(head + 32, 8);
        // The file's own length says whether the size can be true, before any memory is asked for.
        in.seekg(0, std::ios::end);
        const std::streamoff length = in.tellg();
        if (length < 0 || static_cast<uint64_t>(length) != kHead + size)
            return Read::Damaged;
        in.seekg(static_cast<std::streamoff>(kHead), std::ios::beg);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (static_cast<uint64_t>(in.gcount()) != size || markOf(bytes.data(), bytes.size()) != sum)
            return Read::Damaged;
        content = std::move(bytes);
        return Read::Taken;
    }

    bool write(const std::string &path, uint64_t mark, const uint8_t *content, size_t size)
    {
        const std::string part = path + ".part";
        uint8_t head[kHead] = {};
        std::memcpy(head, kLetters, sizeof(kLetters));
        put(head + 8, kFormat, 4);
        put(head + 16, mark, 8);
        put(head + 24, size, 8);
        put(head + 32, markOf(content, size), 8);
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            out.write(reinterpret_cast<const char *>(head), static_cast<std::streamsize>(kHead));
            out.write(reinterpret_cast<const char *>(content), static_cast<std::streamsize>(size));
            out.flush();
            if (!out)
            {
                out.close();
                std::remove(part.c_str());
                return false;
            }
        }
        // The part file takes the store's place in one step (std::filesystem::rename replaces a file that is there,
        // on every system): a reader finds the old store or the new one, never half of either.
        std::error_code failed;
        std::filesystem::rename(part, path, failed);
        if (failed)
        {
            std::remove(part.c_str());
            return false;
        }
        return true;
    }
} // namespace ps2x::gs::shader_store
