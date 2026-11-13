#ifndef PS2_GS_HANDOVER_PLACES_H
#define PS2_GS_HANDOVER_PLACES_H

// DW5XL (issue 049, amendment v3.12 step 3): the places of the pictures a renderer hands to a presenter on the
// graphics card. The renderer copies each finished picture into a place; the presenter takes the newest one, shows
// it for as long as it likes and gives the place back. Who may use which place when is decided here, so that it can
// be tested without a graphics card. The rules:
//  - the renderer never waits for the presenter: with no free place it takes the oldest picture the presenter has
//    not taken yet (that picture is given up), and with every place held by the presenter the new picture is not
//    handed over;
//  - a picture the presenter holds is never written to;
//  - the presenter gets the newest picture; the older ones it did not take become free.
// The callers keep one lock around every call (VulkanGsHandover's).

#include <cstdint>
#include <vector>

namespace ps2x::gs
{
    class HandoverPlaces
    {
    public:
        static constexpr uint32_t kNone = UINT32_MAX;
        enum class State : uint8_t
        {
            Free,    // nobody uses it
            Writing, // the renderer is copying a picture into it
            Ready,   // a finished picture the presenter has not taken
            Held     // the presenter has the picture
        };

        explicit HandoverPlaces(uint32_t count) : m_places(count) {}

        uint32_t count() const { return static_cast<uint32_t>(m_places.size()); }
        State state(uint32_t place) const { return m_places[place].state; }
        uint64_t field(uint32_t place) const { return m_places[place].field; }

        // The renderer: the place for a new picture, from now on Writing. A free place; without one the oldest
        // ready one (`gaveUp`: its picture is lost); kNone when the presenter holds every other place.
        uint32_t forNewPicture(bool &gaveUp)
        {
            gaveUp = false;
            uint32_t chosen = kNone;
            for (uint32_t i = 0; i < count(); ++i)
            {
                if (m_places[i].state == State::Free)
                {
                    chosen = i;
                    break;
                }
            }
            if (chosen == kNone)
            {
                for (uint32_t i = 0; i < count(); ++i)
                    if (m_places[i].state == State::Ready && (chosen == kNone || m_places[i].order < m_places[chosen].order))
                        chosen = i;
                gaveUp = chosen != kNone;
            }
            if (chosen != kNone)
                m_places[chosen].state = State::Writing;
            return chosen;
        }

        // The renderer: the picture in `place` (taken with forNewPicture) is complete, of field `field`.
        void ready(uint32_t place, uint64_t field)
        {
            if (place >= count() || m_places[place].state != State::Writing)
                return;
            m_places[place].state = State::Ready;
            m_places[place].field = field;
            m_places[place].order = ++m_order;
        }

        // The renderer: the picture for `place` was not made after all.
        void abandon(uint32_t place)
        {
            if (place < count() && m_places[place].state == State::Writing)
                m_places[place].state = State::Free;
        }

        // The presenter: the newest ready picture of a field up to `maxField`, from now on Held; the ready pictures
        // older than it become free (`superseded` counts them). kNone when there is none.
        uint32_t takeNewest(uint64_t maxField, uint32_t &superseded)
        {
            superseded = 0;
            uint32_t chosen = kNone;
            for (uint32_t i = 0; i < count(); ++i)
            {
                const Place &place = m_places[i];
                if (place.state == State::Ready && place.field <= maxField &&
                    (chosen == kNone || place.order > m_places[chosen].order))
                    chosen = i;
            }
            if (chosen == kNone)
                return kNone;
            for (uint32_t i = 0; i < count(); ++i)
            {
                if (i != chosen && m_places[i].state == State::Ready && m_places[i].order < m_places[chosen].order)
                {
                    m_places[i].state = State::Free;
                    ++superseded;
                }
            }
            m_places[chosen].state = State::Held;
            return chosen;
        }

        // The presenter: done with the picture in `place`.
        void release(uint32_t place)
        {
            if (place < count() && m_places[place].state == State::Held)
                m_places[place].state = State::Free;
        }

        // The renderer: the ready pictures are given up (their size is no longer the output's). Returns how many.
        uint32_t giveUpReady()
        {
            uint32_t given = 0;
            for (Place &place : m_places)
            {
                if (place.state == State::Ready)
                {
                    place.state = State::Free;
                    ++given;
                }
            }
            return given;
        }

    private:
        struct Place
        {
            State state = State::Free;
            uint64_t field = 0;
            uint64_t order = 0; // the higher, the later it became ready
        };
        std::vector<Place> m_places;
        uint64_t m_order = 0;
    };
} // namespace ps2x::gs

#endif
