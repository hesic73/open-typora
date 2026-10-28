#include "BlockRenderer.h"

#include "Highlighter.h"
#include "ImageStore.h"
#include "TextObjects.h"

#include <QAbstractTextDocumentLayout>
#include <QCache>
#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QTextTable>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace ot {

namespace {

constexpr QChar LineSeparator(0x2028);

QCache<QString, QList<InlineNode>> &inlineCache()
{
    static QCache<QString, QList<InlineNode>> cache(4000);
    return cache;
}

/// Where a block's layout has its origin, in document coordinates: line
/// positions are relative to it.
QPointF layoutOrigin(const QTextDocument *doc, const QTextBlock &block)
{
    return doc->documentLayout()->blockBoundingRect(block).topLeft();
}

QColor tokenColor(Token token, const EditorTheme &theme)
{
    switch (token) {
    case Token::Keyword: return theme.hlKeyword;
    case Token::String: return theme.hlString;
    case Token::Number: return theme.hlNumber;
    case Token::Comment: return theme.hlComment;
    case Token::Title: return theme.hlTitle;
    case Token::Attribute: return theme.hlAttribute;
    case Token::Added: return theme.hlAdded;
    case Token::Removed: return theme.hlRemoved;
    case Token::Plain: break;
    }
    return theme.codeText;
}

bool nodeHolds(const InlineNode &node, int caret)
{
    return caret >= 0 && caret >= node.start && caret <= node.end;
}

void collectReveal(const QList<InlineNode> &nodes, int caret, QString &key)
{
    for (const InlineNode &node : nodes) {
        if (node.type == InlineType::Text || !nodeHolds(node, caret))
            continue;
        key += QStringLiteral("%1-%2,").arg(node.start).arg(node.end);
        collectReveal(node.children, caret, key);
    }
}

/// Builds the laid-out document for one block.
class Builder
{
public:
    Builder(RenderedBlock &out, const Block &block, const RenderContext &context)
        : m_out(out)
        , m_block(block)
        , m_context(context)
        , m_theme(*context.theme)
        , m_zoom(context.zoom)
        , m_fontSize(context.theme->fontSize * context.zoom)
    {
        m_out.doc = std::make_unique<QTextDocument>();
        QTextDocument *doc = m_out.doc.get();
        doc->setUndoRedoEnabled(false);
        doc->setDocumentMargin(0);
        doc->setDefaultFont(m_theme.bodyFont(m_zoom));
        QTextOption option = doc->defaultTextOption();
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        doc->setDefaultTextOption(option);
        m_cursor = QTextCursor(doc);
    }

    void build()
    {
        switch (m_block.kind) {
        case BlockKind::Paragraph: paragraph(); break;
        case BlockKind::Heading: heading(); break;
        case BlockKind::ListItem: listItem(); break;
        case BlockKind::Quote: quote(); break;
        case BlockKind::CodeFence:
        case BlockKind::IndentedCode:
        case BlockKind::FrontMatter: code(); break;
        case BlockKind::MathBlock: mathBlock(); break;
        case BlockKind::Table: table(); break;
        case BlockKind::Rule: rule(); break;
        case BlockKind::Html: html(); break;
        }
        finish();
    }

private:
    RenderedBlock &m_out;
    const Block &m_block;
    const RenderContext &m_context;
    const EditorTheme &m_theme;
    qreal m_zoom;
    qreal m_fontSize;
    QTextCursor m_cursor;
    bool m_codeMode = false;
    qreal m_docWidth = 0;
    qreal m_bottomPad = 0;
    qreal m_rightPad = 0;
    int m_imageCount = 0;
    /// Fixed height, for blocks drawn without their document (a rule).
    qreal m_fixedHeight = -1;
    /// Set by kinds whose decorations need the finished layout.
    std::function<void()> m_decorate;

    bool active() const { return m_context.caret >= 0; }

    void setDocWidth(qreal left, qreal right)
    {
        m_out.docOffset.setX(left);
        m_rightPad = right;
        m_docWidth = std::max<qreal>(40, m_context.width - left - right);
        m_out.doc->setTextWidth(m_docWidth);
    }

    void addRun(int docPos, int docLength, int source, int sourceLength, bool atomic)
    {
        m_out.runs.append({docPos, docLength, source, sourceLength, atomic});
    }

    // ---- Formats ---------------------------------------------------------

    QTextCharFormat bodyFormat(const QColor &color = {}) const
    {
        QTextCharFormat format;
        format.setFont(m_theme.bodyFont(m_zoom));
        format.setForeground(color.isValid() ? color : m_theme.text);
        return format;
    }

    QTextCharFormat markerFormat(QTextCharFormat format) const
    {
        format.setForeground(m_theme.marker);
        format.setAnchor(false);
        format.setAnchorHref(QString());
        format.setFontUnderline(false);
        format.setFontStrikeOut(false);
        if (format.fontFixedPitch() == false)
            format.clearBackground();
        return format;
    }

    QTextCharFormat codeFormat(QTextCharFormat format) const
    {
        const QFont base = format.font();
        QFont mono = m_theme.monoFont(1.0);
        const int pixels = base.pixelSize() > 0 ? base.pixelSize() : qRound(m_fontSize);
        mono.setPixelSize(qRound(pixels * 0.9));
        mono.setBold(base.bold());
        mono.setItalic(base.italic());
        format.setFont(mono);
        format.setFontFixedPitch(true);
        format.setBackground(m_theme.inlineCodeBackground);
        format.setForeground(m_theme.inlineCodeText);
        return format;
    }

    QTextCharFormat linkFormat(QTextCharFormat format, const QString &href) const
    {
        format.setForeground(m_theme.link);
        format.setAnchor(true);
        format.setAnchorHref(href);
        format.setFontUnderline(true);
        format.setUnderlineColor(m_theme.link);
        return format;
    }

