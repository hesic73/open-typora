#include "MarkdownEditor.h"

#include "ImageStore.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStyleHints>
#include <QTextBlock>
#include <QTextLayout>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace ot {

namespace {

bool isTextKind(BlockKind kind)
{
    return kind == BlockKind::Paragraph || kind == BlockKind::Heading || kind == BlockKind::ListItem
        || kind == BlockKind::Quote;
}

bool isCodeLike(BlockKind kind)
{
    return kind == BlockKind::CodeFence || kind == BlockKind::IndentedCode || kind == BlockKind::FrontMatter
        || kind == BlockKind::MathBlock || kind == BlockKind::Html;
}

QString spaces(int count)
{
    return QString(std::max(0, count), QLatin1Char(' '));
}

int lineStartOf(const QString &text, int offset)
{
    if (offset <= 0)
        return 0;
    return int(text.lastIndexOf(QLatin1Char('\n'), offset - 1)) + 1;
}

int lineEndOf(const QString &text, int offset)
{
    const qsizetype end = text.indexOf(QLatin1Char('\n'), offset);
    return end < 0 ? int(text.size()) : int(end);
}

int leadingSpaceCount(QStringView line)
{
    int count = 0;
    while (count < line.size() && line.at(count) == QLatin1Char(' '))
        ++count;
    return count;
}

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

/// Length of the quote markers ("> > ") at the start of a line.
int quotePrefixLength(const QString &text, int lineStart)
{
    int at = lineStart;
    const int end = lineEndOf(text, lineStart);
    for (;;) {
        int probe = at;
        while (probe < end && text.at(probe) == QLatin1Char(' ') && probe - at < 3)
            ++probe;
        if (probe < end && text.at(probe) == QLatin1Char('>')) {
            at = probe + 1;
            if (at < end && text.at(at) == QLatin1Char(' '))
                ++at;
        } else {
            break;
        }
    }
    return at - lineStart;
}

void numberLists(const QList<Block> &blocks, QList<int> &numbers, QList<int> &depths)
{
    numbers = QList<int>(blocks.size(), 1);
    depths = QList<int>(blocks.size(), 0);
    struct Level {
        int indent;
        int number;
        bool ordered;
    };
    QList<Level> stack;
    for (int i = 0; i < blocks.size(); ++i) {
        if (blocks[i].kind != BlockKind::ListItem) {
            stack.clear();
            continue;
        }
        const ListInfo info = listInfo(blocks[i].text);
        while (!stack.isEmpty() && stack.last().indent > info.indent)
            stack.removeLast();
        if (!stack.isEmpty() && stack.last().indent == info.indent && stack.last().ordered == info.ordered)
            ++stack.last().number;
        else if (!stack.isEmpty() && stack.last().indent == info.indent)
            stack.last() = {info.indent, info.number, info.ordered}; // another kind of list starts
        else
            stack.append({info.indent, info.number, info.ordered});
        numbers[i] = stack.last().number;
        depths[i] = int(stack.size()) - 1;
    }
}

QString tableText(const QList<QStringList> &rows, const QList<Align> &aligns, QList<QList<Segment>> *cells = nullptr)
{
    const int columns = aligns.size();
    QList<int> widths(columns, 3);
    for (const QStringList &row : rows) {
        for (int c = 0; c < columns && c < row.size(); ++c)
            widths[c] = std::max(widths[c], int(row[c].size()));
    }
    QString out;
    if (cells)
        cells->clear();
    const auto writeRow = [&](const QStringList &row) {
        QList<Segment> segments;
        out += QLatin1Char('|');
        for (int c = 0; c < columns; ++c) {
            const QString cell = c < row.size() ? row[c] : QString();
            out += QLatin1Char(' ');
            const int start = int(out.size());
            out += cell;
            segments.append({start, int(out.size())});
            out += spaces(widths[c] - int(cell.size()));
            out += QStringLiteral(" |");
        }
        if (cells)
            cells->append(segments);
    };
    for (int r = 0; r < rows.size(); ++r) {
        if (r > 0)
            out += QLatin1Char('\n');
        writeRow(rows[r]);
        if (r == 0) {
            out += QStringLiteral("\n|");
            for (int c = 0; c < columns; ++c) {
                const Align align = aligns[c];
                QString dashes(widths[c], QLatin1Char('-'));
                if (align == Align::Left || align == Align::Center)
                    dashes[0] = QLatin1Char(':');
                if (align == Align::Right || align == Align::Center)
                    dashes[dashes.size() - 1] = QLatin1Char(':');
                out += QLatin1Char(' ') + dashes + QStringLiteral(" |");
            }
        }
    }
    return out;
}

/// Where a pasted or dropped file goes in the Markdown: spaces would end the link.
QString markdownPath(QString path)
{
    path.replace(QLatin1Char(' '), QStringLiteral("%20"));
    return path;
}

bool isImageFile(const QString &path)
{
    static const QStringList suffixes = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                         QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("svg"),
                                         QStringLiteral("webp"), QStringLiteral("tif"), QStringLiteral("tiff"),
                                         QStringLiteral("ico")};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

} // namespace

MarkdownEditor::MarkdownEditor(QWidget *parent)
    : QAbstractScrollArea(parent)
    , m_theme(EditorTheme::github())
{
    m_images = new ImageStore(this);
    connect(m_images, &ImageStore::imageReady, this, [this] {
        ++m_imageGeneration;
        relayout();
    });
    setAttribute(Qt::WA_InputMethodEnabled);
    setFocusPolicy(Qt::StrongFocus);
    setFrameShape(QFrame::NoFrame);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setMouseTracking(true);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    verticalScrollBar()->setSingleStep(24);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_editTimer.start();
    m_lastClick.start();
    setMarkdown(QString());
}

MarkdownEditor::~MarkdownEditor() = default;

// ---- Content -----------------------------------------------------------------

void MarkdownEditor::setMarkdown(const QString &markdown)
{
    QString text = markdown;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (text.startsWith(QChar(0xfeff)))
        text.remove(0, 1);
    m_blocks = blocksFromMarkdown(text);
    m_renders.clear();
    m_undo.clear();
    m_redo.clear();
    m_lastEdit = EditKind::None;
    m_preedit.clear();
    collectReferences();
    m_caret = m_anchor = firstPos(0);
    m_revision = m_savedRevision = Block::nextRevision();
    m_wasModified = false;
    relayout();
    verticalScrollBar()->setValue(0);
    emit contentChanged();
    emit modificationChanged(false);
    emit caretMoved();
}

QString MarkdownEditor::markdown() const
{
    return serializeBlocks(m_blocks);
}

void MarkdownEditor::markSaved()
{
    m_savedRevision = m_revision;
    m_lastEdit = EditKind::None;
    if (m_wasModified) {
        m_wasModified = false;
        emit modificationChanged(false);
    }
}

void MarkdownEditor::setBaseDir(const QString &dir)
{
    if (m_baseDir == dir)
        return;
    m_baseDir = dir;
    ++m_themeGeneration;
    relayout();
}

void MarkdownEditor::setTheme(const EditorTheme &theme)
{
    m_theme = theme;
    ++m_themeGeneration;
    QPalette palette = viewport()->palette();
    palette.setColor(QPalette::Base, theme.background);
    palette.setColor(QPalette::Window, theme.background);
    viewport()->setPalette(palette);
    relayout();
}

void MarkdownEditor::setZoom(qreal zoom)
{
    zoom = std::clamp(zoom, 0.5, 3.0);
    if (qFuzzyCompare(zoom, m_zoom))
        return;
    m_zoom = zoom;
    ++m_themeGeneration;
    relayout();
    ensureCaretVisible();
}

void MarkdownEditor::setFocusMode(bool on)
{
    m_focusMode = on;
    viewport()->update();
}

void MarkdownEditor::setTypewriterMode(bool on)
{
    m_typewriter = on;
    ensureCaretVisible();
}

void MarkdownEditor::collectReferences()
{
    static const QRegularExpression definition(QStringLiteral("^ {0,3}\\[([^\\]^][^\\]]*)\\]:\\s*<?([^\\s>]+)>?"));
    LinkReferences references;
    for (const Block &block : std::as_const(m_blocks)) {
        if (block.kind != BlockKind::Paragraph || !block.text.contains(QLatin1String("]:")))
            continue;
        for (const QString &line : block.text.split(QLatin1Char('\n'))) {
            const QRegularExpressionMatch match = definition.match(line);
            if (match.hasMatch())
                references.insert(referenceKey(match.captured(1)), match.captured(2));
        }
    }
    if (references != m_references) {
        m_references = references;
        clearInlineCache();
        ++m_themeGeneration;
    }
}

QList<MarkdownEditor::Heading> MarkdownEditor::headingsOf(const QList<Block> &blocks)
{
    QList<Heading> out;
    for (int i = 0; i < blocks.size(); ++i) {
        const Block &block = blocks[i];
        if (block.kind != BlockKind::Heading)
            continue;
        const ContentMap map = contentMap(block);
        Heading heading;
        heading.block = i;
        heading.level = headingLevel(block);
        heading.title = plainText(map.text, cachedInline(map.text, nullptr)).simplified();
        out.append(heading);
    }
    return out;
}

void MarkdownEditor::goToBlock(int block)
{
    if (block < 0 || block >= m_blocks.size())
        return;
    setCaret(lastPos(block));
    verticalScrollBar()->setValue(int(m_ys.value(block) - 16 * m_zoom));
    setFocus();
}

int MarkdownEditor::caretOffset() const
{
    QList<qsizetype> offsets;
    serializeBlocks(m_blocks, &offsets);
    return int(offsets.value(m_caret.block)) + m_caret.offset;
}

void MarkdownEditor::setCaretOffset(int offset)
{
    QList<qsizetype> offsets;
    serializeBlocks(m_blocks, &offsets);
    int block = 0;
    for (int i = 0; i < offsets.size(); ++i) {
        if (offsets[i] <= offset)
            block = i;
    }
    const int within = std::clamp(offset - int(offsets.value(block)), 0, int(m_blocks[block].text.size()));
    setCaret(clamp({block, within}));
    if (m_ys.size() > block) {
        const qreal y = m_ys[block];
        verticalScrollBar()->setValue(int(y - viewport()->height() / 3));
    }
}

void MarkdownEditor::setCaret(Pos caret, Pos anchor)
{
    m_caret = clamp(caret);
    m_anchor = clamp(anchor);
    m_preferredX = -1;
    updateCaret();
}

std::pair<Pos, Pos> MarkdownEditor::selectionRange() const
{
    return m_caret < m_anchor ? std::pair{m_caret, m_anchor} : std::pair{m_anchor, m_caret};
}

QString MarkdownEditor::selectedMarkdown() const
{
    if (!hasSelection())
        return {};
    const auto [from, to] = selectionRange();
    if (from.block == to.block)
        return m_blocks[from.block].text.mid(from.offset, to.offset - from.offset);
    QList<Block> parts;
    for (int i = from.block; i <= to.block; ++i) {
        Block part = m_blocks[i];
        if (i == to.block)
            part.text = part.text.left(to.offset);
        if (i == from.block)
            part.text = part.text.mid(from.offset);
        parts.append(part);
    }
    QString out = serializeBlocks(parts);
    if (out.endsWith(QLatin1Char('\n')))
        out.chop(1);
    return out;
}

// ---- Layout --------------------------------------------------------------------

qreal MarkdownEditor::columnWidth() const
{
    const qreal pad = m_theme.padding * m_zoom;
    // Typora's themes widen the column on wide windows.
    const int window = this->window() ? this->window()->width() : viewport()->width();
    qreal maxWidth = m_theme.maxWidth;
    if (window >= 1800)
        maxWidth = std::max<qreal>(maxWidth, 1200);
    else if (window >= 1400)
        maxWidth = std::max<qreal>(maxWidth, 1024);
    const qreal widest = maxWidth * m_zoom - 2 * pad;
    return std::max<qreal>(160, std::min(widest, viewport()->width() - 2 * pad));
}

qreal MarkdownEditor::columnLeft() const
{
    return std::floor((viewport()->width() - columnWidth()) / 2.0);
}

void MarkdownEditor::computeListNumbers()
{
    numberLists(m_blocks, m_listNumbers, m_listDepths);
}

MarkdownEditor::SlotKey MarkdownEditor::slotKey(int index) const
{
    SlotKey key;
    key.width = columnWidth();
    key.zoom = m_zoom;
    key.theme = m_themeGeneration;
    key.listNumber = m_listNumbers.value(index);
    key.listDepth = m_listDepths.value(index);
    if (isTableOfContents(m_blocks[index])) {
        QString headings;
        for (const TocEntry &entry : m_toc)
            headings += QString::number(entry.level) + entry.title + QLatin1Char('\n');
        key.reveal = QStringLiteral("toc:%1|").arg(qHash(headings));
    }
    if (index == m_caret.block) {
        key.reveal += revealKey(m_blocks[index], m_caret.offset, &m_references);
        if (!m_preedit.isEmpty())
            key.reveal += QStringLiteral("|ime:%1:%2:%3").arg(m_caret.offset).arg(m_preeditCursor).arg(m_preedit);
    }
    return key;
}

void MarkdownEditor::ensureRendered(int index)
{
    const Block &block = m_blocks[index];
    Slot &slot = m_renders[block.revision];
    const SlotKey key = slotKey(index);
    if (slot.render && slot.key == key && !(slot.render->hasImages && slot.imageGeneration != m_imageGeneration))
        return;
    RenderContext context;
    context.theme = &m_theme;
    context.width = columnWidth();
    context.zoom = m_zoom;
    context.caret = index == m_caret.block ? m_caret.offset : -1;
    context.listNumber = m_listNumbers.value(index, 1);
    context.listDepth = m_listDepths.value(index, 0);
    context.baseDir = m_baseDir;
    context.references = &m_references;
    context.images = m_images;
    context.toc = &m_toc;
    slot.render = renderBlock(block, context);
    slot.key = key;
    slot.imageGeneration = m_imageGeneration;
    if (index == m_caret.block && !m_preedit.isEmpty())
        applyPreedit(slot.render.get());
}

