// DW5XL (issue 049, amendment v3.12 step 7): see runtime/fatal_message.h. A source of its own: the system's
// header and raylib's name the same things differently (CloseWindow, Rectangle, ...), so the two do not meet.
#include "runtime/fatal_message.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
    std::wstring wide(const std::string &utf8)
    {
        if (utf8.empty())
            return std::wstring();
        const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (count <= 0)
            return std::wstring(utf8.begin(), utf8.end()); // not UTF-8: byte for byte
        std::wstring out(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), count);
        return out;
    }
} // namespace

void ps2x::showFatalMessage(const std::string &title, const std::string &text)
{
    MessageBoxW(nullptr, wide(text).c_str(), wide(title).c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}
#else
void ps2x::showFatalMessage(const std::string &, const std::string &) {}
#endif
