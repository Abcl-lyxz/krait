#include "ime.h"

#include <algorithm>

namespace krait::app::input {

void Composition::clear() {
    m_text.clear();
    m_clusters.clear();
    m_cursorCluster = 0;
}

void Composition::setPreedit(const QString& text, int cursorInChars) {
    m_text = text;
    m_clusters.clear();
    m_cursorCluster = 0;
    if (text.isEmpty()) {
        return;
    }

    // UCS-4 first: a QString is UTF-16, and a surrogate pair fed to the cluster
    // breaker one half at a time segments as two broken codepoints. Emoji and
    // the rarer CJK live above the BMP, so this is not a corner case.
    const std::u32string codepoints = text.toStdU32String();

    // Segment with the SAME breaker the grid uses (T19). A composition split
    // differently from the text it commits to would draw at one width and land
    // at another — the shimmer users notice and cannot describe.
    core::unicode::ClusterBreaker breaker;
    // The IME reports its caret in UTF-16 code units, which is neither
    // characters nor cells; track the running count so the cluster it falls
    // before can be found without a second pass.
    int utf16Consumed = 0;
    bool cursorFound = false;
    for (const char32_t cp : codepoints) {
        if (m_clusters.empty() || breaker.startsNewCluster(cp)) {
            if (!cursorFound && utf16Consumed >= cursorInChars) {
                m_cursorCluster = static_cast<int>(m_clusters.size());
                cursorFound = true;
            }
            m_clusters.emplace_back(1, cp);
        } else {
            m_clusters.back() += cp;
        }
        utf16Consumed += cp > 0xFFFF ? 2 : 1;
    }
    if (!cursorFound) {
        m_cursorCluster = static_cast<int>(m_clusters.size());
    }
}

int Composition::columns(core::unicode::Ambiguous ambiguous) const {
    int total = 0;
    for (const std::u32string& cluster : m_clusters) {
        total += core::unicode::clusterWidth(cluster, ambiguous);
    }
    return total;
}

int Composition::cursorColumns(core::unicode::Ambiguous ambiguous) const {
    const auto limit =
        std::min(static_cast<std::size_t>(std::max(0, m_cursorCluster)), m_clusters.size());
    int total = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        total += core::unicode::clusterWidth(m_clusters[i], ambiguous);
    }
    return total;
}

}  // namespace krait::app::input
