// Issue 027: the save-state archive. Every container type must survive save -> load -> save with identical bytes,
// and a damaged or out-of-step file must be refused (an exception), never half-loaded.
#include "runtime/state_archive.h"

#include <cstdio>
#include <cstring>
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
    std::unordered_map<std::string, uint32_t> named; // string keys (IOP module manager)
    std::unordered_set<std::string> keys;
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
        a.podMap(named);
        a.podSet(keys);
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
    for (const char *name : {"sio2man", "padman", "mcman", "libsd", "cdvdfsv"})
    {
        s.named[name] = uint32_t(std::string(name).size());
        s.keys.insert(name);
    }
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
    check(loaded.named == original.named && loaded.keys == original.keys, "string-keyed maps and sets come back");

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

    // Issue 027 v3: a structure that changed size between the build that saved and the build that loads is refused
    // (the size record), even where the bytes that follow would happen to fill the bigger structure.
    struct Small { uint32_t a; };
    struct Grown { uint32_t a; uint32_t added; };
    refused = false;
    try
    {
        Small one{1u}, two{2u};
        StateArchive w = StateArchive::writer();
        w.value(one);
        w.value(two);
        Grown g{};
        StateArchive t = StateArchive::reader(w.data());
        t.value(g);
    }
    catch (const StateError &)
    {
        refused = true;
    }
    check(refused, "a structure saved smaller than this build's is refused");
    refused = false;
    try
    {
        std::vector<Small> smalls{{1u}, {2u}, {3u}, {4u}};
        StateArchive w = StateArchive::writer();
        w.vector(smalls);
        w.vector(smalls); // enough bytes follow for the bigger elements: only the size record can tell
        std::vector<Grown> grown;
        StateArchive t = StateArchive::reader(w.data());
        t.vector(grown);
    }
    catch (const StateError &)
    {
        refused = true;
    }
    check(refused, "a list whose element size changed is refused");

    // The gap after `irq` holds whatever memory had there; it is saved as zero, so the same state gives the same bytes.
    struct Gapped { int32_t irq; uint64_t delay; };
    {
        Gapped g;
        std::memset(&g, 0xAB, sizeof(g));
        g.irq = 1;
        g.delay = 2;
        StateArchive w = StateArchive::writer();
        w.value(g);
        const std::vector<uint8_t> &bytes = w.data(); // 4-byte size record, then the object
        check(bytes.size() == 4 + sizeof(Gapped) && bytes[4 + 4] == 0 && bytes[4 + 7] == 0,
              "padding inside a saved structure is written as zero");
    }

    std::printf("%s\n", failures ? "STATE ARCHIVE TESTS FAILED" : "ALL STATE ARCHIVE TESTS PASSED");
    return failures ? 1 : 0;
}
