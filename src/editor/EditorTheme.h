#pragma once

#include <QColor>
#include <QFont>
#include <QString>
#include <QStringList>

namespace ot {

/// How the document looks. The names and colours follow the themes that ship
/// with Typora, redrawn from their published values.
struct EditorTheme {
    QString name;
    bool dark = false;

    QColor background;
    QColor text;
    QColor heading;
    QColor muted;
    /// Markdown syntax shown while editing it.
    QColor marker;
    QColor link;
    QColor selection;
    QColor caret;
    QColor findMatch;
    QColor findCurrent;

    QColor codeBackground;
    QColor codeBorder;
    QColor codeText;
    QColor inlineCodeBackground;
    QColor inlineCodeText;
    QColor quoteBar;
    QColor quoteText;
    QColor rule;
    QColor headingRule;
    QColor tableBorder;
    QColor tableHeaderBackground;
    QColor tableStripe;
    QColor highlightBackground;
    QColor error;

    QColor hlKeyword;
    QColor hlString;
    QColor hlNumber;
    QColor hlComment;
    QColor hlTitle;
    QColor hlAttribute;
    QColor hlAdded;
    QColor hlRemoved;

    // The window around the document.
    QColor sidebarBackground;
    QColor sidebarText;
    QColor sidebarActive;
    QColor sidebarHover;
    QColor border;

    QStringList bodyFamilies;
    QStringList headingFamilies;
    QStringList monoFamilies;
    /// Pixel size of body text, before zoom.
    qreal fontSize = 16;
    qreal lineHeight = 1.6;
    qreal maxWidth = 860;
    qreal padding = 30;
    /// Headings are drawn with a line under them, for these levels.
    int headingRuleLevels = 2;
    bool headingsSerif = false;

    QFont bodyFont(qreal zoom = 1.0) const;
    QFont headingFont(int level, qreal zoom = 1.0) const;
    QFont monoFont(qreal zoom = 1.0, qreal relative = 0.9) const;
    qreal headingScale(int level) const;

    static EditorTheme github();
    static EditorTheme night();
    static EditorTheme newsprint();
    static EditorTheme whitey();
    static EditorTheme pixyll();
    static QStringList names();
    static EditorTheme byName(const QString &name);
};

} // namespace ot
