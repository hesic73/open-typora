#include "Inline.h"

#include <QRegularExpression>
#include <QSet>

namespace ot {

namespace {

bool isPunctuation(QChar c)
{
    return c.isPunct() || c.isSymbol();
}

bool isAsciiPunctuation(QChar c)
{
    return c.unicode() < 128 && (c.isPunct() || c.isSymbol());
}

bool isWhite(QChar c)
{
    return c.isSpace();
}

/// Tags that never have a closing partner.
bool isVoidTag(const QString &name)
{
    static const QSet<QString> tags = {QStringLiteral("br"), QStringLiteral("img"), QStringLiteral("hr"),
                                       QStringLiteral("input"), QStringLiteral("wbr"), QStringLiteral("source"),
                                       QStringLiteral("meta"), QStringLiteral("link")};
    return tags.contains(name);
}

class Parser
{
public:
    Parser(const QString &text, const LinkReferences *references)
        : s(text)
        , refs(references)
    {
    }

    QList<InlineNode> run()
    {
        scan();
        processEmphasis(0);
        QList<InlineNode> result = nodes;
        pairTags(result);
        return result;
    }

private:
    struct Delimiter {
        int id;
        QChar ch;
        int length;
        int originalLength;
        bool canOpen;
        bool canClose;
    };
    struct Bracket {
        int id;
        bool image;
        int delimiterBottom;
        bool active;
        int position; // index of '[' in the text
    };

    const QString &s;
    const LinkReferences *refs;
    QList<InlineNode> nodes;
    QList<Delimiter> delimiters;
    QList<Bracket> brackets;
    int nextId = 1;
    int textStart = -1;

    QChar at(int i) const { return i >= 0 && i < s.size() ? s.at(i) : QChar(QLatin1Char('\n')); }

    void flushText(int upTo)
    {
        if (textStart >= 0 && upTo > textStart) {
            InlineNode node;
            node.type = InlineType::Text;
            node.start = node.innerStart = textStart;
            node.end = node.innerEnd = upTo;
            node.id = nextId++;
            nodes.append(node);
        }
        textStart = -1;
    }

    int addNode(InlineNode node)
    {
        node.id = nextId++;
        nodes.append(node);
        return node.id;
    }

    int addText(int start, int end)
    {
        InlineNode node;
        node.type = InlineType::Text;
        node.start = node.innerStart = start;
        node.end = node.innerEnd = end;
        return addNode(node);
    }

    int indexOf(int id) const
    {
        for (int i = int(nodes.size()) - 1; i >= 0; --i) {
            if (nodes[i].id == id)
                return i;
        }
        return -1;
    }

    void scan()
    {
        const int n = int(s.size());
        int i = 0;
        while (i < n) {
            const QChar c = s.at(i);
            const int handled = special(i, c);
            if (handled > 0) {
                i = handled;
                continue;
            }
            if (textStart < 0)
                textStart = i;
            ++i;
        }
        flushText(n);
    }

    /// Handles a construct starting at `i`; returns the index after it, or 0
    /// when the character is ordinary text.
    int special(int i, QChar c)
    {
        switch (c.unicode()) {
        case '\\':
            return escape(i);
        case '`':
            return codeSpan(i);
        case '$':
            return math(i);
        case '*':
        case '_':
        case '~':
        case '=':
            return delimiterRun(i, c);
        case '[':
            return openBracket(i, false);
        case '!':
            if (at(i + 1) == QLatin1Char('['))
                return openBracket(i, true);
            return 0;
        case ']':
            return closeBracket(i);
        case '<':
            return angle(i);
        case '&':
            return entity(i);
        case '\n':
            return lineBreak(i);
        case 'h':
        case 'w':
            return bareUrl(i);
        default:
            return 0;
        }
    }

    int escape(int i)
    {
        const QChar next = at(i + 1);
        if (i + 1 < s.size() && next == QLatin1Char('\n')) {
            flushText(i);
            InlineNode node;
            node.type = InlineType::HardBreak;
            node.start = i;
            node.innerStart = i + 1;
            node.innerEnd = node.end = i + 2;
            addNode(node);
            return i + 2;
        }
        if (i + 1 < s.size() && isAsciiPunctuation(next)) {
            flushText(i);
            InlineNode node;
            node.type = InlineType::Escape;
            node.start = i;
            node.innerStart = i + 1;
            node.innerEnd = node.end = i + 2;
            addNode(node);
            return i + 2;
        }
        return 0;
    }

