#include "MainWindow.h"

#include "Exporter.h"
#include "FindBar.h"
#include "Icons.h"
#include "ImageStore.h"
#include "MarkdownEditor.h"
#include "SourceEditor.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QGridLayout>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPrintDialog>
#include <QPrinter>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QStyleHints>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QUrlQuery>
#include <QWidgetAction>
#include <QVBoxLayout>

namespace ot {

namespace {

constexpr int MaxRecent = 12;

QString readText(const QString &path, bool *ok)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *ok = false;
        return {};
    }
    const QByteArray data = file.readAll();
    *ok = true;
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder(data);
    if (decoder.hasError()) {
        // Not UTF-8: most likely a legacy single-byte encoding.
        text = QString::fromLatin1(data);
    }
    return text;
}

QString colorName(const QColor &color)
{
    return color.name(QColor::HexArgb);
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    QSettings settings;
    m_theme = EditorTheme::byName(settings.value(QStringLiteral("theme"), QStringLiteral("Github")).toString());
    m_zoom = settings.value(QStringLiteral("zoom"), 1.0).toDouble();

    m_editor = new MarkdownEditor;
    m_editor->setTheme(m_theme);
    m_editor->setZoom(m_zoom);
    m_source = new SourceEditor;
    m_source->setTheme(m_theme);
    m_source->setZoom(m_zoom);
    m_stack = new QStackedWidget;
    m_stack->addWidget(m_editor);
    m_stack->addWidget(m_source);

    m_find = new FindBar(m_editor);
    m_find->hide();

    auto *content = new QWidget;
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    contentLayout->addWidget(m_find);
    contentLayout->addWidget(m_stack, 1);
    buildFooter();
    contentLayout->addWidget(m_footer);

    buildSidebar();
    m_splitter = new QSplitter;
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(1);
    m_splitter->addWidget(m_sidebar);
    m_splitter->addWidget(content);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({260, 1000});
    setCentralWidget(m_splitter);

    buildMenus();

    m_editor->setImageSaver([this](const QImage &image) { return savePastedImage(image); });
    m_editor->setImagePathMapper([this](const QString &path) { return imagePathFor(path); });
    m_editor->setContextMenuBuilder([this](QMenu *menu, const QPoint &point) { fillContextMenu(menu, point); });
    m_editor->setHtmlForCopy([this](const QString &markdown) { return markdownToHtml(markdown, m_theme); });
    connect(m_editor, &MarkdownEditor::modificationChanged, this, &MainWindow::updateTitle);
    connect(m_editor, &MarkdownEditor::contentChanged, this, [this] {
        m_outlineTimer.start();
        m_countTimer.start();
        if (m_autosaveAction->isChecked() && !m_path.isEmpty())
            m_autosaveTimer.start();
    });
    connect(m_editor, &MarkdownEditor::linkActivated, this, &MainWindow::openLink);
    connect(m_editor, &MarkdownEditor::zoomRequested, this, &MainWindow::zoom);
    connect(m_editor, &MarkdownEditor::openFileRequested, this, [this](const QString &path) {
        if (!m_editor->isModified() && m_path.isEmpty() && m_editor->markdown().trimmed().isEmpty())
            openFile(path);
        else {
            auto *window = new MainWindow;
            window->openFile(path);
            window->show();
        }
    });
    connect(m_editor, &MarkdownEditor::caretMoved, this, [this] {
        m_countTimer.start();
        // The outline follows the caret.
        const int block = m_editor->caretBlock();
        QTreeWidgetItem *best = nullptr;
        for (QTreeWidgetItemIterator it(m_outline); *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toInt() <= block)
                best = *it;
        }
        if (best && m_outline->currentItem() != best) {
            const QSignalBlocker blocker(m_outline);
            m_outline->setCurrentItem(best);
        }
    });
    connect(m_source, &SourceEditor::modificationChanged, this, &MainWindow::updateTitle);
    connect(m_source, &SourceEditor::textChanged, this, [this] {
        m_countTimer.start();
        m_outlineTimer.start();
        if (m_autosaveAction->isChecked() && !m_path.isEmpty())
            m_autosaveTimer.start();
    });

    m_outlineTimer.setSingleShot(true);
    m_outlineTimer.setInterval(300);
    connect(&m_outlineTimer, &QTimer::timeout, this, &MainWindow::updateOutline);
    m_countTimer.setSingleShot(true);
    m_countTimer.setInterval(400);
    connect(&m_countTimer, &QTimer::timeout, this, &MainWindow::updateCounts);
    m_autosaveTimer.setSingleShot(true);
    m_autosaveTimer.setInterval(1500);
    connect(&m_autosaveTimer, &QTimer::timeout, this, [this] {
        if (!m_path.isEmpty())
            save();
    });

    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
        if (path != m_path || m_writing)
            return;
        // Editors that save by replacing the file drop it from the watch list.
        QTimer::singleShot(200, this, [this] {
            if (!m_path.isEmpty() && QFileInfo::exists(m_path) && !m_watcher->files().contains(m_path))
                m_watcher->addPath(m_path);
            reloadFromDisk();
        });
    });

    applyChrome();
    const bool sidebar = settings.value(QStringLiteral("sidebar"), true).toBool();
    setSidebarVisible(sidebar);
    setOutlineShown(settings.value(QStringLiteral("sidebarOutline"), true).toBool());
    const QString folder = settings.value(QStringLiteral("folder")).toString();
    if (!folder.isEmpty() && QFileInfo(folder).isDir())
        openFolderPath(folder);
    if (!restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray())) {
        const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
        resize(std::min(1280, screen.width() * 4 / 5), std::min(860, screen.height() * 4 / 5));
    }
    updateTitle();
    updateOutline();
    updateCounts();
    m_editor->setFocus();
}

MainWindow::~MainWindow() = default;

QAction *MainWindow::action(QMenu *menu, const QString &text, const QKeySequence &shortcut, std::function<void()> slot)
{
    QAction *a = menu->addAction(text);
    if (!shortcut.isEmpty())
        a->setShortcut(shortcut);
    connect(a, &QAction::triggered, this, [slot = std::move(slot)] { slot(); });
    return a;
}

bool MainWindow::isSourceMode() const
{
    return m_stack->currentWidget() == m_source;
}

// ---- Menus -------------------------------------------------------------------------------

