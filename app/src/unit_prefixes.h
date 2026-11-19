#pragma once

// The calculator only knows a fixed list of prefixed symbols (nC, kV, mm...), while the interface
// writes any engineering prefix (pV, MV, fC...). This rewrites a prefixed symbol the calculator does
// not list as "(1e-12 V)", with an explicit '*' after a number or ')'. Plain C++ so it is testable
// without Qt.

#include <cctype>
#include <set>
#include <string>

namespace qsig::app::detail
{
inline std::string replaceAll(std::string s, const std::string &from, const std::string &to)
{
    for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
    return s;
}

inline int prefixExponent(char c)
{
    switch (c)
    {
    case 'T':
        return 12;
    case 'G':
        return 9;
    case 'M':
        return 6;
    case 'k':
        return 3;
    case 'm':
        return -3;
    case 'u':
        return -6;
    case 'n':
        return -9;
    case 'p':
        return -12;
    case 'f':
        return -15;
    default:
        return 0;
    }
}

// 'known' holds the symbols the calculator accepts (EmCalculator::supportedUnitSymbols()).
inline std::string expandUnitPrefixes(const std::string &text, const std::set<std::string> &known)
{
    std::string s = replaceAll(text, "\xC2\xB5", "u"); // micro sign
    s = replaceAll(s, "\xCE\xBC", "u");                // Greek mu
    s = replaceAll(s, "\xCE\xA9", "Ohm");              // Omega

    std::string out;
    out.reserve(s.size() + 16);
    std::size_t i = 0;
    while (i < s.size())
    {
        if (!std::isalpha(static_cast<unsigned char>(s[i])))
        {
            out += s[i++];
            continue;
        }
        std::size_t j = i;
        while (j < s.size() && std::isalpha(static_cast<unsigned char>(s[j]))) ++j;
        const std::string token = s.substr(i, j - i);
        const int exponent = token.size() >= 2 ? prefixExponent(token[0]) : 0;
        if (exponent != 0 && known.count(token) == 0 && known.count(token.substr(1)) != 0)
        {
            const std::size_t last = out.find_last_not_of(' ');
            if (last != std::string::npos &&
                (std::isdigit(static_cast<unsigned char>(out[last])) || out[last] == '.' || out[last] == ')'))
                out += '*';
            out += "(1e" + std::to_string(exponent) + " " + token.substr(1) + ")";
        }
        else
        {
            out += token;
        }
        i = j;
    }
    return out;
}
} // namespace qsig::app::detail