    int runLength(int i, QChar c) const
    {
        int j = i;
        while (j < s.size() && s.at(j) == c)
            ++j;
        return j - i;
    }

    int codeSpan(int i)
    {
        const int length = runLength(i, QLatin1Char('`'));
        int j = i + length;
        while (j < s.size()) {
            if (s.at(j) == QLatin1Char('`')) {
                const int closing = runLength(j, QLatin1Char('`'));
                if (closing == length) {
                    flushText(i);
                    InlineNode node;
                    node.type = InlineType::Code;
                    node.start = i;
                    node.innerStart = i + length;
                    node.innerEnd = j;
                    node.end = j + length;
                    addNode(node);
                    return j + length;
                }
                j += closing;
            } else {
                ++j;
            }
        }
        // No closing run: the backticks are text.
        if (textStart < 0)
            textStart = i;
        return i + length;
    }

    int math(int i)
    {
        const int n = int(s.size());
        if (at(i + 1) == QLatin1Char('$')) {
            const int close = int(s.indexOf(QLatin1String("$$"), i + 2));
            if (close > i + 2) {
                flushText(i);
                InlineNode node;
                node.type = InlineType::Math;
                node.start = i;
                node.innerStart = i + 2;
                node.innerEnd = close;
                node.end = close + 2;
                node.target = s.mid(i + 2, close - i - 2);
                node.title = QStringLiteral("display");
                addNode(node);
                return close + 2;
            }
            return 0;
        }
        if (i + 1 >= n || isWhite(s.at(i + 1)))
            return 0;
        for (int j = i + 1; j < n; ++j) {
            const QChar c = s.at(j);
            if (c == QLatin1Char('\\')) {
                ++j;
                continue;
            }
            if (c == QLatin1Char('\n') && at(j + 1) == QLatin1Char('\n'))
                return 0;
            if (c != QLatin1Char('$'))
                continue;
            if (isWhite(s.at(j - 1)) || at(j + 1).isDigit())
                return 0;
            flushText(i);
            InlineNode node;
            node.type = InlineType::Math;
            node.start = i;
            node.innerStart = i + 1;
            node.innerEnd = j;
            node.end = j + 1;
            node.target = s.mid(i + 1, j - i - 1);
            addNode(node);
            return j + 1;
        }
        return 0;
    }

    int delimiterRun(int i, QChar c)
    {
        const int length = runLength(i, c);
        const int end = i + length;
        if ((c == QLatin1Char('~') || c == QLatin1Char('=')) && length != 2) {
            if (textStart < 0)
                textStart = i;
            return end;
        }
        const QChar before = at(i - 1);
        const QChar after = at(end);
        const bool leftFlanking =
            !isWhite(after) && (!isPunctuation(after) || isWhite(before) || isPunctuation(before));
        const bool rightFlanking =
            !isWhite(before) && (!isPunctuation(before) || isWhite(after) || isPunctuation(after));
        bool canOpen = leftFlanking;
        bool canClose = rightFlanking;
        if (c == QLatin1Char('_')) {
            canOpen = leftFlanking && (!rightFlanking || isPunctuation(before));
            canClose = rightFlanking && (!leftFlanking || isPunctuation(after));
        }
        flushText(i);
        const int id = addText(i, end);
        if (canOpen || canClose)
            delimiters.append({id, c, length, length, canOpen, canClose});
        return end;
    }

    int openBracket(int i, bool image)
    {
        flushText(i);
        const int end = i + (image ? 2 : 1);
        const int id = addText(i, end);
        brackets.append({id, image, int(delimiters.size()), true, end - 1});
        return end;
    }