void MainWindow::buildMenus()
{
    QMenuBar *bar = menuBar();
    MarkdownEditor *e = m_editor;
    // Commands that change the document go to whichever view is showing; in
    // source mode the formatted view is brought up to date first.
    const auto rich = [this](std::function<void()> slot) {
        return [this, slot] {
            if (isSourceMode())
                setSourceMode(false);
            slot();
        };
    };

    QMenu *file = bar->addMenu(tr("&File"));
    action(file, tr("&New"), QKeySequence::New, [this] { newFile(); });
    action(file, tr("New &Window"), QKeySequence(tr("Ctrl+Shift+N")), [] {
        auto *window = new MainWindow;
        window->show();
    });
    file->addSeparator();
    action(file, tr("&Open..."), QKeySequence::Open, [this] { open(); });
    action(file, tr("Open &Folder..."), {}, [this] { openFolder(); });
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    rebuildRecent();
    file->addSeparator();
    action(file, tr("&Save"), QKeySequence::Save, [this] { save(); });
    action(file, tr("Save &As..."), QKeySequence(tr("Ctrl+Shift+S")), [this] { saveAs(); });
    m_autosaveAction = file->addAction(tr("A&uto Save"));
    m_autosaveAction->setCheckable(true);
    m_autosaveAction->setChecked(QSettings().value(QStringLiteral("autosave"), false).toBool());
    connect(m_autosaveAction, &QAction::toggled, this,
            [](bool on) { QSettings().setValue(QStringLiteral("autosave"), on); });
    action(file, tr("Re&load from Disk"), {}, [this] { reloadFromDisk(); });
    action(file, tr("Reveal in File &Manager"), {}, [this] {
        if (!m_path.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_path).absolutePath()));
    });
    file->addSeparator();
    QMenu *exportMenu = file->addMenu(tr("&Export"));
    action(exportMenu, tr("&PDF..."), {}, [this] { exportPdf(); });
    action(exportMenu, tr("&HTML..."), {}, [this] { exportHtml(); });
    action(file, tr("&Print..."), QKeySequence(tr("Ctrl+Alt+P")), [this] { print(); });
    file->addSeparator();
    action(file, tr("&Close"), QKeySequence(tr("Ctrl+W")), [this] { close(); });
    action(file, tr("&Quit"), QKeySequence::Quit, [] { QApplication::closeAllWindows(); });

    QMenu *edit = bar->addMenu(tr("&Edit"));
    const auto routed = [this](std::function<void()> richSlot, std::function<void()> sourceSlot) {
        return [this, richSlot, sourceSlot] {
            if (isSourceMode())
                sourceSlot();
            else
                richSlot();
        };
    };
    // Table commands do nothing outside a table, and never leave source mode.
    const auto inTable = [this](std::function<void()> slot) {
        return [this, slot] {
            if (!isSourceMode() && m_editor->inTable())
                slot();
        };
    };
    const auto keep = [this](const QString &name, QAction *a) {
        m_actions.insert(name, a);
        return a;
    };

    keep(QStringLiteral("undo"), action(edit, tr("&Undo"), QKeySequence::Undo,
                                        routed([e] { e->undo(); }, [this] { m_source->undo(); })));
    keep(QStringLiteral("redo"), action(edit, tr("&Redo"), QKeySequence(tr("Ctrl+Y")),
                                        routed([e] { e->redo(); }, [this] { m_source->redo(); })))
        ->setShortcuts({QKeySequence(tr("Ctrl+Y")), QKeySequence(tr("Ctrl+Shift+Z"))});
    edit->addSeparator();
    keep(QStringLiteral("cut"), action(edit, tr("Cu&t"), QKeySequence::Cut, routed([e] { e->cut(); }, [this] { m_source->cut(); })));
    keep(QStringLiteral("copy"), action(edit, tr("&Copy"), QKeySequence::Copy, routed([e] { e->copy(); }, [this] { m_source->copy(); })));
    keep(QStringLiteral("paste"), action(edit, tr("&Paste"), QKeySequence::Paste, routed([e] { e->paste(); }, [this] { m_source->paste(); })));
    keep(QStringLiteral("copyMarkdown"), action(edit, tr("Copy as &Markdown"), QKeySequence(tr("Ctrl+Shift+C")),
                                                routed([e] { e->copyAsMarkdown(); }, [this] { m_source->copy(); })));
    keep(QStringLiteral("pastePlain"), action(edit, tr("Paste as Plain &Text"), QKeySequence(tr("Ctrl+Shift+V")),
                                              routed([e] { e->pastePlain(); }, [this] { m_source->paste(); })));
    edit->addSeparator();
    action(edit, tr("Select &All"), QKeySequence::SelectAll,
           routed([e] { e->selectAll(); }, [this] { m_source->selectAll(); }));
    edit->addSeparator();
    action(edit, tr("&Find..."), QKeySequence::Find, [this] { showFind(false); });
    action(edit, tr("Find &Next"), QKeySequence(Qt::Key_F3), [this] { m_find->next(); });
    action(edit, tr("Find Pre&vious"), QKeySequence(tr("Shift+F3")), [this] { m_find->previous(); });
    action(edit, tr("&Replace..."), QKeySequence(tr("Ctrl+H")), [this] { showFind(true); });

    // Paragraph, in Typora's order.
    QMenu *paragraph = bar->addMenu(tr("&Paragraph"));
    for (int level = 1; level <= 6; ++level) {
        QAction *heading = action(paragraph, tr("Heading %1").arg(level), QKeySequence(tr("Ctrl+%1").arg(level)),
                                  rich([e, level] { e->setHeading(level); }));
        heading->setCheckable(true);
        keep(QStringLiteral("heading%1").arg(level), heading);
    }
    paragraph->addSeparator();
    QAction *plain = keep(QStringLiteral("heading0"), action(paragraph, tr("&Paragraph"), QKeySequence(tr("Ctrl+0")),
                                                             rich([e] { e->setHeading(0); })));
    plain->setCheckable(true);
    paragraph->addSeparator();
    action(paragraph, tr("Increase Heading Level"), QKeySequence(tr("Ctrl+=")), rich([e] { e->promoteHeading(1); }));
    action(paragraph, tr("Decrease Heading Level"), QKeySequence(tr("Ctrl+-")), rich([e] { e->promoteHeading(-1); }));
    paragraph->addSeparator();

    QMenu *table = paragraph->addMenu(tr("&Table"));
    m_tableMenu = table;
    keep(QStringLiteral("table"), action(table, tr("&Insert Table..."), QKeySequence(tr("Ctrl+T")), rich([this, e] {
             bool ok = false;
             const QString size = QInputDialog::getText(this, tr("Insert Table"), tr("Columns × rows:"),
                                                        QLineEdit::Normal, QStringLiteral("3 x 2"), &ok);
             if (!ok)
                 return;
             const QStringList parts = size.split(QRegularExpression(QStringLiteral("[^0-9]+")), Qt::SkipEmptyParts);
             const int columns = parts.value(0).toInt() > 0 ? parts.value(0).toInt() : 3;
             const int rows = parts.value(1).toInt() > 0 ? parts.value(1).toInt() : 2;
             e->insertTable(std::min(rows, 200), std::min(columns, 30));
         })));
    table->addSeparator();
    keep(QStringLiteral("rowAbove"), action(table, tr("Add Row Above"), {}, inTable([e] { e->tableInsertRow(false); })));
    // Ctrl+Enter is the editor's own key: shown here, handled there.
    keep(QStringLiteral("rowBelow"), action(table, tr("Add Row Below\tCtrl+Enter"), {}, inTable([e] { e->tableInsertRow(true); })));
    keep(QStringLiteral("columnBefore"), action(table, tr("Add Column Before"), {}, inTable([e] { e->tableInsertColumn(false); })));
    keep(QStringLiteral("columnAfter"), action(table, tr("Add Column After"), {}, inTable([e] { e->tableInsertColumn(true); })));
    table->addSeparator();
    keep(QStringLiteral("rowUp"), action(table, tr("Move Row Up"), QKeySequence(tr("Alt+Up")), inTable([e] { e->tableMoveRow(-1); })));
    keep(QStringLiteral("rowDown"), action(table, tr("Move Row Down"), QKeySequence(tr("Alt+Down")), inTable([e] { e->tableMoveRow(1); })));
    keep(QStringLiteral("columnLeft"), action(table, tr("Move Column Left"), QKeySequence(tr("Alt+Left")), inTable([e] { e->tableMoveColumn(-1); })));
    keep(QStringLiteral("columnRight"), action(table, tr("Move Column Right"), QKeySequence(tr("Alt+Right")), inTable([e] { e->tableMoveColumn(1); })));
    table->addSeparator();
    keep(QStringLiteral("deleteRow"), action(table, tr("Delete Row"), QKeySequence(tr("Ctrl+Shift+Backspace")),
                                             inTable([e] { e->tableDeleteRow(); })));
    keep(QStringLiteral("deleteColumn"), action(table, tr("Delete Column"), {}, inTable([e] { e->tableDeleteColumn(); })));
    table->addSeparator();
    QMenu *align = table->addMenu(tr("Align Column"));
    keep(QStringLiteral("alignLeft"), action(align, tr("Left"), {}, inTable([e] { e->tableAlign(Align::Left); })));
    keep(QStringLiteral("alignCenter"), action(align, tr("Center"), {}, inTable([e] { e->tableAlign(Align::Center); })));
    keep(QStringLiteral("alignRight"), action(align, tr("Right"), {}, inTable([e] { e->tableAlign(Align::Right); })));
    m_actions.insert(QStringLiteral("alignMenu"), align->menuAction());
    keep(QStringLiteral("copyTable"), action(table, tr("Copy Table"), {}, inTable([e] { e->copyTable(); })));
    keep(QStringLiteral("prettifyTable"), action(table, tr("Prettify Source Code"), {},
                                                 inTable([e] { e->formatTable(e->caretBlock()); })));
    table->addSeparator();
    keep(QStringLiteral("deleteTable"), action(table, tr("Delete Table"), {}, inTable([e] { e->tableDelete(); })));

    keep(QStringLiteral("codeFences"), action(paragraph, tr("Code &Fences"), QKeySequence(tr("Ctrl+Shift+K")),
                                              rich([e] { e->insertCodeBlock(); })));
    keep(QStringLiteral("mathBlock"), action(paragraph, tr("&Math Block"), QKeySequence(tr("Ctrl+Shift+M")),
                                             rich([e] { e->insertMathBlock(); })));
    paragraph->addSeparator();
    keep(QStringLiteral("quote"), action(paragraph, tr("&Quote"), QKeySequence(tr("Ctrl+Shift+Q")), rich([e] { e->toggleQuote(); })));
    paragraph->addSeparator();
    keep(QStringLiteral("orderedList"), action(paragraph, tr("&Ordered List"), QKeySequence(tr("Ctrl+Shift+[")),
                                               rich([e] { e->toggleList(true); })))
        ->setShortcuts({QKeySequence(tr("Ctrl+Shift+[")), QKeySequence(tr("Ctrl+{"))});
    keep(QStringLiteral("unorderedList"), action(paragraph, tr("&Unordered List"), QKeySequence(tr("Ctrl+Shift+]")),
                                                 rich([e] { e->toggleList(false); })))
        ->setShortcuts({QKeySequence(tr("Ctrl+Shift+]")), QKeySequence(tr("Ctrl+}"))});
    keep(QStringLiteral("taskList"), action(paragraph, tr("Task &List"), QKeySequence(tr("Ctrl+Shift+X")),
                                            rich([e] { e->toggleList(false, true); })));
    QMenu *indentation = paragraph->addMenu(tr("List &Indentation"));
    action(indentation, tr("&Indent"), QKeySequence(tr("Ctrl+]")), rich([e] { e->indentList(true); }));
    action(indentation, tr("&Outdent"), QKeySequence(tr("Ctrl+[")), rich([e] { e->indentList(false); }));
    paragraph->addSeparator();
    keep(QStringLiteral("paragraphBefore"), action(paragraph, tr("Insert Paragraph &Before"), {},
                                                   rich([e] { e->insertParagraph(false); })));
    keep(QStringLiteral("paragraphAfter"), action(paragraph, tr("Insert Paragraph &After"), {},
                                                  rich([e] { e->insertParagraph(true); })));
    paragraph->addSeparator();
    keep(QStringLiteral("linkReference"), action(paragraph, tr("Link &Reference"), {}, rich([e] { e->insertLinkReference(); })));
    keep(QStringLiteral("footnote"), action(paragraph, tr("Foot&notes"), {}, rich([e] { e->insertFootnote(); })));
    paragraph->addSeparator();
    keep(QStringLiteral("rule"), action(paragraph, tr("&Horizontal Line"), {}, rich([e] { e->insertRule(); })));
    keep(QStringLiteral("toc"), action(paragraph, tr("Table of &Contents"), {}, rich([e] { e->insertTableOfContents(); })));
    keep(QStringLiteral("frontMatter"), action(paragraph, tr("&YAML Front Matter"), {}, rich([e] { e->insertFrontMatter(); })));
    connect(paragraph, &QMenu::aboutToShow, this, &MainWindow::updateParagraphChecks);

    QMenu *format = bar->addMenu(tr("F&ormat"));
    keep(QStringLiteral("strong"), action(format, tr("&Strong"), QKeySequence::Bold, rich([e] { e->toggleBold(); })));
    keep(QStringLiteral("emphasis"), action(format, tr("&Emphasis"), QKeySequence::Italic, rich([e] { e->toggleItalic(); })));
    keep(QStringLiteral("underline"), action(format, tr("&Underline"), QKeySequence::Underline, rich([e] { e->toggleUnderline(); })));
    keep(QStringLiteral("code"), action(format, tr("&Code"), QKeySequence(tr("Ctrl+Shift+`")), rich([e] { e->toggleCode(); })))
        ->setShortcuts({QKeySequence(tr("Ctrl+Shift+`")), QKeySequence(tr("Ctrl+~"))});
    keep(QStringLiteral("strike"), action(format, tr("S&trike"), QKeySequence(tr("Alt+Shift+5")), rich([e] { e->toggleStrike(); })))
        ->setShortcuts({QKeySequence(tr("Alt+Shift+5")), QKeySequence(tr("Alt+%"))});
    keep(QStringLiteral("highlight"), action(format, tr("&Highlight"), QKeySequence(tr("Ctrl+Shift+H")), rich([e] { e->toggleHighlight(); })));
    keep(QStringLiteral("inlineMath"), action(format, tr("Inline &Math"), QKeySequence(tr("Ctrl+Shift+E")), rich([e] { e->toggleInlineMath(); })));
    format->addSeparator();
    keep(QStringLiteral("link"), action(format, tr("&Hyperlink"), QKeySequence(tr("Ctrl+K")), rich([e] { e->insertLink(); })));
    QMenu *image = format->addMenu(tr("&Image"));
    keep(QStringLiteral("image"), action(image, tr("Insert Image..."), QKeySequence(tr("Ctrl+Shift+I")), rich([this, e] {
             const QString start = m_path.isEmpty() ? QDir::homePath() : QFileInfo(m_path).absolutePath();
             const QString path = QFileDialog::getOpenFileName(
                 this, tr("Insert Image"), start, tr("Images (*.png *.jpg *.jpeg *.gif *.svg *.webp *.bmp)"));
             if (!path.isEmpty())
                 e->insertImage(path);
         })));
    action(image, tr("Insert Image Syntax"), {}, rich([e] { e->insertImage(); }));
    format->addSeparator();
    action(format, tr("C&lear Format"), QKeySequence(tr("Ctrl+\\")), rich([e] { e->clearFormat(); }));
    keep(QStringLiteral("blockToParagraph"), new QAction(tr("&Paragraph"), this));
    m_actions.value(QStringLiteral("blockToParagraph"))->setShortcut(QKeySequence(tr("Ctrl+0")));
    m_actions.value(QStringLiteral("blockToParagraph"))->setShortcutContext(Qt::WidgetShortcut);
    connect(m_actions.value(QStringLiteral("blockToParagraph")), &QAction::triggered, e, &MarkdownEditor::blockToParagraph);

    QMenu *view = bar->addMenu(tr("&View"));
    m_sourceAction = action(view, tr("&Source Code Mode"), QKeySequence(tr("Ctrl+/")), [this] {
        setSourceMode(!isSourceMode());
    });
    m_sourceAction->setCheckable(true);
    view->addSeparator();
    m_sidebarAction = action(view, tr("Show Si&debar"), QKeySequence(tr("Ctrl+Shift+L")), [this] {
        setSidebarVisible(!m_sidebar->isVisible());
    });
    m_sidebarAction->setCheckable(true);
    m_outlineAction = action(view, tr("&Outline"), QKeySequence(tr("Ctrl+Shift+1")), [this] {
        setSidebarVisible(true);
        setOutlineShown(true);
    });
    m_outlineAction->setCheckable(true);
    m_filesAction = action(view, tr("&Files"), QKeySequence(tr("Ctrl+Shift+3")), [this] {
        setSidebarVisible(true);
        setOutlineShown(false);
    });
    m_filesAction->setCheckable(true);
    view->addSeparator();
    m_focusAction = action(view, tr("&Focus Mode"), QKeySequence(Qt::Key_F8), [this] {
        m_editor->setFocusMode(m_focusAction->isChecked());
    });
    m_focusAction->setCheckable(true);
    m_typewriterAction = action(view, tr("&Typewriter Mode"), QKeySequence(Qt::Key_F9), [this] {
        m_editor->setTypewriterMode(m_typewriterAction->isChecked());
    });
    m_typewriterAction->setCheckable(true);
    m_statusAction = action(view, tr("Show Status &Bar"), {}, [this] { m_footer->setVisible(m_statusAction->isChecked()); });
    m_statusAction->setCheckable(true);
    m_statusAction->setChecked(true);
    view->addSeparator();
    action(view, tr("&Actual Size"), QKeySequence(tr("Ctrl+Shift+0")), [this] { setZoom(1.0); });
    action(view, tr("Zoom &In"), QKeySequence(tr("Ctrl+Shift+=")), [this] { zoom(1); })
        ->setShortcuts({QKeySequence(tr("Ctrl+Shift+=")), QKeySequence(tr("Ctrl++"))});
    action(view, tr("Zoom &Out"), QKeySequence(tr("Ctrl+Shift+-")), [this] { zoom(-1); })
        ->setShortcuts({QKeySequence(tr("Ctrl+Shift+-")), QKeySequence(tr("Ctrl+_"))});
    view->addSeparator();
    action(view, tr("Toggle F&ullscreen"), QKeySequence(Qt::Key_F11), [this] {
        if (isFullScreen())
            showNormal();
        else
            showFullScreen();
    });

    QMenu *themes = bar->addMenu(tr("&Themes"));
    m_themeGroup = new QActionGroup(this);
    for (const QString &name : EditorTheme::names()) {
        QAction *a = themes->addAction(name);
        a->setCheckable(true);
        a->setChecked(name == m_theme.name);
        m_themeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, name] { setTheme(name); });
    }

    QMenu *help = bar->addMenu(tr("&Help"));
    action(help, tr("&Markdown Reference"), {}, [this] { showMarkdownReference(); });
    action(help, tr("&About Open Typora"), {}, [this] { showAbout(); });
}

