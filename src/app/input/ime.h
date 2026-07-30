#pragma once

#include "core/unicode/width.h"

#include <QString>

#include <string>
#include <vector>

namespace krait::app::input {

// The in-flight composition (plan T29).
//
// A preedit is NOT grid content: it belongs to the IME until it commits, so it
// never enters the parser and never touches scrollback. It is drawn over the
// cells at the cursor and vanishes on commit or cancel — which is why it lives
// here rather than as a Grid mutation. Getting that wrong is how terminals end
// up with half-composed Thai in their scrollback.
class Composition {
  public:
    // Replaces the composition. An IME sends the whole preedit on every event
    // rather than a delta, so this is assignment and not an append.
    void setPreedit(const QString& text, int cursorInChars);

    void clear();

    bool active() const { return !m_clusters.empty(); }

    const QString& text() const { return m_text; }

    // The codepoints, already grapheme-segmented into clusters — the same shape
    // the renderer wants, so the drawing path does no segmentation of its own.
    const std::vector<std::u32string>& clusters() const { return m_clusters; }

    // Display width in CELLS, measured with the project's width engine rather
    // than counted in characters. Thai marks are zero-width and Japanese is
    // double-width, so a character count puts the candidate window in the wrong
    // place for both of the scripts the M1 acceptance script exercises.
    int columns(core::unicode::Ambiguous ambiguous) const;

    // Where the caret sits inside the composition, in cells. The IME anchors
    // its candidate list here, not at the start of the preedit.
    int cursorColumns(core::unicode::Ambiguous ambiguous) const;

  private:
    QString m_text;
    std::vector<std::u32string> m_clusters;
    // Cluster index the caret precedes, derived once in setPreedit: the IME
    // reports it in UTF-16 code units, which is neither characters nor cells.
    int m_cursorCluster = 0;
};

}  // namespace krait::app::input
