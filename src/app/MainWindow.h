#pragma once

#include "EditorTheme.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

class QAction;
class QActionGroup;
class QFileSystemModel;
class QLabel;
class QMenu;
class QSplitter;
class QStackedWidget;
class QToolButton;
class QTreeView;
class QTreeWidget;
class QFileSystemWatcher;

namespace ot {

class FindBar;
class MarkdownEditor;
class SourceEditor;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    /// Opens a file in this window, or a folder in its sidebar.
    void openPath(const QString &path);
    bool openFile(const QString &path);
    QString filePath() const { return m_path; }
    MarkdownEditor *editor() const { return m_editor; }
    bool isSourceMode() const;

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void buildMenus();
    void buildSidebar();
    void buildFooter();
    QAction *action(QMenu *menu, const QString &text, const QKeySequence &shortcut, std::function<void()> slot);

    // Files
    void newFile();
    void open();
    void openFolder();
    void openFolderPath(const QString &path);
    bool save();
    bool saveAs();
    bool writeFile(const QString &path);
    bool maybeSave();
    void reloadFromDisk();
    void addRecent(const QString &path);
    void rebuildRecent();
    void exportHtml();
    void exportPdf();
    void print();
    QString savePastedImage(const QImage &image);
    QString imagePathFor(const QString &path);

    // View
    void setTheme(const QString &name);
    void applyChrome();
    void setSidebarVisible(bool visible);
    void setOutlineShown(bool outline);
    void setSourceMode(bool on);
    void updateTitle();
    void updateOutline();
    void updateCounts();
    void showFind(bool replace);
    void zoom(int steps);
    void setZoom(qreal zoom);
    void openLink(const QString &href);
    void showAbout();
    void showMarkdownReference();
    void fillContextMenu(QMenu *menu, const QPoint &point);
    void updateParagraphChecks();

    QString m_path;
    QString m_folder;
    EditorTheme m_theme;
    qreal m_zoom = 1.0;

    MarkdownEditor *m_editor = nullptr;
    SourceEditor *m_source = nullptr;
    QStackedWidget *m_stack = nullptr;
    FindBar *m_find = nullptr;
    QSplitter *m_splitter = nullptr;
    QWidget *m_sidebar = nullptr;
    QToolButton *m_filesTab = nullptr;
    QToolButton *m_outlineTab = nullptr;
    QStackedWidget *m_sidebarStack = nullptr;
    QTreeView *m_files = nullptr;
    QFileSystemModel *m_fileModel = nullptr;
    QTreeWidget *m_outline = nullptr;
    QLabel *m_folderLabel = nullptr;
    QWidget *m_footer = nullptr;
    QToolButton *m_sidebarButton = nullptr;
    QToolButton *m_sourceButton = nullptr;
    QToolButton *m_countButton = nullptr;
    QMenu *m_recentMenu = nullptr;
    QMenu *m_tableMenu = nullptr;
    QHash<QString, QAction *> m_actions;
    QActionGroup *m_themeGroup = nullptr;
    QAction *m_sidebarAction = nullptr;
    QAction *m_outlineAction = nullptr;
    QAction *m_filesAction = nullptr;
    QAction *m_sourceAction = nullptr;
    QAction *m_focusAction = nullptr;
    QAction *m_typewriterAction = nullptr;
    QAction *m_autosaveAction = nullptr;
    QAction *m_statusAction = nullptr;
    QTimer m_outlineTimer;
    QTimer m_countTimer;
    QTimer m_autosaveTimer;
    QFileSystemWatcher *m_watcher = nullptr;
    bool m_writing = false;
};

} // namespace ot