void MainWindow::updateParagraphChecks()
{
    const int level = isSourceMode() ? -1 : m_editor->caretHeadingLevel();
    for (int i = 0; i <= 6; ++i)
        m_actions.value(QStringLiteral("heading%1").arg(i))->setChecked(i == level);
}

void MainWindow::fillContextMenu(QMenu *menu, const QPoint &point)
{
    // The same menu as Typora's: what is under the pointer first, then editing,
    // the formatting buttons, and what fits the block the caret is in.
    MarkdownEditor *e = m_editor;
    const auto shared = [this](const char *name) { return m_actions.value(QString::fromLatin1(name)); };
    const QString link = e->linkAt(point);

    if (e->hasSelection()) {
        const QString selected = e->selectedMarkdown();
        menu->addAction(tr("Search with Google"), this, [selected] {
            QUrl url(QStringLiteral("https://www.google.com/search"));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("q"), selected.simplified().left(500));
            url.setQuery(query);
            QDesktopServices::openUrl(url);
        });
        menu->addSeparator();
    }
    if (!link.isEmpty()) {
        menu->addAction(tr("Open Link") + QStringLiteral("\tCtrl+Click"), this, [this, link] { openLink(link); });
        menu->addSeparator();
    }

    // Cut and Copy are disabled without a selection here only: the menu bar's
    // actions keep their shortcuts.
    QAction *cut = menu->addAction(tr("Cut"), QKeySequence::Cut, e, &MarkdownEditor::cut);
    cut->setEnabled(e->hasSelection());
    QAction *copy = menu->addAction(tr("Copy"), QKeySequence::Copy, e, &MarkdownEditor::copy);
    copy->setEnabled(e->hasSelection());
    if (!link.isEmpty()) {
        menu->addAction(tr("Copy Link Address"), this, [link] { QApplication::clipboard()->setText(link); });
    }
    menu->addAction(shared("paste"));
    QMenu *as = menu->addMenu(tr("Copy / Paste As…"));
    QAction *copyMarkdown = as->addAction(shared("copyMarkdown")->text().remove(QLatin1Char('&')), e,
                                          &MarkdownEditor::copyAsMarkdown);
    copyMarkdown->setShortcut(shared("copyMarkdown")->shortcut());
    copyMarkdown->setEnabled(e->hasSelection());
    as->addAction(shared("pastePlain"));
    menu->addSeparator();

    // Two rows of buttons, as in Typora.
    auto *buttons = new QWidgetAction(menu);
    auto *grid = new QWidget;
    grid->setObjectName(QStringLiteral("menuButtons"));
    auto *layout = new QGridLayout(grid);
    layout->setContentsMargins(14, 4, 14, 4);
    layout->setSpacing(4);
    const QColor iconColor = m_theme.dark ? QColor(0xdd, 0xdd, 0xdd) : QColor(0x33, 0x33, 0x33);
    const struct {
        const char *icon;
        const char *action;
    } items[] = {{"bold", "strong"},  {"italic", "emphasis"},       {"code", "code"},         {"link", "link"},
                 {"quote", "quote"}, {"ordered", "orderedList"}, {"bullets", "unorderedList"}, {"task", "taskList"}};
    for (int i = 0; i < 8; ++i) {
        QAction *target = shared(items[i].action);
        auto *button = new QToolButton;
        button->setIcon(menuIcon(QString::fromLatin1(items[i].icon), iconColor));
        button->setIconSize(QSize(16, 16));
        button->setFixedSize(40, 24);
        button->setFocusPolicy(Qt::NoFocus);
        QString tip = target->text().remove(QLatin1Char('&'));
        if (!target->shortcut().isEmpty())
            tip += QStringLiteral(" (%1)").arg(target->shortcut().toString(QKeySequence::NativeText));
        button->setToolTip(tip);
        connect(button, &QToolButton::clicked, menu, [menu, target] {
            menu->close();
            target->trigger();
        });
        layout->addWidget(button, i / 4, i % 4);
    }
    buttons->setDefaultWidget(grid);
    menu->addAction(buttons);
    menu->addSeparator();

    const BlockKind kind = e->caretKind();
    if (e->inTable()) {
        menu->addMenu(m_tableMenu)->setText(tr("Table"));
    } else if (kind == BlockKind::CodeFence || kind == BlockKind::IndentedCode || kind == BlockKind::MathBlock) {
        QMenu *block = menu->addMenu(kind == BlockKind::MathBlock ? tr("Math Block") : tr("Code Fences"));
        block->addAction(shared("blockToParagraph"));
    } else {
        QMenu *paragraph = menu->addMenu(tr("Paragraph"));
        updateParagraphChecks();
        for (int level = 1; level <= 6; ++level)
            paragraph->addAction(m_actions.value(QStringLiteral("heading%1").arg(level)));
        paragraph->addSeparator();
        paragraph->addAction(shared("heading0"));
    }

    QMenu *insert = menu->addMenu(tr("Insert"));
    insert->addAction(shared("image"));
    insert->addSeparator();
    for (const char *name : {"footnote", "linkReference", "rule", "table", "codeFences", "mathBlock", "toc", "frontMatter"})
        insert->addAction(shared(name));
    insert->addSeparator();
    insert->addAction(shared("paragraphBefore"));
    insert->addAction(shared("paragraphAfter"));
}