    /// Parses "(destination "title")" at `i`. Returns the index after ')' or -1.
    int linkTail(int i, QString &destination, QString &title) const
    {
        const int n = int(s.size());
        if (at(i) != QLatin1Char('('))
            return -1;
        int j = i + 1;
        while (j < n && isWhite(s.at(j)))
            ++j;
        if (at(j) == QLatin1Char('<')) {
            const int close = int(s.indexOf(QLatin1Char('>'), j + 1));
            if (close < 0)
                return -1;
            destination = s.mid(j + 1, close - j - 1);
            j = close + 1;
        } else {
            int depth = 0;
            const int start = j;
            while (j < n) {
                const QChar c = s.at(j);
                if (c == QLatin1Char('\\') && j + 1 < n) {
                    j += 2;
                    continue;
                }
                if (isWhite(c))
                    break;
                if (c == QLatin1Char('('))
                    ++depth;
                if (c == QLatin1Char(')')) {
                    if (depth == 0)
                        break;
                    --depth;
                }
                ++j;
            }
            destination = s.mid(start, j - start);
        }
        while (j < n && isWhite(s.at(j)))
            ++j;
        const QChar quote = at(j);
        if (quote == QLatin1Char('"') || quote == QLatin1Char('\'') || quote == QLatin1Char('(')) {
            const QChar closing = quote == QLatin1Char('(') ? QChar(QLatin1Char(')')) : quote;
            const int close = int(s.indexOf(closing, j + 1));
            if (close < 0)
                return -1;
            title = s.mid(j + 1, close - j - 1);
            j = close + 1;
            while (j < n && isWhite(s.at(j)))
                ++j;
        }
        if (at(j) != QLatin1Char(')'))
            return -1;
        return j + 1;
    }

    int closeBracket(int i)
    {
        if (brackets.isEmpty())
            return 0;
        const Bracket opener = brackets.takeLast();
        if (!opener.active)
            return 0;
        const int openerIndex = indexOf(opener.id);
        if (openerIndex < 0)
            return 0;

        QString destination;
        QString title;
        int end = linkTail(i + 1, destination, title);
        bool footnote = false;
        if (end < 0) {
            const QString label = s.mid(opener.position + 1, i - opener.position - 1);
            if (!opener.image && label.startsWith(QLatin1Char('^')) && label.size() > 1
                && !label.contains(QLatin1Char(' '))) {
                footnote = true;
                end = i + 1;
                destination = label.mid(1);
            } else if (refs) {
                // [text][label], [text][] or [text]
                QString key;
                if (at(i + 1) == QLatin1Char('[')) {
                    const int close = int(s.indexOf(QLatin1Char(']'), i + 2));
                    if (close >= 0) {
                        key = s.mid(i + 2, close - i - 2);
                        if (key.isEmpty())
                            key = label;
                        end = close + 1;
                    }
                } else {
                    key = label;
                    end = i + 1;
                }
                const auto found = refs->constFind(referenceKey(key));
                if (key.isEmpty() || found == refs->constEnd())
                    end = -1;
                else
                    destination = found.value();
            }
        }
        if (end < 0)
            return 0;

        flushText(i);
        processEmphasis(opener.delimiterBottom);
        const int from = indexOf(opener.id);
        InlineNode link;
        link.type = footnote ? InlineType::FootnoteRef : opener.image ? InlineType::Image : InlineType::Link;
        link.start = nodes[from].start;
        link.innerStart = nodes[from].end;
        link.innerEnd = i;
        link.end = end;
        link.target = destination.trimmed();
        link.title = title;
        link.children = nodes.mid(from + 1);
        nodes.resize(from);
        addNode(link);
        if (!opener.image) {
            // No links inside links.
            for (Bracket &bracket : brackets) {
                if (!bracket.image)
                    bracket.active = false;
            }
        }
        return end;
    }

    int angle(int i)
    {
        static const QRegularExpression autolink(
            QStringLiteral("\\G<([a-zA-Z][a-zA-Z0-9+.-]{1,31}:[^\\s<>]*|[^\\s@<>]+@[^\\s@<>]+\\.[^\\s@<>]+)>"));
        static const QRegularExpression tag(QStringLiteral(
            "\\G(<!--[\\s\\S]*?-->|</?[a-zA-Z][a-zA-Z0-9-]*(\\s+[a-zA-Z_:][a-zA-Z0-9_.:-]*(\\s*=\\s*(\"[^\"]*\"|'[^']*'|"
            "[^\\s\"'=<>`]+))?)*\\s*/?>)"));
        QRegularExpressionMatch match = autolink.match(s, i);
        if (match.hasMatch()) {
            flushText(i);
            InlineNode node;
            node.type = InlineType::AutoLink;
            node.start = i;
            node.innerStart = i + 1;
            node.innerEnd = int(match.capturedEnd(0)) - 1;
            node.end = int(match.capturedEnd(0));
            node.target = match.captured(1);
            if (!node.target.contains(QLatin1Char(':')))
                node.target.prepend(QStringLiteral("mailto:"));
            addNode(node);
            return node.end;
        }
        match = tag.match(s, i);
        if (match.hasMatch()) {
            flushText(i);
            InlineNode node;
            node.type = InlineType::HtmlTag;
            node.start = node.innerStart = i;
            node.end = node.innerEnd = int(match.capturedEnd(0));
            node.title = match.captured(0);
            static const QRegularExpression name(QStringLiteral("^</?([a-zA-Z][a-zA-Z0-9-]*)"));
            const QRegularExpressionMatch named = name.match(node.title);
            if (named.hasMatch()) {
                node.target = named.captured(1).toLower();
                if (node.title.startsWith(QLatin1String("</")))
                    node.target.prepend(QLatin1Char('/'));
            } else {
                node.target = QStringLiteral("!--");
            }
            addNode(node);
            return node.end;
        }
        return 0;
    }