void MarkdownEditor::applyPreedit(RenderedBlock *render)
{
    const int position = render->toDoc(m_caret.offset);
    QTextBlock block = render->doc->findBlock(position);
    if (!block.isValid() || !block.layout())
        return;
    QTextLayout *layout = block.layout();
    layout->setPreeditArea(position - block.position(), m_preedit);
    QTextLayout::FormatRange underline;
    underline.start = position - block.position();
    underline.length = int(m_preedit.size());
    underline.format.setFontUnderline(true);
    underline.format.setUnderlineStyle(QTextCharFormat::SingleUnderline);
    layout->setFormats({underline});
    render->doc->markContentsDirty(block.position(), block.length());
    render->height = std::ceil(render->docOffset.y() + render->docHeight() + render->bottomPad);
}

RenderedBlock *MarkdownEditor::render(int index)
{
    ensureRendered(index);
    return m_renders[m_blocks[index].revision].render.get();
}

qreal MarkdownEditor::gapBetween(const Block &a, const RenderedBlock &ra, const Block &b, const RenderedBlock &rb,
                                 qreal zoom)
{
    if (a.kind == BlockKind::ListItem && b.kind == BlockKind::ListItem && a.blankAfter == 0)
        return std::round(8 * zoom);
    return std::max(ra.marginBottom, rb.marginTop);
}

qreal MarkdownEditor::gapBefore(int index) const
{
    if (index <= 0)
        return 0;
    const RenderedBlock *a = m_renders.value(m_blocks[index - 1].revision).render.get();
    const RenderedBlock *b = m_renders.value(m_blocks[index].revision).render.get();
    if (!a || !b)
        return 0;
    return gapBetween(m_blocks[index - 1], *a, m_blocks[index], *b, m_zoom);
}

void MarkdownEditor::relayout()
{
    computeListNumbers();
    // "[TOC]" paragraphs show the headings, so they follow them.
    bool hasToc = false;
    for (const Block &block : std::as_const(m_blocks))
        hasToc = hasToc || isTableOfContents(block);
    m_toc.clear();
    if (hasToc) {
        for (const Heading &heading : headingsOf(m_blocks))
            m_toc.append({heading.level, heading.title});
    }
    QHash<quint64, Slot> kept;
    kept.reserve(m_blocks.size());
    for (const Block &block : std::as_const(m_blocks)) {
        const auto found = m_renders.constFind(block.revision);
        if (found != m_renders.constEnd())
            kept.insert(block.revision, *found);
    }
    m_renders = std::move(kept);

    m_ys.resize(m_blocks.size());
    qreal y = std::round((m_theme.padding + 20) * m_zoom);
    for (int i = 0; i < m_blocks.size(); ++i) {
        ensureRendered(i);
        y += gapBefore(i);
        m_ys[i] = y;
        y += m_renders[m_blocks[i].revision].render->height;
    }
    m_contentHeight = y + std::round(100 * m_zoom);
    QScrollBar *bar = verticalScrollBar();
    const int extra = m_typewriter ? viewport()->height() / 2 : 0;
    bar->setRange(0, std::max(0, int(std::ceil(m_contentHeight)) - viewport()->height() + extra));
    bar->setPageStep(viewport()->height());
    viewport()->update();
}

int MarkdownEditor::blockAtY(qreal contentY) const
{
    if (m_ys.isEmpty())
        return 0;
    const auto it = std::upper_bound(m_ys.cbegin(), m_ys.cend(), contentY);
    int index = std::max(0, int(it - m_ys.cbegin()) - 1);
    // In the gap after a block, the nearer block wins.
    if (index + 1 < m_ys.size()) {
        const RenderedBlock *current = m_renders.value(m_blocks[index].revision).render.get();
        const qreal bottom = m_ys[index] + (current ? current->height : 0);
        if (contentY > bottom && contentY - bottom > m_ys[index + 1] - contentY)
            ++index;
    }
    return index;
}

Pos MarkdownEditor::positionAt(const QPoint &viewportPoint)
{
    const qreal contentY = viewportPoint.y() + verticalScrollBar()->value();
    const int index = blockAtY(contentY);
    RenderedBlock *r = render(index);
    const QPointF local(viewportPoint.x() - columnLeft(), contentY - m_ys[index]);
    return clamp({index, r->hitTest(local)});
}

QRectF MarkdownEditor::caretRectInViewport() const
{
    const auto found = m_renders.constFind(m_blocks.value(m_caret.block).revision);
    if (found == m_renders.constEnd() || !found->render || m_caret.block >= m_ys.size())
        return {};
    const QRectF rect = found->render->caretRect(m_caret.offset, m_preedit.isEmpty() ? 0 : m_preeditCursor);
    return rect.translated(columnLeft(), m_ys[m_caret.block] - verticalScrollBar()->value());
}

void MarkdownEditor::ensureCaretVisible()
{
    const QRectF rect = caretRectInViewport();
    if (rect.isNull())
        return;
    QScrollBar *bar = verticalScrollBar();
    if (m_typewriter) {
        bar->setValue(int(bar->value() + rect.center().y() - viewport()->height() / 2.0));
        return;
    }
    const int margin = int(24 * m_zoom);
    if (rect.top() < margin)
        bar->setValue(int(bar->value() + rect.top() - margin));
    else if (rect.bottom() > viewport()->height() - margin)
        bar->setValue(int(bar->value() + rect.bottom() - viewport()->height() + margin));
}

void MarkdownEditor::updateCaret()
{
    relayout();
    ensureCaretVisible();
    m_caretVisible = true;
    if (hasFocus())
        m_blink.start(QApplication::cursorFlashTime() > 0 ? QApplication::cursorFlashTime() / 2 : 530, this);
    viewport()->update();
    emit caretMoved();
    if (QInputMethod *im = QGuiApplication::inputMethod(); im && hasFocus())
        im->update(Qt::ImCursorRectangle);
}

// ---- Positions ---------------------------------------------------------------------

Pos MarkdownEditor::clamp(Pos pos, bool forward) const
{
    pos.block = std::clamp(pos.block, 0, int(m_blocks.size()) - 1);
    const QList<Segment> segments = caretSegments(m_blocks[pos.block]);
    for (const Segment &segment : segments) {
        if (pos.offset >= segment.start && pos.offset <= segment.end)
            return pos;
    }
    int best = segments.first().start;
    int distance = std::numeric_limits<int>::max();
    for (const Segment &segment : segments) {
        for (const int edge : {segment.start, segment.end}) {
            const int d = std::abs(edge - pos.offset);
            if (d < distance || (d == distance && forward == (edge > pos.offset))) {
                distance = d;
                best = edge;
            }
        }
    }
    pos.offset = best;
    return pos;
}

Pos MarkdownEditor::firstPos(int block) const
{
    block = std::clamp(block, 0, int(m_blocks.size()) - 1);
    return {block, caretSegments(m_blocks[block]).first().start};
}

Pos MarkdownEditor::lastPos(int block) const
{
    block = std::clamp(block, 0, int(m_blocks.size()) - 1);
    return {block, caretSegments(m_blocks[block]).last().end};
}

Pos MarkdownEditor::nextChar(Pos pos) const
{
    const Block &block = m_blocks[pos.block];
    const QList<Segment> segments = caretSegments(block);
    for (int i = 0; i < segments.size(); ++i) {
        const Segment &segment = segments[i];
        if (pos.offset < segment.start || pos.offset > segment.end)
            continue;
        if (pos.offset < segment.end) {
            int step = 1;
            if (block.text.at(pos.offset).isHighSurrogate() && pos.offset + 1 < segment.end)
                step = 2;
            return {pos.block, pos.offset + step};
        }
        if (i + 1 < segments.size())
            return {pos.block, segments[i + 1].start};
        break;
    }
    if (pos.block + 1 < m_blocks.size())
        return firstPos(pos.block + 1);
    return pos;
}

Pos MarkdownEditor::previousChar(Pos pos) const
{
    const Block &block = m_blocks[pos.block];
    const QList<Segment> segments = caretSegments(block);
    for (int i = int(segments.size()) - 1; i >= 0; --i) {
        const Segment &segment = segments[i];
        if (pos.offset < segment.start || pos.offset > segment.end)
            continue;
        if (pos.offset > segment.start) {
            int step = 1;
            if (block.text.at(pos.offset - 1).isLowSurrogate() && pos.offset - 2 >= segment.start)
                step = 2;
            return {pos.block, pos.offset - step};
        }
        if (i > 0)
            return {pos.block, segments[i - 1].end};
        break;
    }
    if (pos.block > 0)
        return lastPos(pos.block - 1);
    return pos;
}

Pos MarkdownEditor::nextWord(Pos pos) const
{
    const QString &text = m_blocks[pos.block].text;
    const Pos end = lastPos(pos.block);
    if (pos.offset >= end.offset)
        return nextChar(pos);
    int at = pos.offset;
    while (at < end.offset && !isWordChar(text.at(at)))
        ++at;
    while (at < end.offset && isWordChar(text.at(at)))
        ++at;
    return clamp({pos.block, at}, true);
}

Pos MarkdownEditor::previousWord(Pos pos) const
{
    const QString &text = m_blocks[pos.block].text;
    const Pos start = firstPos(pos.block);
    if (pos.offset <= start.offset)
        return previousChar(pos);
    int at = pos.offset;
    while (at > start.offset && !isWordChar(text.at(at - 1)))
        --at;
    while (at > start.offset && isWordChar(text.at(at - 1)))
        --at;
    return clamp({pos.block, at}, false);
}

Pos MarkdownEditor::lineStart(Pos pos)
{
    const Segment line = render(pos.block)->lineAt(pos.offset);
    return clamp({pos.block, line.start}, true);
}

Pos MarkdownEditor::lineEnd(Pos pos)
{
    const Segment line = render(pos.block)->lineAt(pos.offset);
    return clamp({pos.block, line.end}, false);
}

bool MarkdownEditor::verticalTarget(Pos from, int direction, Pos &to)
{
    RenderedBlock *r = render(from.block);
    const QRectF line = r->lineRect(from.offset);
    if (m_preferredX < 0)
        m_preferredX = r->caretRect(from.offset).left();
    const qreal x = m_preferredX;
    const qreal top = r->docOffset.y();
    const qreal bottom = r->docOffset.y() + r->docHeight();
    for (const qreal step : {1.0, line.height() / 2.0, line.height()}) {
        const qreal y = direction < 0 ? line.top() - step : line.bottom() + step;
        if (y < top || y > bottom)
            break;
        const Pos candidate = clamp({from.block, r->hitTest(QPointF(x, y))}, direction > 0);
        if (candidate != from && std::abs(r->lineRect(candidate.offset).top() - line.top()) > 0.5) {
            to = candidate;
            return true;
        }
    }
    int next = from.block + direction;
    while (next >= 0 && next < m_blocks.size() && m_blocks[next].kind == BlockKind::Rule)
        next += direction;
    if (next < 0 || next >= m_blocks.size()) {
        to = direction < 0 ? docStart() : docEnd();
        return to != from;
    }
    RenderedBlock *target = render(next);
    const qreal y = direction < 0 ? target->docOffset.y() + target->docHeight() - 2 : target->docOffset.y() + 2;
    to = clamp({next, target->hitTest(QPointF(x, y))}, direction > 0);
    return true;
}

void MarkdownEditor::moveCaret(Pos to, bool extend)
{
    m_caret = to;
    if (!extend)
        m_anchor = to;
    updateCaret();
}

// ---- Editing primitives -----------------------------------------------------------

MarkdownEditor::Snapshot MarkdownEditor::snapshot() const
{
    return {m_blocks, m_caret, m_anchor, m_revision};
}

void MarkdownEditor::restore(const Snapshot &state)
{
    m_blocks = state.blocks;
    m_caret = clamp(state.caret);
    m_anchor = clamp(state.anchor);
    m_revision = state.revision;
    m_preferredX = -1;
    collectReferences();
    relayout();
    ensureCaretVisible();
    emit contentChanged();
    if (isModified() != m_wasModified) {
        m_wasModified = isModified();
        emit modificationChanged(m_wasModified);
    }
    emit caretMoved();
}

void MarkdownEditor::beginEdit(EditKind kind)
{
    const bool merge = kind != EditKind::Other && kind == m_lastEdit && m_caret == m_lastEditCaret
        && m_caret == m_anchor && m_editTimer.elapsed() < 1500;
    if (!merge) {
        m_undo.append(snapshot());
        while (m_undo.size() > 500)
            m_undo.removeFirst();
    }
    m_redo.clear();
    m_lastEdit = kind;
    m_editTimer.restart();
    m_editing = true;
}

void MarkdownEditor::endEdit()
{
    m_editing = false;
    m_revision = Block::nextRevision();
    if (m_blocks.isEmpty()) {
        Block empty;
        empty.revision = Block::nextRevision();
        m_blocks.append(empty);
    }
    m_caret = clamp(m_caret);
    m_anchor = clamp(m_anchor);
    m_lastEditCaret = m_caret;
    m_preferredX = -1;
    collectReferences();
    relayout();
    ensureCaretVisible();
    m_caretVisible = true;
    viewport()->update();
    emit contentChanged();
    if (isModified() != m_wasModified) {
        m_wasModified = isModified();
        emit modificationChanged(m_wasModified);
    }
    emit caretMoved();
    if (QInputMethod *im = QGuiApplication::inputMethod(); im && hasFocus())
        im->update(Qt::ImCursorRectangle | Qt::ImSurroundingText);
}

void MarkdownEditor::touch(Block &block)
{
    block.revision = Block::nextRevision();
}

void MarkdownEditor::reparse(int index)
{
    Block &block = m_blocks[index];
    const QList<ParsedBlock> parsed = parseBlocks(block.text);
    if (parsed.size() <= 1) {
        const BlockKind kind = parsed.isEmpty() ? BlockKind::Paragraph : parsed.first().block.kind;
        if (kind != block.kind) {
            block.kind = kind;
            touch(block);
        }
        return;
    }
    QList<Block> pieces;
    for (const ParsedBlock &piece : parsed) {
        Block b = piece.block;
        b.revision = Block::nextRevision();
        pieces.append(b);
    }
    pieces.last().blankAfter = block.blankAfter;
    const auto map = [&](Pos &pos) {
        if (pos.block > index) {
            pos.block += int(pieces.size()) - 1;
            return;
        }
        if (pos.block < index)
            return;
        const int offset = pos.offset;
        for (int j = 0; j < parsed.size(); ++j) {
            const int start = int(parsed[j].start);
            const int end = start + int(parsed[j].block.text.size());
            if (offset <= end) {
                pos = {index + j, std::max(0, offset - start)};
                return;
            }
        }
        pos = {index + int(parsed.size()) - 1, int(parsed.last().block.text.size())};
    };
    map(m_caret);
    map(m_anchor);
    m_blocks.removeAt(index);
    for (int j = 0; j < pieces.size(); ++j)
        m_blocks.insert(index + j, pieces[j]);
}

