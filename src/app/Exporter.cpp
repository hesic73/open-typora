#include "Exporter.h"

#include "Inline.h"
#include "MarkdownEditor.h"
#include "MathRenderer.h"

#include <QBuffer>
#include <QPagedPaintDevice>
#include <QPainter>
#include <QRegularExpression>
#include <QSvgGenerator>

namespace ot {

namespace {

QString escape(const QString &text)
{
    return text.toHtmlEscaped();
}

QString mathSvg(const QString &latex, bool display, const EditorTheme &theme)
{
    const qreal size = theme.fontSize * (display ? 1.2 : 1.08);
    const std::shared_ptr<const Formula> formula = math::typeset(latex, display, size, theme.text);
    if (!formula)
        return QStringLiteral("<code class=\"math-error\">%1</code>").arg(escape(latex));
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    QSvgGenerator generator;
    generator.setOutputDevice(&buffer);
    // Its sizes are written in millimetres; at 96 dpi they come out as CSS pixels.
    generator.setResolution(96);
    const QSizeF extent(formula->width() + 2, formula->ascent() + formula->descent() + 2);
    generator.setSize(extent.toSize() + QSize(1, 1));
    generator.setViewBox(QRectF(QPointF(0, 0), extent));
    {
        QPainter painter(&generator);
        formula->draw(&painter, QPointF(1, 1 + formula->ascent()), 1.0);
    }
    QString svg = QString::fromUtf8(buffer.data());
    const qsizetype start = svg.indexOf(QLatin1String("<svg"));
    if (start > 0)
        svg = svg.mid(start);
    const qreal depth = formula->descent() + 1;
    const QString style = display ? QStringLiteral("display:block;margin:0 auto;")
                                  : QStringLiteral("vertical-align:-%1px;").arg(depth, 0, 'f', 1);
    svg.replace(0, 4, QStringLiteral("<svg style=\"%1\"").arg(style));
    return svg;
}

QString slug(const QString &title)
{
    QString out = title.toLower();
    out.replace(QRegularExpression(QStringLiteral("[^\\w\\- ]")), QString());
    out.replace(QLatin1Char(' '), QLatin1Char('-'));
    return out;
}

class HtmlWriter
{
public:
    HtmlWriter(const EditorTheme &theme, const LinkReferences &references)
        : m_theme(theme)
        , m_references(references)
    {
    }

    QString inlineHtml(const QString &text)
    {
        return nodes(text, parseInline(text, &m_references));
    }

    QString nodes(const QString &text, const QList<InlineNode> &list)
    {
        QString out;
        for (const InlineNode &node : list) {
            const QString inner = text.mid(node.innerStart, node.innerEnd - node.innerStart);
            switch (node.type) {
            case InlineType::Text:
            case InlineType::Escape:
                out += escape(inner);
                break;
            case InlineType::Entity:
                out += text.mid(node.start, node.end - node.start);
                break;
            case InlineType::SoftBreak:
                out += QStringLiteral("\n");
                break;
            case InlineType::HardBreak:
                out += QStringLiteral("<br>\n");
                break;
            case InlineType::Emph:
                out += QStringLiteral("<em>") + nodes(text, node.children) + QStringLiteral("</em>");
                break;
            case InlineType::Strong:
                out += QStringLiteral("<strong>") + nodes(text, node.children) + QStringLiteral("</strong>");
                break;
            case InlineType::Strike:
                out += QStringLiteral("<del>") + nodes(text, node.children) + QStringLiteral("</del>");
                break;
            case InlineType::Highlight:
                out += QStringLiteral("<mark>") + nodes(text, node.children) + QStringLiteral("</mark>");
                break;
            case InlineType::Code: {
                QString code = inner;
                if (code.size() > 2 && code.startsWith(QLatin1Char(' ')) && code.endsWith(QLatin1Char(' '))
                    && !code.trimmed().isEmpty())
                    code = code.mid(1, code.size() - 2);
                out += QStringLiteral("<code>") + escape(code) + QStringLiteral("</code>");
                break;
            }
            case InlineType::Math:
                out += mathSvg(node.target, node.title == QLatin1String("display"), m_theme);
                break;
            case InlineType::Link:
                out += QStringLiteral("<a href=\"%1\"%2>").arg(escape(node.target),
                                                               node.title.isEmpty() ? QString()
                                                                                    : QStringLiteral(" title=\"%1\"").arg(escape(node.title)))
                    + nodes(text, node.children) + QStringLiteral("</a>");
                break;
            case InlineType::Image:
                out += QStringLiteral("<img src=\"%1\" alt=\"%2\">").arg(escape(node.target), escape(plainText(text, node.children)));
                break;
            case InlineType::AutoLink:
                out += QStringLiteral("<a href=\"%1\">%2</a>").arg(escape(node.target), escape(inner));
                break;
            case InlineType::HtmlTag:
                out += node.title;
                break;
            case InlineType::HtmlSpan:
                out += text.mid(node.start, node.innerStart - node.start) + nodes(text, node.children)
                    + text.mid(node.innerEnd, node.end - node.innerEnd);
                break;
            case InlineType::FootnoteRef:
                out += QStringLiteral("<sup class=\"footnote\"><a href=\"#fn-%1\">%1</a></sup>").arg(escape(node.target));
                break;
            }
        }
        return out;
    }

