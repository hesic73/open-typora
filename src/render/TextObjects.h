#pragma once

#include <QTextCursor>
#include <QTextFormat>

namespace ot {

/// Inline objects drawn by this code rather than by Qt.
constexpr int MathObjectType = QTextFormat::UserObject + 2;
constexpr int MathSourceProperty = QTextFormat::UserProperty + 20;

/// Inserts typeset LaTeX that sits on the baseline of `textFormat`'s font.
/// `maxWidth` shrinks a formula that would not fit. Returns false, and
/// inserts nothing, when the source is not valid LaTeX.
bool insertMath(QTextCursor &cursor, const QString &latex, bool display, const QTextCharFormat &textFormat,
                qreal maxWidth = 0, qreal sizeFactor = 0);

/// Whether the LaTeX typesets.
bool mathTypesets(const QString &latex, bool display, qreal pixelSize);

} // namespace ot
