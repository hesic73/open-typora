#include "Document.h"

#include <QRegularExpression>

#include <algorithm>
#include <atomic>

namespace ot {

quint64 Block::nextRevision()
{
    static std::atomic<quint64> counter{1};
    return counter++;
}

namespace {

struct Line {
    qsizetype start = 0;
    QStringView text;
};

int leadingSpaces(QStringView line)
{
    int count = 0;
    for (const QChar c : line) {
        if (c == QLatin1Char(' '))
            ++count;
        else if (c == QLatin1Char('\t'))
            count += 4 - count % 4;
        else
            break;
    }
    return count;
}

/// Index of the first character that is not a space or tab.
qsizetype firstNonSpace(QStringView line)
{
    qsizetype i = 0;
    while (i < line.size() && (line.at(i) == QLatin1Char(' ') || line.at(i) == QLatin1Char('\t')))
        ++i;
    return i;
}

struct FenceOpen {
    bool ok = false;
    QChar ch;
    int length = 0;
    int indent = 0;
};

FenceOpen fenceOpen(QStringView line)
{
    FenceOpen result;
    const int indent = leadingSpaces(line);
    if (indent > 3)
        return result;
    const qsizetype at = firstNonSpace(line);
    if (at >= line.size())
        return result;
    const QChar ch = line.at(at);
    if (ch != QLatin1Char('`') && ch != QLatin1Char('~'))
        return result;
    qsizetype end = at;
    while (end < line.size() && line.at(end) == ch)
        ++end;
    if (end - at < 3)
        return result;
    if (ch == QLatin1Char('`') && line.mid(end).contains(QLatin1Char('`')))
        return result;
    result.ok = true;
    result.ch = ch;
    result.length = int(end - at);
    result.indent = indent;
    return result;
}

bool fenceCloses(QStringView line, QChar ch, int length)
{
    if (leadingSpaces(line) > 3)
        return false;
    qsizetype at = firstNonSpace(line);
    qsizetype end = at;
    while (end < line.size() && line.at(end) == ch)
        ++end;
    return end - at >= length && line.mid(end).trimmed().isEmpty();
}

bool isAtxHeading(QStringView line)
{
    if (leadingSpaces(line) > 3)
        return false;
    qsizetype at = firstNonSpace(line);
    qsizetype end = at;
    while (end < line.size() && line.at(end) == QLatin1Char('#'))
        ++end;
    const qsizetype count = end - at;
    return count >= 1 && count <= 6 && end < line.size()
        && (line.at(end) == QLatin1Char(' ') || line.at(end) == QLatin1Char('\t'));
}

bool isRuleLine(QStringView line)
{
    if (leadingSpaces(line) > 3)
        return false;
    QChar ch;
    int count = 0;
    for (const QChar c : line) {
        if (c == QLatin1Char(' ') || c == QLatin1Char('\t'))
            continue;
        if (c != QLatin1Char('-') && c != QLatin1Char('*') && c != QLatin1Char('_'))
            return false;
        if (count > 0 && c != ch)
            return false;
        ch = c;
        ++count;
    }
    return count >= 3;
}

/// 1 for "===", 2 for "---", 0 otherwise.
int setextLevel(QStringView line)
{
    if (leadingSpaces(line) > 3)
        return 0;
    const QStringView trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return 0;
    const QChar ch = trimmed.at(0);
    if (ch != QLatin1Char('=') && ch != QLatin1Char('-'))
        return 0;
    for (const QChar c : trimmed) {
        if (c != ch)
            return 0;
    }
    return ch == QLatin1Char('=') ? 1 : 2;
}

const QRegularExpression &listPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^( *)([-+*]|(\\d{1,9})([.)]))([ \\t]+)"));
    return pattern;
}

bool isQuoteStart(QStringView line)
{
    if (leadingSpaces(line) > 3)
        return false;
    const qsizetype at = firstNonSpace(line);
    // A lone ">" is not yet a quote: typing one should not turn the line into one.
    return at < line.size() && line.at(at) == QLatin1Char('>') && at + 1 < line.size();
}