    QString blocks(const QList<Block> &list)
    {
        QString out;
        struct Open {
            int indent;
            bool ordered;
        };
        QList<Open> lists;
        const auto closeLists = [&](int indent) {
            while (!lists.isEmpty() && lists.last().indent >= indent) {
                out += lists.last().ordered ? QStringLiteral("</li></ol>\n") : QStringLiteral("</li></ul>\n");
                lists.removeLast();
            }
        };
        for (const Block &block : list) {
            if (block.kind != BlockKind::ListItem)
                closeLists(0);
            switch (block.kind) {
            case BlockKind::Paragraph:
                if (!block.text.trimmed().isEmpty())
                    out += QStringLiteral("<p>") + inlineHtml(contentMap(block).text) + QStringLiteral("</p>\n");
                break;
            case BlockKind::Heading: {
                const int level = headingLevel(block);
                const QString text = contentMap(block).text;
                out += QStringLiteral("<h%1 id=\"%2\">").arg(level).arg(escape(slug(plainText(text, parseInline(text)))))
                    + inlineHtml(text) + QStringLiteral("</h%1>\n").arg(level);
                break;
            }
            case BlockKind::ListItem: {
                const ListInfo info = listInfo(block.text);
                if (!lists.isEmpty() && lists.last().indent > info.indent)
                    closeLists(info.indent + 1);
                if (!lists.isEmpty() && lists.last().indent == info.indent && lists.last().ordered != info.ordered)
                    closeLists(info.indent);
                if (lists.isEmpty() || lists.last().indent < info.indent) {
                    out += info.ordered ? QStringLiteral("<ol start=\"%1\">\n").arg(info.number) : QStringLiteral("<ul>\n");
                    lists.append({info.indent, info.ordered});
                } else {
                    out += QStringLiteral("</li>\n");
                }
                out += info.task ? QStringLiteral("<li class=\"task\"><input type=\"checkbox\" disabled%1> ")
                                       .arg(info.checked ? QStringLiteral(" checked") : QString())
                                 : QStringLiteral("<li>");
                out += inlineHtml(contentMap(block).text);
                break;
            }
            case BlockKind::Quote: {
                const QString text = contentMap(block).text;
                out += QStringLiteral("<blockquote>\n");
                for (const QString &paragraph : text.split(QRegularExpression(QStringLiteral("\\n\\s*\\n"))))
                    out += QStringLiteral("<p>") + inlineHtml(paragraph) + QStringLiteral("</p>\n");
                out += QStringLiteral("</blockquote>\n");
                break;
            }
            case BlockKind::CodeFence: {
                const FenceInfo info = fenceInfo(block.text);
                out += QStringLiteral("<pre><code%1>").arg(info.language.isEmpty() ? QString()
                                                                                   : QStringLiteral(" class=\"language-%1\"").arg(escape(info.language)))
                    + escape(block.text.mid(info.code.start, info.code.end - info.code.start)) + QStringLiteral("</code></pre>\n");
                break;
            }
            case BlockKind::IndentedCode: {
                QStringList lines = block.text.split(QLatin1Char('\n'));
                for (QString &line : lines)
                    line = line.mid(std::min<qsizetype>(4, line.size() - line.trimmed().size()));
                out += QStringLiteral("<pre><code>") + escape(lines.join(QLatin1Char('\n'))) + QStringLiteral("</code></pre>\n");
                break;
            }
            case BlockKind::MathBlock: {
                const MathInfo info = mathInfo(block.text);
                out += QStringLiteral("<div class=\"math\">")
                    + mathSvg(block.text.mid(info.latex.start, info.latex.end - info.latex.start).trimmed(), true, m_theme)
                    + QStringLiteral("</div>\n");
                break;
            }
            case BlockKind::Table: {
                const TableInfo info = tableInfo(block.text);
                out += QStringLiteral("<table>\n");
                for (int r = 0; r < info.rows.size(); ++r) {
                    out += r == 0 ? QStringLiteral("<thead><tr>") : QStringLiteral("<tr>");
                    for (int c = 0; c < info.columns; ++c) {
                        const QString tag = r == 0 ? QStringLiteral("th") : QStringLiteral("td");
                        const Align align = info.aligns.value(c);
                        const QString style = align == Align::Center ? QStringLiteral(" style=\"text-align:center\"")
                            : align == Align::Right                  ? QStringLiteral(" style=\"text-align:right\"")
                            : align == Align::Left                   ? QStringLiteral(" style=\"text-align:left\"")
                                                                     : QString();
                        QString cell;
                        if (c < info.rows[r].cells.size()) {
                            const Segment segment = info.rows[r].cells[c];
                            cell = inlineHtml(block.text.mid(segment.start, segment.end - segment.start));
                        }
                        out += QStringLiteral("<%1%2>%3</%1>").arg(tag, style, cell);
                    }
                    out += r == 0 ? QStringLiteral("</tr></thead>\n<tbody>\n") : QStringLiteral("</tr>\n");
                }
                out += QStringLiteral("</tbody></table>\n");
                break;
            }
            case BlockKind::Rule:
                out += QStringLiteral("<hr>\n");
                break;
            case BlockKind::Html:
                out += block.text + QLatin1Char('\n');
                break;
            case BlockKind::FrontMatter:
                break;
            }
        }
        closeLists(0);
        return out;
    }

private:
    const EditorTheme &m_theme;
    const LinkReferences &m_references;
};

QString css(const EditorTheme &t)
{
    const QString body = t.bodyFamilies.isEmpty() ? QStringLiteral("sans-serif")
                                                  : QStringLiteral("\"%1\", sans-serif").arg(t.bodyFamilies.join(QStringLiteral("\", \"")));
    const QString mono = QStringLiteral("\"%1\", monospace").arg(t.monoFamilies.join(QStringLiteral("\", \"")));
    return QStringLiteral(R"(
html { font-size: %1px; background: %2; }
body { font-family: %3; color: %4; line-height: %5; max-width: %6px; margin: 0 auto; padding: 30px 30px 100px; background: %2; }
a { color: %7; }
h1, h2, h3, h4, h5, h6 { color: %8; font-weight: bold; line-height: 1.4; margin: 1rem 0; }
h1 { font-size: 2.25em; line-height: 1.2; border-bottom: 1px solid %9; padding-bottom: .3em; }
h2 { font-size: 1.75em; line-height: 1.225; border-bottom: 1px solid %9; padding-bottom: .3em; }
h3 { font-size: 1.5em; line-height: 1.43; } h4 { font-size: 1.25em; } h6 { color: %10; }
p, blockquote, ul, ol, table, pre { margin: 0.8em 0; }
ul, ol { padding-left: 30px; } li.task { list-style: none; } li.task input { margin: 0 0.4em 0 -1.4em; }
blockquote { border-left: 4px solid %11; padding: 0 15px; color: %12; }
code { font-family: %13; font-size: 0.9em; background: %14; padding: 0 2px; border-radius: 2px; }
pre { background: %15; border: 1px solid %16; border-radius: 3px; padding: 8px 12px; overflow: auto; line-height: 1.5; }
pre code { background: none; padding: 0; }
table { border-collapse: collapse; width: 100%; } th, td { border: 1px solid %17; padding: 6px 13px; }
tr:nth-child(2n) td { background: %18; }
hr { height: 2px; border: 0; background: %19; margin: 16px 0; }
mark { background: %20; }
.math { text-align: center; margin: 0.8em 0; overflow-x: auto; }
img { max-width: 100%; }
)")
        .arg(t.fontSize)
        .arg(t.background.name(), body, t.text.name())
        .arg(t.lineHeight)
        .arg(t.maxWidth)
        .arg(t.link.name(), t.heading.name(), t.headingRule.name(), t.muted.name(), t.quoteBar.name(),
             t.quoteText.name(), mono, t.inlineCodeBackground.name(), t.codeBackground.name(), t.codeBorder.name(),
             t.tableBorder.name(), t.tableStripe.name(), t.rule.name())
        .arg(t.highlightBackground.name());
}

} // namespace

QString exportToHtml(const QList<Block> &blocks, const EditorTheme &theme, const QString &title, const QString &baseDir)
{
    Q_UNUSED(baseDir);
    LinkReferences references;
    static const QRegularExpression definition(QStringLiteral("^ {0,3}\\[([^\\]^][^\\]]*)\\]:\\s*<?([^\\s>]+)>?"));
    for (const Block &block : blocks) {
        if (block.kind != BlockKind::Paragraph)
            continue;
        for (const QString &line : block.text.split(QLatin1Char('\n'))) {
            const QRegularExpressionMatch match = definition.match(line);
            if (match.hasMatch())
                references.insert(referenceKey(match.captured(1)), match.captured(2));
        }
    }
    HtmlWriter writer(theme, references);
    return QStringLiteral("<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
                          "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                          "<title>%1</title>\n<style>%2</style>\n</head>\n<body>\n%3</body>\n</html>\n")
        .arg(escape(title), css(theme), writer.blocks(blocks));
}

QString markdownToHtml(const QString &markdown, const EditorTheme &theme)
{
    const LinkReferences references;
    HtmlWriter writer(theme, references);
    return QStringLiteral("<html><head><meta charset=\"utf-8\"></head><body>%1</body></html>")
        .arg(writer.blocks(blocksFromMarkdown(markdown)));
}

bool printDocument(const MarkdownEditor *editor, QPagedPaintDevice *device, const EditorTheme &theme)
{
    QPainter painter;
    if (!painter.begin(device))
        return false;
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    // Lay out in CSS pixels, as on screen, and scale to the device.
    const qreal scale = device->logicalDpiX() / 96.0;
    const QRectF page = device->pageLayout().paintRectPixels(device->logicalDpiX());
    const qreal width = page.width() / scale;
    const qreal height = page.height() / scale;
    painter.scale(scale, scale);

    const std::vector<std::unique_ptr<RenderedBlock>> rendered = editor->renderForPrint(width, theme);
    const QList<Block> &blocks = editor->blocks();
    qreal y = 0;
    const auto newPage = [&] {
        device->newPage();
        y = 0;
    };
    for (size_t i = 0; i < rendered.size(); ++i) {
        const RenderedBlock &block = *rendered[i];
        if (i > 0)
            y += MarkdownEditor::gapBetween(blocks[int(i) - 1], *rendered[i - 1], blocks[int(i)], block, 1.0);
        if (blocks[int(i)].kind == BlockKind::Paragraph && blocks[int(i)].text.trimmed().isEmpty())
            continue;
        if (y > 0 && y + block.height > height && block.height <= height)
            newPage();
        // A block taller than a page is drawn in slices, one per page.
        qreal drawn = 0;
        while (true) {
            const qreal room = height - y;
            painter.save();
            painter.translate(0, y - drawn);
            const QRectF clip(0, drawn, width, std::min(room, block.height - drawn));
            painter.setClipRect(clip);
            MarkdownEditor::paintRendered(painter, block, theme, clip, 1.0);
            painter.restore();
            if (block.height - drawn <= room) {
                y += block.height - drawn;
                break;
            }
            drawn += room;
            newPage();
        }
    }
    painter.end();
    return true;
}

QString markdownReference()
{
    return QStringLiteral(R"md(# Markdown Reference

Open Typora shows a document as it reads. Put the caret into **anything** and its Markdown appears, so it can be changed; move away and it is formatted again.

## Text

Write **strong**, *emphasis*, ***both***, `code`, ~~strike~~, ==highlight==, <u>underline</u>, H<sub>2</sub>O and x<sup>2</sup>. A [link](https://typora.io), an autolink <https://example.com>, and a bare one: https://www.qt.io.

Escapes such as \*this\* stay literal. Entities: &copy; &rarr; &hellip;

## Lists

- An item
- Another, with a nested list
  - nested item
    - deeper
- [ ] a task to do
- [x] a task done

1. First
2. Second
3. Third

## Quotes

> A quote can run
> over several lines.
>
> And hold paragraphs.

## Code

```cpp
#include <iostream>

int main() {
    std::cout << "Hello, world!" << std::endl; // greet
    return 0;
}
```

## Math

Inline math sits in the line: $e^{i\pi} + 1 = 0$, and $\sum_{k=1}^n k = \frac{n(n+1)}{2}$.

$$
\int_0^\infty e^{-x^2}\,dx = \frac{\sqrt{\pi}}{2}
$$

## Tables

| Shortcut | Action | Notes |
| :------- | :----: | ----: |
| Ctrl+B | Strong | **bold** |
| Ctrl+I | Emphasis | *italic* |
| Ctrl+/ | Source mode | toggles |

---

## Typing shortcuts

Start a line with `# ` for a heading, `- ` or `1. ` for a list, `> ` for a quote, `- [ ] ` for a task. Type ```` ``` ```` or `$$` and press Enter for a code or math block, or `| a | b |` and Enter for a table.
)md");
}

} // namespace ot
