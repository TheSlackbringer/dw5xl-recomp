#ifndef PS2_DEBUG_PANEL_H
#define PS2_DEBUG_PANEL_H

#include <functional>

class PS2Runtime;

class PS2DebugPanel
{
public:
    void initialize();
    void shutdown();
    void draw(PS2Runtime &runtime);

    bool isVisible() const { return m_visible; }
    void setVisible(bool visible) { m_visible = visible; }
    void toggleVisible() { m_visible = !m_visible; }

    // F1 debugger available (a game's player build turns it off).
    void setDebuggerEnabled(bool enabled) { m_debuggerEnabled = enabled; }
    // A game overlay drawn in the same imgui frame as the debugger (e.g. DW5XL's disc prompt): `wanted` is asked
    // every frame, `draw` then draws its own imgui windows.
    void setOverlay(std::function<bool()> wanted, std::function<void()> draw)
    {
        m_overlayWanted = std::move(wanted);
        m_overlayDraw = std::move(draw);
    }

private:
    bool m_initialized = false;
    bool m_visible = true;
    bool m_debuggerEnabled = true;
    std::function<bool()> m_overlayWanted;
    std::function<void()> m_overlayDraw;
    bool m_showRegisters = true;
    unsigned int m_memoryAddress = 0x00100000u;
    unsigned int m_memoryBytes = 0x100u;
};

#endif // PS2_DEBUG_PANEL_H
