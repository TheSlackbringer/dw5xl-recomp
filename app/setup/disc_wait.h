#pragma once
// Load Original without an extra click (issue 011 M10): what the game's disc check is told while the Insert-disc
// window is open. The game's check (OVL02 0x637E90) asks sceCdDiskReady twice in a row (states 3 and 4) and then
// the disc type; "not ready" keeps it waiting at state 3, as a PS2 drive spinning up.
// The game thread asks notReady() at every ready poll and calls checked() at the disc-type query; the window calls
// cancel(). Thread-safe.
#include <atomic>

namespace dw5xl::setup {

class DiscWait
{
public:
    enum class Event
    {
        None,
        WindowOpened, // the requested disc is missing: open the Insert-disc window
        DiscReady,    // it is installed now: close the window, the game continues
    };

    // `installed`: the requested disc is installed in the player data folder.
    // Returns true for "not ready"; `event` says what the window should do.
    bool notReady(bool installed, Event &event)
    {
        event = Event::None;
        if (m_cancelled)
            return false; // the player cancelled: ready until the game has checked the disc (and found none)
        if (installed)
        {
            if (m_waiting.exchange(false))
                event = Event::DiscReady;
            return false;
        }
        if (!m_waiting.exchange(true))
            event = Event::WindowOpened;
        return true;
    }

    // The player closed the Insert-disc window without installing.
    void cancel()
    {
        if (m_waiting.exchange(false))
            m_cancelled = true;
    }

    // The game asked for the disc type: this check is over (a cancel has led to the game's own question; if the
    // player chooses NO there, the next check waits for the disc again).
    void checked() { m_cancelled = false; }

    bool waiting() const { return m_waiting; }

private:
    std::atomic<bool> m_waiting{false};
    std::atomic<bool> m_cancelled{false};
};

} // namespace dw5xl::setup