    int entity(int i)
    {
        static const QRegularExpression pattern(
            QStringLiteral("\\G&(#[0-9]{1,7}|#[xX][0-9a-fA-F]{1,6}|[A-Za-z][A-Za-z0-9]{1,31});"));
        const QRegularExpressionMatch match = pattern.match(s, i);
        if (!match.hasMatch())
            return 0;
        const QString decoded = decodeEntity(match.captured(0));
        if (decoded.isEmpty())
            return 0;
        flushText(i);
        InlineNode node;
        node.type = InlineType::Entity;
        node.start = node.innerStart = i;
        node.end = node.innerEnd = int(match.capturedEnd(0));
        node.target = decoded;
        addNode(node);
        return node.end;
    }

    int lineBreak(int i)
    {
        flushText(i);
        InlineNode node;
        node.type = InlineType::SoftBreak;
        node.start = node.innerStart = i;
        node.end = node.innerEnd = i + 1;
        addNode(node);
        return i + 1;
    }

    int bareUrl(int i)
    {
        const QChar before = at(i - 1);
        if (i > 0 && !isWhite(before) && before != QLatin1Char('(') && before != QLatin1Char('*')
            && before != QLatin1Char('_') && before != QLatin1Char('~'))
            return 0;
        static const QRegularExpression pattern(QStringLiteral("\\G(https?://|www\\.)[^\\s<]+"));
        const QRegularExpressionMatch match = pattern.match(s, i);
        if (!match.hasMatch())
            return 0;
        int end = int(match.capturedEnd(0));
        // Trailing punctuation is the sentence's, as is a ')' with no '(' in the link.
        while (end > i) {
            const QChar last = s.at(end - 1);
            if (QStringLiteral(".,:;!?\"'*_~").contains(last)) {
                --end;
                continue;
            }
            if (last == QLatin1Char(')')) {
                const QStringView url = QStringView(s).mid(i, end - i);
                if (url.count(QLatin1Char(')')) > url.count(QLatin1Char('('))) {
                    --end;
                    continue;
                }
            }
            break;
        }
        if (end - i < 8)
            return 0;
        flushText(i);
        InlineNode node;
        node.type = InlineType::AutoLink;
        node.start = node.innerStart = i;
        node.end = node.innerEnd = end;
        node.target = s.mid(i, end - i);
        if (node.target.startsWith(QLatin1String("www.")))
            node.target.prepend(QStringLiteral("http://"));
        addNode(node);
        return end;
    }

    void removeDelimiter(int index) { delimiters.removeAt(index); }

