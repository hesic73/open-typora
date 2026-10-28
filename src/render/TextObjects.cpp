#include "TextObjects.h"

#include "MathRenderer.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QHash>
#include <QFontInfo>
#include <QPainter>
#include <QPointer>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextObjectInterface>

namespace ot {

namespace {

constexpr int MathDisplayProperty = QTextFormat::UserProperty + 21;
constexpr int MathSizeProperty = QTextFormat::UserProperty + 22;
constexpr int MathColorProperty = QTextFormat::UserProperty + 23;
constexpr int MathScaleProperty = QTextFormat::UserProperty + 24;

std::shared_ptr<const Formula> formulaFor(const QTextFormat &format)
{
    return math::typeset(format.stringProperty(MathSourceProperty), format.boolProperty(MathDisplayProperty),
                         format.doubleProperty(MathSizeProperty), format.colorProperty(MathColorProperty));
}

/// Draws typeset LaTeX. Qt gives an inline object the descent of its format's
/// font, so each formula carries a font chosen for a descent as deep as the
/// formula's own; the line then makes room below the baseline too.
class MathObject : public QObject, public QTextObjectInterface
{
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    using QObject::QObject;

    QSizeF intrinsicSize(QTextDocument *, int, const QTextFormat &format) override
    {
        const std::shared_ptr<const Formula> formula = formulaFor(format);
        if (!formula)
            return {};
        const qreal scale = format.doubleProperty(MathScaleProperty);
        const QFontMetricsF metrics(format.toCharFormat().font());
        return QSizeF(formula->width() * scale, formula->ascent() * scale + metrics.descent());
    }

    void drawObject(QPainter *painter, const QRectF &rect, QTextDocument *document, int position,
                    const QTextFormat &format) override
    {
        const std::shared_ptr<const Formula> formula = formulaFor(format);
        if (!formula)
            return;
        qreal baseline = rect.top() + formula->ascent() * format.doubleProperty(MathScaleProperty);
        const QTextBlock block = document->findBlock(position);
        if (const QTextLayout *layout = block.layout()) {
            const QTextLine line = layout->lineForTextPosition(position - block.position());
            if (line.isValid())
                baseline = document->documentLayout()->blockBoundingRect(block).top() + line.y() + line.ascent();
        }
        formula->draw(painter, QPointF(rect.left(), baseline), format.doubleProperty(MathScaleProperty));
    }
};

MathObject *mathObject()
{
    static QPointer<MathObject> object;
    if (!object)
        object = new MathObject(QCoreApplication::instance());
    return object;
}

/// The smallest pixel size of the font whose descent reaches `depth`.
QFont fontWithDescent(const QFont &base, qreal depth)
{
    // Measuring fonts is slow, and the same few depths come back all the time.
    static QHash<QString, int> cache;
    const QString key = base.key() + QLatin1Char('|') + QString::number(qRound(depth * 8));
    QFont font = base;
    if (const auto found = cache.constFind(key); found != cache.constEnd()) {
        font.setPixelSize(*found);
        return font;
    }
    int chosen = 399;
    for (int pixels = 4; pixels < 400; pixels += 1) {
        font.setPixelSize(pixels);
        if (QFontMetricsF(font).descent() >= depth) {
            chosen = pixels;
            break;
        }
    }
    if (cache.size() > 4096)
        cache.clear();
    cache.insert(key, chosen);
    font.setPixelSize(chosen);
    return font;
}

} // namespace

bool mathTypesets(const QString &latex, bool display, qreal pixelSize)
{
    return math::typeset(latex, display, pixelSize, Qt::black) != nullptr;
}

bool insertMath(QTextCursor &cursor, const QString &latex, bool display, const QTextCharFormat &textFormat,
                qreal maxWidth, qreal sizeFactor)
{
    const QFont font = textFormat.font();
    const int pixels = font.pixelSize() > 0 ? font.pixelSize() : QFontInfo(font).pixelSize();
    // A touch larger than the text, as in typeset documents; display math more so.
    const qreal factor = sizeFactor > 0 ? sizeFactor : (display ? 1.2 : 1.08);
    const qreal pixelSize = pixels * factor;
    const QColor color = textFormat.foreground().color();
    const std::shared_ptr<const Formula> formula = math::typeset(latex, display, pixelSize, color);
    if (!formula)
        return false;

    QAbstractTextDocumentLayout *layout = cursor.document()->documentLayout();
    if (!layout->handlerForObject(MathObjectType))
        layout->registerHandler(MathObjectType, mathObject());

    // A formula wider than the column is shrunk, down to a limit where it stays readable.
    const qreal scale = maxWidth > 0 && formula->width() > maxWidth ? qMax(0.5, maxWidth / formula->width()) : 1.0;
    QTextCharFormat format = textFormat;
    format.setObjectType(MathObjectType);
    format.setVerticalAlignment(QTextCharFormat::AlignBaseline);
    format.setFont(fontWithDescent(textFormat.font(), formula->descent() * scale));
    format.setProperty(MathSourceProperty, latex);
    format.setProperty(MathDisplayProperty, display);
    format.setProperty(MathSizeProperty, pixelSize);
    format.setProperty(MathColorProperty, color);
    format.setProperty(MathScaleProperty, scale);
    format.setBackground(Qt::NoBrush);
    cursor.insertText(QString(QChar::ObjectReplacementCharacter), format);
    return true;
}

} // namespace ot

#include "TextObjects.moc"