    QTextBlockFormat lineFormat(qreal lineHeight, Qt::Alignment align = Qt::AlignLeft) const
    {
        QTextBlockFormat format;
        format.setLineHeight(lineHeight, QTextBlockFormat::FixedHeight);
        format.setAlignment(align);
        return format;
    }

    qreal bodyLineHeight() const { return std::round(m_fontSize * m_theme.lineHeight); }

    // ---- Inserting text ----------------------------------------------------

    /// Block text [from, to), one to one.
    void source(int from, int to, const QTextCharFormat &format)
    {
        const int position = m_cursor.position();
        if (to <= from) {
            addRun(position, 0, from, 0, false);
            return;
        }
        QString text = m_block.text.mid(from, to - from);
        if (!m_codeMode)
            text.replace(QLatin1Char('\n'), LineSeparator);
        m_cursor.insertText(text, format);
        addRun(position, int(text.size()), from, int(text.size()), false);
    }

    /// Content [from, to) of a content map, split where the source jumps.
    void content(const ContentMap &map, int from, int to, const QTextCharFormat &format)
    {
        if (from >= to) {
            addRun(m_cursor.position(), 0, map.map(from), 0, false);
            return;
        }
        int i = from;
        while (i < to) {
            int j = i + 1;
            while (j < to && map.sourceOf[j] == map.sourceOf[j - 1] + 1)
                ++j;
            source(map.sourceOf[i], map.sourceOf[j - 1] + 1, format);
            i = j;
        }
    }

    void atomic(const QString &display, int sourceFrom, int sourceTo, const QTextCharFormat &format)
    {
        const int position = m_cursor.position();
        m_cursor.insertText(display, format);
        addRun(position, int(display.size()), sourceFrom, sourceTo - sourceFrom, true);
    }

    bool math(const QString &latex, bool display, const QTextCharFormat &format, int sourceFrom, int sourceTo,
              qreal sizeFactor = 0)
    {
        const int position = m_cursor.position();
        QTextCharFormat plain = format;
        plain.setAnchor(false);
        plain.clearBackground();
        if (!insertMath(m_cursor, latex, display, plain, m_docWidth, sizeFactor))
            return false;
        addRun(position, m_cursor.position() - position, sourceFrom, sourceTo - sourceFrom, true);
        return true;
    }

    /// A picture; when it cannot be shown, its description in its place.
    void image(const QString &target, const QString &alt, const QString &width, const QTextCharFormat &format,
               int sourceFrom, int sourceTo)
    {
        m_out.hasImages = true;
        ImageStore::State state = ImageStore::State::Failed;
        const QImage picture = m_context.images ? m_context.images->image(target, m_context.baseDir, &state) : QImage();
        if (picture.isNull()) {
            QTextCharFormat note = format;
            note.setForeground(m_theme.muted);
            note.setFontItalic(true);
            QString label = alt.isEmpty() ? target : alt;
            if (state == ImageStore::State::Loading) {
                m_out.waitingForImages = true;
                label = QStringLiteral("⧗ ") + label;
            } else {
                label = QStringLiteral("⚠ ") + label;
            }
            atomic(label, sourceFrom, sourceTo, note);
            return;
        }
        const QString name = QStringLiteral("ot-image://%1").arg(++m_imageCount);
        m_out.doc->addResource(QTextDocument::ImageResource, QUrl(name), picture);
        qreal w = picture.width() / picture.devicePixelRatio();
        qreal h = picture.height() / picture.devicePixelRatio();
        const QString given = width.trimmed();
        if (!given.isEmpty()) {
            bool ok = false;
            qreal value = 0;
            if (given.endsWith(QLatin1Char('%'))) {
                value = given.chopped(1).toDouble(&ok) / 100.0 * m_docWidth;
            } else {
                QString number = given;
                if (number.endsWith(QLatin1String("px")))
                    number.chop(2);
                value = number.toDouble(&ok) * m_zoom;
            }
            if (ok && value > 0) {
                h = h * value / w;
                w = value;
            }
        }
        const qreal limit = m_docWidth - 2;
        if (w > limit) {
            h = h * limit / w;
            w = limit;
        }
        QTextImageFormat imageFormat;
        imageFormat.merge(format);
        imageFormat.setName(name);
        imageFormat.setWidth(w);
        imageFormat.setHeight(h);
        imageFormat.clearBackground();
        imageFormat.setVerticalAlignment(QTextCharFormat::AlignBaseline);
        const int position = m_cursor.position();
        m_cursor.insertImage(imageFormat);
        addRun(position, 1, sourceFrom, sourceTo - sourceFrom, true);
    }

    // ---- Inline Markdown -----------------------------------------------------