    void processEmphasis(int bottom)
    {
        int closerIndex = bottom;
        while (closerIndex < delimiters.size()) {
            Delimiter closer = delimiters[closerIndex];
            if (!closer.canClose) {
                ++closerIndex;
                continue;
            }
            int openerIndex = -1;
            for (int k = closerIndex - 1; k >= bottom; --k) {
                const Delimiter &candidate = delimiters[k];
                if (candidate.ch != closer.ch || !candidate.canOpen)
                    continue;
                if (closer.ch == QLatin1Char('*') || closer.ch == QLatin1Char('_')) {
                    const bool oddMatch = (candidate.canClose || closer.canOpen)
                        && (candidate.originalLength + closer.originalLength) % 3 == 0
                        && !(candidate.originalLength % 3 == 0 && closer.originalLength % 3 == 0);
                    if (oddMatch)
                        continue;
                }
                openerIndex = k;
                break;
            }
            if (openerIndex < 0) {
                if (!closer.canOpen)
                    removeDelimiter(closerIndex);
                else
                    ++closerIndex;
                continue;
            }

            Delimiter &opener = delimiters[openerIndex];
            int use = 2;
            if (closer.ch == QLatin1Char('*') || closer.ch == QLatin1Char('_'))
                use = opener.length >= 2 && closer.length >= 2 ? 2 : 1;

            const int on = indexOf(opener.id);
            const int cn = indexOf(closer.id);
            InlineNode wrapper;
            if (closer.ch == QLatin1Char('~'))
                wrapper.type = InlineType::Strike;
            else if (closer.ch == QLatin1Char('='))
                wrapper.type = InlineType::Highlight;
            else
                wrapper.type = use == 2 ? InlineType::Strong : InlineType::Emph;
            wrapper.start = nodes[on].end - use;
            wrapper.innerStart = nodes[on].end;
            wrapper.innerEnd = nodes[cn].start;
            wrapper.end = nodes[cn].start + use;
            wrapper.children = nodes.mid(on + 1, cn - on - 1);
            wrapper.id = nextId++;

            nodes[on].end -= use;
            nodes[on].innerEnd = nodes[on].end;
            nodes[cn].start += use;
            nodes[cn].innerStart = nodes[cn].start;
            opener.length -= use;
            delimiters[closerIndex].length -= use;

            nodes.remove(on + 1, cn - on - 1);
            nodes.insert(on + 1, wrapper);
            // Delimiters between the two are used up.
            delimiters.remove(openerIndex + 1, closerIndex - openerIndex - 1);
            closerIndex = openerIndex + 1;

            if (delimiters[openerIndex].length == 0) {
                nodes.removeAt(indexOf(delimiters[openerIndex].id));
                removeDelimiter(openerIndex);
                --closerIndex;
            }
            if (delimiters[closerIndex].length == 0) {
                nodes.removeAt(indexOf(delimiters[closerIndex].id));
                removeDelimiter(closerIndex);
            }
        }
        delimiters.resize(bottom);
    }