void MarkdownEditor::removeBlock(int index)
{
    m_blocks.removeAt(index);
    for (Pos *pos : {&m_caret, &m_anchor}) {
        if (pos->block > index)
            --pos->block;
        else if (pos->block == index)
            pos->offset = 0;
    }
    if (m_blocks.isEmpty()) {
        Block empty;
        empty.revision = Block::nextRevision();
        m_blocks.append(empty);
    }
}

void MarkdownEditor::insertBlockAt(int index, const Block &block)
{
    Block b = block;
    b.revision = Block::nextRevision();
    m_blocks.insert(index, b);
    for (Pos *pos : {&m_caret, &m_anchor}) {
        if (pos->block >= index)
            ++pos->block;
    }
}

void MarkdownEditor::replaceRange(Pos from, Pos to, const QString &text)
{
    if (to < from)
        std::swap(from, to);
    Block &first = m_blocks[from.block];
    const Block last = m_blocks[to.block];
    first.text = first.text.left(from.offset) + text + last.text.mid(to.offset);
    first.blankAfter = last.blankAfter;
    touch(first);
    for (int i = to.block; i > from.block; --i)
        m_blocks.removeAt(i);
    m_caret = m_anchor = {from.block, from.offset + int(text.size())};
    reparse(from.block);
    m_caret = clamp(m_caret);
    m_anchor = m_caret;
}

void MarkdownEditor::insertText(const QString &input)
{
    if (input.isEmpty())
        return;
    // Typing the closing bracket of a pair steps over it.
    if (!hasSelection() && input.size() == 1
        && (input == QLatin1String(")") || input == QLatin1String("]") || input == QLatin1String("}"))) {
        const QString &current = m_blocks[m_caret.block].text;
        if (m_caret.offset < current.size() && current.at(m_caret.offset) == input.at(0)
            && m_lastEdit == EditKind::Typing && m_caret == m_lastEditCaret) {
            m_caret.offset += 1;
            m_anchor = m_caret;
            m_lastEditCaret = m_caret;
            updateCaret();
            return;
        }
    }
    beginEdit(EditKind::Typing);
    if (hasSelection()) {
        const auto [from, to] = selectionRange();
        replaceRange(from, to, QString());
    }
    Block &block = m_blocks[m_caret.block];
    QString text = input;
    if (block.kind == BlockKind::Table) {
        text.replace(QLatin1Char('\n'), QLatin1Char(' '));
        text.replace(QLatin1Char('|'), QStringLiteral("\\|"));
    }
    // Brackets come in pairs, as in Typora, when nothing follows the caret.
    static const QHash<QChar, QChar> pairs = {{QLatin1Char('('), QLatin1Char(')')},
                                              {QLatin1Char('['), QLatin1Char(']')},
                                              {QLatin1Char('{'), QLatin1Char('}')}};
    const int offset = m_caret.offset;
    const QChar next = offset < block.text.size() ? block.text.at(offset) : QChar(QLatin1Char(' '));
    QString closing;
    // Not where a task box is being typed ("- [ ]").
    static const QRegularExpression listStart(QStringLiteral("^\\s*([-+*]|\\d+[.)])\\s+$"));
    const bool taskBox = text == QLatin1String("[") && listStart.match(block.text.left(offset)).hasMatch();
    if (text.size() == 1 && pairs.contains(text.at(0)) && (next.isSpace() || next == QLatin1Char(')')
                                                           || next == QLatin1Char(']')) && !isCodeLike(block.kind)
        && !taskBox)
        closing = pairs.value(text.at(0));
    block.text.insert(offset, text + closing);
    touch(block);
    m_caret.offset += int(text.size());
    m_anchor = m_caret;
    reparse(m_caret.block);
    endEdit();
}

void MarkdownEditor::typeText(const QString &text)
{
    for (const QChar c : text) {
        if (c == QLatin1Char('\n'))
            enter();
        else
            insertText(QString(c));
    }
}

void MarkdownEditor::deleteSelection()
{
    if (!hasSelection())
        return;
    beginEdit(EditKind::Other);
    const auto [from, to] = selectionRange();
    replaceRange(from, to, QString());
    endEdit();
}

int MarkdownEditor::parentIndent(int index) const
{
    const ListInfo info = listInfo(m_blocks[index].text);
    for (int j = index - 1; j >= 0 && m_blocks[j].kind == BlockKind::ListItem; --j) {
        const ListInfo other = listInfo(m_blocks[j].text);
        if (other.indent < info.indent)
            return other.indent;
    }
    return 0;
}

void MarkdownEditor::setListIndent(int index, int indent)
{
    Block &block = m_blocks[index];
    const ListInfo info = listInfo(block.text);
    const int delta = indent - info.indent;
    if (delta == 0)
        return;
    QStringList lines = block.text.split(QLatin1Char('\n'));
    int start = 0;
    for (int i = 0; i < lines.size(); ++i) {
        QString &line = lines[i];
        const int lead = leadingSpaceCount(line);
        int change = delta;
        if (delta > 0)
            line.prepend(spaces(delta));
        else {
            change = -std::min(lead, -delta);
            line.remove(0, -change);
        }
        for (Pos *pos : {&m_caret, &m_anchor}) {
            if (pos->block == index && pos->offset >= start && pos->offset <= start + line.size() - change)
                pos->offset = std::max(start, pos->offset + change);
        }
        start += int(line.size()) + 1;
    }
    block.text = lines.join(QLatin1Char('\n'));
    touch(block);
}

void MarkdownEditor::backspace(bool word)
{
    if (hasSelection()) {
        deleteSelection();
        return;
    }
    const int index = m_caret.block;
    Block &block = m_blocks[index];
    const QList<Segment> segments = caretSegments(block);
    int segmentIndex = 0;
    for (int i = 0; i < segments.size(); ++i) {
        if (m_caret.offset >= segments[i].start && m_caret.offset <= segments[i].end) {
            segmentIndex = i;
            break;
        }
    }
    const Segment segment = segments[segmentIndex];
    const int offset = m_caret.offset;

    if (offset > segment.start) {
        int from = offset - 1;
        if (word)
            from = std::max(segment.start, previousWord(m_caret).offset);
        else if (from > segment.start && block.text.at(from).isLowSurrogate())
            --from;
        beginEdit(EditKind::Deleting);
        // An empty pair of brackets goes together.
        if (!word && from + 2 <= block.text.size() && offset < block.text.size()) {
            const QStringView pair = QStringView(block.text).mid(from, 2);
            if (pair == QLatin1String("()") || pair == QLatin1String("[]") || pair == QLatin1String("{}"))
                block.text.remove(from + 1, 1);
        }
        block.text.remove(from, offset - from);
        touch(block);
        m_caret = m_anchor = {index, from};
        reparse(index);
        endEdit();
        return;
    }
    if (segmentIndex > 0) {
        if (block.kind == BlockKind::Table) {
            moveCaret({index, segments[segmentIndex - 1].end}, false);
            return;
        }
        beginEdit(EditKind::Deleting);
        const int joinAt = segments[segmentIndex - 1].end;
        block.text.remove(joinAt, segment.start - joinAt);
        touch(block);
        m_caret = m_anchor = {index, joinAt};
        reparse(index);
        endEdit();
        return;
    }

    // At the very start of the block.
    switch (block.kind) {
    case BlockKind::ListItem: {
        beginEdit(EditKind::Other);
        const ListInfo info = listInfo(block.text);
        if (info.task) {
            block.text.remove(info.taskStart, info.contentStart - info.taskStart);
            m_caret = m_anchor = {index, info.taskStart};
        } else if (info.indent > 0) {
            setListIndent(index, parentIndent(index));
        } else {
            block.text = contentMap(block).text;
            block.kind = BlockKind::Paragraph;
            if (index > 0 && m_blocks[index - 1].kind == BlockKind::ListItem)
                m_blocks[index - 1].blankAfter = std::max(1, m_blocks[index - 1].blankAfter);
            m_caret = m_anchor = {index, 0};
        }
        touch(block);
        reparse(index);
        endEdit();
        return;
    }
    case BlockKind::Quote: {
        beginEdit(EditKind::Other);
        QStringList lines = block.text.split(QLatin1Char('\n'));
        for (QString &line : lines) {
            const qsizetype at = line.indexOf(QLatin1Char('>'));
            if (at >= 0 && line.left(at).trimmed().isEmpty()) {
                line.remove(0, at + 1);
                if (line.startsWith(QLatin1Char(' ')))
                    line.remove(0, 1);
            }
        }
        block.text = lines.join(QLatin1Char('\n'));
        touch(block);
        m_caret = m_anchor = {index, 0};
        reparse(index);
        endEdit();
        return;
    }
    case BlockKind::CodeFence:
    case BlockKind::IndentedCode:
    case BlockKind::MathBlock:
    case BlockKind::FrontMatter:
    case BlockKind::Html:
    case BlockKind::Table: {
        bool empty = block.text.trimmed().isEmpty();
        if (block.kind == BlockKind::CodeFence) {
            const FenceInfo info = fenceInfo(block.text);
            empty = block.text.mid(info.code.start, info.code.end - info.code.start).trimmed().isEmpty()
                && info.language.isEmpty();
        } else if (block.kind == BlockKind::MathBlock) {
            const MathInfo info = mathInfo(block.text);
            empty = block.text.mid(info.latex.start, info.latex.end - info.latex.start).trimmed().isEmpty();
        }
        if (empty) {
            beginEdit(EditKind::Other);
            block.text.clear();
            block.kind = BlockKind::Paragraph;
            touch(block);
            m_caret = m_anchor = {index, 0};
            endEdit();
        } else if (index > 0) {
            moveCaret(lastPos(index - 1), false);
        }
        return;
    }
    case BlockKind::Rule: {
        beginEdit(EditKind::Other);
        removeBlock(index);
        m_caret = m_anchor = index > 0 ? lastPos(index - 1) : firstPos(0);
        endEdit();
        return;
    }
    case BlockKind::Heading:
        // At the start of a heading: it becomes a paragraph.
        setHeading(0);
        return;
    case BlockKind::Paragraph:
        break;
    }

    if (index == 0)
        return;
    const Block &previous = m_blocks[index - 1];
    beginEdit(EditKind::Other);
    if (previous.kind == BlockKind::Rule) {
        removeBlock(index - 1);
    } else if (block.text.isEmpty()) {
        removeBlock(index);
        m_caret = m_anchor = lastPos(index - 1);
    } else if (previous.kind == BlockKind::Paragraph && previous.text.trimmed().isEmpty()) {
        removeBlock(index - 1);
    } else if (isTextKind(previous.kind)) {
        Block &target = m_blocks[index - 1];
        const int joinAt = int(target.text.size());
        QString tail = block.text;
        if (block.kind == BlockKind::Heading)
            tail = block.text.mid(contentMap(block).sourceOf.first());
        target.text += tail;
        target.blankAfter = block.blankAfter;
        touch(target);
        removeBlock(index);
        m_caret = m_anchor = {index - 1, joinAt};
        reparse(index - 1);
    } else {
        m_undo.removeLast();
        m_editing = false;
        moveCaret(lastPos(index - 1), false);
        return;
    }
    endEdit();
}

void MarkdownEditor::deleteForward(bool word)
{
    if (hasSelection()) {
        deleteSelection();
        return;
    }
    const int index = m_caret.block;
    Block &block = m_blocks[index];
    const QList<Segment> segments = caretSegments(block);
    int segmentIndex = int(segments.size()) - 1;
    for (int i = 0; i < segments.size(); ++i) {
        if (m_caret.offset >= segments[i].start && m_caret.offset <= segments[i].end) {
            segmentIndex = i;
            break;
        }
    }
    const Segment segment = segments[segmentIndex];
    const int offset = m_caret.offset;
    if (offset < segment.end) {
        int to = offset + 1;
        if (word)
            to = std::min(segment.end, nextWord(m_caret).offset);
        else if (to < segment.end && block.text.at(offset).isHighSurrogate())
            ++to;
        beginEdit(EditKind::Deleting);
        block.text.remove(offset, to - offset);
        touch(block);
        reparse(index);
        endEdit();
        return;
    }
    if (segmentIndex + 1 < segments.size()) {
        if (block.kind == BlockKind::Table)
            return;
        beginEdit(EditKind::Deleting);
        block.text.remove(offset, segments[segmentIndex + 1].start - offset);
        touch(block);
        reparse(index);
        endEdit();
        return;
    }
    if (index + 1 >= m_blocks.size())
        return;
    const Block &next = m_blocks[index + 1];
    if (next.kind == BlockKind::Rule) {
        beginEdit(EditKind::Other);
        removeBlock(index + 1);
        endEdit();
    } else if (block.kind == BlockKind::Paragraph && block.text.isEmpty()) {
        beginEdit(EditKind::Other);
        removeBlock(index);
        m_caret = m_anchor = firstPos(index);
        endEdit();
    } else if (isTextKind(block.kind) && isTextKind(next.kind)) {
        beginEdit(EditKind::Other);
        const int from = firstPos(index + 1).offset;
        block.text += next.text.mid(from);
        block.blankAfter = next.blankAfter;
        touch(block);
        removeBlock(index + 1);
        m_caret = m_anchor = {index, offset};
        reparse(index);
        endEdit();
    }
}

void MarkdownEditor::placeNewBlock(const Block &block, int caretOffset)
{
    const int index = m_caret.block;
    Block &current = m_blocks[index];
    int target = index + 1;
    if (current.kind == BlockKind::Paragraph && current.text.trimmed().isEmpty()) {
        const int blank = current.blankAfter;
        current = block;
        current.blankAfter = blank;
        touch(current);
        target = index;
    } else {
        Block b = block;
        b.blankAfter = current.blankAfter > 0 ? current.blankAfter : 1;
        current.blankAfter = 1;
        insertBlockAt(target, b);
    }
    m_caret = m_anchor = {target, caretOffset};
}