    void inlineNodes(const ContentMap &map, const QList<InlineNode> &nodes, const QTextCharFormat &format, int caret)
    {
        for (const InlineNode &node : nodes) {
            const bool shown = nodeHolds(node, caret);
            switch (node.type) {
            case InlineType::Text:
            case InlineType::SoftBreak:
                content(map, node.start, node.end, format);
                break;
            case InlineType::HardBreak:
            case InlineType::Escape:
                if (shown)
                    content(map, node.start, node.innerStart, markerFormat(format));
                content(map, node.innerStart, node.innerEnd, format);
                break;
            case InlineType::Entity:
                if (shown)
                    content(map, node.start, node.end, format);
                else
                    atomic(node.target, map.map(node.start), map.map(node.end), format);
                break;
            case InlineType::Emph:
            case InlineType::Strong:
            case InlineType::Strike:
            case InlineType::Highlight:
            case InlineType::HtmlSpan: {
                const QTextCharFormat styled = styledFormat(node, format);
                if (shown)
                    content(map, node.start, node.innerStart, markerFormat(styled));
                inlineNodes(map, node.children, styled, caret);
                if (shown)
                    content(map, node.innerEnd, node.end, markerFormat(styled));
                break;
            }
            case InlineType::Code: {
                const QTextCharFormat code = codeFormat(format);
                int from = node.innerStart;
                int to = node.innerEnd;
                // "`` `x` ``": one space inside each end is padding, not code.
                const QStringView inner = QStringView(map.text).mid(from, to - from);
                if (!shown && inner.size() > 2 && inner.startsWith(QLatin1Char(' ')) && inner.endsWith(QLatin1Char(' '))
                    && !inner.trimmed().isEmpty()) {
                    ++from;
                    --to;
                }
                if (shown)
                    content(map, node.start, node.innerStart, markerFormat(code));
                content(map, from, to, code);
                if (shown)
                    content(map, node.innerEnd, node.end, markerFormat(code));
                break;
            }
            case InlineType::Math: {
                const bool display = node.title == QLatin1String("display");
                if (shown) {
                    QTextCharFormat code = codeFormat(format);
                    code.clearBackground();
                    content(map, node.start, node.innerStart, markerFormat(code));
                    content(map, node.innerStart, node.innerEnd, code);
                    content(map, node.innerEnd, node.end, markerFormat(code));
                    // The formula as it will look, after its source.
                    if (!m_context.print) {
                        QTextCharFormat preview = format;
                        preview.setForeground(m_theme.muted);
                        const int at = map.map(node.end);
                        const int position = m_cursor.position();
                        m_cursor.insertText(QStringLiteral(" "), format);
                        if (insertMath(m_cursor, node.target, display, preview, m_docWidth))
                            addRun(position, m_cursor.position() - position, at, 0, true);
                        else {
                            m_cursor.setPosition(position);
                            m_cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
                            m_cursor.removeSelectedText();
                        }
                    }
                } else if (!math(node.target, display, format, map.map(node.start), map.map(node.end))) {
                    QTextCharFormat error = format;
                    error.setForeground(m_theme.error);
                    content(map, node.start, node.end, error);
                }
                break;
            }
            case InlineType::Link: {
                const QTextCharFormat link = linkFormat(format, node.target);
                if (shown)
                    content(map, node.start, node.innerStart, markerFormat(format));
                inlineNodes(map, node.children, link, caret);
                if (shown)
                    content(map, node.innerEnd, node.end, markerFormat(format));
                break;
            }
            case InlineType::Image: {
                const QString alt = plainText(map.text, node.children);
                if (shown) {
                    content(map, node.start, node.end, markerFormat(format));
                    const int at = map.map(node.end);
                    const int before = int(m_out.runs.size());
                    m_cursor.insertText(QString(LineSeparator), format);
                    addRun(m_cursor.position() - 1, 1, at, 0, true);
                    image(node.target, alt, QString(), format, at, at);
                    if (m_out.runs.size() > before + 1) {
                        // The picture and the break before it are one unit.
                        Run &brk = m_out.runs[before];
                        brk.docLength += m_out.runs.last().docLength;
                        m_out.runs.removeLast();
                    }
                } else {
                    image(node.target, alt, QString(), format, map.map(node.start), map.map(node.end));
                }
                break;
            }
            case InlineType::AutoLink: {
                const QTextCharFormat link = linkFormat(format, node.target);
                if (shown)
                    content(map, node.start, node.end, link);
                else
                    content(map, node.innerStart, node.innerEnd, link);
                break;
            }
            case InlineType::HtmlTag:
                if (shown) {
                    content(map, node.start, node.end, markerFormat(format));
                } else if (node.target == QLatin1String("br")) {
                    atomic(QString(LineSeparator), map.map(node.start), map.map(node.end), format);
                } else if (node.target == QLatin1String("img")) {
                    image(htmlAttribute(node.title, QStringLiteral("src")), htmlAttribute(node.title, QStringLiteral("alt")),
                          htmlAttribute(node.title, QStringLiteral("width")), format, map.map(node.start),
                          map.map(node.end));
                } else if (node.target == QLatin1String("!--")) {
                    QTextCharFormat comment = format;
                    comment.setForeground(m_theme.marker);
                    content(map, node.start, node.end, comment);
                } else {
                    // Other tags are invisible; a zero-width run keeps the caret mapping whole.
                    addRun(m_cursor.position(), 0, map.map(node.end), 0, false);
                }
                break;
            case InlineType::FootnoteRef: {
                if (shown) {
                    content(map, node.start, node.end, markerFormat(format));
                } else {
                    QTextCharFormat note = linkFormat(format, QStringLiteral("#fn:") + node.target);
                    note.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
                    note.setFontUnderline(false);
                    atomic(node.target, map.map(node.start), map.map(node.end), note);
                }
                break;
            }
            }
        }
    }

    QTextCharFormat styledFormat(const InlineNode &node, QTextCharFormat format) const
    {
        switch (node.type) {
        case InlineType::Emph: format.setFontItalic(true); break;
        case InlineType::Strong: format.setFontWeight(QFont::Bold); break;
        case InlineType::Strike: format.setFontStrikeOut(true); break;
        case InlineType::Highlight:
            format.setBackground(m_theme.highlightBackground);
            if (m_theme.dark)
                format.setForeground(QColor(0xff, 0xff, 0xff));
            break;
        case InlineType::HtmlSpan: {
            const QString &tag = node.target;
            if (tag == QLatin1String("u") || tag == QLatin1String("ins"))
                format.setFontUnderline(true);
            else if (tag == QLatin1String("b") || tag == QLatin1String("strong"))
                format.setFontWeight(QFont::Bold);
            else if (tag == QLatin1String("i") || tag == QLatin1String("em"))
                format.setFontItalic(true);
            else if (tag == QLatin1String("s") || tag == QLatin1String("del") || tag == QLatin1String("strike"))
                format.setFontStrikeOut(true);
            else if (tag == QLatin1String("sup"))
                format.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
            else if (tag == QLatin1String("sub"))
                format.setVerticalAlignment(QTextCharFormat::AlignSubScript);
            else if (tag == QLatin1String("mark"))
                format.setBackground(m_theme.highlightBackground);
            else if (tag == QLatin1String("kbd") || tag == QLatin1String("code"))
                format = codeFormat(format);
            else if (tag == QLatin1String("a")) {
                const QString href = htmlAttribute(node.title, QStringLiteral("href"));
                if (!href.isEmpty())
                    format = linkFormat(format, href);
            } else if (tag == QLatin1String("font") || tag == QLatin1String("span")) {
                QString color = htmlAttribute(node.title, QStringLiteral("color"));
                const QString style = htmlAttribute(node.title, QStringLiteral("style"));
                const qsizetype at = style.indexOf(QLatin1String("color:"));
                if (color.isEmpty() && at >= 0 && (at == 0 || (!style.at(at - 1).isLetter() && style.at(at - 1) != QLatin1Char('-'))))
                    color = style.mid(at + 6).section(QLatin1Char(';'), 0, 0).trimmed();
                const QColor parsed = QColor::fromString(color);
                if (parsed.isValid())
                    format.setForeground(parsed);
            }
            break;
        }
        default: break;
        }
        return format;
    }