    /// Wraps <u>...</u> and the like into one node, at every level.
    static void pairTags(QList<InlineNode> &list)
    {
        for (InlineNode &node : list)
            pairTags(node.children);
        for (int i = 0; i < list.size(); ++i) {
            const InlineNode &open = list[i];
            if (open.type != InlineType::HtmlTag || open.target.startsWith(QLatin1Char('/'))
                || open.target == QLatin1String("!--") || isVoidTag(open.target) || open.title.endsWith(QLatin1String("/>")))
                continue;
            const QString closing = QLatin1Char('/') + open.target;
            int depth = 0;
            int match = -1;
            for (int j = i + 1; j < list.size(); ++j) {
                if (list[j].type != InlineType::HtmlTag)
                    continue;
                if (list[j].target == open.target)
                    ++depth;
                else if (list[j].target == closing && depth-- == 0) {
                    match = j;
                    break;
                }
            }
            if (match < 0)
                continue;
            InlineNode span;
            span.type = InlineType::HtmlSpan;
            span.start = open.start;
            span.innerStart = open.end;
            span.innerEnd = list[match].start;
            span.end = list[match].end;
            span.target = open.target;
            span.title = open.title;
            span.children = list.mid(i + 1, match - i - 1);
            span.id = open.id;
            list.remove(i, match - i + 1);
            list.insert(i, span);
        }
    }
};

void appendPlain(const QString &text, const QList<InlineNode> &nodes, QString &out)
{
    for (const InlineNode &node : nodes) {
        switch (node.type) {
        case InlineType::Text:
        case InlineType::Escape:
        case InlineType::Code:
        case InlineType::AutoLink:
            out += QStringView(text).mid(node.innerStart, node.innerEnd - node.innerStart);
            break;
        case InlineType::Math:
            out += node.target;
            break;
        case InlineType::Entity:
            out += node.target;
            break;
        case InlineType::SoftBreak:
        case InlineType::HardBreak:
            out += QLatin1Char(' ');
            break;
        case InlineType::HtmlTag:
        case InlineType::FootnoteRef:
            break;
        default:
            appendPlain(text, node.children, out);
            break;
        }
    }
}

} // namespace

QList<InlineNode> parseInline(const QString &text, const LinkReferences *references)
{
    return Parser(text, references).run();
}

QString plainText(const QString &text, const QList<InlineNode> &nodes)
{
    QString out;
    appendPlain(text, nodes, out);
    return out;
}

QString referenceKey(const QString &label)
{
    return label.simplified().toCaseFolded();
}

QString decodeEntity(const QString &entity)
{
    static const QHash<QString, QString> named = {
        {QStringLiteral("amp"), QStringLiteral("&")},      {QStringLiteral("lt"), QStringLiteral("<")},
        {QStringLiteral("gt"), QStringLiteral(">")},       {QStringLiteral("quot"), QStringLiteral("\"")},
        {QStringLiteral("apos"), QStringLiteral("'")},     {QStringLiteral("nbsp"), QString(QChar(0xa0))},
        {QStringLiteral("copy"), QString(QChar(0xa9))},    {QStringLiteral("reg"), QString(QChar(0xae))},
        {QStringLiteral("trade"), QString(QChar(0x2122))}, {QStringLiteral("hellip"), QString(QChar(0x2026))},
        {QStringLiteral("mdash"), QString(QChar(0x2014))}, {QStringLiteral("ndash"), QString(QChar(0x2013))},
        {QStringLiteral("laquo"), QString(QChar(0xab))},   {QStringLiteral("raquo"), QString(QChar(0xbb))},
        {QStringLiteral("times"), QString(QChar(0xd7))},   {QStringLiteral("divide"), QString(QChar(0xf7))},
        {QStringLiteral("deg"), QString(QChar(0xb0))},     {QStringLiteral("plusmn"), QString(QChar(0xb1))},
        {QStringLiteral("middot"), QString(QChar(0xb7))},  {QStringLiteral("bull"), QString(QChar(0x2022))},
        {QStringLiteral("larr"), QString(QChar(0x2190))},  {QStringLiteral("rarr"), QString(QChar(0x2192))},
        {QStringLiteral("uarr"), QString(QChar(0x2191))},  {QStringLiteral("darr"), QString(QChar(0x2193))},
        {QStringLiteral("harr"), QString(QChar(0x2194))},  {QStringLiteral("le"), QString(QChar(0x2264))},
        {QStringLiteral("ge"), QString(QChar(0x2265))},    {QStringLiteral("ne"), QString(QChar(0x2260))},
        {QStringLiteral("emsp"), QString(QChar(0x2003))},  {QStringLiteral("ensp"), QString(QChar(0x2002))},
        {QStringLiteral("thinsp"), QString(QChar(0x2009))}, {QStringLiteral("zwj"), QString(QChar(0x200d))},
        {QStringLiteral("lsquo"), QString(QChar(0x2018))}, {QStringLiteral("rsquo"), QString(QChar(0x2019))},
        {QStringLiteral("ldquo"), QString(QChar(0x201c))}, {QStringLiteral("rdquo"), QString(QChar(0x201d))},
        {QStringLiteral("sect"), QString(QChar(0xa7))},    {QStringLiteral("para"), QString(QChar(0xb6))},
        {QStringLiteral("euro"), QString(QChar(0x20ac))},  {QStringLiteral("pound"), QString(QChar(0xa3))},
        {QStringLiteral("yen"), QString(QChar(0xa5))},     {QStringLiteral("cent"), QString(QChar(0xa2))},
    };
    if (entity.size() < 3 || !entity.startsWith(QLatin1Char('&')) || !entity.endsWith(QLatin1Char(';')))
        return {};
    const QString body = entity.mid(1, entity.size() - 2);
    if (body.startsWith(QLatin1Char('#'))) {
        bool ok = false;
        const uint code = body.at(1).toLower() == QLatin1Char('x') ? body.mid(2).toUInt(&ok, 16) : body.mid(1).toUInt(&ok);
        if (!ok || code == 0 || code > 0x10ffff)
            return QString(QChar(0xfffd));
        const char32_t point = code;
        return QString::fromUcs4(&point, 1);
    }
    return named.value(body);
}

QString htmlAttribute(const QString &tag, const QString &name)
{
    const QRegularExpression pattern(QStringLiteral("\\s%1\\s*=\\s*(\"([^\"]*)\"|'([^']*)'|([^\\s\"'=<>`/]+))")
                                         .arg(QRegularExpression::escape(name)),
                                     QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(tag);
    if (!match.hasMatch())
        return {};
    for (int group : {2, 3, 4}) {
        if (match.capturedStart(group) >= 0)
            return match.captured(group);
    }
    return {};
}

} // namespace ot
