// 042: deterministic interval accounting, nested clocks and interrupted stages; no sleeps/game data.
#include "stage_timing.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace ps2recomp::timing;
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }

int main()
{
    try
    {
        int64_t now = 0;
        std::ostringstream output;
        Session session(output, "run\"identity", "synthetic", "main", [&] { return now; });
        {
            Stage parent(&session, "profile");
            parent.field("release","fixture-release");
            parent.field("reason","no \"completed\" record\nretry");
            now = 100;
            {
                Stage child(&session, "child", parent.id());
                now = 160;
                child.count("functions", 2);
                child.finish();
            }
            now = 200;
            { Stage interrupted(&session, "interrupted", parent.id()); now = 220; }
            now = 250;
            parent.finish("cancelled");
        }
        const std::string text = output.str();
        require(text.find("run\\\"identity") != std::string::npos, "JSON identity escaping");
        require(text.find("\"duration_us\":60") != std::string::npos, "child duration");
        require(text.find("\"duration_us\":250") != std::string::npos, "parent is wall time, not summed children");
        require(text.find("\"result\":\"incomplete\"") != std::string::npos, "abandoned interval is incomplete");
        require(text.find("\"result\":\"cancelled\"") != std::string::npos, "cancelled result retained");
        require(text.find("\"functions\":2") != std::string::npos, "numeric counts retained");
        require(text.find("\"release\":\"fixture-release\"")!=std::string::npos,"release provenance retained");
        require(text.find("no \\\"completed\\\" record\\nretry")!=std::string::npos,"cache reason fields safely retained as JSON");
        require(session.healthy(), "healthy timing sink");
        Stage disabled(nullptr, "disabled");
        disabled.finish();
        std::cout << "setup timing tests passed\n";
        return 0;
    }
    catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
