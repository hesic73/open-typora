#include "FindBar.h"

#include "MarkdownEditor.h"
#include "SourceEditor.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QToolButton>
#include <QVBoxLayout>

namespace ot {

FindBar::FindBar(MarkdownEditor *editor, QWidget *parent)
    : QWidget(parent)
    , m_editor(editor)
{
    setObjectName(QStringLiteral("findBar"));
    setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 6, 12, 6);
    layout->setSpacing(4);

    auto *findRow = new QHBoxLayout;
    findRow->setSpacing(4);
    m_find = new QLineEdit;
    m_find->setPlaceholderText(tr("Find"));
    m_find->setClearButtonEnabled(true);
    m_case = new QToolButton;
    m_case->setText(QStringLiteral("Aa"));
    m_case->setToolTip(tr("Match case"));
    m_word = new QToolButton;
    m_word->setText(QStringLiteral("W"));
    m_word->setToolTip(tr("Whole words"));
    m_regex = new QToolButton;
    m_regex->setText(QStringLiteral(".*"));
    m_regex->setToolTip(tr("Regular expression"));
    for (QToolButton *button : {m_case, m_word, m_regex}) {
        button->setCheckable(true);
        button->setAutoRaise(true);
        connect(button, &QToolButton::toggled, this, &FindBar::search);
    }
    auto *previousButton = new QToolButton;
    previousButton->setText(QStringLiteral("↑"));
    previousButton->setToolTip(tr("Previous (Shift+Enter)"));
    auto *nextButton = new QToolButton;
    nextButton->setText(QStringLiteral("↓"));
    nextButton->setToolTip(tr("Next (Enter)"));
    auto *closeButton = new QToolButton;
    closeButton->setText(QStringLiteral("✕"));
    closeButton->setToolTip(tr("Close (Esc)"));
    for (QToolButton *button : {previousButton, nextButton, closeButton})
        button->setAutoRaise(true);
    m_count = new QLabel;
    m_count->setMinimumWidth(80);
    findRow->addWidget(m_find, 1);
    findRow->addWidget(m_case);
    findRow->addWidget(m_word);
    findRow->addWidget(m_regex);
    findRow->addWidget(m_count);
    findRow->addWidget(previousButton);
    findRow->addWidget(nextButton);
    findRow->addWidget(closeButton);
    layout->addLayout(findRow);

    m_replaceRow = new QWidget;
    auto *replaceRow = new QHBoxLayout(m_replaceRow);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    replaceRow->setSpacing(4);
    m_replace = new QLineEdit;
    m_replace->setPlaceholderText(tr("Replace with"));
    auto *replaceButton = new QToolButton;
    replaceButton->setText(tr("Replace"));
    auto *replaceAllButton = new QToolButton;
    replaceAllButton->setText(tr("All"));
    replaceAllButton->setToolTip(tr("Replace all"));
    replaceRow->addWidget(m_replace, 1);
    replaceRow->addWidget(replaceButton);
    replaceRow->addWidget(replaceAllButton);
    layout->addWidget(m_replaceRow);

    connect(m_find, &QLineEdit::textChanged, this, &FindBar::search);
    connect(m_find, &QLineEdit::returnPressed, this, [this] { find(false); });
    connect(m_replace, &QLineEdit::returnPressed, this, &FindBar::replaceOne);
    connect(previousButton, &QToolButton::clicked, this, [this] { find(true); });
    connect(nextButton, &QToolButton::clicked, this, [this] { find(false); });
    connect(closeButton, &QToolButton::clicked, this, &FindBar::close);
    connect(replaceButton, &QToolButton::clicked, this, &FindBar::replaceOne);
    connect(replaceAllButton, &QToolButton::clicked, this, &FindBar::replaceEvery);
}

void FindBar::setTheme(const EditorTheme &theme)
{
    const QString sheet = QStringLiteral(
        "#findBar { background: %1; border-bottom: 1px solid %2; }"
        "#findBar QLineEdit { background: %3; color: %4; border: 1px solid %2; border-radius: 3px; padding: 3px 6px; }"
        "#findBar QToolButton { color: %5; border: none; padding: 3px 6px; border-radius: 3px; }"
        "#findBar QToolButton:hover { background: %6; color: %4; }"
        "#findBar QToolButton:checked { background: %6; color: %4; }"
        "#findBar QLabel { color: %5; }")
                              .arg(theme.sidebarBackground.name(), theme.border.name(), theme.background.name(),
                                   theme.text.name(), theme.muted.name(), theme.sidebarActive.name());
    setStyleSheet(sheet);
}

void FindBar::setSourceEditor(SourceEditor *source)
{
    m_source = source;
    if (isVisible())
        search();
}

void FindBar::open(bool replace)
{
    m_replaceRow->setVisible(replace);
    show();
    QString selected = m_source ? m_source->textCursor().selectedText() : m_editor->selectedMarkdown();
    if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator) && !selected.contains(QLatin1Char('\n')))
        m_find->setText(selected);
    m_find->setFocus();
    m_find->selectAll();
    search();
}

void FindBar::next()
{
    if (!isVisible())
        open(false);
    find(false);
}

void FindBar::previous()
{
    if (!isVisible())
        open(false);
    find(true);
}

void FindBar::search()
{
    MarkdownEditor::FindOptions options;
    options.caseSensitive = m_case->isChecked();
    options.wholeWord = m_word->isChecked();
    options.regex = m_regex->isChecked();
    m_editor->setSearch(isVisible() ? m_find->text() : QString(), options);
    if (m_source)
        m_source->setSearch(isVisible() ? m_find->text() : QString(), options.caseSensitive, options.wholeWord,
                            options.regex);
    updateCount();
}

void FindBar::updateCount()
{
    if (m_find->text().isEmpty()) {
        m_count->clear();
        return;
    }
    const int count = m_source ? m_source->matchCount() : m_editor->matchCount();
    m_count->setText(count == 0 ? tr("No results") : tr("%n match(es)", nullptr, count));
}

void FindBar::find(bool backwards)
{
    if (m_find->text().isEmpty())
        return;
    if (m_source)
        m_source->findNext(backwards);
    else
        m_editor->findNext(backwards);
}

void FindBar::replaceOne()
{
    if (m_source)
        m_source->replaceCurrent(m_replace->text());
    else
        m_editor->replaceCurrent(m_replace->text());
    updateCount();
}

void FindBar::replaceEvery()
{
    if (m_source)
        m_source->replaceAll(m_replace->text());
    else
        m_editor->replaceAll(m_replace->text());
    updateCount();
}

void FindBar::close()
{
    hide();
    search();
    if (m_source)
        m_source->setFocus();
    else
        m_editor->setFocus();
}

void FindBar::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && (event->modifiers() & Qt::ShiftModifier)) {
        find(true);
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace ot