// ---- Sidebar and footer -----------------------------------------------------------------------

void MainWindow::buildSidebar()
{
    m_sidebar = new QWidget;
    m_sidebar->setObjectName(QStringLiteral("sidebar"));
    m_sidebar->setMinimumWidth(160);
    auto *layout = new QVBoxLayout(m_sidebar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *tabs = new QWidget;
    tabs->setObjectName(QStringLiteral("sidebarTabs"));
    auto *tabLayout = new QHBoxLayout(tabs);
    tabLayout->setContentsMargins(12, 10, 12, 4);
    tabLayout->setSpacing(4);
    m_filesTab = new QToolButton;
    m_filesTab->setText(tr("Files"));
    m_outlineTab = new QToolButton;
    m_outlineTab->setText(tr("Outline"));
    for (QToolButton *tab : {m_filesTab, m_outlineTab}) {
        tab->setCheckable(true);
        tab->setAutoRaise(true);
        tab->setObjectName(QStringLiteral("sidebarTab"));
        tab->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        tabLayout->addWidget(tab);
    }
    connect(m_filesTab, &QToolButton::clicked, this, [this] { setOutlineShown(false); });
    connect(m_outlineTab, &QToolButton::clicked, this, [this] { setOutlineShown(true); });
    layout->addWidget(tabs);

    m_sidebarStack = new QStackedWidget;

    auto *filesPage = new QWidget;
    auto *filesLayout = new QVBoxLayout(filesPage);
    filesLayout->setContentsMargins(0, 0, 0, 0);
    filesLayout->setSpacing(0);
    m_folderLabel = new QLabel;
    m_folderLabel->setObjectName(QStringLiteral("folderLabel"));
    m_folderLabel->setContentsMargins(14, 8, 10, 6);
    filesLayout->addWidget(m_folderLabel);
    m_fileModel = new QFileSystemModel(this);
    m_fileModel->setNameFilters({QStringLiteral("*.md"), QStringLiteral("*.markdown"), QStringLiteral("*.mdown"),
                                 QStringLiteral("*.txt"), QStringLiteral("*.mkd")});
    m_fileModel->setNameFilterDisables(false);
    m_fileModel->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    m_files = new QTreeView;
    m_files->setModel(m_fileModel);
    for (int column = 1; column < 4; ++column)
        m_files->hideColumn(column);
    m_files->header()->hide();
    m_files->setFrameShape(QFrame::NoFrame);
    m_files->setAnimated(true);
    m_files->setIndentation(14);
    connect(m_files, &QTreeView::activated, this, [this](const QModelIndex &index) {
        const QString path = m_fileModel->filePath(index);
        if (QFileInfo(path).isFile() && path != m_path && maybeSave())
            openFile(path);
    });
    connect(m_files, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        const QString path = m_fileModel->filePath(index);
        if (QFileInfo(path).isFile() && path != m_path && maybeSave())
            openFile(path);
    });
    filesLayout->addWidget(m_files, 1);
    auto *openFolderButton = new QToolButton;
    openFolderButton->setText(tr("Open Folder…"));
    openFolderButton->setAutoRaise(true);
    openFolderButton->setObjectName(QStringLiteral("openFolder"));
    connect(openFolderButton, &QToolButton::clicked, this, &MainWindow::openFolder);
    filesLayout->addWidget(openFolderButton);
    m_sidebarStack->addWidget(filesPage);

    m_outline = new QTreeWidget;
    m_outline->setHeaderHidden(true);
    m_outline->setFrameShape(QFrame::NoFrame);
    m_outline->setIndentation(12);
    m_outline->setRootIsDecorated(false);
    m_outline->setObjectName(QStringLiteral("outline"));
    connect(m_outline, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        const int block = item->data(0, Qt::UserRole).toInt();
        if (isSourceMode())
            setSourceMode(false);
        m_editor->goToBlock(block);
    });
    m_sidebarStack->addWidget(m_outline);
    layout->addWidget(m_sidebarStack, 1);
}

