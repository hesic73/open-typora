#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace ot {

// The document is a list of blocks, each holding the exact Markdown source it
// was read from. Editing changes a block's source; the block is then parsed
// again, which may change its kind or split it into several blocks. Nothing is
// ever rewritten behind the user's back, so a file that is opened and saved
// without edits comes back the same (apart from blank lines at its ends).

enum class BlockKind {
    Paragraph,
    Heading,
    CodeFence,
    IndentedCode,
    MathBlock,
    Quote,
    ListItem,
    Table,
    Rule,
    Html,
    FrontMatter,
};

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    QString text;
    /// Blank lines between this block and the next in the file.
    int blankAfter = 1;
    /// Changes whenever the text or kind does; the editor's caches key on it.
    quint64 revision = 0;

    static quint64 nextRevision();
};

struct ParsedBlock {
    Block block;
    /// Offset of the block's text in the parsed source.
    qsizetype start = 0;
};

/// A range of block text, [start, end).
struct Segment {
    int start = 0;
    int end = 0;
};

/// Splits Markdown into blocks. Line endings must already be "\n".
QList<ParsedBlock> parseBlocks(const QString &source);

/// Joins blocks back into Markdown. `offsets`, when given, receives where each
/// block's text starts in the result.
QString serializeBlocks(const QList<Block> &blocks, QList<qsizetype> *offsets = nullptr);

/// The blocks of a file, never empty: an empty file is one empty paragraph.
QList<Block> blocksFromMarkdown(const QString &source);

// ---- What a block's text says about itself -------------------------------

/// 1..6 for headings (ATX or setext), 0 otherwise.
int headingLevel(const Block &block);

struct ListInfo {
    bool valid = false;
    int indent = 0;          // leading spaces
    bool ordered = false;
    int number = 1;          // ordered lists
    QChar delimiter;         // '.' or ')' for ordered, the bullet otherwise
    int markerStart = 0;
    int markerEnd = 0;       // past the marker, before the spaces
    int contentStart = 0;    // first character of the content (after a task box)
    int contentIndent = 0;   // continuation lines are indented this far
    bool task = false;
    bool checked = false;
    int taskStart = -1;      // position of '[' of the task box
};
ListInfo listInfo(const QString &text);

struct FenceInfo {
    bool valid = false;
    QChar fenceChar;
    int fenceLength = 0;
    int indent = 0;
    QString language;
    Segment code;            // the lines between the fences, without the final newline
    bool closed = false;
};
FenceInfo fenceInfo(const QString &text);

struct MathInfo {
    Segment latex;           // the formula, without the $$ fences
};
MathInfo mathInfo(const QString &text);

enum class Align { None, Left, Center, Right };
struct TableInfo {
    struct Row {
        int lineStart = 0;
        int lineEnd = 0;
        QList<Segment> cells; // trimmed cell contents
    };
    QList<Row> rows;          // header first; the delimiter row is not listed
    Segment delimiterLine;
    QList<Align> aligns;
    int columns = 0;
};
TableInfo tableInfo(const QString &text);
QStringList splitTableRow(const QString &line, QList<Segment> *cells = nullptr);

/// The part of a block that holds inline Markdown, with the structural
/// markers (list markers, quote markers, indentation) taken out. `sourceOf`
/// maps each character of `text`, and one past the end, to the block text.
struct ContentMap {
    QString text;
    QList<int> sourceOf;
    /// For quotes: nesting depth of each content line.
    QList<int> lineDepth;
    int map(int contentIndex) const { return sourceOf.at(qBound(0, contentIndex, int(sourceOf.size()) - 1)); }
    /// Content index of a source position, -1 when it is outside the content.
    int fromSource(int source) const;
};
ContentMap contentMap(const Block &block);

/// Ranges of the block text where the caret may stand (ends included).
QList<Segment> caretSegments(const Block &block);

/// Whether a line starts a list item, and what the item's marker would be.
bool isListLine(QStringView line);
bool isBlankLine(QStringView line);

QString kindName(BlockKind kind);

} // namespace ot
