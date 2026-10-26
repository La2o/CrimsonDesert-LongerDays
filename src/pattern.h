#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace longerdays::mem
{
    // A byte signature with wildcards ("48 8B 05 ?? ?? ?? ?? C3").
    //
    // Searching uses the longest wildcard-free run as an anchor and finds it
    // with Boyer-Moore-Horspool, which skips most of a ~100 MB image without
    // looking at it; only anchor hits are checked against the full pattern.
    // Header-only and platform-free so the unit tests run anywhere.
    class BytePattern
    {
    public:
        static std::optional<BytePattern> Parse(std::string_view text)
        {
            BytePattern p;
            size_t i = 0;
            while (i < text.size())
            {
                const char c = text[i];
                if (c == ' ' || c == '\t') { ++i; continue; }
                if (c == '?')
                {
                    p.m_bytes.push_back(0);
                    p.m_fixed.push_back(false);
                    i += (i + 1 < text.size() && text[i + 1] == '?') ? 2 : 1;
                    continue;
                }
                const int hi = Hex(c);
                const int lo = i + 1 < text.size() ? Hex(text[i + 1]) : -1;
                if (hi < 0 || lo < 0) return std::nullopt; // malformed: refuse
                p.m_bytes.push_back(static_cast<uint8_t>(hi << 4 | lo));
                p.m_fixed.push_back(true);
                i += 2;
            }
            if (!p.PickAnchor()) return std::nullopt; // empty or all wildcards
            return p;
        }

        size_t size() const { return m_bytes.size(); }

        // First match in [begin, end), or nullptr.
        const uint8_t* Find(const uint8_t* begin, const uint8_t* end) const
        {
            if (static_cast<size_t>(end - begin) < m_bytes.size()) return nullptr;
            const auto anchorBegin = m_bytes.begin() + static_cast<std::ptrdiff_t>(m_anchorPos);
            const std::boyer_moore_horspool_searcher searcher(anchorBegin, anchorBegin + static_cast<std::ptrdiff_t>(m_anchorLen));

            // Anchor hits must leave room for the bytes before and after it.
            const uint8_t* from = begin + m_anchorPos;
            const uint8_t* to   = end - (m_bytes.size() - m_anchorPos - m_anchorLen);
            while (from < to)
            {
                const uint8_t* hit = std::search(from, to, searcher);
                if (hit == to) return nullptr;
                const uint8_t* start = hit - m_anchorPos;
                if (MatchesAt(start)) return start;
                from = hit + 1;
            }
            return nullptr;
        }

    private:
        static int Hex(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        bool PickAnchor()
        {
            for (size_t i = 0; i < m_fixed.size();)
            {
                if (!m_fixed[i]) { ++i; continue; }
                size_t j = i;
                while (j < m_fixed.size() && m_fixed[j]) ++j;
                if (j - i > m_anchorLen) { m_anchorPos = i; m_anchorLen = j - i; }
                i = j;
            }
            return m_anchorLen > 0;
        }

        bool MatchesAt(const uint8_t* p) const
        {
            for (size_t i = 0; i < m_bytes.size(); ++i)
                if (m_fixed[i] && p[i] != m_bytes[i]) return false;
            return true;
        }

        std::vector<uint8_t> m_bytes;
        std::vector<bool>    m_fixed;
        size_t m_anchorPos = 0;
        size_t m_anchorLen = 0;
    };
}