void MarkdownEditor::enter()
{
    if (hasSelection()) {
        beginEdit(EditKind::Other);
        const auto [from, to] = selectionRange();
        replaceRange(from, to, QString());
    } else {
        beginEdit(EditKind::Other);
    }
    const int index = m_caret.block;
    Block &block = m_blocks[index];
    const QString text = block.text;
    const int offset = m_caret.offset;

    switch (block.kind) {
    case BlockKind::Paragraph: {
        static const QRegularExpression fence(QStringLiteral("^ {0,3}(`{3,}|~{3,})([^`\\n]*)$"));
        const QRegularExpressionMatch fenced = fence.match(text);
        if (offset == text.size() && fenced.hasMatch()) {
            block.text = text + QStringLiteral("\n\n") + fenced.captured(1);
            block.kind = BlockKind::CodeFence;
            touch(block);
            m_caret = m_anchor = {index, int(text.size()) + 1};
            break;
        }
        if (offset == text.size() && text.trimmed() == QLatin1String("$$")) {
            block.text = QStringLiteral("$$\n\n$$");
            block.kind = BlockKind::MathBlock;
            touch(block);
            m_caret = m_anchor = {index, 3};
            break;
        }
        const QString trimmed = text.trimmed();
        if (offset == text.size() && !text.contains(QLatin1Char('\n')) && trimmed.startsWith(QLatin1Char('|'))
            && trimmed.endsWith(QLatin1Char('|')) && trimmed.size() > 2) {
            const QStringList header = splitTableRow(trimmed);
            QList<Align> aligns(header.size(), Align::None);
            QList<QList<Segment>> cells;
            block.text = tableText({header, QStringList(header.size(), QString())}, aligns, &cells);
            block.kind = BlockKind::Table;
            touch(block);
            m_caret = m_anchor = {index, cells[1][0].start};
            break;
        }
        Block tail;
        tail.kind = BlockKind::Paragraph;
        tail.text = text.mid(offset);
        tail.blankAfter = block.blankAfter;
        block.text = text.left(offset);
        block.blankAfter = 1;
        touch(block);
        insertBlockAt(index + 1, tail);
        m_caret = m_anchor = {index + 1, 0};
        reparse(index + 1);
        reparse(index);
        break;
    }
    case BlockKind::Heading: {
        const ContentMap map = contentMap(block);
        const int contentStart = map.sourceOf.first();
        const int contentEnd = map.sourceOf.last();
        Block paragraph;
        if (offset <= contentStart && contentEnd > contentStart) {
            paragraph.blankAfter = 1;
            insertBlockAt(index, paragraph);
        } else if (offset >= contentEnd) {
            paragraph.blankAfter = block.blankAfter;
            block.blankAfter = 1;
            insertBlockAt(index + 1, paragraph);
            m_caret = m_anchor = {index + 1, 0};
        } else {
            paragraph.text = text.mid(offset);
            paragraph.blankAfter = block.blankAfter;
            block.text = text.left(offset);
            block.blankAfter = 1;
            touch(block);
            insertBlockAt(index + 1, paragraph);
            m_caret = m_anchor = {index + 1, 0};
            reparse(index + 1);
        }
        break;
    }
    case BlockKind::ListItem: {
        const ListInfo info = listInfo(text);
        if (contentMap(block).text.trimmed().isEmpty()) {
            if (info.indent > 0) {
                setListIndent(index, parentIndent(index));
            } else {
                block.text.clear();
                block.kind = BlockKind::Paragraph;
                touch(block);
                if (index > 0 && m_blocks[index - 1].kind == BlockKind::ListItem)
                    m_blocks[index - 1].blankAfter = std::max(1, m_blocks[index - 1].blankAfter);
                block.blankAfter = std::max(1, block.blankAfter);
                m_caret = m_anchor = {index, 0};
            }
            break;
        }
        const int split = std::max(offset, info.contentStart);
        QString marker = info.ordered ? QString::number(info.number + 1) + info.delimiter : QString(info.delimiter);
        QString prefix = spaces(info.indent) + marker + QLatin1Char(' ');
        if (info.task)
            prefix += QStringLiteral("[ ] ");
        Block item;
        item.kind = BlockKind::ListItem;
        item.text = prefix + text.mid(split);
        item.blankAfter = block.blankAfter;
        const bool nextIsItem = index + 1 < m_blocks.size() && m_blocks[index + 1].kind == BlockKind::ListItem;
        block.blankAfter = nextIsItem && block.blankAfter > 0 ? block.blankAfter : 0;
        block.text = text.left(split);
        touch(block);
        insertBlockAt(index + 1, item);
        m_caret = m_anchor = {index + 1, int(prefix.size())};
        reparse(index + 1);
        break;
    }
    case BlockKind::Quote: {
        const int lineStart = lineStartOf(text, offset);
        const int lineEnd = lineEndOf(text, offset);
        const int prefix = quotePrefixLength(text, lineStart);
        const QString lineContent = text.mid(lineStart + prefix, lineEnd - lineStart - prefix);
        if (lineContent.trimmed().isEmpty() && lineEnd == text.size()) {
            Block paragraph;
            if (lineStart == 0) {
                block.text.clear();
                block.kind = BlockKind::Paragraph;
                touch(block);
                m_caret = m_anchor = {index, 0};
            } else {
                block.text = text.left(lineStart - 1);
                touch(block);
                paragraph.blankAfter = block.blankAfter;
                block.blankAfter = 1;
                insertBlockAt(index + 1, paragraph);
                m_caret = m_anchor = {index + 1, 0};
            }
        } else {
            const QString insert = QLatin1Char('\n') + text.mid(lineStart, prefix);
            block.text.insert(offset, insert);
            touch(block);
            m_caret = m_anchor = {index, offset + int(insert.size())};
        }
        break;
    }
    case BlockKind::CodeFence:
        if (!text.contains(QLatin1Char('\n')) && offset == text.size()) {
            // "```lang" just typed: open the block with its closing fence.
            const FenceInfo info = fenceInfo(text);
            block.text = text + QStringLiteral("\n\n") + QString(info.fenceLength, info.fenceChar);
            touch(block);
            m_caret = m_anchor = {index, int(text.size()) + 1};
            break;
        }
        [[fallthrough]];
    case BlockKind::IndentedCode:
    case BlockKind::FrontMatter: {
        const int lineStart = lineStartOf(text, offset);
        const int indent = std::min(offset - lineStart, leadingSpaceCount(QStringView(text).mid(lineStart)));
        const QString insert = QLatin1Char('\n') + text.mid(lineStart, indent);
        block.text.insert(offset, insert);
        touch(block);
        m_caret = m_anchor = {index, offset + int(insert.size())};
        reparse(m_caret.block);
        break;
    }
    case BlockKind::MathBlock:
    case BlockKind::Html:
        block.text.insert(offset, QLatin1Char('\n'));
        touch(block);
        m_caret = m_anchor = {index, offset + 1};
        reparse(index);
        break;
    case BlockKind::Table: {
        const TableInfo info = tableInfo(text);
        int row = 0;
        int column = 0;
        for (int r = 0; r < info.rows.size(); ++r) {
            for (int c = 0; c < info.rows[r].cells.size(); ++c) {
                const Segment cell = info.rows[r].cells[c];
                if (offset >= cell.start && offset <= cell.end) {
                    row = r;
                    column = c;
                }
            }
        }
        // Enter goes on to the next row, as a new record: its first cell.
        Q_UNUSED(column);
        m_undo.removeLast();
        m_editing = false;
        if (row + 1 < info.rows.size() && !info.rows[row + 1].cells.isEmpty()) {
            moveCaret({index, info.rows[row + 1].cells.first().end}, false);
            return;
        }
        tableInsertRow(true);
        const TableInfo grown = tableInfo(m_blocks[index].text);
        if (row + 1 < grown.rows.size() && !grown.rows[row + 1].cells.isEmpty())
            moveCaret({index, grown.rows[row + 1].cells.first().start}, false);
        return;
    }
    case BlockKind::Rule: {
        Block paragraph;
        paragraph.blankAfter = block.blankAfter;
        block.blankAfter = 1;
        insertBlockAt(index + 1, paragraph);
        m_caret = m_anchor = {index + 1, 0};
        break;
    }
    }
    endEdit();
}

void MarkdownEditor::softBreak()
{
    Block &block = m_blocks[m_caret.block];
    switch (block.kind) {
    case BlockKind::Paragraph:
        insertText(QStringLiteral("\n"));
        return;
    case BlockKind::ListItem: {
        const ListInfo info = listInfo(block.text);
        insertText(QLatin1Char('\n') + spaces(info.contentIndent));
        return;
    }
    case BlockKind::Quote: {
        const int lineStart = lineStartOf(block.text, m_caret.offset);
        insertText(QLatin1Char('\n') + block.text.mid(lineStart, quotePrefixLength(block.text, lineStart)));
        return;
    }
    case BlockKind::Table:
        insertText(QStringLiteral("<br>"));
        return;
    default:
        enter();
        return;
    }
}

void MarkdownEditor::tab(bool backwards)
{
    const int index = m_caret.block;
    Block &block = m_blocks[index];
    switch (block.kind) {
    case BlockKind::ListItem: {
        const ListInfo info = listInfo(block.text);
        int indent = info.indent;
        if (backwards) {
            if (info.indent == 0)
                return;
            indent = parentIndent(index);
        } else {
            int j = index - 1;
            while (j >= 0 && m_blocks[j].kind == BlockKind::ListItem && listInfo(m_blocks[j].text).indent > info.indent)
                --j;
            if (j < 0 || m_blocks[j].kind != BlockKind::ListItem)
                return;
            const ListInfo sibling = listInfo(m_blocks[j].text);
            if (sibling.indent != info.indent)
                return;
            indent = sibling.contentIndent;
        }
        beginEdit(EditKind::Other);
        setListIndent(index, indent);
        if (index > 0 && m_blocks[index - 1].kind == BlockKind::ListItem)
            m_blocks[index - 1].blankAfter = std::min(m_blocks[index - 1].blankAfter, 0);
        reparse(index);
        endEdit();
        return;
    }
    case BlockKind::Table: {
        const QList<Segment> cells = caretSegments(block);
        int current = 0;
        for (int i = 0; i < cells.size(); ++i) {
            if (m_caret.offset >= cells[i].start && m_caret.offset <= cells[i].end)
                current = i;
        }
        const int next = current + (backwards ? -1 : 1);
        if (next < 0)
            return;
        if (next >= cells.size()) {
            tableInsertRow(true);
            return;
        }
        // Tab selects the next cell's text, as in a spreadsheet.
        setCaret({index, cells[next].end}, {index, cells[next].start});
        return;
    }
    case BlockKind::CodeFence:
    case BlockKind::IndentedCode:
    case BlockKind::FrontMatter:
        if (backwards) {
            const int lineStart = lineStartOf(block.text, m_caret.offset);
            const int remove = std::min(4, leadingSpaceCount(QStringView(block.text).mid(lineStart)));
            if (remove == 0)
                return;
            beginEdit(EditKind::Other);
            block.text.remove(lineStart, remove);
            touch(block);
            m_caret.offset = std::max(lineStart, m_caret.offset - remove);
            m_anchor = m_caret;
            endEdit();
        } else {
            insertText(QStringLiteral("    "));
        }
        return;
    default:
        if (!backwards)
            insertText(QStringLiteral("    "));
        return;
    }
}

// ---- Clipboard ---------------------------------------------------------------------------

void MarkdownEditor::copy()
{
    if (!hasSelection())
        return;
    auto *mime = new QMimeData;
    const QString markdown = selectedMarkdown();
    mime->setText(markdown);
    // Other applications get it formatted; this one reads the Markdown back.
    if (m_htmlForCopy && !isCodeLike(m_blocks[m_caret.block].kind))
        mime->setHtml(m_htmlForCopy(markdown));
    QApplication::clipboard()->setMimeData(mime);
}

void MarkdownEditor::copyAsMarkdown()
{
    if (!hasSelection())
        return;
    auto *mime = new QMimeData;
    mime->setText(selectedMarkdown());
    QApplication::clipboard()->setMimeData(mime);
}

void MarkdownEditor::cut()
{
    if (!hasSelection())
        return;
    copy();
    deleteSelection();
}

void MarkdownEditor::paste()
{
    pasteMime(QApplication::clipboard()->mimeData(), false);
}

void MarkdownEditor::pastePlain()
{
    pasteMime(QApplication::clipboard()->mimeData(), true);
}

QString MarkdownEditor::imageMarkdownFor(const QString &path) const
{
    QString target = m_imagePathMapper ? m_imagePathMapper(path) : path;
    if (target == path && !m_baseDir.isEmpty()) {
        const QString relative = QDir(m_baseDir).relativeFilePath(path);
        if (!relative.startsWith(QLatin1String("../../")))
            target = relative;
    }
    return QStringLiteral("![%1](%2)").arg(QFileInfo(path).completeBaseName(), markdownPath(target));
}

void MarkdownEditor::pasteMime(const QMimeData *mime, bool plain)
{
    if (!mime)
        return;
    const BlockKind kind = m_blocks[m_caret.block].kind;
    if (!plain && mime->hasUrls()) {
        QStringList parts;
        for (const QUrl &url : mime->urls()) {
            if (!url.isLocalFile()) {
                parts.clear();
                break;
            }
            const QString path = url.toLocalFile();
            if (isImageFile(path))
                parts.append(imageMarkdownFor(path));
            else
                parts.append(QStringLiteral("[%1](%2)").arg(QFileInfo(path).fileName(), markdownPath(path)));
        }
        if (!parts.isEmpty()) {
            insertMarkdown(parts.join(QLatin1Char(' ')));
            return;
        }
    }
    if (!plain && mime->hasImage() && !mime->hasText() && m_imageSaver) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        const QString path = image.isNull() ? QString() : m_imageSaver(image);
        if (!path.isEmpty()) {
            insertMarkdown(QStringLiteral("![image](%1)").arg(markdownPath(path)));
            return;
        }
    }
    if (!mime->hasText())
        return;
    QString text = mime->text();
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (text.isEmpty())
        return;
    if (kind == BlockKind::Table) {
        insertText(text.simplified());
        return;
    }
    insertMarkdown(text);
}

