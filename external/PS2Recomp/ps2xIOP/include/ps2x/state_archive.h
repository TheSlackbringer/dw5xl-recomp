#pragma once
// DW5XL (issue 027): binary archive for save states. One class reads and writes, so every component serializes
// itself with the same code in both directions (save: `archive.value(x)` copies x out; load: copies into x). A
// state saved, loaded and saved again must give the same bytes (the issue's round-trip proof).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ps2x
{
    struct StateError : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    class StateArchive
    {
    public:
        static StateArchive writer() { return StateArchive(true, {}); }
        static StateArchive reader(std::vector<uint8_t> data) { return StateArchive(false, std::move(data)); }

        bool saving() const { return m_saving; }
        bool loading() const { return !m_saving; }
        const std::vector<uint8_t> &data() const { return m_data; }
        bool atEnd() const { return m_pos == m_data.size(); }

        // A 4-character section marker: a load that is out of step with the save fails here, not later.
        void tag(const char (&name)[5])
        {
            char got[4];
            std::memcpy(got, name, 4);
            raw(got, 4);
            if (loading() && std::memcmp(got, name, 4) != 0)
                throw StateError(std::string("state section '") + name + "' expected, found '" + std::string(got, 4) + "'");
        }

        void raw(void *bytes, size_t size)
        {
            if (m_saving)
            {
                const auto *p = static_cast<const uint8_t *>(bytes);
                m_data.insert(m_data.end(), p, p + size);
                return;
            }
            if (size > m_data.size() - m_pos)
                throw StateError("state file ends early");
            std::memcpy(bytes, m_data.data() + m_pos, size);
            m_pos += size;
        }

        // Copies the object's bytes. Padding between and after fields holds whatever memory had there before; it is
        // saved as zero, so the same state always gives the same bytes (the round-trip proof).
        template <typename T>
        void value(T &v)
        {
            static_assert(std::is_trivially_copyable_v<T>, "StateArchive::value needs a trivially copyable type");
            size(sizeof(T));
            items(&v, 1);
        }

        // The size record (issue 027 v3): every structure is saved with its size, so a state made by another build
        // loads as long as what is saved is unchanged, and a structure that grew or shrank is refused.
        void size(size_t bytes)
        {
            uint32_t recorded = static_cast<uint32_t>(bytes);
            raw(&recorded, sizeof(recorded));
            if (loading() && recorded != bytes)
                throw StateError("a saved structure has " + std::to_string(recorded) + " bytes in the state and " +
                                 std::to_string(bytes) + " in this build (the state format changed)");
        }

        // A count written before a container; loading rejects absurd counts instead of allocating them.
        uint64_t count(uint64_t n)
        {
            value(n);
            if (loading() && n > (m_data.size() - m_pos) + (uint64_t(1) << 20))
                throw StateError("state file has an impossible element count");
            return n;
        }

        void string(std::string &s)
        {
            const uint64_t n = count(s.size());
            if (loading())
                s.resize(size_t(n));
            raw(s.data(), size_t(n));
        }

        template <typename T>
        void vector(std::vector<T> &v)
        {
            static_assert(std::is_trivially_copyable_v<T>, "StateArchive::vector needs trivially copyable elements");
            const uint64_t n = count(v.size());
            size(sizeof(T));
            if (loading())
                v.resize(size_t(n));
            items(v.data(), size_t(n));
        }

        void boolVector(std::vector<bool> &v)
        {
            const uint64_t n = count(v.size());
            if (loading())
                v.assign(size_t(n), false);
            for (size_t i = 0; i < n; ++i)
            {
                uint8_t b = v[i] ? 1u : 0u;
                value(b);
                v[i] = b != 0;
            }
        }

        template <typename T, typename Each>
        void sequence(std::vector<T> &v, Each each)
        {
            const uint64_t n = count(v.size());
            if (loading())
                v.assign(size_t(n), T{});
            for (T &item : v)
                each(*this, item);
        }

        template <typename T>
        void deque(std::deque<T> &d)
        {
            static_assert(std::is_trivially_copyable_v<T>, "StateArchive::deque needs trivially copyable elements");
            std::vector<T> v(d.begin(), d.end());
            vector(v);
            if (loading())
                d.assign(v.begin(), v.end());
        }

        // A map key or set element: a string, or a trivially copyable value.
        template <typename Key>
        void key(Key &k)
        {
            if constexpr (std::is_same_v<Key, std::string>)
                string(k);
            else
            {
                static_assert(std::is_trivially_copyable_v<Key>, "StateArchive: keys must be strings or trivially copyable");
                value(k);
            }
        }

        // Maps are written in key order so the same contents always give the same bytes.
        template <typename Map, typename Each>
        void map(Map &m, Each each)
        {
            using Key = typename Map::key_type;
            using Value = typename Map::mapped_type;
            if (saving())
            {
                std::map<Key, Value *> ordered;
                for (auto &[k, v] : m)
                    ordered.emplace(k, &v);
                count(ordered.size());
                for (auto &[k, v] : ordered)
                {
                    Key copy = k;
                    key(copy);
                    each(*this, *v);
                }
                return;
            }
            const uint64_t n = count(0);
            m.clear();
            for (uint64_t i = 0; i < n; ++i)
            {
                Key k{};
                key(k);
                Value item{};
                each(*this, item);
                m.emplace(std::move(k), std::move(item));
            }
        }

        template <typename Map>
        void podMap(Map &m)
        {
            using Value = typename Map::mapped_type;
            static_assert(std::is_trivially_copyable_v<Value>, "StateArchive::podMap needs trivially copyable values");
            map(m, [](StateArchive &a, Value &v) { a.value(v); });
        }

        template <typename Set>
        void podSet(Set &s)
        {
            using Key = typename Set::key_type;
            std::vector<Key> keys(s.begin(), s.end());
            std::sort(keys.begin(), keys.end());
            const uint64_t n = count(keys.size());
            if (loading())
                keys.assign(size_t(n), Key{});
            for (Key &k : keys)
                key(k);
            if (loading())
                s = Set(keys.begin(), keys.end());
        }

    private:
        // n objects of T: types without padding as they are; others through a copy whose padding is zeroed.
        template <typename T>
        void items(T *first, size_t n)
        {
            if (loading() || std::has_unique_object_representations_v<T>)
            {
                raw(first, n * sizeof(T));
                return;
            }
            std::vector<uint8_t> copy(n * sizeof(T));
            std::memcpy(copy.data(), first, copy.size());
#if defined(__has_builtin)
#if __has_builtin(__builtin_clear_padding)
            for (size_t i = 0; i < n; ++i)
                __builtin_clear_padding(reinterpret_cast<T *>(copy.data() + i * sizeof(T)));
#endif
#endif
            raw(copy.data(), copy.size());
        }

        StateArchive(bool saving, std::vector<uint8_t> data) : m_saving(saving), m_data(std::move(data)) {}
        bool m_saving;
        std::vector<uint8_t> m_data;
        size_t m_pos = 0;
    };
} // namespace ps2x