    void inlineContent(const ContentMap &map, const QTextCharFormat &format)
    {
        const QList<InlineNode> nodes = cachedInline(map.text, m_context.references);
        const int caret = active() ? map.fromSource(m_context.caret) : -1;
        inlineNodes(map, nodes, format, caret);
    }

    // ---- Kinds ---------------------------------------------------------------

    void paragraph()
    {
        setDocWidth(0, 0);
        m_cursor.setBlockFormat(lineFormat(bodyLineHeight()));
        m_out.marginTop = m_out.marginBottom = std::round(0.8 * m_fontSize);
        if (m_context.toc && isTableOfContents(m_block)) {
            tableOfContents();
            return;
        }
        inlineContent(contentMap(m_block), bodyFormat());
    }

    /// The headings of the document, each a link to its heading, indented by level.
    void tableOfContents()
    {
        const int length = int(m_block.text.size());
        if (active())
            source(0, length, markerFormat(bodyFormat()));
        const QList<TocEntry> &entries = *m_context.toc;
        int top = 6;
        for (const TocEntry &entry : entries)
            top = std::min(top, entry.level);
        const int start = m_cursor.position();
        bool first = !active();
        for (const TocEntry &entry : entries) {
            QTextBlockFormat line = lineFormat(bodyLineHeight());
            line.setLeftMargin(std::round(24 * m_zoom) * (entry.level - top));
            if (first)
                m_cursor.setBlockFormat(line);
            else
                m_cursor.insertBlock(line);
            first = false;
            QTextCharFormat link = linkFormat(bodyFormat(), QLatin1Char('#') + entry.title);
            link.setFontUnderline(false);
            m_cursor.insertText(entry.title.isEmpty() ? QStringLiteral(" ") : entry.title, link);
        }
        if (entries.isEmpty()) {
            QTextCharFormat note = bodyFormat(m_theme.muted);
            note.setFontItalic(true);
            if (active())
                m_cursor.insertBlock(lineFormat(bodyLineHeight()));
            m_cursor.insertText(QStringLiteral("(no headings yet)"), note);
        }
        // All of it stands for the source as one unit: the caret goes to its end.
        addRun(start, m_cursor.position() - start, active() ? length : 0, active() ? 0 : length, true);
    }

    void heading()
    {
        setDocWidth(0, 0);
        const int level = headingLevel(m_block);
        static const qreal lineHeights[] = {1.2, 1.2, 1.225, 1.43, 1.4, 1.4, 1.4};
        QTextCharFormat format;
        const QFont font = m_theme.headingFont(level, m_zoom);
        format.setFont(font);
        format.setForeground(level == 6 ? m_theme.muted : m_theme.heading);
        m_cursor.setBlockFormat(lineFormat(std::round(font.pixelSize() * lineHeights[level])));
        // As in Typora, the "#" marks stay hidden while editing: the level is set with Ctrl+1..6.
        inlineContent(contentMap(m_block), format);
        m_out.marginTop = std::round(1.0 * m_fontSize + (level <= 2 ? 0.25 * m_fontSize : 0));
        m_out.marginBottom = std::round(1.0 * m_fontSize);
        if (level <= m_theme.headingRuleLevels) {
            const qreal pad = std::round(0.3 * font.pixelSize());
            m_bottomPad = pad + 1;
            m_decorate = [this] {
                Decoration line;
                line.type = Decoration::HeadingRule;
                line.rect = QRectF(0, m_out.height - 1, m_context.width, 1);
                m_out.decorations.append(line);
            };
        }
    }

    void listItem()
    {
        const ListInfo info = listInfo(m_block.text);
        const qreal indent = std::round(30 * m_zoom) * (m_context.listDepth + 1);
        setDocWidth(indent, 0);
        m_cursor.setBlockFormat(lineFormat(bodyLineHeight()));
        m_out.marginTop = m_out.marginBottom = std::round(0.8 * m_fontSize);
        inlineContent(contentMap(m_block), bodyFormat());
        const int depth = m_context.listDepth;
        const int number = m_context.listNumber;
        m_decorate = [this, info, indent, depth, number] {
            const QTextBlock first = m_out.doc->begin();
            const QTextLayout *layout = first.layout();
            if (!layout || layout->lineCount() == 0)
                return;
            const QTextLine line = layout->lineAt(0);
            const QPointF origin = layoutOrigin(m_out.doc.get(), first);
            const qreal baseline = m_out.docOffset.y() + origin.y() + line.y() + line.ascent();
            const QFontMetricsF metrics(m_theme.bodyFont(m_zoom));
            Decoration mark;
            mark.level = depth;
            if (info.task) {
                const qreal size = std::round(14 * m_zoom);
                const qreal centreY = baseline - metrics.xHeight() / 2.0;
                mark.type = Decoration::Checkbox;
                mark.checked = info.checked;
                mark.rect = QRectF(indent - std::round(8 * m_zoom) - size, centreY - size / 2.0, size, size);
            } else if (info.ordered) {
                mark.type = Decoration::Number;
                mark.text = QString::number(number) + info.delimiter;
                const qreal width = metrics.horizontalAdvance(mark.text);
                // The rectangle ends on the baseline, where the number is drawn.
                mark.rect = QRectF(indent - std::round(6 * m_zoom) - width, baseline - metrics.ascent(), width,
                                   metrics.ascent());
            } else {
                const qreal size = std::max<qreal>(4, std::round(metrics.xHeight() * 0.62));
                const qreal centreY = baseline - metrics.xHeight() / 2.0;
                mark.type = Decoration::Bullet;
                mark.rect = QRectF(indent - std::round(15 * m_zoom) - size / 2.0, centreY - size / 2.0, size, size);
            }
            m_out.decorations.append(mark);
        };
    }