void MarkdownEditor::insertMarkdown(const QString &markdown)
{
    beginEdit(EditKind::Other);
    const auto [from, to] = selectionRange();
    replaceRange(from, to, markdown);
    endEdit();
}

void MarkdownEditor::selectAll()
{
    m_anchor = docStart();
    m_caret = docEnd();
    updateCaret();
}

void MarkdownEditor::undo()
{
    if (m_undo.isEmpty())
        return;
    m_redo.append(snapshot());
    const Snapshot state = m_undo.takeLast();
    m_lastEdit = EditKind::None;
    restore(state);
}

void MarkdownEditor::redo()
{
    if (m_redo.isEmpty())
        return;
    m_undo.append(snapshot());
    const Snapshot state = m_redo.takeLast();
    m_lastEdit = EditKind::None;
    restore(state);
}

// ---- Format commands -------------------------------------------------------------------

void MarkdownEditor::toggleWrap(const QString &marker, InlineType type)
{
    const QString open = marker;
    const QString close = marker;
    const int index = m_caret.block;
    Block &block = m_blocks[index];
    if (isCodeLike(block.kind))
        return;

    if (!hasSelection()) {
        // Inside such a construct: take its markers away.
        if (isTextKind(block.kind) || block.kind == BlockKind::Table) {
            ContentMap map = contentMap(block);
            if (block.kind == BlockKind::Table) {
                map.text = block.text;
                map.sourceOf.clear();
                for (int i = 0; i <= block.text.size(); ++i)
                    map.sourceOf.append(i);
            }
            const int at = map.fromSource(m_caret.offset);
            const InlineNode *found = nullptr;
            std::function<void(const QList<InlineNode> &)> search = [&](const QList<InlineNode> &nodes) {
                for (const InlineNode &node : nodes) {
                    if (at < node.start || at > node.end)
                        continue;
                    if (node.type == type)
                        found = &node;
                    search(node.children);
                }
            };
            const QList<InlineNode> nodes = cachedInline(map.text, &m_references);
            search(nodes);
            if (found) {
                beginEdit(EditKind::Other);
                const int start = map.map(found->start);
                const int innerStart = map.map(found->innerStart);
                const int innerEnd = map.map(found->innerEnd);
                const int end = map.map(found->end);
                block.text.remove(innerEnd, end - innerEnd);
                block.text.remove(start, innerStart - start);
                touch(block);
                int offset = m_caret.offset;
                if (offset > innerEnd)
                    offset -= end - innerEnd;
                if (offset > start)
                    offset = std::max(start, offset - (innerStart - start));
                m_caret = m_anchor = {index, offset};
                reparse(index);
                endEdit();
                return;
            }
        }
        // On a word: wrap the word. Elsewhere: an empty pair with the caret inside.
        const QString &text = block.text;
        int start = m_caret.offset;
        int end = m_caret.offset;
        while (start > 0 && isWordChar(text.at(start - 1)))
            --start;
        while (end < text.size() && isWordChar(text.at(end)))
            ++end;
        beginEdit(EditKind::Other);
        if (start < end && type != InlineType::Code && type != InlineType::Math) {
            block.text.insert(end, close);
            block.text.insert(start, open);
            m_caret = m_anchor = {index, m_caret.offset + int(open.size())};
        } else {
            block.text.insert(m_caret.offset, open + close);
            m_caret = m_anchor = {index, m_caret.offset + int(open.size())};
        }
        touch(block);
        reparse(index);
        endEdit();
        return;
    }

    const auto [from, to] = selectionRange();
    beginEdit(EditKind::Other);
    if (from.block == to.block) {
        QString &text = block.text;
        const int s = from.offset;
        const int e = to.offset;
        const bool wrapped = s >= open.size() && QStringView(text).mid(s - open.size(), open.size()) == open
            && QStringView(text).mid(e, close.size()) == close;
        const bool inside = QStringView(text).mid(s, open.size()) == open && e - s >= open.size() + close.size()
            && QStringView(text).mid(e - close.size(), close.size()) == close;
        if (wrapped) {
            text.remove(e, close.size());
            text.remove(s - open.size(), open.size());
            m_anchor = {index, s - int(open.size())};
            m_caret = {index, e - int(open.size())};
        } else if (inside) {
            text.remove(e - close.size(), close.size());
            text.remove(s, open.size());
            m_anchor = {index, s};
            m_caret = {index, e - int(open.size()) - int(close.size())};
        } else {
            text.insert(e, close);
            text.insert(s, open);
            m_anchor = {index, s + int(open.size())};
            m_caret = {index, e + int(open.size())};
        }
        touch(block);
        reparse(index);
    } else {
        for (int i = to.block; i >= from.block; --i) {
            Block &b = m_blocks[i];
            if (!isTextKind(b.kind))
                continue;
            const int s = i == from.block ? from.offset : firstPos(i).offset;
            const int e = i == to.block ? to.offset : lastPos(i).offset;
            if (e <= s)
                continue;
            b.text.insert(e, close);
            b.text.insert(s, open);
            touch(b);
        }
        m_anchor = {from.block, from.offset + int(open.size())};
        m_caret = {to.block, to.offset + int(open.size())};
    }
    endEdit();
}

void MarkdownEditor::toggleUnderline()
{
    Block &block = m_blocks[m_caret.block];
    if (isCodeLike(block.kind))
        return;
    const auto [from, to] = selectionRange();
    if (from.block != to.block)
        return;
    beginEdit(EditKind::Other);
    QString &text = block.text;
    const int s = from.offset;
    const int e = to.offset;
    const QString open = QStringLiteral("<u>");
    const QString close = QStringLiteral("</u>");
    if (s >= 3 && QStringView(text).mid(s - 3, 3) == open && QStringView(text).mid(e, 4) == close) {
        text.remove(e, 4);
        text.remove(s - 3, 3);
        m_anchor = {from.block, s - 3};
        m_caret = {from.block, e - 3};
    } else {
        text.insert(e, close);
        text.insert(s, open);
        m_anchor = {from.block, s + 3};
        m_caret = {from.block, e + 3};
    }
    touch(block);
    reparse(from.block);
    endEdit();
}

void MarkdownEditor::insertLink()
{
    const auto [from, to] = selectionRange();
    if (from.block != to.block || isCodeLike(m_blocks[from.block].kind))
        return;
    QString url;
    const QString clip = QApplication::clipboard()->text().trimmed();
    if (clip.startsWith(QLatin1String("http://")) || clip.startsWith(QLatin1String("https://")))
        url = clip;
    beginEdit(EditKind::Other);
    Block &block = m_blocks[from.block];
    const QString label = block.text.mid(from.offset, to.offset - from.offset);
    const QString link = QStringLiteral("[%1](%2)").arg(label, url);
    block.text.replace(from.offset, to.offset - from.offset, link);
    touch(block);
    // The caret goes where something is missing: the address, or the label.
    const int caret = label.isEmpty() ? from.offset + 1 : from.offset + int(label.size()) + 3 + int(url.size());
    m_caret = m_anchor = {from.block, caret};
    reparse(from.block);
    endEdit();
}

void MarkdownEditor::insertImage(const QString &path)
{
    if (!path.isEmpty()) {
        insertMarkdown(imageMarkdownFor(path));
        return;
    }
    const Pos at = selectionRange().first;
    insertMarkdown(QStringLiteral("![]()"));
    setCaret({at.block, at.offset + 4});
}

void MarkdownEditor::clearFormat()
{
    const auto [from, to] = selectionRange();
    if (from.block != to.block || from == to || isCodeLike(m_blocks[from.block].kind))
        return;
    const QString selected = m_blocks[from.block].text.mid(from.offset, to.offset - from.offset);
    const QString plain = plainText(selected, parseInline(selected, &m_references));
    beginEdit(EditKind::Other);
    replaceRange(from, to, plain);
    m_anchor = from;
    endEdit();
}

void MarkdownEditor::setHeading(int level)
{
    const auto [from, to] = selectionRange();
    const BlockKind kind = m_blocks[m_caret.block].kind;
    if (level == 0 && from.block == to.block
        && (kind == BlockKind::CodeFence || kind == BlockKind::IndentedCode || kind == BlockKind::MathBlock)) {
        blockToParagraph();
        return;
    }
    beginEdit(EditKind::Other);
    for (int i = to.block; i >= from.block; --i) {
        Block &block = m_blocks[i];
        if (block.kind != BlockKind::Paragraph && block.kind != BlockKind::Heading)
            continue;
        int prefix = 0;
        QString content = block.text;
        int target = level;
        if (block.kind == BlockKind::Heading) {
            const ContentMap map = contentMap(block);
            prefix = map.sourceOf.first();
            content = block.text.mid(prefix, map.sourceOf.last() - prefix);
            if (headingLevel(block) == level && from.block == to.block)
                target = 0;
        }
        content.replace(QLatin1Char('\n'), QLatin1Char(' '));
        const QString marker = target > 0 ? QString(target, QLatin1Char('#')) + QLatin1Char(' ') : QString();
        block.text = marker + content;
        touch(block);
        for (Pos *pos : {&m_caret, &m_anchor}) {
            if (pos->block == i)
                pos->offset = std::clamp(pos->offset - prefix, 0, int(content.size())) + int(marker.size());
        }
        reparse(i);
    }
    endEdit();
}

void MarkdownEditor::promoteHeading(int delta)
{
    const Block &block = m_blocks[m_caret.block];
    const int level = headingLevel(block);
    if (block.kind != BlockKind::Paragraph && block.kind != BlockKind::Heading)
        return;
    int next = level;
    if (delta > 0)
        next = level == 0 ? 6 : std::max(1, level - 1);
    else
        next = level == 0 ? 0 : (level >= 6 ? 0 : level + 1);
    if (next == level)
        return;
    // setHeading toggles a level that is already set, so go through a paragraph when needed.
    setHeading(next);
}

void MarkdownEditor::toggleQuote()
{
    const auto [from, to] = selectionRange();
    bool allQuotes = true;
    for (int i = from.block; i <= to.block; ++i)
        allQuotes = allQuotes && m_blocks[i].kind == BlockKind::Quote;
    beginEdit(EditKind::Other);
    if (allQuotes) {
        for (int i = to.block; i >= from.block; --i) {
            Block &block = m_blocks[i];
            QStringList lines = block.text.split(QLatin1Char('\n'));
            int removedBefore = 0;
            int lineStart = 0;
            for (QString &line : lines) {
                const int prefix = quotePrefixLength(line, 0);
                const qsizetype at = line.indexOf(QLatin1Char('>'));
                int removed = 0;
                if (at >= 0 && at < prefix) {
                    removed = int(at) + 1 + (at + 1 < line.size() && line.at(at + 1) == QLatin1Char(' ') ? 1 : 0);
                    line.remove(0, removed);
                }
                for (Pos *pos : {&m_caret, &m_anchor}) {
                    if (pos->block == i && pos->offset >= lineStart && pos->offset <= lineStart + line.size() + removed)
                        pos->offset = std::max(lineStart - removedBefore, pos->offset - removedBefore - removed);
                }
                lineStart += int(line.size()) + removed + 1;
                removedBefore += removed;
            }
            block.text = lines.join(QLatin1Char('\n'));
            touch(block);
            reparse(i);
        }
    } else {
        QStringList parts;
        for (int i = from.block; i <= to.block; ++i) {
            QStringList lines = m_blocks[i].text.split(QLatin1Char('\n'));
            for (QString &line : lines)
                line = line.isEmpty() ? QStringLiteral(">") : QStringLiteral("> ") + line;
            parts.append(lines.join(QLatin1Char('\n')));
        }
        const bool single = from.block == to.block;
        const int caretLine = m_blocks[m_caret.block].text.left(m_caret.offset).count(QLatin1Char('\n'));
        Block quote;
        quote.kind = BlockKind::Quote;
        quote.text = parts.join(QStringLiteral("\n>\n"));
        quote.blankAfter = m_blocks[to.block].blankAfter;
        quote.revision = Block::nextRevision();
        const int caretOffset = m_caret.offset + 2 * (caretLine + 1);
        for (int i = to.block; i > from.block; --i)
            m_blocks.removeAt(i);
        m_blocks[from.block] = quote;
        m_caret = m_anchor = single ? Pos{from.block, caretOffset} : Pos{from.block, int(quote.text.size())};
        reparse(from.block);
    }
    endEdit();
}

void MarkdownEditor::toggleList(bool ordered, bool task)
{
    const auto [from, to] = selectionRange();
    bool allSame = true;
    for (int i = from.block; i <= to.block; ++i) {
        const Block &block = m_blocks[i];
        const ListInfo info = listInfo(block.text);
        allSame = allSame && block.kind == BlockKind::ListItem && info.ordered == ordered && info.task == task;
    }
    beginEdit(EditKind::Other);
    int number = 1;
    for (int i = from.block; i <= to.block; ++i) {
        Block &block = m_blocks[i];
        if (block.kind != BlockKind::Paragraph && block.kind != BlockKind::ListItem)
            continue;
        int oldPrefix = 0;
        QString content = block.text;
        if (block.kind == BlockKind::ListItem) {
            const ContentMap map = contentMap(block);
            oldPrefix = map.sourceOf.first();
            content = map.text;
        }
        QString prefix;
        if (!allSame) {
            prefix = ordered ? QString::number(number++) + QStringLiteral(". ") : QStringLiteral("- ");
            if (task)
                prefix += QStringLiteral("[ ] ");
        }
        // Continuation lines line up under the content.
        QStringList lines = content.split(QLatin1Char('\n'));
        for (int l = 1; l < lines.size(); ++l)
            lines[l] = spaces(int(prefix.size())) + lines[l];
        block.text = prefix + lines.join(QLatin1Char('\n'));
        touch(block);
        for (Pos *pos : {&m_caret, &m_anchor}) {
            if (pos->block == i && pos->offset <= oldPrefix + lines.first().size())
                pos->offset = std::max(0, pos->offset - oldPrefix) + int(prefix.size());
        }
        if (allSame)
            block.blankAfter = std::max(1, block.blankAfter);
        else if (i < to.block)
            block.blankAfter = 0;
        reparse(i);
    }
    endEdit();
}