bool isQuoteLine(QStringView line)
{
    const qsizetype at = firstNonSpace(line);
    return leadingSpaces(line) <= 3 && at < line.size() && line.at(at) == QLatin1Char('>');
}

bool isMathOpen(QStringView line)
{
    return leadingSpaces(line) <= 3 && line.trimmed().startsWith(QLatin1String("$$"));
}

bool isSingleLineMath(QStringView line)
{
    const QStringView trimmed = line.trimmed();
    return trimmed.size() > 4 && trimmed.startsWith(QLatin1String("$$")) && trimmed.endsWith(QLatin1String("$$"));
}

const QRegularExpression &htmlPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^ {0,3}<(!--|/?(address|article|aside|blockquote|body|center|details|dialog|dd|div|dl|dt|"
                       "fieldset|figcaption|figure|footer|form|h[1-6]|head|header|hr|html|iframe|li|main|menu|nav|"
                       "ol|p|pre|script|section|style|summary|table|tbody|td|tfoot|th|thead|tr|ul|video|audio|"
                       "picture|svg)(\\s|/?>|$))"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

bool isHtmlOpen(QStringView line)
{
    return htmlPattern().matchView(line).hasMatch();
}

bool isDelimiterRow(QStringView line)
{
    static const QRegularExpression pattern(
        QStringLiteral("^\\s*\\|?\\s*:?-+:?\\s*(\\|\\s*:?-+:?\\s*)*\\|?\\s*$"));
    return line.contains(QLatin1Char('|')) && pattern.matchView(line).hasMatch();
}

bool isTableStart(const QList<Line> &lines, int i)
{
    if (i + 1 >= lines.size() || !lines[i].text.contains(QLatin1Char('|')) || !isDelimiterRow(lines[i + 1].text))
        return false;
    const QString header = lines[i].text.toString();
    const QString delimiter = lines[i + 1].text.toString();
    return splitTableRow(header).size() == splitTableRow(delimiter).size();
}

/// Whether a line, met inside a paragraph, ends it and starts something else.
bool interruptsParagraph(QStringView line)
{
    if (fenceOpen(line).ok || isAtxHeading(line) || isRuleLine(line) || isQuoteStart(line) || isMathOpen(line)
        || isHtmlOpen(line))
        return true;
    const QRegularExpressionMatch list = listPattern().matchView(line);
    if (list.hasMatch() && list.capturedLength(1) <= 3) {
        const bool hasContent = !line.mid(list.capturedEnd(0)).trimmed().isEmpty();
        const bool ordered = list.capturedLength(3) > 0;
        return hasContent && (!ordered || list.capturedView(3) == QLatin1String("1"));
    }
    return false;
}

/// Whether a line inside a list item or quote, indented less than its content, starts another block.
bool startsOtherBlock(QStringView line)
{
    return fenceOpen(line).ok || isAtxHeading(line) || isRuleLine(line) || isQuoteStart(line) || isMathOpen(line)
        || isHtmlOpen(line);
}

QList<Line> splitLines(const QString &source)
{
    QList<Line> lines;
    qsizetype start = 0;
    const qsizetype size = source.size();
    while (start <= size) {
        qsizetype end = source.indexOf(QLatin1Char('\n'), start);
        if (end < 0)
            end = size;
        lines.append({start, QStringView(source).mid(start, end - start)});
        start = end + 1;
    }
    // A final newline ends the last line; it does not start another.
    if (!lines.isEmpty() && lines.last().text.isEmpty() && source.endsWith(QLatin1Char('\n')))
        lines.removeLast();
    return lines;
}

} // namespace

bool isBlankLine(QStringView line)
{
    return line.trimmed().isEmpty();
}

bool isListLine(QStringView line)
{
    return listPattern().matchView(line).hasMatch();
}

QList<ParsedBlock> parseBlocks(const QString &source)
{
    QList<ParsedBlock> out;
    if (source.isEmpty())
        return out;
    const QList<Line> lines = splitLines(source);
    const int n = int(lines.size());
    const auto blank = [&](int i) { return isBlankLine(lines[i].text); };
    const auto addBlock = [&](BlockKind kind, int from, int to) {
        const qsizetype start = lines[from].start;
        const qsizetype end = lines[to - 1].start + lines[to - 1].text.size();
        ParsedBlock parsed;
        parsed.block.kind = kind;
        parsed.block.text = source.mid(start, end - start);
        parsed.block.blankAfter = 0;
        parsed.block.revision = Block::nextRevision();
        parsed.start = start;
        out.append(parsed);
    };

    int i = 0;
    while (i < n) {
        const QStringView line = lines[i].text;
        if (blank(i)) {
            if (!out.isEmpty())
                ++out.last().block.blankAfter;
            ++i;
            continue;
        }

        if (i == 0 && line == QLatin1String("---")) {
            int j = 1;
            while (j < n && lines[j].text != QLatin1String("---") && lines[j].text != QLatin1String("..."))
                ++j;
            if (j < n) {
                addBlock(BlockKind::FrontMatter, 0, j + 1);
                i = j + 1;
                continue;
            }
        }

        if (const FenceOpen fence = fenceOpen(line); fence.ok) {
            int j = i + 1;
            while (j < n && !fenceCloses(lines[j].text, fence.ch, fence.length))
                ++j;
            const int end = j < n ? j + 1 : n;
            addBlock(BlockKind::CodeFence, i, end);
            i = end;
            continue;
        }

        if (isMathOpen(line)) {
            if (isSingleLineMath(line)) {
                addBlock(BlockKind::MathBlock, i, i + 1);
                ++i;
                continue;
            }
            int j = i + 1;
            while (j < n && !blank(j) && !lines[j].text.trimmed().endsWith(QLatin1String("$$")))
                ++j;
            if (j < n && !blank(j)) {
                addBlock(BlockKind::MathBlock, i, j + 1);
                i = j + 1;
                continue;
            }
            // Never closed: an ordinary paragraph that starts with "$$".
        }

        if (isAtxHeading(line)) {
            addBlock(BlockKind::Heading, i, i + 1);
            ++i;
            continue;
        }

        if (isRuleLine(line)) {
            addBlock(BlockKind::Rule, i, i + 1);
            ++i;
            continue;
        }

        if (isTableStart(lines, i)) {
            int j = i + 2;
            while (j < n && !blank(j) && lines[j].text.contains(QLatin1Char('|')) && !startsOtherBlock(lines[j].text))
                ++j;
            addBlock(BlockKind::Table, i, j);
            i = j;
            continue;
        }

        if (const QRegularExpressionMatch list = listPattern().matchView(line); list.hasMatch()) {
            const ListInfo info = listInfo(line.toString());
            const int contentIndent = info.contentIndent;
            int j = i + 1;
            while (j < n) {
                if (blank(j)) {
                    int k = j;
                    while (k < n && blank(k))
                        ++k;
                    if (k < n && leadingSpaces(lines[k].text) >= contentIndent && !isListLine(lines[k].text)) {
                        j = k;
                        continue;
                    }
                    break;
                }
                if (isListLine(lines[j].text))
                    break;
                if (leadingSpaces(lines[j].text) < contentIndent && startsOtherBlock(lines[j].text))
                    break;
                ++j;
            }
            addBlock(BlockKind::ListItem, i, j);
            i = j;
            continue;
        }

        if (isQuoteStart(line)) {
            int j = i + 1;
            while (j < n && !blank(j)) {
                const QStringView next = lines[j].text;
                if (!isQuoteLine(next) && (startsOtherBlock(next) || isListLine(next)))
                    break;
                ++j;
            }
            addBlock(BlockKind::Quote, i, j);
            i = j;
            continue;
        }

        if (isHtmlOpen(line)) {
            int j = i;
            if (line.trimmed().startsWith(QLatin1String("<!--"))) {
                while (j < n && !lines[j].text.contains(QLatin1String("-->")))
                    ++j;
                j = j < n ? j + 1 : n;
            } else {
                while (j < n && !blank(j))
                    ++j;
            }
            addBlock(BlockKind::Html, i, j);
            i = j;
            continue;
        }

        if (leadingSpaces(line) >= 4) {
            int j = i + 1;
            int last = i;
            while (j < n && (blank(j) || leadingSpaces(lines[j].text) >= 4)) {
                if (!blank(j))
                    last = j;
                ++j;
            }
            addBlock(BlockKind::IndentedCode, i, last + 1);
            i = last + 1;
            continue;
        }

        // A paragraph, unless an underline makes it a heading.
        int j = i + 1;
        bool heading = false;
        while (j < n && !blank(j)) {
            if (setextLevel(lines[j].text) > 0) {
                heading = true;
                break;
            }
            if (interruptsParagraph(lines[j].text) || isTableStart(lines, j))
                break;
            ++j;
        }
        if (heading) {
            addBlock(BlockKind::Heading, i, j + 1);
            i = j + 1;
        } else {
            addBlock(BlockKind::Paragraph, i, j);
            i = j;
        }
    }
    return out;
}

namespace {

/// Whether two blocks written with no blank line between them read back as the same two blocks.
bool canTouch(const Block &a, const Block &b)
{
    if (a.text.isEmpty() || b.text.isEmpty())
        return false;
    const QList<ParsedBlock> parsed = parseBlocks(a.text + QLatin1Char('\n') + b.text);
    return parsed.size() == 2 && parsed[0].block.kind == a.kind && parsed[1].block.kind == b.kind
        && parsed[0].block.text == a.text;
}

} // namespace

QString serializeBlocks(const QList<Block> &blocks, QList<qsizetype> *offsets)
{
    QString out;
    if (offsets)
        offsets->clear();
    for (qsizetype i = 0; i < blocks.size(); ++i) {
        if (offsets)
            offsets->append(out.size());
        out += blocks[i].text;
        if (i + 1 < blocks.size()) {
            int blank = blocks[i].blankAfter;
            if (blank <= 0 && !canTouch(blocks[i], blocks[i + 1]))
                blank = 1;
            out += QString(blank + 1, QLatin1Char('\n'));
        }
    }
    if (!out.isEmpty() && !out.endsWith(QLatin1Char('\n')))
        out += QLatin1Char('\n');
    return out;
}

QList<Block> blocksFromMarkdown(const QString &source)
{
    QList<Block> blocks;
    for (const ParsedBlock &parsed : parseBlocks(source))
        blocks.append(parsed.block);
    if (blocks.isEmpty()) {
        Block empty;
        empty.revision = Block::nextRevision();
        blocks.append(empty);
    }
    return blocks;
}

int headingLevel(const Block &block)
{
    if (block.kind != BlockKind::Heading)
        return 0;
    const QStringView text(block.text);
    if (isAtxHeading(text)) {
        const qsizetype at = firstNonSpace(text);
        int count = 0;
        while (at + count < text.size() && text.at(at + count) == QLatin1Char('#'))
            ++count;
        return count;
    }
    const qsizetype lastBreak = text.lastIndexOf(QLatin1Char('\n'));
    const int level = setextLevel(text.mid(lastBreak + 1));
    return level > 0 ? level : 1;
}

ListInfo listInfo(const QString &text)
{
    ListInfo info;
    const qsizetype lineEnd = text.indexOf(QLatin1Char('\n')) < 0 ? text.size() : text.indexOf(QLatin1Char('\n'));
    const QStringView line = QStringView(text).left(lineEnd);
    const QRegularExpressionMatch match = listPattern().matchView(line);
    if (!match.hasMatch())
        return info;
    info.valid = true;
    info.indent = int(match.capturedLength(1));
    info.markerStart = int(match.capturedStart(2));
    info.markerEnd = int(match.capturedEnd(2));
    info.ordered = match.capturedLength(3) > 0;
    if (info.ordered) {
        info.number = match.capturedView(3).toInt();
        info.delimiter = match.capturedView(4).at(0);
    } else {
        info.delimiter = match.capturedView(2).at(0);
    }
    int spaces = int(match.capturedLength(5));
    if (spaces > 4)
        spaces = 1;
    info.contentStart = info.markerEnd + spaces;
    if (match.capturedLength(5) == 0)
        info.contentStart = info.markerEnd;
    info.contentIndent = info.markerEnd + std::max(1, spaces);

    // A task box: "[ ]" or "[x]" followed by a space or the end of the line.
    const QStringView rest = line.mid(info.contentStart);
    if (rest.size() >= 3 && rest.at(0) == QLatin1Char('[') && rest.at(2) == QLatin1Char(']')
        && (rest.at(1) == QLatin1Char(' ') || rest.at(1) == QLatin1Char('x') || rest.at(1) == QLatin1Char('X'))
        && (rest.size() == 3 || rest.at(3) == QLatin1Char(' '))) {
        info.task = true;
        info.checked = rest.at(1) != QLatin1Char(' ');
        info.taskStart = info.contentStart;
        info.contentStart += rest.size() == 3 ? 3 : 4;
    }
    return info;
}

FenceInfo fenceInfo(const QString &text)
{
    FenceInfo info;
    const qsizetype firstBreak = text.indexOf(QLatin1Char('\n'));
    const QStringView first = QStringView(text).left(firstBreak < 0 ? text.size() : firstBreak);
    const FenceOpen open = fenceOpen(first);
    if (!open.ok)
        return info;
    info.valid = true;
    info.fenceChar = open.ch;
    info.fenceLength = open.length;
    info.indent = open.indent;
    const qsizetype at = firstNonSpace(first) + open.length;
    info.language = first.mid(at).trimmed().toString().section(QLatin1Char(' '), 0, 0);
    if (firstBreak < 0) {
        info.code = {int(text.size()), int(text.size())};
        return info;
    }
    const qsizetype lastBreak = text.lastIndexOf(QLatin1Char('\n'));
    const QStringView last = QStringView(text).mid(lastBreak + 1);
    info.closed = lastBreak >= firstBreak && fenceCloses(last, open.ch, open.length);
    const int codeStart = int(firstBreak) + 1;
    if (info.closed) {
        if (lastBreak == firstBreak)
            info.code = {int(firstBreak), int(firstBreak)}; // no code lines at all
        else
            info.code = {codeStart, int(lastBreak)};
    } else {
        info.code = {codeStart, int(text.size())};
    }
    return info;
}

MathInfo mathInfo(const QString &text)
{
    MathInfo info;
    const qsizetype open = text.indexOf(QLatin1String("$$"));
    if (open < 0) {
        info.latex = {0, int(text.size())};
        return info;
    }
    const qsizetype close = text.lastIndexOf(QLatin1String("$$"));
    info.latex.start = int(open) + 2;
    info.latex.end = close > open ? int(close) : int(text.size());
    return info;
}

QStringList splitTableRow(const QString &line, QList<Segment> *cells)
{
    QStringList out;
    QList<Segment> segments;
    // Pieces between unescaped pipes.
    QList<Segment> pieces;
    int start = 0;
    for (int i = 0; i < line.size(); ++i) {
        if (line.at(i) == QLatin1Char('\\')) {
            ++i;
            continue;
        }
        if (line.at(i) == QLatin1Char('|')) {
            pieces.append({start, i});
            start = i + 1;
        }
    }
    pieces.append({start, int(line.size())});
    const qsizetype first = firstNonSpace(line);
    const bool leadingPipe = first < line.size() && line.at(first) == QLatin1Char('|');
    if (leadingPipe && !pieces.isEmpty())
        pieces.removeFirst();
    if (pieces.size() > 1 && QStringView(line).mid(pieces.last().start, pieces.last().end - pieces.last().start)
                                 .trimmed()
                                 .isEmpty())
        pieces.removeLast();
    for (const Segment &piece : pieces) {
        int s = piece.start;
        int e = piece.end;
        while (s < e && line.at(s).isSpace())
            ++s;
        while (e > s && line.at(e - 1).isSpace())
            --e;
        if (s == e)
            s = e = std::min(piece.start + 1, piece.end);
        segments.append({s, e});
        out.append(line.mid(s, e - s));
    }
    if (cells)
        *cells = segments;
    return out;
}

TableInfo tableInfo(const QString &text)
{
    TableInfo info;
    const QList<Line> lines = splitLines(text);
    for (int i = 0; i < lines.size(); ++i) {
        const int lineStart = int(lines[i].start);
        const QString line = lines[i].text.toString();
        if (i == 1) {
            info.delimiterLine = {lineStart, lineStart + int(line.size())};
            for (const QString &cell : splitTableRow(line)) {
                const QString c = cell.trimmed();
                const bool left = c.startsWith(QLatin1Char(':'));
                const bool right = c.endsWith(QLatin1Char(':'));
                info.aligns.append(left && right ? Align::Center : right ? Align::Right : left ? Align::Left : Align::None);
            }
            continue;
        }
        TableInfo::Row row;
        row.lineStart = lineStart;
        row.lineEnd = lineStart + int(line.size());
        QList<Segment> cells;
        splitTableRow(line, &cells);
        for (Segment &cell : cells) {
            cell.start += lineStart;
            cell.end += lineStart;
        }
        row.cells = cells;
        info.rows.append(row);
    }
    info.columns = info.rows.isEmpty() ? 0 : int(info.rows.first().cells.size());
    while (info.aligns.size() < info.columns)
        info.aligns.append(Align::None);
    return info;
}

int ContentMap::fromSource(int source) const
{
    const auto it = std::lower_bound(sourceOf.cbegin(), sourceOf.cend(), source);
    if (it == sourceOf.cend() || *it != source)
        return -1;
    return int(it - sourceOf.cbegin());
}

namespace {

void appendRange(ContentMap &map, const QString &text, int start, int end)
{
    for (int i = start; i < end; ++i) {
        map.text += text.at(i);
        map.sourceOf.append(i);
    }
}

} // namespace

ContentMap contentMap(const Block &block)
{
    ContentMap map;
    const QString &text = block.text;
    switch (block.kind) {
    case BlockKind::ListItem: {
        const ListInfo info = listInfo(text);
        const QList<Line> lines = splitLines(text);
        for (int i = 0; i < lines.size(); ++i) {
            const int lineStart = int(lines[i].start);
            const int lineEnd = lineStart + int(lines[i].text.size());
            int from = lineStart;
            if (i == 0) {
                from = std::min(lineEnd, info.valid ? info.contentStart : 0);
            } else {
                map.text += QLatin1Char('\n');
                map.sourceOf.append(lineStart - 1);
                int stripped = 0;
                while (from < lineEnd && stripped < info.contentIndent && text.at(from) == QLatin1Char(' ')) {
                    ++from;
                    ++stripped;
                }
            }
            appendRange(map, text, from, lineEnd);
        }
        map.sourceOf.append(int(text.size()));
        break;
    }
    case BlockKind::Quote: {
        const QList<Line> lines = splitLines(text);
        for (int i = 0; i < lines.size(); ++i) {
            const int lineStart = int(lines[i].start);
            const int lineEnd = lineStart + int(lines[i].text.size());
            if (i > 0) {
                map.text += QLatin1Char('\n');
                map.sourceOf.append(lineStart - 1);
            }
            int from = lineStart;
            int depth = 0;
            for (;;) {
                int at = from;
                while (at < lineEnd && text.at(at) == QLatin1Char(' ') && at - from < 3)
                    ++at;
                if (at < lineEnd && text.at(at) == QLatin1Char('>')) {
                    ++depth;
                    from = at + 1;
                    if (from < lineEnd && text.at(from) == QLatin1Char(' '))
                        ++from;
                } else {
                    break;
                }
            }
            // A lazy line, without '>', continues at the depth of the line before.
            map.lineDepth.append(depth > 0 ? depth : (map.lineDepth.isEmpty() ? 1 : map.lineDepth.last()));
            appendRange(map, text, from, lineEnd);
        }
        map.sourceOf.append(int(text.size()));
        break;
    }
    case BlockKind::Heading: {
        const int level = headingLevel(block);
        Q_UNUSED(level);
        if (isAtxHeading(text)) {
            int from = int(firstNonSpace(text));
            while (from < text.size() && text.at(from) == QLatin1Char('#'))
                ++from;
            while (from < text.size() && (text.at(from) == QLatin1Char(' ') || text.at(from) == QLatin1Char('\t')))
                ++from;
            int to = int(text.size());
            // A closing run of '#' after a space is not part of the title.
            int back = to;
            while (back > from && text.at(back - 1) == QLatin1Char(' '))
                --back;
            int hashes = back;
            while (hashes > from && text.at(hashes - 1) == QLatin1Char('#'))
                --hashes;
            if (hashes < back && (hashes == from || text.at(hashes - 1) == QLatin1Char(' '))) {
                to = hashes;
                while (to > from && text.at(to - 1) == QLatin1Char(' '))
                    --to;
            }
            appendRange(map, text, from, to);
            map.sourceOf.append(to);
        } else {
            const qsizetype lastBreak = text.lastIndexOf(QLatin1Char('\n'));
            const int to = lastBreak < 0 ? int(text.size()) : int(lastBreak);
            appendRange(map, text, 0, to);
            map.sourceOf.append(to);
        }
        break;
    }
    default:
        appendRange(map, text, 0, int(text.size()));
        map.sourceOf.append(int(text.size()));
        break;
    }
    return map;
}

QList<Segment> caretSegments(const Block &block)
{
    QList<Segment> segments;
    const QString &text = block.text;
    switch (block.kind) {
    case BlockKind::ListItem:
    case BlockKind::Quote: {
        // Every run of consecutive mapped characters is one segment.
        const ContentMap map = contentMap(block);
        int start = map.sourceOf.first();
        int previous = start;
        for (int i = 1; i < map.sourceOf.size(); ++i) {
            const int at = map.sourceOf[i];
            if (at != previous + 1) {
                segments.append({start, previous});
                start = at;
            }
            previous = at;
        }
        segments.append({start, previous});
        // The newline positions are ends of lines, where the caret may stand too.
        break;
    }
    case BlockKind::Heading: {
        // The "#" marks are never shown, so the caret stays in the title.
        const ContentMap map = contentMap(block);
        segments.append({map.sourceOf.first(), map.sourceOf.last()});
        break;
    }
    case BlockKind::Table: {
        const TableInfo info = tableInfo(text);
        for (const TableInfo::Row &row : info.rows) {
            for (const Segment &cell : row.cells)
                segments.append(cell);
        }
        if (segments.isEmpty())
            segments.append({0, int(text.size())});
        break;
    }
    default:
        segments.append({0, int(text.size())});
        break;
    }
    return segments;
}

QString kindName(BlockKind kind)
{
    switch (kind) {
    case BlockKind::Paragraph: return QStringLiteral("paragraph");
    case BlockKind::Heading: return QStringLiteral("heading");
    case BlockKind::CodeFence: return QStringLiteral("code");
    case BlockKind::IndentedCode: return QStringLiteral("indented code");
    case BlockKind::MathBlock: return QStringLiteral("math");
    case BlockKind::Quote: return QStringLiteral("quote");
    case BlockKind::ListItem: return QStringLiteral("list item");
    case BlockKind::Table: return QStringLiteral("table");
    case BlockKind::Rule: return QStringLiteral("rule");
    case BlockKind::Html: return QStringLiteral("html");
    case BlockKind::FrontMatter: return QStringLiteral("front matter");
    }
    return {};
}

} // namespace ot