    void quote()
    {
        const ContentMap map = contentMap(m_block);
        int depth = 1;
        for (int d : map.lineDepth)
            depth = std::max(depth, d);
        const qreal step = std::round(19 * m_zoom);
        setDocWidth(step * depth, std::round(15 * m_zoom));
        m_cursor.setBlockFormat(lineFormat(bodyLineHeight()));
        m_out.marginTop = m_out.marginBottom = std::round(0.8 * m_fontSize);
        inlineContent(map, bodyFormat(m_theme.quoteText));
        m_decorate = [this, depth, step] {
            for (int d = 0; d < depth; ++d) {
                Decoration bar;
                bar.type = Decoration::QuoteBar;
                bar.rect = QRectF(d * step, 0, std::round(4 * m_zoom), m_out.height);
                m_out.decorations.append(bar);
            }
        };
    }

    void codeText(int from, int to, const QString &language, const QTextCharFormat &format)
    {
        if (to <= from) {
            addRun(m_cursor.position(), 0, from, 0, false);
            return;
        }
        const QString code = m_block.text.mid(from, to - from);
        const QList<TokenSpan> spans = highlight(code, language);
        int at = 0;
        for (const TokenSpan &span : spans) {
            if (span.start > at)
                source(from + at, from + int(span.start), format);
            QTextCharFormat colored = format;
            colored.setForeground(tokenColor(span.token, m_theme));
            if (span.token == Token::Comment)
                colored.setFontItalic(false);
            source(from + int(span.start), from + int(span.start + span.length), colored);
            at = int(span.start + span.length);
        }
        if (at < code.size())
            source(from + at, to, format);
    }

    void code()
    {
        m_codeMode = true;
        const qreal padX = std::round(12 * m_zoom);
        const qreal padTop = std::round(8 * m_zoom);
        m_out.docOffset.setY(padTop);
        m_bottomPad = std::round(8 * m_zoom);
        setDocWidth(padX, padX);
        const QFont mono = m_theme.monoFont(m_zoom);
        QTextCharFormat format;
        format.setFont(mono);
        format.setForeground(m_theme.codeText);
        m_out.doc->setDefaultFont(mono);
        QTextOption option = m_out.doc->defaultTextOption();
        option.setTabStopDistance(QFontMetricsF(mono).horizontalAdvance(QLatin1Char(' ')) * 4);
        m_out.doc->setDefaultTextOption(option);
        m_cursor.setBlockFormat(lineFormat(std::round(mono.pixelSize() * 1.5)));
        m_out.marginTop = m_out.marginBottom = std::round(15 * m_zoom);

        Segment code{0, int(m_block.text.size())};
        QString language;
        if (m_block.kind == BlockKind::CodeFence) {
            const FenceInfo info = fenceInfo(m_block.text);
            code = info.code;
            language = info.language;
        } else if (m_block.kind == BlockKind::FrontMatter) {
            const int first = int(m_block.text.indexOf(QLatin1Char('\n')));
            const int last = int(m_block.text.lastIndexOf(QLatin1Char('\n')));
            code = first < last ? Segment{first + 1, last} : Segment{first, first};
            language = QStringLiteral("yaml");
        }
        if (active() && code.start > 0)
            source(0, code.start, markerFormat(format));
        codeText(code.start, code.end, language, format);
        if (active() && code.end < m_block.text.size())
            source(code.end, int(m_block.text.size()), markerFormat(format));

        const bool showLanguage = active() && m_block.kind == BlockKind::CodeFence;
        m_decorate = [this, language, showLanguage] {
            Decoration box;
            box.type = Decoration::CodeBox;
            box.rect = QRectF(0, 0, m_context.width, m_out.height);
            m_out.decorations.prepend(box);
            if (showLanguage) {
                Decoration label;
                label.type = Decoration::Label;
                label.text = language.isEmpty() ? QStringLiteral("plain text") : language;
                label.rect = QRectF(0, 0, m_context.width - std::round(8 * m_zoom), m_out.height - std::round(4 * m_zoom));
                m_out.decorations.append(label);
            }
        };
    }