void MarkdownEditor::insertTable(int rows, int columns)
{
    rows = std::max(1, rows);
    columns = std::max(1, columns);
    QList<QStringList> cells(rows + 1, QStringList(columns, QString()));
    QList<QList<Segment>> segments;
    Block table;
    table.kind = BlockKind::Table;
    table.text = tableText(cells, QList<Align>(columns, Align::None), &segments);
    beginEdit(EditKind::Other);
    placeNewBlock(table, segments[0][0].start);
    endEdit();
}

void MarkdownEditor::insertCodeBlock(const QString &language)
{
    Block &current = m_blocks[m_caret.block];
    beginEdit(EditKind::Other);
    if (current.kind == BlockKind::Paragraph && !current.text.trimmed().isEmpty()) {
        const QString fence = QStringLiteral("```") + language + QLatin1Char('\n');
        current.text = fence + current.text + QStringLiteral("\n```");
        current.kind = BlockKind::CodeFence;
        touch(current);
        m_caret = m_anchor = {m_caret.block, m_caret.offset + int(fence.size())};
    } else {
        Block code;
        code.kind = BlockKind::CodeFence;
        code.text = QStringLiteral("```") + language + QStringLiteral("\n\n```");
        placeNewBlock(code, 4 + int(language.size()));
    }
    endEdit();
}

void MarkdownEditor::insertMathBlock()
{
    Block &current = m_blocks[m_caret.block];
    beginEdit(EditKind::Other);
    if (current.kind == BlockKind::Paragraph && !current.text.trimmed().isEmpty()) {
        current.text = QStringLiteral("$$\n") + current.text + QStringLiteral("\n$$");
        current.kind = BlockKind::MathBlock;
        touch(current);
        m_caret = m_anchor = {m_caret.block, m_caret.offset + 3};
    } else {
        Block math;
        math.kind = BlockKind::MathBlock;
        math.text = QStringLiteral("$$\n\n$$");
        placeNewBlock(math, 3);
    }
    endEdit();
}

void MarkdownEditor::insertRule()
{
    beginEdit(EditKind::Other);
    Block rule;
    rule.kind = BlockKind::Rule;
    rule.text = QStringLiteral("---");
    placeNewBlock(rule, 0);
    const int index = m_caret.block;
    if (index + 1 >= m_blocks.size() || !isTextKind(m_blocks[index + 1].kind)) {
        Block paragraph;
        paragraph.blankAfter = m_blocks[index].blankAfter;
        m_blocks[index].blankAfter = 1;
        insertBlockAt(index + 1, paragraph);
    }
    m_caret = m_anchor = firstPos(index + 1);
    endEdit();
}

void MarkdownEditor::insertParagraph(bool after)
{
    beginEdit(EditKind::Other);
    const int index = m_caret.block;
    Block paragraph;
    paragraph.blankAfter = 1;
    if (after) {
        paragraph.blankAfter = std::max(1, m_blocks[index].blankAfter);
        m_blocks[index].blankAfter = 1;
        insertBlockAt(index + 1, paragraph);
        m_caret = m_anchor = {index + 1, 0};
    } else {
        insertBlockAt(index, paragraph);
        m_caret = m_anchor = {index, 0};
    }
    endEdit();
}

void MarkdownEditor::indentList(bool deeper)
{
    if (m_blocks[m_caret.block].kind == BlockKind::ListItem)
        tab(!deeper);
}

void MarkdownEditor::toggleTask(int index)
{
    Block &block = m_blocks[index];
    const ListInfo info = listInfo(block.text);
    if (!info.task)
        return;
    beginEdit(EditKind::Other);
    block.text[info.taskStart + 1] = info.checked ? QLatin1Char(' ') : QLatin1Char('x');
    touch(block);
    endEdit();
}

void MarkdownEditor::insertFootnote()
{
    // The next free number, a reference at the caret and its note at the end.
    int number = 1;
    static const QRegularExpression used(QStringLiteral("\\[\\^(\\d+)\\]"));
    for (const Block &block : std::as_const(m_blocks)) {
        QRegularExpressionMatchIterator it = used.globalMatch(block.text);
        while (it.hasNext())
            number = std::max(number, it.next().captured(1).toInt() + 1);
    }
    const QString label = QStringLiteral("[^%1]").arg(number);
    beginEdit(EditKind::Other);
    Block &block = m_blocks[m_caret.block];
    if (!isCodeLike(block.kind)) {
        const int at = selectionRange().second.offset;
        block.text.insert(at, label);
        touch(block);
        reparse(m_caret.block);
    }
    Block note;
    note.text = label + QStringLiteral(": ");
    m_blocks.last().blankAfter = std::max(1, m_blocks.last().blankAfter);
    insertBlockAt(int(m_blocks.size()), note);
    m_caret = m_anchor = {int(m_blocks.size()) - 1, int(note.text.size())};
    endEdit();
}

void MarkdownEditor::insertLinkReference()
{
    beginEdit(EditKind::Other);
    Block reference;
    reference.text = QStringLiteral("[]: ");
    placeNewBlock(reference, 1);
    endEdit();
}

void MarkdownEditor::insertTableOfContents()
{
    beginEdit(EditKind::Other);
    Block toc;
    toc.text = QStringLiteral("[TOC]");
    placeNewBlock(toc, 5);
    endEdit();
}

void MarkdownEditor::insertFrontMatter()
{
    if (m_blocks.first().kind == BlockKind::FrontMatter) {
        setCaret({0, 4});
        return;
    }
    beginEdit(EditKind::Other);
    Block front;
    front.kind = BlockKind::FrontMatter;
    front.text = QStringLiteral("---\n\n---");
    insertBlockAt(0, front);
    m_caret = m_anchor = {0, 4};
    endEdit();
}

void MarkdownEditor::blockToParagraph()
{
    const int index = m_caret.block;
    Block &block = m_blocks[index];
    Segment inner{0, int(block.text.size())};
    if (block.kind == BlockKind::CodeFence)
        inner = fenceInfo(block.text).code;
    else if (block.kind == BlockKind::MathBlock)
        inner = mathInfo(block.text).latex;
    else if (block.kind != BlockKind::IndentedCode && block.kind != BlockKind::Html)
        return;
    beginEdit(EditKind::Other);
    const int caret = std::clamp(m_caret.offset - inner.start, 0, inner.end - inner.start);
    block.text = block.text.mid(inner.start, inner.end - inner.start).trimmed();
    block.kind = BlockKind::Paragraph;
    touch(block);
    m_caret = m_anchor = {index, std::min(caret, int(block.text.size()))};
    reparse(index);
    endEdit();
}

int MarkdownEditor::caretHeadingLevel() const
{
    const Block &block = m_blocks[m_caret.block];
    if (block.kind == BlockKind::Heading)
        return headingLevel(block);
    return block.kind == BlockKind::Paragraph ? 0 : -1;
}

QString MarkdownEditor::linkAt(const QPoint &viewportPoint)
{
    const qreal contentY = viewportPoint.y() + verticalScrollBar()->value();
    const int index = blockAtY(contentY);
    return render(index)->anchorAt(QPointF(viewportPoint.x() - columnLeft(), contentY - m_ys.value(index)));
}

int MarkdownEditor::selectionWordCount() const
{
    if (!hasSelection())
        return 0;
    return wordCountOf(blocksFromMarkdown(selectedMarkdown()));
}

// ---- Tables ---------------------------------------------------------------------------------

bool MarkdownEditor::inTable() const
{
    return m_blocks[m_caret.block].kind == BlockKind::Table;
}

namespace {

struct TableCells {
    QList<QStringList> rows;
    QList<Align> aligns;
    int row = 0;
    int column = 0;
};

TableCells readTable(const Block &block, int caret)
{
    TableCells out;
    const TableInfo info = tableInfo(block.text);
    out.aligns = info.aligns;
    while (out.aligns.size() < info.columns)
        out.aligns.append(Align::None);
    out.aligns.resize(info.columns);
    for (int r = 0; r < info.rows.size(); ++r) {
        QStringList row;
        for (int c = 0; c < info.columns; ++c) {
            if (c < info.rows[r].cells.size()) {
                const Segment cell = info.rows[r].cells[c];
                row.append(block.text.mid(cell.start, cell.end - cell.start));
                if (caret >= cell.start && caret <= cell.end) {
                    out.row = r;
                    out.column = c;
                }
            } else {
                row.append(QString());
            }
        }
        out.rows.append(row);
    }
    return out;
}

} // namespace

void MarkdownEditor::formatTable(int index)
{
    Block &block = m_blocks[index];
    if (block.kind != BlockKind::Table)
        return;
    TableCells table = readTable(block, m_caret.block == index ? m_caret.offset : -1);
    QList<QList<Segment>> cells;
    beginEdit(EditKind::Other);
    block.text = tableText(table.rows, table.aligns, &cells);
    touch(block);
    if (m_caret.block == index)
        m_caret = m_anchor = {index, cells[table.row][table.column].end};
    endEdit();
}

void MarkdownEditor::tableInsertRow(bool below)
{
    if (!inTable())
        return;
    const int index = m_caret.block;
    Block &block = m_blocks[index];
    TableCells table = readTable(block, m_caret.offset);
    int row = below ? table.row + 1 : std::max(1, table.row);
    table.rows.insert(row, QStringList(table.aligns.size(), QString()));
    QList<QList<Segment>> cells;
    beginEdit(EditKind::Other);
    block.text = tableText(table.rows, table.aligns, &cells);
    touch(block);
    m_caret = m_anchor = {index, cells[row][std::min(table.column, int(cells[row].size()) - 1)].start};
    endEdit();
}

void MarkdownEditor::tableInsertColumn(bool right)
{
    if (!inTable())
        return;
    const int index = m_caret.block;
    Block &block = m_blocks[index];
    TableCells table = readTable(block, m_caret.offset);
    const int column = right ? table.column + 1 : table.column;
    for (QStringList &row : table.rows)
        row.insert(column, QString());
    table.aligns.insert(column, Align::None);
    QList<QList<Segment>> cells;
    beginEdit(EditKind::Other);
    block.text = tableText(table.rows, table.aligns, &cells);
    touch(block);
    m_caret = m_anchor = {index, cells[table.row][column].start};
    endEdit();
}

void MarkdownEditor::tableDeleteRow()
{
    if (!inTable())
        return;
    const int index = m_caret.block;
    Block &block = m_blocks[index];
    TableCells table = readTable(block, m_caret.offset);
    beginEdit(EditKind::Other);
    if (table.rows.size() <= 1) {
        block.text.clear();
        block.kind = BlockKind::Paragraph;
        touch(block);
        m_caret = m_anchor = {index, 0};
        endEdit();
        return;
    }
    table.rows.removeAt(table.row);
    const int row = std::min(table.row, int(table.rows.size()) - 1);
    QList<QList<Segment>> cells;
    block.text = tableText(table.rows, table.aligns, &cells);
    touch(block);
    m_caret = m_anchor = {index, cells[row][table.column].end};
    endEdit();
}

void MarkdownEditor::tableDeleteColumn()
{
    if (!inTable())
        return;
    const int index = m_caret.block;
    Block &block = m_blocks[index];
    TableCells table = readTable(block, m_caret.offset);
    beginEdit(EditKind::Other);
    if (table.aligns.size() <= 1) {
        block.text.clear();
        block.kind = BlockKind::Paragraph;
        touch(block);
        m_caret = m_anchor = {index, 0};
        endEdit();
        return;
    }
    for (QStringList &row : table.rows)
        row.removeAt(table.column);
    table.aligns.removeAt(table.column);
    const int column = std::min(table.column, int(table.aligns.size()) - 1);
    QList<QList<Segment>> cells;
    block.text = tableText(table.rows, table.aligns, &cells);
    touch(block);
    m_caret = m_anchor = {index, cells[table.row][column].end};
    endEdit();
}

void MarkdownEditor::tableAlign(Align align)
{
    if (!inTable())
        return;
    const int index = m_caret.block;
    Block &block = m_blocks[index];
    TableCells table = readTable(block, m_caret.offset);
    table.aligns[table.column] = align;
    QList<QList<Segment>> cells;
    beginEdit(EditKind::Other);
    block.text = tableText(table.rows, table.aligns, &cells);
    touch(block);
    m_caret = m_anchor = {index, cells[table.row][table.column].end};
    endEdit();
}

void MarkdownEditor::tableMoveRow(int delta)
{
    if (!inTable())
        return;
    const int index = m_caret.block;
    Block &block = m_blocks[index];
    TableCells table = readTable(block, m_caret.offset);
    // The header row stays where it is.
    const int target = table.row + delta;
    if (table.row == 0 || target < 1 || target >= table.rows.size())
        return;
    table.rows.swapItemsAt(table.row, target);
    QList<QList<Segment>> cells;
    beginEdit(EditKind::Other);
    block.text = tableText(table.rows, table.aligns, &cells);
    touch(block);
    m_caret = m_anchor = {index, cells[target][table.column].end};
    endEdit();
}

void MarkdownEditor::tableMoveColumn(int delta)
{
    if (!inTable())
        return;
    const int index = m_caret.block;
    Block &block = m_blocks[index];
    TableCells table = readTable(block, m_caret.offset);
    const int target = table.column + delta;
    if (target < 0 || target >= table.aligns.size())
        return;
    for (QStringList &row : table.rows)
        row.swapItemsAt(table.column, target);
    table.aligns.swapItemsAt(table.column, target);
    QList<QList<Segment>> cells;
    beginEdit(EditKind::Other);
    block.text = tableText(table.rows, table.aligns, &cells);
    touch(block);
    m_caret = m_anchor = {index, cells[table.row][target].end};
    endEdit();
}

void MarkdownEditor::tableDelete()
{
    if (!inTable())
        return;
    beginEdit(EditKind::Other);
    Block &block = m_blocks[m_caret.block];
    block.text.clear();
    block.kind = BlockKind::Paragraph;
    touch(block);
    m_caret = m_anchor = {m_caret.block, 0};
    endEdit();
}

