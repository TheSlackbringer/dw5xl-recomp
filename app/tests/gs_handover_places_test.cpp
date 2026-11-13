// DW5XL-TEST issue 049 (amendment v3.12 step 3): the places of the pictures a renderer hands to a presenter on the
// graphics card (runtime/gs/gs_handover_places.h). No graphics card and no game data.
//  - the renderer always gets a place while the presenter holds fewer than all of them, and never a held one;
//  - with no free place the oldest picture the presenter has not taken is given up;
//  - the presenter gets the newest picture up to the field it asks for; the older ones become free, a newer one
//    of a later field stays;
//  - a picture being written is not handed out; one not made after all leaves its place free;
//  - a made-up renderer and presenter taking turns at random for 200,000 steps never break these rules.
#include "runtime/gs/gs_handover_places.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
using ps2x::gs::HandoverPlaces;
using State = HandoverPlaces::State;
constexpr uint32_t kNone = HandoverPlaces::kNone;

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

// A picture made by the renderer: place taken, written, ready.
uint32_t make(HandoverPlaces &places, uint64_t field, bool *gaveUp = nullptr)
{
    bool lost = false;
    const uint32_t place = places.forNewPicture(lost);
    if (gaveUp)
        *gaveUp = lost;
    if (place != kNone)
        places.ready(place, field);
    return place;
}
} // namespace

