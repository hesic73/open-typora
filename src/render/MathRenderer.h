#pragma once

#include <QColor>
#include <QPointF>
#include <QString>

#include <memory>

class QPainter;

namespace ot {

/// One laid-out formula, ready to draw.
class Formula
{
public:
    virtual ~Formula() = default;
    virtual qreal width() const = 0;
    /// Height above the baseline.
    virtual qreal ascent() const = 0;
    /// Depth below the baseline.
    virtual qreal descent() const = 0;
    /// Draws with the baseline starting at `origin`, scaled by `scale`.
    virtual void draw(QPainter *painter, const QPointF &origin, qreal scale) const = 0;
};

/// Typesets LaTeX math. Results are cached, so asking again is cheap.
namespace math {

/// False when the math fonts could not be found; formulas then stay as text.
bool available();

/// Whether braces, environments and \\left/\\right pairs all close. The
/// typesetter draws unfinished input half-done instead of refusing it.
bool isComplete(const QString &latex);

/// Returns nothing when the source is not valid LaTeX.
std::shared_ptr<const Formula> typeset(const QString &latex, bool display, qreal pixelSize, const QColor &color);

} // namespace math

} // namespace ot