void MarkdownEditor::copyTable()
{
    if (!inTable())
        return;
    const QString markdown = m_blocks[m_caret.block].text;
    auto *mime = new QMimeData;
    mime->setText(markdown);
    if (m_htmlForCopy)
        mime->setHtml(m_htmlForCopy(markdown));
    QApplication::clipboard()->setMimeData(mime);
}

// ---- Find -------------------------------------------------------------------------------------

void MarkdownEditor::setSearch(const QString &text, const FindOptions &options)
{
    m_search = text;
    m_searchOptions = options;
    viewport()->update();
}

QList<Segment> MarkdownEditor::matchesIn(int block) const
{
    QList<Segment> out;
    if (m_search.isEmpty())
        return out;
    QString pattern = m_searchOptions.regex ? m_search : QRegularExpression::escape(m_search);
    if (m_searchOptions.wholeWord)
        pattern = QStringLiteral("\\b(?:%1)\\b").arg(pattern);
    QRegularExpression expression(pattern, m_searchOptions.caseSensitive ? QRegularExpression::NoPatternOption
                                                                        : QRegularExpression::CaseInsensitiveOption);
    if (!expression.isValid())
        return out;
    QRegularExpressionMatchIterator it = expression.globalMatch(m_blocks[block].text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        if (match.capturedLength() == 0)
            continue;
        out.append({int(match.capturedStart()), int(match.capturedEnd())});
    }
    return out;
}

int MarkdownEditor::matchCount() const
{
    int count = 0;
    for (int i = 0; i < m_blocks.size(); ++i)
        count += int(matchesIn(i).size());
    return count;
}

bool MarkdownEditor::findNext(bool backwards)
{
    if (m_search.isEmpty())
        return false;
    const auto [from, to] = selectionRange();
    const int count = int(m_blocks.size());
    for (int step = 0; step <= count; ++step) {
        const int index = backwards ? ((from.block - step) % count + count) % count : (to.block + step) % count;
        QList<Segment> matches = matchesIn(index);
        if (backwards)
            std::reverse(matches.begin(), matches.end());
        for (const Segment &match : std::as_const(matches)) {
            if (step == 0 && !backwards && match.start < to.offset)
                continue;
            if (step == 0 && backwards && match.end > from.offset)
                continue;
            m_anchor = {index, match.start};
            m_caret = {index, match.end};
            m_preferredX = -1;
            updateCaret();
            return true;
        }
        if (step == count)
            break;
    }
    // Wrapped around within the starting block.
    const QList<Segment> matches = matchesIn(backwards ? from.block : to.block);
    if (!matches.isEmpty()) {
        const Segment match = backwards ? matches.last() : matches.first();
        const int index = backwards ? from.block : to.block;
        m_anchor = {index, match.start};
        m_caret = {index, match.end};
        updateCaret();
        return true;
    }
    return false;
}

bool MarkdownEditor::replaceCurrent(const QString &replacement)
{
    if (!hasSelection()) {
        return findNext();
    }
    const auto [from, to] = selectionRange();
    if (from.block != to.block)
        return findNext();
    bool matches = false;
    for (const Segment &match : matchesIn(from.block))
        matches = matches || (match.start == from.offset && match.end == to.offset);
    if (!matches)
        return findNext();
    QString text = replacement;
    if (m_searchOptions.regex) {
        QRegularExpression expression(m_search, m_searchOptions.caseSensitive ? QRegularExpression::NoPatternOption
                                                                             : QRegularExpression::CaseInsensitiveOption);
        const QString selected = m_blocks[from.block].text.mid(from.offset, to.offset - from.offset);
        text = QString(selected).replace(expression, replacement);
    }
    beginEdit(EditKind::Other);
    replaceRange(from, to, text);
    endEdit();
    findNext();
    return true;
}

int MarkdownEditor::replaceAll(const QString &replacement)
{
    if (m_search.isEmpty())
        return 0;
    QString pattern = m_searchOptions.regex ? m_search : QRegularExpression::escape(m_search);
    if (m_searchOptions.wholeWord)
        pattern = QStringLiteral("\\b(?:%1)\\b").arg(pattern);
    const QRegularExpression expression(pattern, m_searchOptions.caseSensitive
                                                     ? QRegularExpression::NoPatternOption
                                                     : QRegularExpression::CaseInsensitiveOption);
    if (!expression.isValid())
        return 0;
    const int total = matchCount();
    if (total == 0)
        return 0;
    beginEdit(EditKind::Other);
    QString escaped = replacement;
    if (!m_searchOptions.regex)
        escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    for (int i = int(m_blocks.size()) - 1; i >= 0; --i) {
        Block &block = m_blocks[i];
        const QString before = block.text;
        block.text.replace(expression, escaped);
        if (block.text != before) {
            touch(block);
            reparse(i);
        }
    }
    m_anchor = m_caret = clamp(m_caret);
    endEdit();
    return total;
}

// ---- Statistics ---------------------------------------------------------------------------------

namespace {

QString readableText(const Block &block, const LinkReferences *references)
{
    switch (block.kind) {
    case BlockKind::Paragraph:
    case BlockKind::Heading:
    case BlockKind::ListItem:
    case BlockKind::Quote: {
        const ContentMap map = contentMap(block);
        return plainText(map.text, cachedInline(map.text, references));
    }
    case BlockKind::Table: {
        QString out;
        const TableInfo info = tableInfo(block.text);
        for (const TableInfo::Row &row : info.rows) {
            for (const Segment &cell : row.cells) {
                const QString text = block.text.mid(cell.start, cell.end - cell.start);
                out += plainText(text, cachedInline(text, references)) + QLatin1Char(' ');
            }
        }
        return out;
    }
    case BlockKind::CodeFence: {
        const FenceInfo info = fenceInfo(block.text);
        return block.text.mid(info.code.start, info.code.end - info.code.start);
    }
    case BlockKind::IndentedCode:
    case BlockKind::Html:
        return block.text;
    default:
        return {};
    }
}

bool isCjk(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 0x4e00 && u <= 0x9fff) || (u >= 0x3400 && u <= 0x4dbf) || (u >= 0x3040 && u <= 0x30ff)
        || (u >= 0xac00 && u <= 0xd7af) || (u >= 0xf900 && u <= 0xfaff);
}

} // namespace

int MarkdownEditor::wordCount() const
{
    return wordCountOf(m_blocks);
}

int MarkdownEditor::wordCountOf(const QList<Block> &blocks)
{
    int words = 0;
    for (const Block &block : blocks) {
        const QString text = readableText(block, nullptr);
        bool inWord = false;
        for (const QChar c : text) {
            if (isCjk(c)) {
                ++words;
                inWord = false;
            } else if (c.isLetterOrNumber() || c == QLatin1Char('\'') || c == QLatin1Char('_')) {
                if (!inWord)
                    ++words;
                inWord = true;
            } else {
                inWord = false;
            }
        }
    }
    return words;
}

int MarkdownEditor::characterCount() const
{
    return characterCountOf(m_blocks);
}

int MarkdownEditor::characterCountOf(const QList<Block> &blocks)
{
    int count = 0;
    for (const Block &block : blocks) {
        for (const QChar c : readableText(block, nullptr)) {
            if (!c.isSpace() && !c.isLowSurrogate())
                ++count;
        }
    }
    return count;
}

int MarkdownEditor::lineCount() const
{
    return int(markdown().count(QLatin1Char('\n')));
}

// ---- Printing -------------------------------------------------------------------------------------

std::vector<std::unique_ptr<RenderedBlock>> MarkdownEditor::renderForPrint(qreal width, const EditorTheme &theme) const
{
    QList<int> numbers;
    QList<int> depths;
    numberLists(m_blocks, numbers, depths);
    QList<TocEntry> toc;
    for (const Heading &heading : headingsOf(m_blocks))
        toc.append({heading.level, heading.title});
    std::vector<std::unique_ptr<RenderedBlock>> out;
    for (int i = 0; i < m_blocks.size(); ++i) {
        RenderContext context;
        context.theme = &theme;
        context.width = width;
        context.zoom = 1.0;
        context.listNumber = numbers[i];
        context.listDepth = depths[i];
        context.baseDir = m_baseDir;
        context.references = &m_references;
        context.images = m_images;
        context.print = true;
        context.toc = &toc;
        out.push_back(renderBlock(m_blocks[i], context));
    }
    return out;
}

// ---- Painting ---------------------------------------------------------------------------------------

namespace {

void paintDecorations(QPainter &painter, const RenderedBlock &block, const EditorTheme &theme, qreal zoom,
                      bool foreground)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    for (const Decoration &decoration : block.decorations) {
        const QRectF rect = decoration.rect;
        switch (decoration.type) {
        case Decoration::CodeBox:
        case Decoration::MathBox:
            if (foreground)
                break;
            painter.setPen(QPen(theme.codeBorder, 1));
            painter.setBrush(theme.codeBackground);
            painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 3 * zoom, 3 * zoom);
            break;
        case Decoration::QuoteBar:
            if (!foreground)
                painter.fillRect(rect, theme.quoteBar);
            break;
        case Decoration::Rule:
            if (!foreground)
                painter.fillRect(rect, theme.rule);
            break;
        case Decoration::HeadingRule:
            if (!foreground)
                painter.fillRect(rect, theme.headingRule);
            break;
        case Decoration::Bullet: {
            if (!foreground)
                break;
            const int style = decoration.level % 3;
            if (style == 0) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(theme.text);
                painter.drawEllipse(rect);
            } else if (style == 1) {
                painter.setPen(QPen(theme.text, 1));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(rect.adjusted(0.5, 0.5, -0.5, -0.5));
            } else {
                painter.setPen(Qt::NoPen);
                painter.setBrush(theme.text);
                painter.drawRect(rect);
            }
            break;
        }
        case Decoration::Number: {
            if (!foreground)
                break;
            painter.setPen(theme.text);
            painter.setFont(theme.bodyFont(zoom));
            painter.drawText(QPointF(rect.left(), rect.bottom()), decoration.text);
            break;
        }
        case Decoration::Checkbox: {
            if (!foreground)
                break;
            const QRectF box = rect.adjusted(0.5, 0.5, -0.5, -0.5);
            if (decoration.checked) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(theme.dark ? theme.link : QColor(0x00, 0x75, 0xff));
                painter.drawRoundedRect(box, 2.5 * zoom, 2.5 * zoom);
                QPainterPath tick;
                tick.moveTo(box.left() + box.width() * 0.22, box.top() + box.height() * 0.52);
                tick.lineTo(box.left() + box.width() * 0.42, box.top() + box.height() * 0.72);
                tick.lineTo(box.left() + box.width() * 0.78, box.top() + box.height() * 0.3);
                painter.setPen(QPen(Qt::white, std::max(1.5, 1.8 * zoom), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.setBrush(Qt::NoBrush);
                painter.drawPath(tick);
            } else {
                painter.setPen(QPen(theme.dark ? theme.muted : QColor(0x76, 0x76, 0x76), 1));
                painter.setBrush(theme.background);
                painter.drawRoundedRect(box, 2.5 * zoom, 2.5 * zoom);
            }
            break;
        }
        case Decoration::Label: {
            if (!foreground)
                break;
            QFont font = theme.monoFont(zoom, 0.75);
            painter.setFont(font);
            painter.setPen(theme.marker);
            painter.drawText(rect, Qt::AlignRight | Qt::AlignBottom, decoration.text);
            break;
        }
        }
    }
    painter.restore();
}

void paintDocument(QPainter &painter, const RenderedBlock &block, const EditorTheme &theme, const QRectF &clip)
{
    painter.save();
    painter.translate(block.docOffset);
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, theme.text);
    context.clip = clip.translated(-block.docOffset);
    context.cursorPosition = -1;
    block.doc->documentLayout()->draw(&painter, context);
    painter.restore();
}

} // namespace

void MarkdownEditor::paintRendered(QPainter &painter, const RenderedBlock &block, const EditorTheme &theme,
                                   const QRectF &clip, qreal zoom)
{
    paintDecorations(painter, block, theme, zoom, false);
    paintDocument(painter, block, theme, clip);
    paintDecorations(painter, block, theme, zoom, true);
}

void MarkdownEditor::paintBlock(QPainter &painter, int index, const QPointF &origin, const QRect &clip)
{
    RenderedBlock *r = render(index);
    painter.save();
    painter.translate(origin);
    const QRectF localClip = QRectF(clip).translated(-origin);
    if (m_focusMode && index != m_caret.block)
        painter.setOpacity(0.32);

    paintDecorations(painter, *r, m_theme, m_zoom, false);

    // Search hits, then the selection, under the text.
    if (!m_search.isEmpty()) {
        const auto [from, to] = selectionRange();
        for (const Segment &match : matchesIn(index)) {
            const bool current = from.block == index && to.block == index && from.offset == match.start
                && to.offset == match.end;
            for (const QRectF &rect : r->rangeRects(match.start, match.end))
                painter.fillRect(rect, current ? m_theme.findCurrent : m_theme.findMatch);
        }
    }
    if (hasSelection()) {
        const auto [from, to] = selectionRange();
        if (index >= from.block && index <= to.block) {
            const int s = index == from.block ? from.offset : 0;
            const int e = index == to.block ? to.offset : int(m_blocks[index].text.size());
            QList<QRectF> rects = r->rangeRects(s, e);
            const bool whole = index > from.block && index < to.block;
            if (rects.isEmpty() && (whole || (s <= firstPos(index).offset && e >= lastPos(index).offset && s != e)))
                rects.append(QRectF(0, 0, columnWidth(), r->height));
            QColor selection = m_theme.selection;
            if (!hasFocus())
                selection.setAlpha(150);
            for (const QRectF &rect : std::as_const(rects))
                painter.fillRect(rect, selection);
        }
    }

    paintDocument(painter, *r, m_theme, localClip);
    paintDecorations(painter, *r, m_theme, m_zoom, true);

    if (index == m_caret.block && hasFocus() && m_caretVisible) {
        QRectF caret = r->caretRect(m_caret.offset, m_preedit.isEmpty() ? 0 : m_preeditCursor);
        caret.setWidth(std::max<qreal>(1.0, std::round(1.2 * m_zoom)));
        painter.fillRect(caret, m_theme.caret);
    }
    painter.restore();
}

