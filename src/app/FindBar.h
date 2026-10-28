#pragma once

#include "EditorTheme.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QToolButton;

namespace ot {

class MarkdownEditor;
class SourceEditor;

/// Find and replace, above the document.
class FindBar : public QWidget
{
    Q_OBJECT

public:
    explicit FindBar(MarkdownEditor *editor, QWidget *parent = nullptr);

    void setTheme(const EditorTheme &theme);
    /// While set, searches go to the source view instead.
    void setSourceEditor(SourceEditor *source);
    void open(bool replace);
    void next();
    void previous();

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void search();
    void find(bool backwards);
    void replaceOne();
    void replaceEvery();
    void close();
    void updateCount();

    MarkdownEditor *m_editor;
    SourceEditor *m_source = nullptr;
    QLineEdit *m_find;
    QLineEdit *m_replace;
    QWidget *m_replaceRow;
    QToolButton *m_case;
    QToolButton *m_word;
    QToolButton *m_regex;
    QLabel *m_count;
};

} // namespace ot
