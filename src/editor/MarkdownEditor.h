#pragma once

#include "BlockRenderer.h"
#include "Document.h"
#include "EditorTheme.h"

#include <QAbstractScrollArea>
#include <QBasicTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>

#include <functional>
#include <memory>

class QMenu;
class QMimeData;

namespace ot {

class ImageStore;

/// A place in the document: a block and an offset into its Markdown text.
struct Pos {
    int block = 0;
    int offset = 0;
    friend bool operator==(const Pos &, const Pos &) = default;
    friend auto operator<=>(const Pos &a, const Pos &b)
    {
        if (a.block != b.block)
            return a.block <=> b.block;
        return a.offset <=> b.offset;
    }
};

/// The editor: Markdown shown as formatted text, edited in place. The block
/// with the caret shows the syntax of whatever construct the caret stands in,
/// and only that, as Typora does.
class MarkdownEditor : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit MarkdownEditor(QWidget *parent = nullptr);
    ~MarkdownEditor() override;

    void setMarkdown(const QString &markdown);
    QString markdown() const;
    const QList<Block> &blocks() const { return m_blocks; }

    /// Folder that relative picture paths are read from.
    void setBaseDir(const QString &dir);
    QString baseDir() const { return m_baseDir; }

    void setTheme(const EditorTheme &theme);
    const EditorTheme &theme() const { return m_theme; }
    void setZoom(qreal zoom);
    qreal zoom() const { return m_zoom; }
    void setFocusMode(bool on);
    bool focusMode() const { return m_focusMode; }
    void setTypewriterMode(bool on);
    bool typewriterMode() const { return m_typewriter; }

    bool isModified() const { return m_revision != m_savedRevision; }
    /// Marks the current state as saved.
    void markSaved();

    struct Heading {
        int block = 0;
        int level = 1;
        QString title;
    };
    QList<Heading> headings() const { return headingsOf(m_blocks); }
    static QList<Heading> headingsOf(const QList<Block> &blocks);
    static int wordCountOf(const QList<Block> &blocks);
    static int characterCountOf(const QList<Block> &blocks);
    void goToBlock(int block);
    int caretBlock() const { return m_caret.block; }

    /// The caret as an offset into markdown(), for switching to source mode and back.
    int caretOffset() const;
    void setCaretOffset(int offset);

    Pos caret() const { return m_caret; }
    Pos anchor() const { return m_anchor; }
    void setCaret(Pos caret, Pos anchor);
    void setCaret(Pos caret) { setCaret(caret, caret); }
    bool hasSelection() const { return m_caret != m_anchor; }
    QString selectedMarkdown() const;

    // Editing
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    void undo();
    void redo();
    void cut();
    /// Copies the selection as Markdown, and as formatted text for other applications.
    void copy();
    void copyAsMarkdown();
    void paste();
    void pastePlain();
    void selectAll();
    void deleteSelection();
    /// Inserts Markdown at the caret, parsing what it holds into blocks.
    void insertMarkdown(const QString &markdown);
    /// Types text at the caret, as if from the keyboard.
    void typeText(const QString &text);

    // Format menu
    void toggleBold() { toggleWrap(QStringLiteral("**"), InlineType::Strong); }
    void toggleItalic() { toggleWrap(QStringLiteral("*"), InlineType::Emph); }
    void toggleStrike() { toggleWrap(QStringLiteral("~~"), InlineType::Strike); }
    void toggleHighlight() { toggleWrap(QStringLiteral("=="), InlineType::Highlight); }
    void toggleCode() { toggleWrap(QStringLiteral("`"), InlineType::Code); }
    void toggleInlineMath() { toggleWrap(QStringLiteral("$"), InlineType::Math); }
    void toggleUnderline();
    void insertLink();
    void insertImage(const QString &path = {});
    void clearFormat();

    // Paragraph menu
    void setHeading(int level);
    void promoteHeading(int delta);
    void toggleQuote();
    void toggleList(bool ordered, bool task = false);
    void insertTable(int rows = 2, int columns = 3);
    void insertCodeBlock(const QString &language = {});
    void insertMathBlock();
    void insertRule();
    void insertParagraph(bool after);
    void insertFootnote();
    void insertLinkReference();
    void insertTableOfContents();
    void insertFrontMatter();
    /// Turns a code or math block back into a paragraph of its contents.
    void blockToParagraph();
    void indentList(bool deeper);