    void mathBlock()
    {
        const MathInfo info = mathInfo(m_block.text);
        const QString latex = m_block.text.mid(info.latex.start, info.latex.end - info.latex.start).trimmed();
        m_out.marginTop = m_out.marginBottom = std::round(0.8 * m_fontSize);
        QTextCharFormat format = bodyFormat();
        if (active()) {
            m_codeMode = true;
            const qreal padX = std::round(12 * m_zoom);
            m_out.docOffset.setY(std::round(8 * m_zoom));
            m_bottomPad = std::round(8 * m_zoom);
            setDocWidth(padX, padX);
            const QFont mono = m_theme.monoFont(m_zoom);
            QTextCharFormat code;
            code.setFont(mono);
            code.setForeground(m_theme.codeText);
            m_cursor.setBlockFormat(lineFormat(std::round(mono.pixelSize() * 1.5)));
            source(0, info.latex.start, markerFormat(code));
            source(info.latex.start, info.latex.end, code);
            source(info.latex.end, int(m_block.text.size()), markerFormat(code));
            const int sourceBlocks = m_out.doc->blockCount();
            // The formula as it will look, under its source.
            const int position = m_cursor.position();
            QTextBlockFormat preview = lineFormat(bodyLineHeight(), Qt::AlignHCenter);
            preview.setLineHeight(0, QTextBlockFormat::SingleHeight);
            preview.setTopMargin(std::round(10 * m_zoom));
            m_cursor.insertBlock(preview);
            if (latex.isEmpty() || !insertMath(m_cursor, latex, true, format, m_docWidth)) {
                QTextCharFormat note = bodyFormat(latex.isEmpty() ? m_theme.muted : m_theme.error);
                note.setFontItalic(true);
                m_cursor.insertText(latex.isEmpty() ? QStringLiteral("(empty formula)") : QStringLiteral("(cannot typeset this formula)"),
                                    note);
            }
            addRun(position, m_cursor.position() - position, int(m_block.text.size()), 0, true);
            m_decorate = [this, sourceBlocks] {
                QRectF area;
                QTextBlock block = m_out.doc->begin();
                for (int i = 0; i < sourceBlocks && block.isValid(); ++i, block = block.next())
                    area |= m_out.doc->documentLayout()->blockBoundingRect(block);
                Decoration box;
                box.type = Decoration::MathBox;
                box.rect = QRectF(0, 0, m_context.width, area.bottom() + m_out.docOffset.y() + std::round(8 * m_zoom));
                m_out.decorations.prepend(box);
            };
            return;
        }
        setDocWidth(0, 0);
        QTextBlockFormat centred;
        centred.setAlignment(Qt::AlignHCenter);
        m_cursor.setBlockFormat(centred);
        if (latex.isEmpty()) {
            QTextCharFormat note = bodyFormat(m_theme.marker);
            atomic(QStringLiteral("$$ $$"), info.latex.start, info.latex.start, note);
            return;
        }
        if (!math(latex, true, format, info.latex.start, info.latex.end)) {
            QTextCharFormat error = m_theme.monoFont(m_zoom).toString().isEmpty() ? format : format;
            error.setFont(m_theme.monoFont(m_zoom));
            error.setForeground(m_theme.error);
            m_codeMode = true;
            source(0, int(m_block.text.size()), error);
        }
    }

    void table()
    {
        setDocWidth(0, 0);
        const TableInfo info = tableInfo(m_block.text);
        const int columns = std::max(1, info.columns);
        const int rows = std::max(1, int(info.rows.size()));
        m_out.marginTop = m_out.marginBottom = std::round(0.8 * m_fontSize);

        QTextTableFormat format;
        format.setBorder(1);
        format.setBorderBrush(m_theme.tableBorder);
        format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        format.setBorderCollapse(true);
        format.setCellSpacing(0);
        format.setCellPadding(0);
        format.setMargin(0);
        format.setWidth(QTextLength(QTextLength::PercentageLength, 100));
        QTextTable *table = m_cursor.insertTable(rows, columns, format);

        const qreal padX = std::round(13 * m_zoom);
        const qreal padY = std::round(6 * m_zoom);
        for (int r = 0; r < rows; ++r) {
            const TableInfo::Row row = r < info.rows.size() ? info.rows[r] : TableInfo::Row{};
            for (int c = 0; c < columns; ++c) {
                QTextTableCell cell = table->cellAt(r, c);
                QTextTableCellFormat cellFormat = cell.format().toTableCellFormat();
                cellFormat.setLeftPadding(padX);
                cellFormat.setRightPadding(padX);
                cellFormat.setTopPadding(padY);
                cellFormat.setBottomPadding(padY);
                if (r == 0)
                    cellFormat.setBackground(m_theme.tableHeaderBackground);
                else if (r % 2 == 0)
                    cellFormat.setBackground(m_theme.tableStripe);
                cell.setFormat(cellFormat);

                m_cursor = cell.firstCursorPosition();
                const Align align = c < info.aligns.size() ? info.aligns[c] : Align::None;
                const Qt::Alignment alignment = align == Align::Center ? Qt::AlignHCenter
                    : align == Align::Right                         ? Qt::AlignRight
                                                                    : Qt::AlignLeft;
                m_cursor.setBlockFormat(lineFormat(bodyLineHeight(), alignment));
                if (c >= row.cells.size())
                    continue;
                const Segment segment = row.cells[c];
                ContentMap map;
                map.text = m_block.text.mid(segment.start, segment.end - segment.start);
                for (int i = segment.start; i <= segment.end; ++i)
                    map.sourceOf.append(i);
                QTextCharFormat text = bodyFormat();
                if (r == 0)
                    text.setFontWeight(QFont::Bold);
                const QList<InlineNode> nodes = cachedInline(map.text, m_context.references);
                const bool inCell = active() && m_context.caret >= segment.start && m_context.caret <= segment.end;
                inlineNodes(map, nodes, text, inCell ? m_context.caret - segment.start : -1);
            }
        }
        // Qt keeps an empty paragraph before and after a table; they should take no room.
        for (QTextBlock block = m_out.doc->begin(); block.isValid(); block = block.next()) {
            if (m_out.doc->rootFrame() != QTextCursor(block).currentFrame())
                continue;
            QTextCursor collapse(block);
            QTextBlockFormat tiny;
            tiny.setLineHeight(0.01, QTextBlockFormat::FixedHeight);
            collapse.setBlockFormat(tiny);
            QTextCharFormat small;
            QFont font = m_theme.bodyFont(m_zoom);
            font.setPixelSize(1);
            small.setFont(font);
            collapse.setBlockCharFormat(small);
        }
    }

    void rule()
    {
        setDocWidth(0, 0);
        m_out.marginTop = m_out.marginBottom = std::round(16 * m_zoom);
        if (active()) {
            m_cursor.setBlockFormat(lineFormat(bodyLineHeight()));
            source(0, int(m_block.text.size()), markerFormat(bodyFormat()));
            return;
        }
        addRun(0, 0, 0, int(m_block.text.size()), true);
        m_fixedHeight = std::max<qreal>(2, std::round(2 * m_zoom));
        m_decorate = [this] {
            Decoration line;
            line.type = Decoration::Rule;
            line.rect = QRectF(0, 0, m_context.width, m_out.height);
            m_out.decorations.append(line);
        };
    }