void MainWindow::buildFooter()
{
    m_footer = new QWidget;
    m_footer->setObjectName(QStringLiteral("footer"));
    auto *layout = new QHBoxLayout(m_footer);
    layout->setContentsMargins(8, 2, 14, 2);
    layout->setSpacing(4);
    m_sidebarButton = new QToolButton;
    m_sidebarButton->setText(QStringLiteral("‹"));
    m_sidebarButton->setToolTip(tr("Toggle sidebar (Ctrl+Shift+L)"));
    m_sourceButton = new QToolButton;
    m_sourceButton->setText(QStringLiteral("</>"));
    m_sourceButton->setToolTip(tr("Toggle source code mode (Ctrl+/)"));
    m_sourceButton->setCheckable(true);
    m_countButton = new QToolButton;
    m_countButton->setToolTip(tr("Word count"));
    for (QToolButton *button : {m_sidebarButton, m_sourceButton, m_countButton}) {
        button->setAutoRaise(true);
        button->setObjectName(QStringLiteral("footerButton"));
        button->setFocusPolicy(Qt::NoFocus);
    }
    connect(m_sidebarButton, &QToolButton::clicked, this, [this] { setSidebarVisible(!m_sidebar->isVisible()); });
    connect(m_sourceButton, &QToolButton::clicked, this, [this] { setSourceMode(!isSourceMode()); });
    connect(m_countButton, &QToolButton::clicked, this, [this] {
        QMenu menu(this);
        const QString text = isSourceMode() ? m_source->markdown() : m_editor->markdown();
        const QList<Block> blocks = isSourceMode() ? blocksFromMarkdown(text) : m_editor->blocks();
        menu.addAction(tr("%n Words", nullptr, MarkdownEditor::wordCountOf(blocks)))->setEnabled(false);
        menu.addAction(tr("%n Characters", nullptr, MarkdownEditor::characterCountOf(blocks)))->setEnabled(false);
        menu.addAction(tr("%n Lines", nullptr, int(text.count(QLatin1Char('\n')))))->setEnabled(false);
        menu.exec(m_countButton->mapToGlobal(QPoint(0, -menu.sizeHint().height())));
    });
    layout->addWidget(m_sidebarButton);
    layout->addWidget(m_sourceButton);
    layout->addStretch(1);
    layout->addWidget(m_countButton);
}

void MainWindow::setSidebarVisible(bool visible)
{
    m_sidebar->setVisible(visible);
    m_sidebarAction->setChecked(visible);
    m_sidebarButton->setText(visible ? QStringLiteral("‹") : QStringLiteral("›"));
    QSettings().setValue(QStringLiteral("sidebar"), visible);
}

void MainWindow::setOutlineShown(bool outline)
{
    m_sidebarStack->setCurrentIndex(outline ? 1 : 0);
    m_outlineTab->setChecked(outline);
    m_filesTab->setChecked(!outline);
    m_outlineAction->setChecked(outline);
    m_filesAction->setChecked(!outline);
    QSettings().setValue(QStringLiteral("sidebarOutline"), outline);
}

