#pragma once

#include "Document.h"
#include "EditorTheme.h"
#include "Inline.h"

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTextDocument>

#include <memory>

namespace ot {

class ImageStore;

/// Ties a stretch of the laid-out document to a stretch of block text. When
/// both lengths are equal and the run is not atomic, characters correspond one
/// to one; otherwise the run is one unit (a formula, a picture, an entity) and
/// only its two ends correspond.
struct Run {
    int docPos = 0;
    int docLength = 0;
    int sourcePos = 0;
    int sourceLength = 0;
    bool atomic = false;
};

struct Decoration {
    enum Type { CodeBox, MathBox, QuoteBar, Rule, HeadingRule, Bullet, Number, Checkbox, Label };
    Type type = CodeBox;
    QRectF rect;
    QString text;
    bool checked = false;
    int level = 0;
};

struct TocEntry {
    int level = 1;
    QString title;
    friend bool operator==(const TocEntry &, const TocEntry &) = default;
};

/// Whether a paragraph is "[TOC]", which shows the document's headings.
bool isTableOfContents(const Block &block);

struct RenderContext {
    const EditorTheme *theme = nullptr;
    qreal width = 600;
    qreal zoom = 1.0;
    /// The caret's offset in the block text while the block is being edited, -1 otherwise.
    int caret = -1;
    int listNumber = 1;
    int listDepth = 0;
    QString baseDir;
    const LinkReferences *references = nullptr;
    ImageStore *images = nullptr;
    /// For printing and export: no editing state, pictures that are loading left out.
    bool print = false;
    /// The headings, for a "[TOC]" paragraph.
    const QList<TocEntry> *toc = nullptr;
};

/// One block, laid out and ready to paint. Coordinates are relative to the
/// block's top-left corner.
class RenderedBlock
{
public:
    std::unique_ptr<QTextDocument> doc;
    QList<Run> runs;
    QList<Decoration> decorations;
    QPointF docOffset;
    qreal height = 0;
    qreal marginTop = 0;
    qreal marginBottom = 0;
    /// Room below the document inside the block (padding of code boxes, heading rules).
    qreal bottomPad = 0;
    bool waitingForImages = false;
    bool hasImages = false;

    int toDoc(int source) const;
    int toSource(int docPos) const;
    /// The caret's rectangle, one pixel wide, at a source offset.
    QRectF caretRect(int source, int extra = 0) const;
    /// The whole line holding a source offset, spacing included.
    QRectF lineRect(int source) const;
    qreal docHeight() const;
    /// The source offset nearest to a point.
    int hitTest(const QPointF &point) const;
    /// Rectangles covering a source range, line by line, for selection and search hits.
    QList<QRectF> rangeRects(int from, int to) const;
    /// The link under a point, empty when none.
    QString anchorAt(const QPointF &point) const;
    /// The visual line that holds a source offset, as source offsets [start, end].
    Segment lineAt(int source) const;
};

std::unique_ptr<RenderedBlock> renderBlock(const Block &block, const RenderContext &context);

/// What decides how a block being edited looks: which constructs the caret
/// stands in. Equal keys render alike, so a block need not be laid out again.
QString revealKey(const Block &block, int caret, const LinkReferences *references);

/// Inline parsing is cached by text; references changing makes the cache stale.
QList<InlineNode> cachedInline(const QString &text, const LinkReferences *references);
void clearInlineCache();

} // namespace ot