    void html()
    {
        setDocWidth(0, 0);
        m_out.marginTop = m_out.marginBottom = std::round(0.8 * m_fontSize);
        m_cursor.setBlockFormat(lineFormat(bodyLineHeight()));
        if (active()) {
            QTextCharFormat format;
            format.setFont(m_theme.monoFont(m_zoom));
            format.setForeground(m_theme.muted);
            source(0, int(m_block.text.size()), format);
            return;
        }
        ContentMap map = contentMap(m_block);
        const QList<InlineNode> nodes = cachedInline(map.text, m_context.references);
        inlineNodes(map, nodes, bodyFormat(), -1);
    }

    void finish()
    {
        QTextDocument *doc = m_out.doc.get();
        // Lines holding a formula or picture grow to fit it; other lines keep the fixed height.
        for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
            if (!block.text().contains(QChar::ObjectReplacementCharacter))
                continue;
            QTextBlockFormat format = block.blockFormat();
            if (format.lineHeightType() == QTextBlockFormat::FixedHeight) {
                format.setLineHeight(format.lineHeight(), QTextBlockFormat::MinimumHeight);
                QTextCursor(block).setBlockFormat(format);
            }
        }
        const qreal docHeight = doc->documentLayout()->documentSize().height();
        m_out.bottomPad = m_bottomPad;
        if (m_fixedHeight >= 0)
            m_out.height = m_fixedHeight;
        else
            m_out.height = std::ceil(m_out.docOffset.y() + docHeight + m_bottomPad);
        if (m_decorate)
            m_decorate();
    }
};

/// The rectangle of a whole line, as tall as the line's share of the block.
QRectF lineBox(const QTextBlock &block, const QTextLine &line)
{
    const QTextBlockFormat format = block.blockFormat();
    const qreal lineHeight = format.lineHeight();
    switch (format.lineHeightType()) {
    case QTextBlockFormat::FixedHeight: {
        const qreal baseline = line.y() + line.ascent();
        return QRectF(line.x(), baseline - lineHeight * 0.8, line.width(), lineHeight);
    }
    case QTextBlockFormat::MinimumHeight: {
        const qreal height = std::max(lineHeight, line.height());
        return QRectF(line.x(), line.y() + line.height() - height, line.width(), height);
    }
    default:
        return QRectF(line.x(), line.y(), line.width(), line.height());
    }
}

} // namespace

int RenderedBlock::toDoc(int source) const
{
    for (const Run &run : runs) {
        const int end = run.sourcePos + run.sourceLength;
        if (!run.atomic) {
            if (source >= run.sourcePos && source <= end)
                return run.docPos + (source - run.sourcePos);
        } else {
            if (source == run.sourcePos)
                return run.docPos;
            if (source == end)
                return run.docPos + run.docLength;
            if (source > run.sourcePos && source < end)
                return source - run.sourcePos < end - source ? run.docPos : run.docPos + run.docLength;
        }
    }
    int best = runs.isEmpty() ? 0 : runs.first().docPos;
    int bestEnd = -1;
    for (const Run &run : runs) {
        const int end = run.sourcePos + run.sourceLength;
        if (end <= source && end >= bestEnd) {
            bestEnd = end;
            best = run.docPos + run.docLength;
        }
    }
    return best;
}

int RenderedBlock::toSource(int docPos) const
{
    // A position where one run ends and the next begins belongs to the next.
    for (const Run &run : runs) {
        if (docPos >= run.docPos && docPos < run.docPos + run.docLength) {
            if (!run.atomic)
                return run.sourcePos + (docPos - run.docPos);
            if (docPos == run.docPos)
                return run.sourcePos;
            const bool nearStart = docPos - run.docPos < run.docPos + run.docLength - docPos;
            return nearStart ? run.sourcePos : run.sourcePos + run.sourceLength;
        }
    }
    for (const Run &run : runs) {
        if (docPos == run.docPos + run.docLength)
            return run.sourcePos + run.sourceLength;
    }
    int best = 0;
    int distance = std::numeric_limits<int>::max();
    for (const Run &run : runs) {
        const int before = std::abs(docPos - run.docPos);
        const int after = std::abs(docPos - (run.docPos + run.docLength));
        if (before < distance) {
            distance = before;
            best = run.sourcePos;
        }
        if (after < distance) {
            distance = after;
            best = run.sourcePos + run.sourceLength;
        }
    }
    return best;
}

QRectF RenderedBlock::caretRect(int source, int extra) const
{
    const int position = toDoc(source) + extra;
    QTextBlock block = doc->findBlock(position);
    if (!block.isValid())
        block = doc->lastBlock();
    const QTextLayout *layout = block.layout();
    const QPointF origin = layoutOrigin(doc.get(), block);
    const int relative = position - block.position();
    QTextLine line = layout ? layout->lineForTextPosition(relative) : QTextLine();
    if (!line.isValid() && layout && layout->lineCount() > 0)
        line = layout->lineAt(layout->lineCount() - 1);
    if (!line.isValid()) {
        const QFontMetricsF metrics(doc->defaultFont());
        return QRectF(docOffset + origin, QSizeF(1, metrics.height()));
    }
    const qreal x = line.cursorToX(relative);
    // As tall as the text, not as the line with its spacing: the font of the
    // character before the caret (or after it, at a line start) decides.
    qreal ascent = line.ascent();
    qreal descent = line.descent();
    const int probe = relative > line.textStart() ? relative - 1 : relative;
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        const int start = fragment.position() - block.position();
        if (probe >= start && probe < start + fragment.length()) {
            const QTextCharFormat format = fragment.charFormat();
            if (!format.isImageFormat() && format.objectType() == QTextFormat::NoObject) {
                const QFontMetricsF fm(format.font());
                ascent = fm.ascent();
                descent = fm.descent();
            }
            break;
        }
    }
    const qreal baseline = line.y() + line.ascent();
    return QRectF(docOffset.x() + origin.x() + x, docOffset.y() + origin.y() + baseline - ascent, 1,
                  ascent + descent);
}