void MainWindow::updateOutline()
{
    const QList<MarkdownEditor::Heading> headings =
        isSourceMode() ? MarkdownEditor::headingsOf(blocksFromMarkdown(m_source->markdown())) : m_editor->headings();
    m_outline->clear();
    QList<QPair<int, QTreeWidgetItem *>> stack;
    for (const MarkdownEditor::Heading &heading : headings) {
        while (!stack.isEmpty() && stack.last().first >= heading.level)
            stack.removeLast();
        auto *item = stack.isEmpty() ? new QTreeWidgetItem(m_outline) : new QTreeWidgetItem(stack.last().second);
        item->setText(0, heading.title.isEmpty() ? tr("(untitled)") : heading.title);
        item->setToolTip(0, heading.title);
        item->setData(0, Qt::UserRole, heading.block);
        if (heading.level <= 2) {
            QFont font = item->font(0);
            font.setWeight(heading.level == 1 ? QFont::DemiBold : QFont::Medium);
            item->setFont(0, font);
        }
        stack.append({heading.level, item});
    }
    m_outline->expandAll();
}

void MainWindow::updateCounts()
{
    const int words = isSourceMode() ? MarkdownEditor::wordCountOf(blocksFromMarkdown(m_source->markdown()))
                                     : m_editor->wordCount();
    // With a selection, as in Typora: "3 / 120 Words".
    const int selected = isSourceMode() || !m_editor->hasSelection() ? 0 : m_editor->selectionWordCount();
    if (selected > 0)
        m_countButton->setText(tr("%1 / %n Words", nullptr, words).arg(selected));
    else
        m_countButton->setText(tr("%n Words", nullptr, words));
}

// ---- Theme -------------------------------------------------------------------------------------

void MainWindow::setTheme(const QString &name)
{
    m_theme = EditorTheme::byName(name);
    m_editor->setTheme(m_theme);
    m_source->setTheme(m_theme);
    QSettings().setValue(QStringLiteral("theme"), m_theme.name);
    for (QAction *a : m_themeGroup->actions())
        a->setChecked(a->text() == m_theme.name);
    applyChrome();
}

void MainWindow::applyChrome()
{
    const EditorTheme &t = m_theme;
    const QColor sideText = t.sidebarText;
    const QColor faint = t.dark ? t.muted : QColor(0x77, 0x77, 0x77);
    QPalette palette = QApplication::palette();
    palette.setColor(QPalette::Window, t.background);
    palette.setColor(QPalette::Base, t.background);
    palette.setColor(QPalette::Text, t.text);
    palette.setColor(QPalette::WindowText, t.text);
    palette.setColor(QPalette::Button, t.sidebarBackground);
    palette.setColor(QPalette::ButtonText, t.text);
    palette.setColor(QPalette::Highlight, t.selection);
    palette.setColor(QPalette::HighlightedText, t.dark ? QColor(Qt::white) : t.text);
    palette.setColor(QPalette::PlaceholderText, faint);
    setPalette(palette);

    const QString sheet = QStringLiteral(
        "QMainWindow { background: %1; }"
        "QMenuBar { background: %2; color: %3; border: none; }"
        "QMenuBar::item { padding: 4px 9px; background: transparent; }"
        "QMenuBar::item:selected { background: %4; border-radius: 3px; }"
        "#sidebar, #sidebarTabs, #sidebar QTreeView, #sidebar QTreeWidget, #sidebar QStackedWidget > QWidget { background: %2; color: %3; }"
        "#sidebar QTreeView, #sidebar QTreeWidget { border: none; outline: 0; padding-left: 4px; }"
        "#sidebar QTreeView::item, #sidebar QTreeWidget::item { padding: 3px 2px; border: none; }"
        "#sidebar QTreeView::item:hover, #sidebar QTreeWidget::item:hover { background: %5; }"
        "#sidebar QTreeView::item:selected, #sidebar QTreeWidget::item:selected { background: %4; color: %3; }"
        "#sidebarTab { border: none; border-bottom: 2px solid transparent; padding: 5px 4px; color: %6; font-size: 13px; }"
        "#sidebarTab:checked { color: %3; border-bottom: 2px solid %3; font-weight: 600; }"
        "#folderLabel { color: %6; font-size: 12px; font-weight: 600; background: %2; }"
        "#openFolder { color: %6; padding: 6px; border: none; border-top: 1px solid %7; background: %2; }"
        "#openFolder:hover { color: %3; }"
        "#footer { background: %1; }"
        "#footerButton { color: %6; border: none; padding: 2px 6px; font-size: 12px; }"
        "#footerButton:hover { color: %3; background: %5; border-radius: 3px; }"
        "#footerButton:checked { color: %3; }"
        "#menuButtons QToolButton { border: 1px solid %7; border-radius: 3px; background: %1; }"
        "#menuButtons QToolButton:hover { background: %5; }"
        "QSplitter::handle { background: %7; }"
        "QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }"
        "QScrollBar::handle:vertical { background: %8; border-radius: 4px; min-height: 30px; margin: 2px; }"
        "QScrollBar::handle:vertical:hover { background: %9; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }")
                              .arg(t.background.name(), t.sidebarBackground.name(), sideText.name(),
                                   t.sidebarActive.name(), t.sidebarHover.name(), faint.name(), t.border.name(),
                                   colorName(t.dark ? QColor(255, 255, 255, 50) : QColor(0, 0, 0, 40)),
                                   colorName(t.dark ? QColor(255, 255, 255, 90) : QColor(0, 0, 0, 80)));
    // Menus as in Typora: flat, a grey band under the pointer, and a tick only on what is chosen.
    const QString tick = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                             .filePath(QStringLiteral("menu-tick-%1.svg").arg(t.text.name().mid(1)));
    if (!QFileInfo::exists(tick)) {
        QDir().mkpath(QFileInfo(tick).absolutePath());
        QFile file(tick);
        if (file.open(QIODevice::WriteOnly))
            file.write(QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><path d='M3.5 8.5l3 3 6-7' "
                                      "fill='none' stroke='%1' stroke-width='1.8' stroke-linecap='round' "
                                      "stroke-linejoin='round'/></svg>")
                           .arg(t.text.name())
                           .toUtf8());
    }
    const QString menus = QStringLiteral(
        "QMenu { background: %1; color: %2; border: 1px solid %3; padding: 4px 0; }"
        "QMenu::item { padding: 4px 28px 4px 28px; background: transparent; }"
        "QMenu::item:selected { background: %4; color: %2; }"
        "QMenu::item:disabled { color: %5; }"
        "QMenu::separator { height: 1px; background: %3; margin: 4px 0; }"
        "QMenu::indicator { width: 14px; height: 14px; left: 8px; }"
        "QMenu::indicator:non-exclusive:unchecked, QMenu::indicator:exclusive:unchecked { image: none; }"
        "QMenu::indicator:non-exclusive:checked, QMenu::indicator:exclusive:checked { image: url(%6); }"
        "QMenu::right-arrow { right: 8px; }")
                              .arg(t.dark ? t.sidebarBackground.name() : QStringLiteral("#ffffff"), t.text.name(),
                                   t.border.name(), t.sidebarActive.name(),
                                   t.dark ? QStringLiteral("#6b6f73") : QStringLiteral("#b5b5b5"), tick);
    setStyleSheet(sheet + menus);
    m_find->setTheme(t);
}

// ---- Files ---------------------------------------------------------------------------------------

