#pragma once

#include "EditorTheme.h"

#include <QPlainTextEdit>

namespace ot {

class MarkdownHighlighter;

/// Source code mode: the Markdown as plain text, coloured.
class SourceEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit SourceEditor(QWidget *parent = nullptr);

    void setMarkdown(const QString &markdown);
    QString markdown() const;
    void setTheme(const EditorTheme &theme);
    void setZoom(qreal zoom);
    int caretOffset() const;
    void setCaretOffset(int offset);

    void setSearch(const QString &text, bool caseSensitive, bool wholeWord, bool regex);
    bool findNext(bool backwards);
    void replaceCurrent(const QString &replacement);
    int replaceAll(const QString &replacement);
    int matchCount() const;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void updateMargins();
    void updateSearchHighlights();
    QRegularExpression searchExpression() const;

    EditorTheme m_theme;
    qreal m_zoom = 1.0;
    MarkdownHighlighter *m_highlighter;
    QString m_search;
    bool m_caseSensitive = false;
    bool m_wholeWord = false;
    bool m_regex = false;
};

} // namespace ot