    // Tables
    bool inTable() const;
    void tableInsertRow(bool below);
    void tableInsertColumn(bool right);
    void tableDeleteRow();
    void tableDeleteColumn();
    void tableAlign(Align align);
    void formatTable(int block);
    void tableMoveRow(int delta);
    void tableMoveColumn(int delta);
    void tableDelete();
    void copyTable();

    // Find and replace
    struct FindOptions {
        bool caseSensitive = false;
        bool wholeWord = false;
        bool regex = false;
    };
    void setSearch(const QString &text, const FindOptions &options);
    /// Selects the next match from the caret; returns false when there is none.
    bool findNext(bool backwards = false);
    bool replaceCurrent(const QString &replacement);
    int replaceAll(const QString &replacement);
    int matchCount() const;

    int wordCount() const;
    int selectionWordCount() const;
    int characterCount() const;
    int lineCount() const;

    /// Saves a pasted picture and returns the path to write in the Markdown;
    /// empty when it could not be saved.
    using ImageSaver = std::function<QString(const QImage &)>;
    void setImageSaver(ImageSaver saver) { m_imageSaver = std::move(saver); }
    /// Turns a picture file into the path to write in the Markdown (copying it when asked to).
    using ImagePathMapper = std::function<QString(const QString &)>;
    void setImagePathMapper(ImagePathMapper mapper) { m_imagePathMapper = std::move(mapper); }

    /// Fills the context menu; the point is where it was asked for, in viewport coordinates.
    void setContextMenuBuilder(std::function<void(QMenu *, const QPoint &)> builder) { m_contextMenu = std::move(builder); }
    /// Turns Markdown into HTML for the clipboard, so that a copy pastes formatted elsewhere.
    void setHtmlForCopy(std::function<QString(const QString &)> converter) { m_htmlForCopy = std::move(converter); }

    /// What stands at the caret, for menus.
    BlockKind caretKind() const { return m_blocks[m_caret.block].kind; }
    /// 1..6 in a heading, 0 in a paragraph, -1 elsewhere.
    int caretHeadingLevel() const;
    /// The link at a point of the viewport, empty when none.
    QString linkAt(const QPoint &viewportPoint);

    /// Lays out the document at a width for printing: every block, not edited.
    std::vector<std::unique_ptr<RenderedBlock>> renderForPrint(qreal width, const EditorTheme &theme) const;
    /// Paints one rendered block with its decorations, at the painter's origin.
    static void paintRendered(QPainter &painter, const RenderedBlock &block, const EditorTheme &theme,
                              const QRectF &clip, qreal zoom);
    /// The gap between two blocks, for print layout.
    static qreal gapBetween(const Block &a, const RenderedBlock &ra, const Block &b, const RenderedBlock &rb, qreal zoom);