void MarkdownEditor::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    painter.fillRect(event->rect(), m_theme.background);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const int scroll = verticalScrollBar()->value();
    const qreal left = columnLeft();
    const QRect clip = event->rect();
    for (int i = blockAtY(clip.top() + scroll - 50); i < m_blocks.size(); ++i) {
        const qreal y = m_ys.value(i) - scroll;
        if (y > clip.bottom())
            break;
        const RenderedBlock *r = render(i);
        if (y + r->height < clip.top())
            continue;
        paintBlock(painter, i, QPointF(left, y), clip);
    }
}

void MarkdownEditor::scrollContentsBy(int, int)
{
    viewport()->update();
    if (QInputMethod *im = QGuiApplication::inputMethod(); im && hasFocus())
        im->update(Qt::ImCursorRectangle);
}

void MarkdownEditor::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    relayout();
}

// ---- Input --------------------------------------------------------------------------------------------

bool MarkdownEditor::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(event);
        const Qt::KeyboardModifiers mods = key->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier);
        // Plain typing and editing keys belong to the editor, not to shortcuts.
        if (mods == Qt::NoModifier
            && (!key->text().isEmpty() || key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete
                || key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab || key->key() == Qt::Key_Return
                || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Home || key->key() == Qt::Key_End)) {
            event->accept();
            return true;
        }
    }
    return QAbstractScrollArea::event(event);
}

bool MarkdownEditor::focusNextPrevChild(bool)
{
    return false;
}

void MarkdownEditor::keyPressEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool shift = mods & Qt::ShiftModifier;
    const bool ctrl = mods & Qt::ControlModifier;
    const bool alt = mods & Qt::AltModifier;

    if (event->matches(QKeySequence::Undo)) {
        undo();
        return;
    }
    if (event->matches(QKeySequence::Redo)) {
        redo();
        return;
    }
    if (event->matches(QKeySequence::Copy)) {
        copy();
        return;
    }
    if (event->matches(QKeySequence::Cut)) {
        cut();
        return;
    }
    if (event->matches(QKeySequence::Paste)) {
        paste();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
        return;
    }

    const auto horizontal = [&](bool right) {
        if (hasSelection() && !shift) {
            const auto [from, to] = selectionRange();
            moveCaret(right ? to : from, false);
            return;
        }
        Pos target = right ? (ctrl ? nextWord(m_caret) : nextChar(m_caret))
                           : (ctrl ? previousWord(m_caret) : previousChar(m_caret));
        // Rules are passed over, unless selecting.
        if (!shift && target.block != m_caret.block && m_blocks[target.block].kind == BlockKind::Rule)
            target = right ? nextChar(lastPos(target.block)) : previousChar(firstPos(target.block));
        m_preferredX = -1;
        moveCaret(target, shift);
    };

    switch (event->key()) {
    case Qt::Key_Left:
        horizontal(false);
        return;
    case Qt::Key_Right:
        horizontal(true);
        return;
    case Qt::Key_Up:
    case Qt::Key_Down: {
        const int direction = event->key() == Qt::Key_Up ? -1 : 1;
        if (ctrl) {
            verticalScrollBar()->setValue(verticalScrollBar()->value() + direction * verticalScrollBar()->singleStep());
            return;
        }
        Pos target;
        const qreal preferred = m_preferredX;
        if (verticalTarget(m_caret, direction, target)) {
            const qreal keep = m_preferredX >= 0 ? m_preferredX : preferred;
            moveCaret(target, shift);
            m_preferredX = keep;
        } else if (direction > 0 && !shift && !isTextKind(m_blocks[m_caret.block].kind)
                   && m_caret.block == m_blocks.size() - 1) {
            // Below the last block, when it is not text: a new paragraph to type in.
            insertParagraph(true);
        }
        return;
    }
    case Qt::Key_Home:
        moveCaret(ctrl ? docStart() : lineStart(m_caret), shift);
        return;
    case Qt::Key_End:
        moveCaret(ctrl ? docEnd() : lineEnd(m_caret), shift);
        return;
    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        const int direction = event->key() == Qt::Key_PageUp ? -1 : 1;
        const QRectF caret = caretRectInViewport();
        QScrollBar *bar = verticalScrollBar();
        bar->setValue(bar->value() + direction * viewport()->height() * 9 / 10);
        moveCaret(positionAt(QPoint(int(caret.center().x()), int(caret.center().y()))), shift);
        return;
    }
    case Qt::Key_Backspace:
        backspace(ctrl);
        return;
    case Qt::Key_Delete:
        deleteForward(ctrl);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (ctrl) {
            if (inTable())
                tableInsertRow(true);
            else
                insertParagraph(true);
        } else if (shift) {
            softBreak();
        } else {
            enter();
        }
        return;
    case Qt::Key_Tab:
        tab(false);
        return;
    case Qt::Key_Backtab:
        tab(true);
        return;
    case Qt::Key_Escape:
        if (hasSelection())
            moveCaret(m_caret, false);
        return;
    default:
        break;
    }

    const QString text = event->text();
    if (!text.isEmpty() && (!ctrl || alt) && text.at(0).isPrint()) {
        insertText(text);
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void MarkdownEditor::inputMethodEvent(QInputMethodEvent *event)
{
    const bool hadPreedit = !m_preedit.isEmpty();
    m_preedit = event->preeditString();
    m_preeditCursor = int(m_preedit.size());
    for (const QInputMethodEvent::Attribute &attribute : event->attributes()) {
        if (attribute.type == QInputMethodEvent::Cursor)
            m_preeditCursor = attribute.start;
    }
    if (!event->commitString().isEmpty()) {
        if (event->replacementLength() > 0 && !hasSelection()) {
            const int from = std::max(0, m_caret.offset + event->replacementStart());
            m_anchor = {m_caret.block, from};
            m_caret = {m_caret.block, std::min(int(m_blocks[m_caret.block].text.size()), from + event->replacementLength())};
        }
        insertText(event->commitString());
    } else if (hadPreedit || !m_preedit.isEmpty()) {
        if (hasSelection() && !m_preedit.isEmpty())
            deleteSelection();
        relayout();
        ensureCaretVisible();
    }
    event->accept();
}

QVariant MarkdownEditor::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImCursorRectangle:
        return caretRectInViewport().toAlignedRect().translated(viewport()->pos());
    case Qt::ImFont:
        return m_theme.bodyFont(m_zoom);
    case Qt::ImCursorPosition:
        return m_caret.offset;
    case Qt::ImAnchorPosition:
        return m_anchor.block == m_caret.block ? m_anchor.offset : m_caret.offset;
    case Qt::ImSurroundingText:
        return m_blocks.value(m_caret.block).text;
    case Qt::ImCurrentSelection:
        return selectedMarkdown();
    case Qt::ImHints:
        return int(Qt::ImhMultiLine);
    default:
        return QAbstractScrollArea::inputMethodQuery(query);
    }
}

void MarkdownEditor::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    const QPoint point = event->position().toPoint();
    const qreal contentY = point.y() + verticalScrollBar()->value();
    const int index = blockAtY(contentY);
    RenderedBlock *r = render(index);
    const QPointF local(point.x() - columnLeft(), contentY - m_ys[index]);

    if (event->button() == Qt::LeftButton) {
        for (const Decoration &decoration : r->decorations) {
            if (decoration.type == Decoration::Checkbox && decoration.rect.adjusted(-4, -4, 4, 4).contains(local)) {
                toggleTask(index);
                return;
            }
        }
        if ((event->modifiers() & Qt::ControlModifier) || isTableOfContents(m_blocks[index])) {
            const QString href = r->anchorAt(local);
            if (!href.isEmpty()) {
                emit linkActivated(href);
                return;
            }
        }
        const bool near = (point - m_lastClickPos).manhattanLength() < QApplication::startDragDistance() * 2;
        if (m_clickCount == 2 && near && m_lastClick.elapsed() < QApplication::doubleClickInterval()) {
            // Third click: the whole block.
            m_clickCount = 3;
            m_anchor = firstPos(index);
            m_caret = lastPos(index);
            m_dragging = false;
            updateCaret();
            return;
        }
        m_clickCount = 1;
        m_lastClick.restart();
        m_lastClickPos = point;
        const Pos pos = positionAt(point);
        m_caret = pos;
        if (!(event->modifiers() & Qt::ShiftModifier))
            m_anchor = pos;
        m_preferredX = -1;
        m_dragging = true;
        m_lastMousePos = point;
        updateCaret();
        return;
    }
    if (event->button() == Qt::RightButton) {
        const Pos pos = positionAt(point);
        const auto [from, to] = selectionRange();
        if (!hasSelection() || pos < from || to < pos)
            moveCaret(pos, false);
    }
}

void MarkdownEditor::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPoint point = event->position().toPoint();
    const Pos pos = positionAt(point);
    const QString &text = m_blocks[pos.block].text;
    int start = pos.offset;
    int end = pos.offset;
    if (end < text.size() && isWordChar(text.at(end))) {
        while (start > 0 && isWordChar(text.at(start - 1)) && !isCjk(text.at(start - 1)) == !isCjk(text.at(end)))
            --start;
        while (end < text.size() && isWordChar(text.at(end)))
            ++end;
    } else if (start > 0 && isWordChar(text.at(start - 1))) {
        while (start > 0 && isWordChar(text.at(start - 1)))
            --start;
    }
    m_anchor = clamp({pos.block, start});
    m_caret = clamp({pos.block, end});
    m_clickCount = 2;
    m_lastClick.restart();
    m_lastClickPos = point;
    m_dragging = false;
    updateCaret();
}

void MarkdownEditor::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint point = event->position().toPoint();
    m_lastMousePos = point;
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        const Pos pos = positionAt(point);
        if (pos != m_caret) {
            m_caret = pos;
            updateCaret();
        }
        if (point.y() < 0 || point.y() > viewport()->height())
            m_autoScroll.start(40, this);
        else
            m_autoScroll.stop();
        return;
    }
    // A pointing hand over things that act on a click.
    const qreal contentY = point.y() + verticalScrollBar()->value();
    const int index = blockAtY(contentY);
    RenderedBlock *r = render(index);
    const QPointF local(point.x() - columnLeft(), contentY - m_ys.value(index));
    bool hand = false;
    for (const Decoration &decoration : r->decorations) {
        if (decoration.type == Decoration::Checkbox && decoration.rect.adjusted(-4, -4, 4, 4).contains(local))
            hand = true;
    }
    if (!hand && (event->modifiers() & Qt::ControlModifier) && !r->anchorAt(local).isEmpty())
        hand = true;
    const QString href = r->anchorAt(local);
    viewport()->setToolTip(href.isEmpty() ? QString() : href + tr("\nCtrl+Click to open"));
    viewport()->setCursor(hand ? Qt::PointingHandCursor : Qt::IBeamCursor);
}

void MarkdownEditor::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    m_dragging = false;
    m_autoScroll.stop();
    if (hasSelection() && QApplication::clipboard()->supportsSelection()) {
        auto *mime = new QMimeData;
        mime->setText(selectedMarkdown());
        QApplication::clipboard()->setMimeData(mime, QClipboard::Selection);
    }
}

void MarkdownEditor::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const int steps = event->angleDelta().y() > 0 ? 1 : event->angleDelta().y() < 0 ? -1 : 0;
        if (steps != 0)
            emit zoomRequested(steps);
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void MarkdownEditor::focusInEvent(QFocusEvent *event)
{
    QAbstractScrollArea::focusInEvent(event);
    m_caretVisible = true;
    m_blink.start(QApplication::cursorFlashTime() > 0 ? QApplication::cursorFlashTime() / 2 : 530, this);
    viewport()->update();
}

void MarkdownEditor::focusOutEvent(QFocusEvent *event)
{
    QAbstractScrollArea::focusOutEvent(event);
    m_blink.stop();
    viewport()->update();
}

void MarkdownEditor::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_blink.timerId()) {
        m_caretVisible = !m_caretVisible;
        const QRectF caret = caretRectInViewport();
        viewport()->update(caret.toAlignedRect().adjusted(-2, -2, 2, 2));
        return;
    }
    if (event->timerId() == m_autoScroll.timerId()) {
        QScrollBar *bar = verticalScrollBar();
        const int y = m_lastMousePos.y();
        const int delta = y < 0 ? y : y - viewport()->height();
        bar->setValue(bar->value() + delta / 2);
        const Pos pos = positionAt(QPoint(m_lastMousePos.x(), std::clamp(y, 0, viewport()->height())));
        if (pos != m_caret) {
            m_caret = pos;
            relayout();
            viewport()->update();
        }
        return;
    }
    QAbstractScrollArea::timerEvent(event);
}

void MarkdownEditor::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    if (m_contextMenu)
        m_contextMenu(&menu, viewport()->mapFrom(this, event->pos()));
    if (!menu.isEmpty())
        menu.exec(event->globalPos());
}

void MarkdownEditor::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() || event->mimeData()->hasText() || event->mimeData()->hasImage())
        event->acceptProposedAction();
}

void MarkdownEditor::dragMoveEvent(QDragMoveEvent *event)
{
    event->acceptProposedAction();
    const Pos pos = positionAt(event->position().toPoint());
    if (pos != m_caret) {
        m_caret = m_anchor = pos;
        relayout();
        viewport()->update();
    }
}

void MarkdownEditor::dropEvent(QDropEvent *event)
{
    const QMimeData *mime = event->mimeData();
    if (mime->hasUrls()) {
        QStringList documents;
        QList<QUrl> others;
        for (const QUrl &url : mime->urls()) {
            const QString suffix = QFileInfo(url.toLocalFile()).suffix().toLower();
            if (url.isLocalFile()
                && (suffix == QLatin1String("md") || suffix == QLatin1String("markdown") || suffix == QLatin1String("txt")))
                documents.append(url.toLocalFile());
            else
                others.append(url);
        }
        for (const QString &path : std::as_const(documents))
            emit openFileRequested(path);
        if (!others.isEmpty()) {
            QMimeData rest;
            rest.setUrls(others);
            m_caret = m_anchor = positionAt(event->position().toPoint());
            pasteMime(&rest, false);
        }
        event->acceptProposedAction();
        return;
    }
    m_caret = m_anchor = positionAt(event->position().toPoint());
    pasteMime(mime, false);
    event->acceptProposedAction();
    setFocus();
}

} // namespace ot