QRectF RenderedBlock::lineRect(int source) const
{
    const int position = toDoc(source);
    QTextBlock block = doc->findBlock(position);
    if (!block.isValid())
        block = doc->lastBlock();
    const QTextLayout *layout = block.layout();
    const QPointF origin = layoutOrigin(doc.get(), block);
    QTextLine line = layout ? layout->lineForTextPosition(position - block.position()) : QTextLine();
    if (!line.isValid() && layout && layout->lineCount() > 0)
        line = layout->lineAt(layout->lineCount() - 1);
    if (!line.isValid())
        return QRectF(docOffset + doc->documentLayout()->blockBoundingRect(block).topLeft(), QSizeF(1, 1));
    const QRectF box = lineBox(block, line);
    return box.translated(docOffset + origin);
}

qreal RenderedBlock::docHeight() const
{
    return doc->documentLayout()->documentSize().height();
}

int RenderedBlock::hitTest(const QPointF &point) const
{
    int position = doc->documentLayout()->hitTest(point - docOffset, Qt::FuzzyHit);
    if (position < 0)
        position = 0;
    return toSource(position);
}

QList<QRectF> RenderedBlock::rangeRects(int from, int to) const
{
    QList<QRectF> rects;
    if (from > to)
        std::swap(from, to);
    const int a = toDoc(from);
    const int b = toDoc(to);
    if (a >= b)
        return rects;
    const QFontMetricsF metrics(doc->defaultFont());
    const qreal newlineWidth = metrics.horizontalAdvance(QLatin1Char(' '));
    for (QTextBlock block = doc->findBlock(a); block.isValid() && block.position() <= b; block = block.next()) {
        const QTextLayout *layout = block.layout();
        if (!layout)
            continue;
        const QPointF origin = layoutOrigin(doc.get(), block);
        for (int i = 0; i < layout->lineCount(); ++i) {
            const QTextLine line = layout->lineAt(i);
            const int lineStart = block.position() + line.textStart();
            const int lineEnd = lineStart + line.textLength();
            const int s = std::max(a, lineStart);
            const int e = std::min(b, lineEnd);
            if (s > e || (s == e && b <= lineEnd))
                continue;
            qreal x1 = line.cursorToX(s - block.position());
            qreal x2 = line.cursorToX(e - block.position());
            if (x2 < x1)
                std::swap(x1, x2);
            const bool lastLine = i == layout->lineCount() - 1;
            if (b > lineEnd || (lastLine && b > block.position() + block.length() - 1))
                x2 += newlineWidth;
            const QRectF box = lineBox(block, line);
            rects.append(QRectF(docOffset.x() + origin.x() + x1, docOffset.y() + origin.y() + box.top(),
                                x2 - x1, box.height()));
        }
    }
    return rects;
}

QString RenderedBlock::anchorAt(const QPointF &point) const
{
    return doc->documentLayout()->anchorAt(point - docOffset);
}

Segment RenderedBlock::lineAt(int source) const
{
    const int position = toDoc(source);
    const QTextBlock block = doc->findBlock(position);
    if (!block.isValid() || !block.layout())
        return {source, source};
    const QTextLayout *layout = block.layout();
    const QTextLine line = layout->lineForTextPosition(position - block.position());
    if (!line.isValid())
        return {source, source};
    const int start = block.position() + line.textStart();
    int end = start + line.textLength();
    const QString text = block.text();
    const int last = end - block.position() - 1;
    if (end > start && last >= 0 && last < text.size()) {
        const QChar c = text.at(last);
        const bool wrapped = line.lineNumber() < layout->lineCount() - 1;
        if (c == LineSeparator || (wrapped && c.isSpace()))
            --end;
    }
    return {toSource(start), toSource(end)};
}

bool isTableOfContents(const Block &block)
{
    return block.kind == BlockKind::Paragraph && block.text.size() <= 12
        && block.text.trimmed().compare(QLatin1String("[TOC]"), Qt::CaseInsensitive) == 0;
}

std::unique_ptr<RenderedBlock> renderBlock(const Block &block, const RenderContext &context)
{
    auto out = std::make_unique<RenderedBlock>();
    Builder builder(*out, block, context);
    builder.build();
    return out;
}

QString revealKey(const Block &block, int caret, const LinkReferences *references)
{
    if (caret < 0)
        return {};
    QString key = QStringLiteral("A");
    switch (block.kind) {
    case BlockKind::Paragraph:
    case BlockKind::Heading:
    case BlockKind::ListItem:
    case BlockKind::Quote: {
        const ContentMap map = contentMap(block);
        const int at = map.fromSource(caret);
        collectReveal(cachedInline(map.text, references), at, key);
        break;
    }
    case BlockKind::Table: {
        const TableInfo info = tableInfo(block.text);
        for (const TableInfo::Row &row : info.rows) {
            for (const Segment &cell : row.cells) {
                if (caret >= cell.start && caret <= cell.end) {
                    key += QStringLiteral("c%1:").arg(cell.start);
                    collectReveal(cachedInline(block.text.mid(cell.start, cell.end - cell.start), references),
                                  caret - cell.start, key);
                }
            }
        }
        break;
    }
    default:
        break;
    }
    return key;
}

QList<InlineNode> cachedInline(const QString &text, const LinkReferences *references)
{
    QCache<QString, QList<InlineNode>> &cache = inlineCache();
    if (const QList<InlineNode> *found = cache.object(text))
        return *found;
    auto *parsed = new QList<InlineNode>(parseInline(text, references));
    const QList<InlineNode> result = *parsed;
    cache.insert(text, parsed, 1);
    return result;
}

void clearInlineCache()
{
    inlineCache().clear();
}

} // namespace ot