void MainWindow::openPath(const QString &path)
{
    const QFileInfo info(path);
    if (info.isDir()) {
        openFolderPath(info.absoluteFilePath());
        setSidebarVisible(true);
        setOutlineShown(false);
    } else {
        openFile(info.absoluteFilePath());
    }
}

bool MainWindow::openFile(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        // A new file: it is created on the first save.
        m_path = info.absoluteFilePath();
        m_editor->setBaseDir(info.absolutePath());
        m_editor->setMarkdown(QString());
        updateTitle();
        return true;
    }
    bool ok = false;
    const QString text = readText(path, &ok);
    if (!ok) {
        QMessageBox::warning(this, tr("Open"), tr("Could not read %1.").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    if (!m_path.isEmpty())
        m_watcher->removePath(m_path);
    m_path = info.absoluteFilePath();
    m_watcher->addPath(m_path);
    m_editor->setBaseDir(info.absolutePath());
    m_editor->setMarkdown(text);
    if (isSourceMode())
        m_source->setMarkdown(m_editor->markdown());
    addRecent(m_path);
    if (m_folder.isEmpty())
        openFolderPath(info.absolutePath());
    else if (m_fileModel)
        m_files->setCurrentIndex(m_fileModel->index(m_path));
    updateTitle();
    updateOutline();
    updateCounts();
    return true;
}

void MainWindow::openFolderPath(const QString &path)
{
    m_folder = path;
    m_fileModel->setRootPath(path);
    m_files->setRootIndex(m_fileModel->index(path));
    m_folderLabel->setText(QFileInfo(path).fileName().toUpper());
    m_folderLabel->setToolTip(QDir::toNativeSeparators(path));
    if (!m_path.isEmpty())
        m_files->setCurrentIndex(m_fileModel->index(m_path));
    QSettings().setValue(QStringLiteral("folder"), path);
}

void MainWindow::newFile()
{
    if (m_path.isEmpty() && !m_editor->isModified() && !m_source->document()->isModified()) {
        m_editor->setMarkdown(QString());
        m_source->setMarkdown(QString());
        return;
    }
    auto *window = new MainWindow;
    window->show();
}

void MainWindow::open()
{
    const QString start = m_path.isEmpty() ? (m_folder.isEmpty() ? QDir::homePath() : m_folder)
                                           : QFileInfo(m_path).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("Open"), start,
                                                      tr("Markdown (*.md *.markdown *.mdown *.mkd *.txt);;All files (*)"));
    if (path.isEmpty())
        return;
    if (m_path.isEmpty() && !m_editor->isModified()) {
        openFile(path);
    } else {
        auto *window = new MainWindow;
        window->openFile(path);
        window->show();
    }
}

void MainWindow::openFolder()
{
    const QString path = QFileDialog::getExistingDirectory(this, tr("Open Folder"),
                                                           m_folder.isEmpty() ? QDir::homePath() : m_folder);
    if (path.isEmpty())
        return;
    openFolderPath(path);
    setSidebarVisible(true);
    setOutlineShown(false);
}

bool MainWindow::writeFile(const QString &path)
{
    const QString text = isSourceMode() ? m_source->markdown() : m_editor->markdown();
    m_writing = true;
    QSaveFile file(path);
    bool ok = file.open(QIODevice::WriteOnly);
    if (ok) {
        file.write(text.toUtf8());
        ok = file.commit();
    }
    // Let the watcher's notice of our own write pass before listening again.
    QTimer::singleShot(400, this, [this] {
        m_writing = false;
        if (!m_path.isEmpty() && QFileInfo::exists(m_path) && !m_watcher->files().contains(m_path))
            m_watcher->addPath(m_path);
    });
    if (!ok) {
        QMessageBox::warning(this, tr("Save"), tr("Could not write %1.").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    m_editor->markSaved();
    m_source->document()->setModified(false);
    updateTitle();
    return true;
}

bool MainWindow::save()
{
    if (m_path.isEmpty() || !QFileInfo(QFileInfo(m_path).absolutePath()).isDir())
        return saveAs();
    return writeFile(m_path);
}

bool MainWindow::saveAs()
{
    QString suggestion = m_path;
    if (suggestion.isEmpty()) {
        const QList<MarkdownEditor::Heading> headings = m_editor->headings();
        QString name = headings.isEmpty() ? tr("Untitled") : headings.first().title;
        name.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral(" "));
        suggestion = QDir(m_folder.isEmpty() ? QDir::homePath() : m_folder).filePath(name.trimmed() + QStringLiteral(".md"));
    }
    QString path = QFileDialog::getSaveFileName(this, tr("Save As"), suggestion, tr("Markdown (*.md *.markdown);;All files (*)"));
    if (path.isEmpty())
        return false;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".md");
    if (!m_path.isEmpty())
        m_watcher->removePath(m_path);
    m_path = QFileInfo(path).absoluteFilePath();
    m_editor->setBaseDir(QFileInfo(m_path).absolutePath());
    const bool ok = writeFile(m_path);
    if (ok) {
        m_watcher->addPath(m_path);
        addRecent(m_path);
        if (m_folder.isEmpty())
            openFolderPath(QFileInfo(m_path).absolutePath());
    }
    return ok;
}

bool MainWindow::maybeSave()
{
    const bool modified = m_editor->isModified() || m_source->document()->isModified();
    if (!modified)
        return true;
    const QString name = m_path.isEmpty() ? tr("Untitled") : QFileInfo(m_path).fileName();
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr("Save Changes"), tr("Save changes to “%1” before closing?").arg(name),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Save)
        return save();
    return true;
}

void MainWindow::reloadFromDisk()
{
    if (m_path.isEmpty() || !QFileInfo::exists(m_path))
        return;
    bool ok = false;
    const QString text = readText(m_path, &ok);
    if (!ok)
        return;
    const QString current = isSourceMode() ? m_source->markdown() : m_editor->markdown();
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (normalized == current || serializeBlocks(blocksFromMarkdown(normalized)) == current)
        return;
    if (m_editor->isModified() || m_source->document()->isModified()) {
        const auto answer = QMessageBox::question(this, tr("File Changed"),
                                                  tr("“%1” was changed on disk. Load the new version and lose your edits?")
                                                      .arg(QFileInfo(m_path).fileName()));
        if (answer != QMessageBox::Yes)
            return;
    }
    const int caret = m_editor->caretOffset();
    m_editor->setMarkdown(text);
    m_editor->setCaretOffset(std::min(caret, int(text.size())));
    if (isSourceMode())
        m_source->setMarkdown(m_editor->markdown());
    updateTitle();
    updateOutline();
}

void MainWindow::addRecent(const QString &path)
{
    QSettings settings;
    QStringList recent = settings.value(QStringLiteral("recent")).toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > MaxRecent)
        recent.removeLast();
    settings.setValue(QStringLiteral("recent"), recent);
    rebuildRecent();
}

void MainWindow::rebuildRecent()
{
    if (!m_recentMenu)
        return;
    m_recentMenu->clear();
    const QStringList recent = QSettings().value(QStringLiteral("recent")).toStringList();
    for (const QString &path : recent) {
        QAction *a = m_recentMenu->addAction(QDir::toNativeSeparators(path));
        connect(a, &QAction::triggered, this, [this, path] {
            if (m_path.isEmpty() && !m_editor->isModified())
                openFile(path);
            else if (path != m_path) {
                auto *window = new MainWindow;
                window->openFile(path);
                window->show();
            }
        });
    }
    if (!recent.isEmpty()) {
        m_recentMenu->addSeparator();
        m_recentMenu->addAction(tr("Clear Items"), this, [this] {
            QSettings().remove(QStringLiteral("recent"));
            rebuildRecent();
        });
    }
    m_recentMenu->setEnabled(!recent.isEmpty());
}