    ImageStore *imageStore() const { return m_images; }

signals:
    void contentChanged();
    void modificationChanged(bool modified);
    void caretMoved();
    void linkActivated(const QString &href);
    void zoomRequested(int steps);
    void openFileRequested(const QString &path);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void timerEvent(QTimerEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool event(QEvent *event) override;
    bool focusNextPrevChild(bool next) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    /// What a rendering depends on besides the block itself.
    struct SlotKey {
        qreal width = 0;
        qreal zoom = 0;
        int theme = 0;
        int listNumber = 0;
        int listDepth = 0;
        /// For the block with the caret: what it reveals, and text being composed.
        QString reveal;
        bool operator==(const SlotKey &) const = default;
    };
    struct Slot {
        std::shared_ptr<RenderedBlock> render;
        SlotKey key;
        int imageGeneration = 0;
    };
    struct Snapshot {
        QList<Block> blocks;
        Pos caret;
        Pos anchor;
        quint64 revision = 0;
    };
    enum class EditKind { None, Typing, Deleting, Other };

    // Layout
    void relayout();
    void ensureRendered(int index);
    SlotKey slotKey(int index) const;
    RenderedBlock *render(int index);
    qreal columnLeft() const;
    qreal columnWidth() const;
    qreal gapBefore(int index) const;
    int blockAtY(qreal contentY) const;
    Pos positionAt(const QPoint &viewportPoint);
    QRectF caretRectInViewport() const;
    void ensureCaretVisible();
    void updateCaret();
    void computeListNumbers();
    void collectReferences();
    int parentIndent(int index) const;
    void applyPreedit(RenderedBlock *render);

    // Positions
    Pos clamp(Pos pos, bool forward = true) const;
    Pos firstPos(int block) const;
    Pos lastPos(int block) const;
    Pos nextChar(Pos pos) const;
    Pos previousChar(Pos pos) const;
    Pos nextWord(Pos pos) const;
    Pos previousWord(Pos pos) const;
    Pos lineStart(Pos pos);
    Pos lineEnd(Pos pos);
    bool verticalTarget(Pos from, int direction, Pos &to);
    void moveCaret(Pos to, bool extend);
    Pos docStart() const { return firstPos(0); }
    Pos docEnd() const { return lastPos(int(m_blocks.size()) - 1); }
    std::pair<Pos, Pos> selectionRange() const;

    // Editing primitives
    void beginEdit(EditKind kind);
    void endEdit();
    Snapshot snapshot() const;
    void restore(const Snapshot &state);
    void touch(Block &block);
    /// Parses a block again after its text changed; may split it.
    void reparse(int index);
    void replaceRange(Pos from, Pos to, const QString &text);
    void removeBlock(int index);
    void insertBlockAt(int index, const Block &block);
    void insertText(const QString &text);
    void backspace(bool word);
    void deleteForward(bool word);
    void enter();
    void softBreak();
    void tab(bool backwards);
    void toggleWrap(const QString &marker, InlineType type);
    void toggleTask(int block);
    void setListIndent(int block, int indent);
    void pasteMime(const QMimeData *mime, bool plain);
    QString imageMarkdownFor(const QString &path) const;
    void placeNewBlock(const Block &block, int caretOffset);

    // Painting
    void paintBlock(QPainter &painter, int index, const QPointF &origin, const QRect &clip);

    QList<Block> m_blocks;
    QHash<quint64, Slot> m_renders;
    QList<qreal> m_ys;
    QList<int> m_listNumbers;
    QList<int> m_listDepths;
    qreal m_contentHeight = 0;

    Pos m_caret;
    Pos m_anchor;
    qreal m_preferredX = -1;
    bool m_caretVisible = true;
    QBasicTimer m_blink;
    QString m_preedit;
    int m_preeditCursor = 0;

    QList<Snapshot> m_undo;
    QList<Snapshot> m_redo;
    EditKind m_lastEdit = EditKind::None;
    Pos m_lastEditCaret;
    QElapsedTimer m_editTimer;
    quint64 m_revision = 1;
    quint64 m_savedRevision = 1;
    bool m_editing = false;
    bool m_wasModified = false;

    EditorTheme m_theme;
    int m_themeGeneration = 1;
    int m_imageGeneration = 1;
    qreal m_zoom = 1.0;
    QString m_baseDir;
    LinkReferences m_references;
    ImageStore *m_images = nullptr;
    bool m_focusMode = false;
    bool m_typewriter = false;

    // Mouse
    bool m_dragging = false;
    int m_clickCount = 0;
    QElapsedTimer m_lastClick;
    QPoint m_lastClickPos;
    Pos m_selectionOrigin;
    QPoint m_lastMousePos;
    QBasicTimer m_autoScroll;

    // Find
    QString m_search;
    FindOptions m_searchOptions;
    QList<Segment> matchesIn(int block) const;

    ImageSaver m_imageSaver;
    ImagePathMapper m_imagePathMapper;
    std::function<void(QMenu *, const QPoint &)> m_contextMenu;
    std::function<QString(const QString &)> m_htmlForCopy;
    QList<TocEntry> m_toc;
};

} // namespace ot
