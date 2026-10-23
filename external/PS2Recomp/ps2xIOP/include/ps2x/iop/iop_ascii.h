#pragma once

#include <cstddef>
#include <string_view>

namespace ps2x::iop
{
    // ASCII-only case folding for IRX library and module names. The runtime never changes the C locale, so this
    // equals std::tolower for every byte value, without a C-runtime call per character (issue 025 v8, fix A).
    constexpr unsigned char asciiLower(unsigned char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
    }

    constexpr bool asciiEqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept
    {
        if (lhs.size() != rhs.size())
            return false;
        for (std::size_t i = 0; i < lhs.size(); ++i)
        {
            if (asciiLower(static_cast<unsigned char>(lhs[i])) != asciiLower(static_cast<unsigned char>(rhs[i])))
                return false;
        }
        return true;
    }
}