int main()
{
    {
        HandoverPlaces places(3);
        uint32_t superseded = 9;
        check(places.takeNewest(UINT64_MAX, superseded) == kNone && superseded == 0, "nothing to take from empty places");
        bool gaveUp = true;
        const uint32_t a = make(places, 10, &gaveUp);
        check(a != kNone && !gaveUp && places.state(a) == State::Ready, "the first picture gets a free place");
        const uint32_t taken = places.takeNewest(UINT64_MAX, superseded);
        check(taken == a && superseded == 0 && places.state(a) == State::Held, "the presenter takes it and holds it");
        check(places.takeNewest(UINT64_MAX, superseded) == kNone, "a held picture is not handed out twice");
        places.release(a);
        check(places.state(a) == State::Free, "released, its place is free");
    }
    {
        // The presenter holds one picture and takes nothing more: the renderer keeps getting places.
        HandoverPlaces places(3);
        uint32_t superseded = 0;
        const uint32_t held = make(places, 1);
        check(places.takeNewest(UINT64_MAX, superseded) == held, "the presenter holds field 1");
        bool gaveUp = false;
        const uint32_t b = make(places, 2, &gaveUp);
        const uint32_t c = make(places, 3, &gaveUp);
        check(b != kNone && c != kNone && b != held && c != held && b != c && !gaveUp,
              "two more pictures get the two other places");
        const uint32_t d = make(places, 4, &gaveUp);
        check(d == b && gaveUp, "with no free place the oldest untaken picture (field 2) is given up");
        const uint32_t e = make(places, 5, &gaveUp);
        check(e == c && gaveUp, "then the next oldest (field 3)");
        check(places.state(held) == State::Held, "the held picture was never touched");
        const uint32_t newest = places.takeNewest(UINT64_MAX, superseded);
        check(newest == e && places.field(newest) == 5 && superseded == 1,
              "the presenter gets the newest (field 5); the older untaken one (field 4) becomes free");
        check(places.state(d) == State::Free, "field 4's place is free");
    }
    {
        // Every place held: the new picture is not handed over, and nothing held is touched.
        HandoverPlaces places(2);
        uint32_t superseded = 0;
        make(places, 1);
        const uint32_t first = places.takeNewest(UINT64_MAX, superseded);
        make(places, 2);
        const uint32_t second = places.takeNewest(UINT64_MAX, superseded);
        check(first != kNone && second != kNone && first != second, "the presenter holds both places");
        bool gaveUp = true;
        check(places.forNewPicture(gaveUp) == kNone && !gaveUp, "no place for a new picture, nothing given up");
        check(places.state(first) == State::Held && places.state(second) == State::Held, "both stay held");
    }
    {
        // Up to a field: the newest picture of a field up to it; a later field's picture stays for later.
        HandoverPlaces places(4);
        uint32_t superseded = 0;
        const uint32_t f5 = make(places, 5), f6 = make(places, 6), f7 = make(places, 7);
        const uint32_t taken = places.takeNewest(6, superseded);
        check(taken == f6 && superseded == 1, "up to field 6: field 6 is taken, field 5 becomes free");
        check(places.state(f5) == State::Free && places.state(f7) == State::Ready, "field 7 stays ready");
        check(places.takeNewest(6, superseded) == kNone, "nothing more up to field 6");
        check(places.takeNewest(7, superseded) == f7 && superseded == 0, "field 7 is taken when asked for");
    }
    {
        // A picture being written is not handed out; one not made after all leaves its place free.
        HandoverPlaces places(2);
        uint32_t superseded = 0;
        bool gaveUp = false;
        const uint32_t writing = places.forNewPicture(gaveUp);
        check(places.state(writing) == State::Writing, "the renderer's place is being written");
        check(places.takeNewest(UINT64_MAX, superseded) == kNone, "a picture being written is not handed out");
        places.abandon(writing);
        check(places.state(writing) == State::Free, "not made after all: the place is free");
        places.ready(writing, 3);
        check(places.state(writing) == State::Free, "a place not being written cannot become ready");
        places.release(writing);
        check(places.state(writing) == State::Free, "a place not held cannot be released into another state");
    }
    {
        // The output's size changes: the untaken pictures are given up, the held one stays with the presenter.
        HandoverPlaces places(3);
        uint32_t superseded = 0;
        make(places, 1);
        const uint32_t held = places.takeNewest(UINT64_MAX, superseded);
        make(places, 2);
        make(places, 3);
        check(places.giveUpReady() == 2, "two untaken pictures are given up");
        check(places.state(held) == State::Held, "the held one stays");
        check(places.takeNewest(UINT64_MAX, superseded) == kNone, "nothing is left to take");
    }
    {
        // A made-up renderer and presenter at random. The model beside it: which field each place holds.
        HandoverPlaces places(3);
        std::vector<uint32_t> held;
        uint64_t field = 0, lastTaken = 0, taken = 0, noPlace = 0, lost = 0;
        uint32_t seed = 0x2468ACE1u;
        bool heldTouched = false, wentBack = false, tookOld = false, refusedWithFree = false;
        const auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 16; };
        for (int step = 0; step < 200000; ++step)
        {
            const uint32_t what = next() % 10u;
            if (what < 5u)
            {
                bool gaveUp = false;
                const uint32_t place = places.forNewPicture(gaveUp);
                if (place == kNone)
                {
                    ++noPlace;
                    refusedWithFree = refusedWithFree || held.size() < places.count();
                    continue;
                }
                for (uint32_t h : held)
                    heldTouched = heldTouched || h == place;
                lost += gaveUp ? 1u : 0u;
                if (next() % 50u == 0u)
                    places.abandon(place);
                else
                    places.ready(place, ++field);
            }
            else if (what < 8u)
            {
                // A presenter holds the picture it shows and, for a moment, the next one; here now and then every
                // place, so that the renderer is refused too.
                if (held.size() >= places.count() || (held.size() == 2u && next() % 4u != 0u))
                    continue;
                uint32_t superseded = 0;
                // Is a newer picture than the taken one still ready? Then the taken one was not the newest.
                const uint32_t place = places.takeNewest(UINT64_MAX, superseded);
                if (place == kNone)
                    continue;
                ++taken;
                wentBack = wentBack || places.field(place) <= lastTaken;
                lastTaken = places.field(place);
                for (uint32_t i = 0; i < places.count(); ++i)
                    tookOld = tookOld || (places.state(i) == State::Ready && places.field(i) > lastTaken);
                held.push_back(place);
            }
            else if (!held.empty())
            {
                const size_t which = next() % held.size();
                places.release(held[which]);
                held.erase(held.begin() + static_cast<std::ptrdiff_t>(which));
            }
        }
        std::printf("      %llu pictures made, %llu taken, %llu given up, %llu without a place\n",
                    static_cast<unsigned long long>(field), static_cast<unsigned long long>(taken),
                    static_cast<unsigned long long>(lost), static_cast<unsigned long long>(noPlace));
        check(!heldTouched, "at random: the renderer never got a place the presenter held");
        check(!wentBack, "at random: the presenter never got an older picture after a newer one");
        check(!tookOld, "at random: the presenter always got the newest picture there was");
        check(!refusedWithFree, "at random: the renderer was refused only when the presenter held every place");
        check(taken > 1000u && lost > 1000u, "at random: pictures were taken and pictures were given up");
    }
    std::printf(failures ? "FAILED: %d check(s)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
