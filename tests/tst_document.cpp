#include "Document.h"
#include "Inline.h"

#include <QTest>

using namespace ot;

class TestDocument : public QObject
{
    Q_OBJECT

private slots:
    void kinds_data()
    {
        QTest::addColumn<QString>("source");
        QTest::addColumn<QList<int>>("kinds");
        const auto k = [](std::initializer_list<BlockKind> list) {
            QList<int> out;
            for (BlockKind kind : list)
                out.append(int(kind));
            return out;
        };
        QTest::newRow("paragraphs") << "a\nb\n\nc\n" << k({BlockKind::Paragraph, BlockKind::Paragraph});
        QTest::newRow("heading") << "# Title\ntext\n" << k({BlockKind::Heading, BlockKind::Paragraph});
        QTest::newRow("setext") << "Title\n===\n" << k({BlockKind::Heading});
        QTest::newRow("list") << "- a\n- b\n  - c\n" << k({BlockKind::ListItem, BlockKind::ListItem, BlockKind::ListItem});
        QTest::newRow("fence") << "```py\nx\n\ny\n```\nafter\n" << k({BlockKind::CodeFence, BlockKind::Paragraph});
        QTest::newRow("math") << "$$\nx^2\n$$\n" << k({BlockKind::MathBlock});
        QTest::newRow("table") << "| a | b |\n| - | - |\n| 1 | 2 |\n" << k({BlockKind::Table});
        QTest::newRow("quote") << "> a\nlazy\n\n> b\n" << k({BlockKind::Quote, BlockKind::Quote});
        QTest::newRow("rule") << "a\n\n---\n\n* * *\n" << k({BlockKind::Paragraph, BlockKind::Rule, BlockKind::Rule});
        QTest::newRow("front matter") << "---\ntitle: x\n---\n# H\n" << k({BlockKind::FrontMatter, BlockKind::Heading});
        QTest::newRow("html") << "<div>\nhi\n</div>\n" << k({BlockKind::Html});
        QTest::newRow("lone marker") << "-\n" << k({BlockKind::Paragraph});
    }

    void kinds()
    {
        QFETCH(QString, source);
        QFETCH(QList<int>, kinds);
        QList<int> found;
        for (const ParsedBlock &block : parseBlocks(source))
            found.append(int(block.block.kind));
        QCOMPARE(found, kinds);
    }

    void roundTrip_data()
    {
        QTest::addColumn<QString>("source");
        QTest::newRow("mixed") << "# T\n\npara **b**\nnext\n\n- a\n- b\n\n1. x\n2. y\n\n```\ncode\n```\n\n> q\n";
        QTest::newRow("tight") << "Text\n- item\n- item\n# Head\n";
        QTest::newRow("blank lines") << "a\n\n\n\nb\n";
    }

    void roundTrip()
    {
        QFETCH(QString, source);
        QCOMPARE(serializeBlocks(blocksFromMarkdown(source)), source);
    }

    void listInfoTask()
    {
        const ListInfo info = listInfo(QStringLiteral("  - [x] done"));
        QVERIFY(info.valid);
        QCOMPARE(info.indent, 2);
        QVERIFY(info.task);
        QVERIFY(info.checked);
        QCOMPARE(info.contentStart, 8);
    }

    void table()
    {
        const TableInfo info = tableInfo(QStringLiteral("| a | b |\n|:-|-:|\n| 1 |  |"));
        QCOMPARE(info.columns, 2);
        QCOMPARE(info.rows.size(), 2);
        QCOMPARE(info.aligns.value(0), Align::Left);
        QCOMPARE(info.aligns.value(1), Align::Right);
        QCOMPARE(info.rows[1].cells[1].start, info.rows[1].cells[1].end);
    }

    void inlineNodes()
    {
        const QString text = QStringLiteral("a **b *c*** `d` $x$ [l](u) ~~s~~");
        const QList<InlineNode> nodes = parseInline(text);
        QStringList types;
        for (const InlineNode &node : nodes)
            types.append(QString::number(int(node.type)));
        QCOMPARE(plainText(text, nodes), QStringLiteral("a b c d x l s"));
        bool strong = false;
        for (const InlineNode &node : nodes) {
            if (node.type == InlineType::Strong) {
                strong = true;
                QCOMPARE(text.mid(node.start, node.end - node.start), QStringLiteral("**b *c***"));
                QCOMPARE(node.children.size(), 2);
            }
        }
        QVERIFY(strong);
    }

    void dollarsAreNotMath()
    {
        const QString text = QStringLiteral("costs $5 and $10");
        for (const InlineNode &node : parseInline(text))
            QVERIFY(node.type != InlineType::Math);
    }

    void contentMapQuote()
    {
        Block block;
        block.kind = BlockKind::Quote;
        block.text = QStringLiteral("> a\n> b");
        const ContentMap map = contentMap(block);
        QCOMPARE(map.text, QStringLiteral("a\nb"));
        QCOMPARE(map.map(2), 6);
    }
};

QTEST_GUILESS_MAIN(TestDocument)
#include "tst_document.moc"
