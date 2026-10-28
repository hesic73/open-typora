#include "MarkdownEditor.h"

#include <QApplication>
#include <QClipboard>
#include <QTest>

using namespace ot;

namespace {

void type(MarkdownEditor &editor, const QString &text)
{
    for (const QChar c : text) {
        if (c == QLatin1Char('\n'))
            QTest::keyClick(&editor, Qt::Key_Return);
        else
            QTest::keyClicks(&editor, QString(c));
    }
}

QList<BlockKind> kinds(const MarkdownEditor &editor)
{
    QList<BlockKind> out;
    for (const Block &block : editor.blocks())
        out.append(block.kind);
    return out;
}

} // namespace

class TestEditor : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<MarkdownEditor> make(const QString &markdown = {})
    {
        auto editor = std::make_unique<MarkdownEditor>();
        editor->resize(900, 700);
        editor->setMarkdown(markdown);
        return editor;
    }

private slots:
    void typingHeading()
    {
        auto e = make();
        type(*e, QStringLiteral("# Hello"));
        QCOMPARE(e->markdown(), QStringLiteral("# Hello\n"));
        QCOMPARE(kinds(*e), QList<BlockKind>{BlockKind::Heading});
    }

    void headingMarksStayHidden()
    {
        auto e = make(QStringLiteral("## Title\n"));
        e->setCaret({0, 0});
        QCOMPARE(e->caret().offset, 3); // the caret cannot stand in "## "
        QTest::keyClick(e.get(), Qt::Key_Backspace);
        QCOMPARE(e->markdown(), QStringLiteral("Title\n"));
    }

    void paragraphsSplitOnEnter()
    {
        auto e = make();
        type(*e, QStringLiteral("one\ntwo"));
        QCOMPARE(e->markdown(), QStringLiteral("one\n\ntwo\n"));
        QTest::keyClick(e.get(), Qt::Key_Home);
        QTest::keyClick(e.get(), Qt::Key_Backspace);
        QCOMPARE(e->markdown(), QStringLiteral("onetwo\n"));
    }

    void listContinuesAndEnds()
    {
        auto e = make();
        type(*e, QStringLiteral("- a\nb\n\nafter"));
        QCOMPARE(e->markdown(), QStringLiteral("- a\n- b\n\nafter\n"));
    }

    void orderedListCounts()
    {
        auto e = make();
        type(*e, QStringLiteral("1. a\nb"));
        QCOMPARE(e->markdown(), QStringLiteral("1. a\n2. b\n"));
    }

    void taskListContinues()
    {
        auto e = make();
        type(*e, QStringLiteral("- [ ] a\nb"));
        QCOMPARE(e->markdown(), QStringLiteral("- [ ] a\n- [ ] b\n"));
    }

    void tabNestsListItem()
    {
        auto e = make();
        type(*e, QStringLiteral("- a\nb"));
        QTest::keyClick(e.get(), Qt::Key_Tab);
        QCOMPARE(e->markdown(), QStringLiteral("- a\n  - b\n"));
        QTest::keyClick(e.get(), Qt::Key_Backtab);
        QCOMPARE(e->markdown(), QStringLiteral("- a\n- b\n"));
    }

    void backspaceAtItemStartMakesParagraph()
    {
        auto e = make();
        type(*e, QStringLiteral("- a"));
        QTest::keyClick(e.get(), Qt::Key_Home);
        QTest::keyClick(e.get(), Qt::Key_Backspace);
        QCOMPARE(e->markdown(), QStringLiteral("a\n"));
    }

    void codeFenceOnEnter()
    {
        auto e = make();
        type(*e, QStringLiteral("```py\nx = 1\ny"));
        QCOMPARE(e->markdown(), QStringLiteral("```py\nx = 1\ny\n```\n"));
        QCOMPARE(kinds(*e), QList<BlockKind>{BlockKind::CodeFence});
    }

    void mathBlockOnEnter()
    {
        auto e = make();
        type(*e, QStringLiteral("$$\nx^2"));
        QCOMPARE(e->markdown(), QStringLiteral("$$\nx^2\n$$\n"));
    }

    void tableOnEnter()
    {
        auto e = make();
        type(*e, QStringLiteral("| a | b |\n1"));
        QCOMPARE(kinds(*e), QList<BlockKind>{BlockKind::Table});
        QVERIFY(e->markdown().contains(QStringLiteral("| 1 ")));
        QTest::keyClick(e.get(), Qt::Key_Tab);
        type(*e, QStringLiteral("2|3"));
        QVERIFY2(e->markdown().contains(QStringLiteral("2\\|3")), qPrintable(e->markdown()));
    }

    void quoteContinuesAndEnds()
    {
        auto e = make();
        type(*e, QStringLiteral("> a\nb\n\nc"));
        QCOMPARE(e->markdown(), QStringLiteral("> a\n> b\n\nc\n"));
    }

    void boldToggle()
    {
        auto e = make(QStringLiteral("hello world\n"));
        e->setCaret({0, 6}, {0, 11});
        e->toggleBold();
        QCOMPARE(e->markdown(), QStringLiteral("hello **world**\n"));
        e->toggleBold();
        QCOMPARE(e->markdown(), QStringLiteral("hello world\n"));
    }

    void undoRedo()
    {
        auto e = make();
        type(*e, QStringLiteral("abc\ndef"));
        e->undo();
        QVERIFY(e->markdown() != QStringLiteral("abc\n\ndef\n"));
        while (e->canUndo())
            e->undo();
        QCOMPARE(e->markdown(), QString());
        e->redo();
        QVERIFY(!e->markdown().isEmpty());
        QVERIFY(e->isModified());
    }

    void arrowsSkipHiddenMarkers()
    {
        auto e = make(QStringLiteral("- one\n- two\n"));
        e->setCaret({0, 5});
        QTest::keyClick(e.get(), Qt::Key_Right);
        QCOMPARE(e->caret().block, 1);
        QCOMPARE(e->caret().offset, 2);
        QTest::keyClick(e.get(), Qt::Key_Left);
        QCOMPARE(e->caret().block, 0);
        QCOMPARE(e->caret().offset, 5);
    }

    void upDownAcrossBlocks()
    {
        auto e = make(QStringLiteral("first\n\nsecond\n\nthird\n"));
        e->setCaret({0, 2});
        QTest::keyClick(e.get(), Qt::Key_Down);
        QCOMPARE(e->caret().block, 1);
        QTest::keyClick(e.get(), Qt::Key_Down);
        QCOMPARE(e->caret().block, 2);
        QTest::keyClick(e.get(), Qt::Key_Up);
        QCOMPARE(e->caret().block, 1);
    }

    void pasteSplitsBlocks()
    {
        auto e = make(QStringLiteral("start\n"));
        e->setCaret({0, 5});
        e->insertMarkdown(QStringLiteral("\n\n- a\n- b"));
        QCOMPARE(kinds(*e), (QList<BlockKind>{BlockKind::Paragraph, BlockKind::ListItem, BlockKind::ListItem}));
        QCOMPARE(e->caret().block, 2);
    }

    void selectionAcrossBlocksDeletes()
    {
        auto e = make(QStringLiteral("alpha\n\n- beta\n\ngamma\n"));
        e->setCaret({2, 2}, {0, 2});
        QTest::keyClick(e.get(), Qt::Key_Backspace);
        QCOMPARE(e->markdown(), QStringLiteral("almma\n"));
    }

    void headingCommands()
    {
        auto e = make(QStringLiteral("title\n"));
        e->setHeading(2);
        QCOMPARE(e->markdown(), QStringLiteral("## title\n"));
        e->setHeading(2);
        QCOMPARE(e->markdown(), QStringLiteral("title\n"));
        e->toggleList(false);
        QCOMPARE(e->markdown(), QStringLiteral("- title\n"));
        e->toggleQuote();
        QCOMPARE(e->markdown(), QStringLiteral("> - title\n"));
    }

    void tableRowsAndColumns()
    {
        auto e = make(QStringLiteral("| a | b |\n| - | - |\n| 1 | 2 |\n"));
        e->setCaret({0, 2});
        e->tableInsertColumn(true);
        QCOMPARE(tableInfo(e->blocks().first().text).columns, 3);
        e->tableInsertRow(true);
        QCOMPARE(tableInfo(e->blocks().first().text).rows.size(), 3);
        e->tableDeleteRow();
        QCOMPARE(tableInfo(e->blocks().first().text).rows.size(), 2);
    }

    void tableMoveRowsAndColumns()
    {
        auto e = make(QStringLiteral("| a | b |\n| - | - |\n| 1 | 2 |\n| 3 | 4 |\n"));
        const TableInfo info = tableInfo(e->blocks().first().text);
        e->setCaret({0, info.rows[1].cells[0].start});
        e->tableMoveRow(1);
        QVERIFY2(e->markdown().indexOf(QLatin1Char('3')) < e->markdown().indexOf(QLatin1Char('1')), qPrintable(e->markdown()));
        e->tableMoveColumn(1);
        const QString first = e->markdown().section(QLatin1Char('\n'), 0, 0);
        QVERIFY2(first.indexOf(QLatin1Char('b')) < first.indexOf(QLatin1Char('a')), qPrintable(first));
        e->tableDelete();
        QCOMPARE(e->markdown(), QString());
    }

    void insertCommands()
    {
        auto e = make(QStringLiteral("# Title\n\ntext\n"));
        e->setCaret({1, 4});
        e->insertFootnote();
        QCOMPARE(e->markdown(), QStringLiteral("# Title\n\ntext[^1]\n\n[^1]: \n"));
        e->insertFrontMatter();
        QCOMPARE(e->blocks().first().kind, BlockKind::FrontMatter);
        QCOMPARE(e->caret().block, 0);
        e->setCaret({1, 0});
        e->insertTableOfContents();
        QVERIFY(e->markdown().contains(QStringLiteral("[TOC]")));
    }

    void codeBlockBackToParagraph()
    {
        auto e = make(QStringLiteral("```\nsome text\n```\n"));
        e->setCaret({0, 6});
        e->setHeading(0);
        QCOMPARE(e->markdown(), QStringLiteral("some text\n"));
        QCOMPARE(e->blocks().first().kind, BlockKind::Paragraph);
    }

    void selectionWords()
    {
        auto e = make(QStringLiteral("one two three\n"));
        e->setCaret({0, 7}, {0, 0});
        QCOMPARE(e->selectionWordCount(), 2);
    }

    void findAndReplace()
    {
        auto e = make(QStringLiteral("cat dog\n\ncat\n"));
        e->setSearch(QStringLiteral("cat"), {});
        QCOMPARE(e->matchCount(), 2);
        QCOMPARE(e->replaceAll(QStringLiteral("cow")), 2);
        QCOMPARE(e->markdown(), QStringLiteral("cow dog\n\ncow\n"));
    }

    void wordCount()
    {
        auto e = make(QStringLiteral("# Hello **world**\n\n中文字\n"));
        QCOMPARE(e->wordCount(), 5);
    }

    void caretOffsetRoundTrip()
    {
        auto e = make(QStringLiteral("# A\n\npara text\n"));
        e->setCaretOffset(9);
        QCOMPARE(e->caret().block, 1);
        QCOMPARE(e->caret().offset, 4);
        QCOMPARE(e->caretOffset(), 9);
    }

    void unchangedFileRoundTrips()
    {
        const QString source = QStringLiteral("---\ntitle: t\n---\n\n# H\n\ntext  \nmore\n\n* a\n* b\n\n    code\n\n<div>x</div>\n");
        auto e = make(source);
        QCOMPARE(e->markdown(), source);
    }
};

QTEST_MAIN(TestEditor)
#include "tst_editor.moc"
