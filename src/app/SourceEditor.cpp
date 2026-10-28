#include "SourceEditor.h"

#include <QKeyEvent>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBlock>

namespace ot {

/// Colours Markdown syntax. Block state: 1 inside a code fence, 2 inside a math block.
class MarkdownHighlighter : public QSyntaxHighlighter
{
public:
    explicit MarkdownHighlighter(QTextDocument *document)
        : QSyntaxHighlighter(document)
    {
    }

    void setTheme(const EditorTheme &theme)
    {
        m_theme = theme;
        rehighlight();
    }

protected:
    void highlightBlock(const QString &text) override
    {
        static const QRegularExpression fence(QStringLiteral("^ {0,3}(`{3,}|~{3,})"));
        const int previous = previousBlockState();
        QTextCharFormat marker;
        marker.setForeground(m_theme.marker);
        QTextCharFormat code;
        code.setForeground(m_theme.hlString);

        if (previous == 1 || previous == 2) {
            const bool closes = previous == 1 ? fence.match(text).hasMatch() : text.trimmed().endsWith(QLatin1String("$$"));
            setFormat(0, int(text.size()), closes ? marker : code);
            setCurrentBlockState(closes ? 0 : previous);
            return;
        }
        if (fence.match(text).hasMatch()) {
            setFormat(0, int(text.size()), marker);
            setCurrentBlockState(1);
            return;
        }
        if (text.trimmed() == QLatin1String("$$") || (text.trimmed().startsWith(QLatin1String("$$")) && !text.trimmed().endsWith(QLatin1String("$$"))) ) {
            setFormat(0, int(text.size()), marker);
            setCurrentBlockState(2);
            return;
        }
        setCurrentBlockState(0);

        static const QRegularExpression heading(QStringLiteral("^ {0,3}(#{1,6})\\s.*$"));
        if (const QRegularExpressionMatch m = heading.match(text); m.hasMatch()) {
            QTextCharFormat format;
            format.setForeground(m_theme.heading);
            format.setFontWeight(QFont::Bold);
            setFormat(0, int(text.size()), format);
            setFormat(int(m.capturedStart(1)), int(m.capturedLength(1)), marker);
            return;
        }
        static const QRegularExpression structure(QStringLiteral("^(\\s*)(>\\s?)+|^(\\s*)([-+*]|\\d+[.)])\\s(\\[[ xX]\\]\\s)?"));
        if (const QRegularExpressionMatch m = structure.match(text); m.hasMatch())
            setFormat(0, int(m.capturedLength()), marker);
        static const QRegularExpression rule(QStringLiteral("^ {0,3}([-*_])(\\s*\\1){2,}\\s*$"));
        if (rule.match(text).hasMatch()) {
            setFormat(0, int(text.size()), marker);
            return;
        }

        const auto apply = [&](const QRegularExpression &pattern, const QTextCharFormat &format, int markerLength) {
            QRegularExpressionMatchIterator it = pattern.globalMatch(text);
            while (it.hasNext()) {
                const QRegularExpressionMatch m = it.next();
                setFormat(int(m.capturedStart()), int(m.capturedLength()), format);
                if (markerLength > 0) {
                    setFormat(int(m.capturedStart()), markerLength, marker);
                    setFormat(int(m.capturedEnd()) - markerLength, markerLength, marker);
                }
            }
        };
        QTextCharFormat strong;
        strong.setFontWeight(QFont::Bold);
        QTextCharFormat emphasis;
        emphasis.setFontItalic(true);
        QTextCharFormat strike;
        strike.setFontStrikeOut(true);
        QTextCharFormat link;
        link.setForeground(m_theme.link);
        QTextCharFormat math;
        math.setForeground(m_theme.hlNumber);
        static const QRegularExpression strongPattern(QStringLiteral("(\\*\\*|__)(?=\\S)(.+?)(?<=\\S)\\1"));
        static const QRegularExpression emphasisPattern(QStringLiteral("(?<![*_\\w])([*_])(?=\\S)([^*_]+?)(?<=\\S)\\1(?![*_\\w])"));
        static const QRegularExpression strikePattern(QStringLiteral("~~(?=\\S)(.+?)(?<=\\S)~~"));
        static const QRegularExpression linkPattern(QStringLiteral("!?\\[[^\\]]*\\]\\([^)]*\\)|<https?://[^>]+>|https?://\\S+"));
        static const QRegularExpression codePattern(QStringLiteral("(`+)(.+?)\\1"));
        static const QRegularExpression mathPattern(QStringLiteral("\\$(?=\\S)[^$]+?(?<=\\S)\\$"));
        static const QRegularExpression htmlPattern(QStringLiteral("</?[a-zA-Z][^>]*>"));
        apply(strongPattern, strong, 2);
        apply(emphasisPattern, emphasis, 1);
        apply(strikePattern, strike, 2);
        apply(linkPattern, link, 0);
        apply(mathPattern, math, 0);
        apply(htmlPattern, marker, 0);
        apply(codePattern, code, 0);
    }

private:
    EditorTheme m_theme = EditorTheme::github();
};

SourceEditor::SourceEditor(QWidget *parent)
    : QPlainTextEdit(parent)
    , m_theme(EditorTheme::github())
{
    setFrameShape(QFrame::NoFrame);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_highlighter = new MarkdownHighlighter(document());
    setTheme(m_theme);
}

void SourceEditor::setMarkdown(const QString &markdown)
{
    setPlainText(markdown);
    document()->setModified(false);
}

QString SourceEditor::markdown() const
{
    QString text = toPlainText();
    if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n')))
        text += QLatin1Char('\n');
    return text;
}

void SourceEditor::setTheme(const EditorTheme &theme)
{
    m_theme = theme;
    QPalette palette = this->palette();
    palette.setColor(QPalette::Base, theme.background);
    palette.setColor(QPalette::Text, theme.text);
    palette.setColor(QPalette::Highlight, theme.selection);
    palette.setColor(QPalette::HighlightedText, theme.dark ? QColor(Qt::white) : theme.text);
    setPalette(palette);
    m_highlighter->setTheme(theme);
    setZoom(m_zoom);
}

void SourceEditor::setZoom(qreal zoom)
{
    m_zoom = zoom;
    QFont font = m_theme.monoFont(zoom, 0.92);
    setFont(font);
    document()->setDefaultFont(font);
    setTabStopDistance(QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')) * 4);
    updateMargins();
}

void SourceEditor::updateMargins()
{
    const int width = int(m_theme.maxWidth * m_zoom);
    const int side = std::max(int(30 * m_zoom), (viewport()->width() + viewportMargins().left() + viewportMargins().right() - width) / 2);
    setViewportMargins(side, int(30 * m_zoom), side, 0);
}

void SourceEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    updateMargins();
}

void SourceEditor::keyPressEvent(QKeyEvent *event)
{
    // Keep the indentation of the line above.
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && event->modifiers() == Qt::NoModifier) {
        const QString line = textCursor().block().text();
        int indent = 0;
        while (indent < line.size() && (line.at(indent) == QLatin1Char(' ') || line.at(indent) == QLatin1Char('\t')))
            ++indent;
        QPlainTextEdit::keyPressEvent(event);
        textCursor().insertText(line.left(indent));
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

int SourceEditor::caretOffset() const
{
    return textCursor().position();
}

void SourceEditor::setCaretOffset(int offset)
{
    QTextCursor cursor = textCursor();
    cursor.setPosition(std::clamp(offset, 0, document()->characterCount() - 1));
    setTextCursor(cursor);
    centerCursor();
}

QRegularExpression SourceEditor::searchExpression() const
{
    QString pattern = m_regex ? m_search : QRegularExpression::escape(m_search);
    if (m_wholeWord)
        pattern = QStringLiteral("\\b(?:%1)\\b").arg(pattern);
    return QRegularExpression(pattern, m_caseSensitive ? QRegularExpression::NoPatternOption
                                                       : QRegularExpression::CaseInsensitiveOption);
}

void SourceEditor::setSearch(const QString &text, bool caseSensitive, bool wholeWord, bool regex)
{
    m_search = text;
    m_caseSensitive = caseSensitive;
    m_wholeWord = wholeWord;
    m_regex = regex;
    updateSearchHighlights();
}

void SourceEditor::updateSearchHighlights()
{
    QList<QTextEdit::ExtraSelection> selections;
    const QRegularExpression expression = searchExpression();
    if (!m_search.isEmpty() && expression.isValid()) {
        const QString text = toPlainText();
        QRegularExpressionMatchIterator it = expression.globalMatch(text);
        while (it.hasNext() && selections.size() < 5000) {
            const QRegularExpressionMatch match = it.next();
            if (match.capturedLength() == 0)
                continue;
            QTextEdit::ExtraSelection selection;
            selection.cursor = QTextCursor(document());
            selection.cursor.setPosition(int(match.capturedStart()));
            selection.cursor.setPosition(int(match.capturedEnd()), QTextCursor::KeepAnchor);
            selection.format.setBackground(m_theme.findMatch);
            selections.append(selection);
        }
    }
    setExtraSelections(selections);
}

int SourceEditor::matchCount() const
{
    const QRegularExpression expression = searchExpression();
    if (m_search.isEmpty() || !expression.isValid())
        return 0;
    int count = 0;
    QRegularExpressionMatchIterator it = expression.globalMatch(toPlainText());
    while (it.hasNext())
        count += it.next().capturedLength() > 0 ? 1 : 0;
    return count;
}

bool SourceEditor::findNext(bool backwards)
{
    const QRegularExpression expression = searchExpression();
    if (m_search.isEmpty() || !expression.isValid())
        return false;
    QTextCursor found = document()->find(expression, textCursor(), backwards ? QTextDocument::FindBackward : QTextDocument::FindFlags());
    if (found.isNull())
        found = document()->find(expression, backwards ? document()->characterCount() - 1 : 0,
                                 backwards ? QTextDocument::FindBackward : QTextDocument::FindFlags());
    if (found.isNull())
        return false;
    setTextCursor(found);
    return true;
}

void SourceEditor::replaceCurrent(const QString &replacement)
{
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection() && searchExpression().match(cursor.selectedText()).capturedLength() == cursor.selectedText().size())
        cursor.insertText(m_regex ? cursor.selectedText().replace(searchExpression(), replacement) : replacement);
    findNext(false);
    updateSearchHighlights();
}

int SourceEditor::replaceAll(const QString &replacement)
{
    const QRegularExpression expression = searchExpression();
    if (m_search.isEmpty() || !expression.isValid())
        return 0;
    const int count = matchCount();
    QString text = toPlainText();
    QString escaped = replacement;
    if (!m_regex)
        escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    text.replace(expression, escaped);
    QTextCursor cursor(document());
    cursor.select(QTextCursor::Document);
    cursor.insertText(text);
    updateSearchHighlights();
    return count;
}

} // namespace ot