QString MainWindow::savePastedImage(const QImage &image)
{
    // Next to the document in "assets", as Typora does; elsewhere for unsaved documents.
    QString folder;
    if (!m_path.isEmpty())
        folder = QDir(QFileInfo(m_path).absolutePath()).filePath(QStringLiteral("assets"));
    else
        folder = QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).filePath(QStringLiteral("open-typora"));
    QDir().mkpath(folder);
    const QString name = QStringLiteral("image-%1.png").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmsszzz")));
    const QString path = QDir(folder).filePath(name);
    if (!image.save(path, "PNG"))
        return {};
    if (m_path.isEmpty())
        return path;
    return QDir(QFileInfo(m_path).absolutePath()).relativeFilePath(path);
}

QString MainWindow::imagePathFor(const QString &path)
{
    if (m_path.isEmpty())
        return path;
    const QString relative = QDir(QFileInfo(m_path).absolutePath()).relativeFilePath(path);
    return relative.startsWith(QLatin1String("../../../")) ? path : relative;
}

void MainWindow::exportHtml()
{
    setSourceMode(false);
    const QString base = m_path.isEmpty() ? QDir(QDir::homePath()).filePath(tr("Untitled"))
                                          : QFileInfo(m_path).absolutePath() + QLatin1Char('/') + QFileInfo(m_path).completeBaseName();
    const QString path = QFileDialog::getSaveFileName(this, tr("Export HTML"), base + QStringLiteral(".html"),
                                                      tr("HTML (*.html *.htm)"));
    if (path.isEmpty())
        return;
    const QString title = m_path.isEmpty() ? tr("Untitled") : QFileInfo(m_path).completeBaseName();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(exportToHtml(m_editor->blocks(), m_theme, title, m_editor->baseDir()).toUtf8()) < 0) {
        QMessageBox::warning(this, tr("Export"), tr("Could not write %1.").arg(QDir::toNativeSeparators(path)));
        return;
    }
    file.close();
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::exportPdf()
{
    setSourceMode(false);
    const QString base = m_path.isEmpty() ? QDir(QDir::homePath()).filePath(tr("Untitled"))
                                          : QFileInfo(m_path).absolutePath() + QLatin1Char('/') + QFileInfo(m_path).completeBaseName();
    const QString path = QFileDialog::getSaveFileName(this, tr("Export PDF"), base + QStringLiteral(".pdf"), tr("PDF (*.pdf)"));
    if (path.isEmpty())
        return;
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(path);
    printer.setDocName(QFileInfo(path).completeBaseName());
    printer.setPageMargins(QMarginsF(18, 18, 18, 18), QPageLayout::Millimeter);
    if (!printDocument(m_editor, &printer, EditorTheme::byName(m_theme.dark ? QStringLiteral("Github") : m_theme.name))) {
        QMessageBox::warning(this, tr("Export"), tr("Could not write %1.").arg(QDir::toNativeSeparators(path)));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::print()
{
    setSourceMode(false);
    QPrinter printer(QPrinter::HighResolution);
    printer.setPageMargins(QMarginsF(18, 18, 18, 18), QPageLayout::Millimeter);
    QPrintDialog dialog(&printer, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    printDocument(m_editor, &printer, EditorTheme::byName(m_theme.dark ? QStringLiteral("Github") : m_theme.name));
}

// ---- View ---------------------------------------------------------------------------------------

void MainWindow::setSourceMode(bool on)
{
    if (on == isSourceMode()) {
        m_sourceAction->setChecked(on);
        m_sourceButton->setChecked(on);
        return;
    }
    if (on) {
        const bool modified = m_editor->isModified();
        m_source->setMarkdown(m_editor->markdown());
        m_source->document()->setModified(modified);
        m_source->setCaretOffset(m_editor->caretOffset());
        m_stack->setCurrentWidget(m_source);
        m_source->setFocus();
    } else {
        const QString text = m_source->markdown();
        const bool modified = m_source->document()->isModified();
        if (text != m_editor->markdown()) {
            // Keep the formatted view's undo history: the switch is one step.
            m_editor->selectAll();
            m_editor->insertMarkdown(text);
        }
        if (!modified)
            m_editor->markSaved();
        m_editor->setCaretOffset(m_source->caretOffset());
        m_stack->setCurrentWidget(m_editor);
        m_editor->setFocus();
    }
    m_sourceAction->setChecked(on);
    m_sourceButton->setChecked(on);
    m_find->setSourceEditor(on ? m_source : nullptr);
    updateTitle();
}

void MainWindow::updateTitle()
{
    const bool modified = isSourceMode() ? m_source->document()->isModified() || m_editor->isModified()
                                         : m_editor->isModified();
    const QString name = m_path.isEmpty() ? tr("Untitled") : QFileInfo(m_path).fileName();
    setWindowTitle(QStringLiteral("%1%2 - Open Typora").arg(name, modified ? QStringLiteral("•") : QString()));
    setWindowFilePath(m_path);
    setWindowModified(modified);
}

void MainWindow::showFind(bool replace)
{
    m_find->setSourceEditor(isSourceMode() ? m_source : nullptr);
    m_find->open(replace);
}

void MainWindow::zoom(int steps)
{
    setZoom(m_zoom + 0.1 * steps);
}

void MainWindow::setZoom(qreal zoom)
{
    m_zoom = std::clamp(std::round(zoom * 10) / 10, 0.5, 3.0);
    m_editor->setZoom(m_zoom);
    m_source->setZoom(m_zoom);
    QSettings().setValue(QStringLiteral("zoom"), m_zoom);
}

void MainWindow::openLink(const QString &href)
{
    if (href.startsWith(QLatin1Char('#'))) {
        // A heading in this document.
        const QString target = href.mid(1).toLower();
        for (const MarkdownEditor::Heading &heading : m_editor->headings()) {
            QString slug = heading.title.toLower();
            slug.replace(QRegularExpression(QStringLiteral("[^\\w\\- ]")), QString());
            slug.replace(QLatin1Char(' '), QLatin1Char('-'));
            if (slug == target || heading.title.toLower() == target) {
                m_editor->goToBlock(heading.block);
                return;
            }
        }
        return;
    }
    QUrl url(href);
    if (url.isRelative()) {
        const QString local = ImageStore::localPath(href.section(QLatin1Char('#'), 0, 0), m_editor->baseDir());
        const QString suffix = QFileInfo(local).suffix().toLower();
        if ((suffix == QLatin1String("md") || suffix == QLatin1String("markdown")) && QFileInfo::exists(local)) {
            auto *window = new MainWindow;
            window->openFile(local);
            window->show();
            return;
        }
        url = QUrl::fromLocalFile(local);
    }
    QDesktopServices::openUrl(url);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About Open Typora"),
                       tr("<h3>Open Typora %1</h3>"
                          "<p>A Markdown editor that shows the document as it will read, and the Markdown "
                          "of whatever the caret is in.</p>"
                          "<p>Built with Qt %2. Math by MicroTeX.</p>")
                           .arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion())));
}

void MainWindow::showMarkdownReference()
{
    if (!maybeSave())
        return;
    m_path.clear();
    m_editor->setBaseDir(QString());
    m_editor->setMarkdown(markdownReference());
    updateTitle();
    updateOutline();
    updateCounts();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_autosaveAction->isChecked() && !m_path.isEmpty() && (m_editor->isModified() || m_source->document()->isModified()))
        save();
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    QSettings().setValue(QStringLiteral("geometry"), saveGeometry());
    event->accept();
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
}

} // namespace ot
