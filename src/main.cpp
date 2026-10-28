#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("open-typora"));
    QApplication::setApplicationDisplayName(QStringLiteral("Open Typora"));
    QApplication::setOrganizationName(QStringLiteral("open-typora"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setDesktopFileName(QStringLiteral("open-typora"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/open-typora.svg")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("A Markdown editor in the manner of Typora."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Markdown files or a folder to open."));
    parser.process(app);

    const QStringList paths = parser.positionalArguments();
    if (paths.isEmpty()) {
        auto *window = new ot::MainWindow;
        window->show();
    } else {
        for (const QString &path : paths) {
            auto *window = new ot::MainWindow;
            window->openPath(path);
            window->show();
        }
    }
    return app.exec();
}
