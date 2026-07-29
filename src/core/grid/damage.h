#pragma once

#include <vector>

namespace krait::core::vt {

// Per-row dirty column spans, coalesced by min/max. Scroll and resize mark
// everything. The renderer consumes with all()/spans() then clear().
class DamageList {
  public:
    // col1 is inclusive; col0 == -1 means the row is clean.
    struct Span {
        int col0 = -1;
        int col1 = -1;
    };

    explicit DamageList(int rowCount = 0) : m_spans(static_cast<std::size_t>(rowCount)) {}

    void reset(int rowCount) {
        m_spans.assign(static_cast<std::size_t>(rowCount), Span{});
        m_all = false;
    }

    void mark(int row, int col0, int col1) {
        Span& s = m_spans[static_cast<std::size_t>(row)];
        if (s.col0 == -1) {
            s = {col0, col1};
        } else {
            s.col0 = col0 < s.col0 ? col0 : s.col0;
            s.col1 = col1 > s.col1 ? col1 : s.col1;
        }
    }

    void markAll() { m_all = true; }

    bool all() const { return m_all; }

    const std::vector<Span>& spans() const { return m_spans; }

    void clear() {
        for (Span& s : m_spans) {
            s = {};
        }
        m_all = false;
    }

  private:
    std::vector<Span> m_spans;
    bool m_all = false;
};

}  // namespace krait::core::vt
