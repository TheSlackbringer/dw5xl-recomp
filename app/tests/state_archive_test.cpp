// Issue 027: the save-state archive. Every container type must survive save -> load -> save with identical bytes,
// and a damaged or out-of-step file must be refused (an exception), never half-loaded.
#include "runtime/state_archive.h"

#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using ps2x::StateArchive;
using ps2x::StateError;

namespace {
int failures = 0;
void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    failures += ok ? 0 : 1;
}

struct Pod { uint32_t a; uint32_t spare; uint64_t b; uint8_t c[8]; }; // no padding: see StateArchive::value
struct Thing
{
    int id = 0;
    std::string name;
    std::vector<uint32_t> words;
    std::deque<int> waiters;
};

struct Sample
{
    uint32_t u32 = 0;
    Pod pod{};
    std::string text;
    std::vector<uint16_t> shorts;
    std::vector<bool> bits;
    std::deque<int> queue;
    std::unordered_map<int, Thing> things;
    std::unordered_map<uint32_t, uint32_t> registers;
    std::unordered_set<uint32_t> addresses;
    std::vector<Thing> list;
    uint8_t block[64] = {};

    void serialize(StateArchive &a)
    {
        auto thing = [](StateArchive &x, Thing &t)
        {
            x.value(t.id);
            x.string(t.name);
            x.vector(t.words);
            x.deque(t.waiters);
        };
        a.tag("SMPL");
        a.value(u32);
        a.value(pod);
        a.string(text);
        a.vector(shorts);
        a.boolVector(bits);
        a.deque(queue);
        a.map(things, thing);
        a.podMap(registers);
        a.podSet(addresses);
        a.sequence(list, thing);
        a.raw(block, sizeof(block));
        a.tag("END ");
    }
};

Sample filled()
{
    Sample s;
    s.u32 = 0xDEADBEEFu;
    s.pod = {7u, 0u, 0x0123456789ABCDEFull, {1, 2, 3}};
    s.text = "Si Shui Gate";
    s.shorts = {1, 2, 65535};
    s.bits = {true, false, true, true};
    s.queue = {5, 4, 3};
    for (int i = 0; i < 20; ++i) // unordered: insertion order must not change the bytes
        s.things[i * 7 % 20] = Thing{i, "thread" + std::to_string(i), {uint32_t(i), uint32_t(i * 2)}, {i, -i}};
    for (uint32_t i = 0; i < 50; ++i)
        s.registers[0x10000000u + i * 0x10u] = i * 3u;
    for (uint32_t i = 0; i < 30; ++i)
        s.addresses.insert(0x00100000u + i * 4u);
    s.list = {Thing{1, "a", {1}, {}}, Thing{2, "b", {}, {9}}};
    for (int i = 0; i < 64; ++i)
        s.block[i] = uint8_t(i * 5);
    return s;
}

std::vector<uint8_t> save(Sample &s)
{
    StateArchive w = StateArchive::writer();
    s.serialize(w);
    return w.data();
}
} // namespace

int main()
{
    Sample original = filled();
    const std::vector<uint8_t> first = save(original);

    Sample loaded;
    StateArchive r = StateArchive::reader(first);
    loaded.serialize(r);
    check(r.atEnd(), "load reads the whole file");
    const std::vector<uint8_t> second = save(loaded);
    check(first == second, "save -> load -> save gives identical bytes");
    check(loaded.things.size() == 20 && loaded.things[13].name == "thread" + std::to_string(loaded.things[13].id),
          "maps come back with their contents");
    check(loaded.bits == original.bits && loaded.queue == original.queue && loaded.text == original.text,
          "strings, bit vectors and deques come back");

    // Same contents, different insertion order: identical bytes (maps and sets are written in key order).
    Sample reordered = filled();
    reordered.things.clear();
    for (int i = 19; i >= 0; --i)
        reordered.things[i * 7 % 20] = Thing{i, "thread" + std::to_string(i), {uint32_t(i), uint32_t(i * 2)}, {i, -i}};
    check(save(reordered) == first, "insertion order does not change the bytes");

    // A truncated file and an out-of-step file are refused.
    bool refused = false;
    try
    {
        std::vector<uint8_t> cut(first.begin(), first.begin() + first.size() / 2);
        Sample s;
        StateArchive t = StateArchive::reader(cut);
        s.serialize(t);
    }
    catch (const StateError &)
    {
        refused = true;
    }
    check(refused, "a truncated state is refused");
    refused = false;
    try
    {
        std::vector<uint8_t> bad = first;
        bad[0] = 'X'; // the first section tag
        Sample s;
        StateArchive t = StateArchive::reader(bad);
        s.serialize(t);
    }
    catch (const StateError &)
    {
        refused = true;
    }
    check(refused, "a state out of step with the code is refused");

    std::printf("%s\n", failures ? "STATE ARCHIVE TESTS FAILED" : "ALL STATE ARCHIVE TESTS PASSED");
    return failures ? 1 : 0;
}
