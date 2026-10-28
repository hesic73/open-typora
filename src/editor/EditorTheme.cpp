#include "EditorTheme.h"

#include <QFontDatabase>

namespace ot {

namespace {

QStringList withFallbacks(QStringList families, bool serif)
{
    // Han text falls back to whichever CJK font fontconfig finds first unless one is named.
    const QStringList cjk = serif ? QStringList{QStringLiteral("Noto Serif CJK SC"), QStringLiteral("Noto Sans CJK SC")}
                                  : QStringList{QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Source Han Sans SC"),
                                                QStringLiteral("WenQuanYi Micro Hei")};
    families += cjk;
    families += {QStringLiteral("Noto Color Emoji"), QStringLiteral("Segoe UI Emoji")};
    return families;
}

QStringList sans()
{
    return {QStringLiteral("Open Sans"), QStringLiteral("Clear Sans"), QStringLiteral("Helvetica Neue"),
            QStringLiteral("Noto Sans"), QStringLiteral("Liberation Sans"), QStringLiteral("Arial"),
            QStringLiteral("DejaVu Sans")};
}

QStringList serif()
{
    return {QStringLiteral("PT Serif"), QStringLiteral("Merriweather"), QStringLiteral("Vollkorn"),
            QStringLiteral("Noto Serif"), QStringLiteral("Liberation Serif"), QStringLiteral("DejaVu Serif"),
            QStringLiteral("Times New Roman")};
}

QStringList mono()
{
    return {QStringLiteral("JetBrains Mono"), QStringLiteral("Fira Code"), QStringLiteral("Source Code Pro"),
            QStringLiteral("Liberation Mono"), QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Noto Sans Mono"),
            QStringLiteral("Noto Sans Mono CJK SC"), QStringLiteral("monospace")};
}

/// Only families that exist, so that the first one is the one used.
QStringList available(const QStringList &families)
{
    static const QStringList installed = QFontDatabase::families();
    QStringList out;
    for (const QString &family : families) {
        if (installed.contains(family) || family == QLatin1String("monospace"))
            out.append(family);
    }
    return out.isEmpty() ? families : out;
}

void setCodeColors(EditorTheme &t, bool dark)
{
    if (dark) {
        t.hlKeyword = QColor(0xc6, 0x78, 0xdd);
        t.hlString = QColor(0x98, 0xc3, 0x79);
        t.hlNumber = QColor(0xd1, 0x9a, 0x66);
        t.hlComment = QColor(0x7f, 0x84, 0x8e);
        t.hlTitle = QColor(0x61, 0xaf, 0xef);
        t.hlAttribute = QColor(0xe0, 0x6c, 0x75);
        t.hlAdded = QColor(0x98, 0xc3, 0x79);
        t.hlRemoved = QColor(0xe0, 0x6c, 0x75);
    } else {
        // The colours of CodeMirror's default theme, which Typora's code blocks use.
        t.hlKeyword = QColor(0x77, 0x00, 0x88);
        t.hlString = QColor(0xaa, 0x11, 0x11);
        t.hlNumber = QColor(0x11, 0x66, 0x44);
        t.hlComment = QColor(0xaa, 0x55, 0x00);
        t.hlTitle = QColor(0x00, 0x00, 0xff);
        t.hlAttribute = QColor(0x00, 0x00, 0xcc);
        t.hlAdded = QColor(0x22, 0x99, 0x22);
        t.hlRemoved = QColor(0xaa, 0x22, 0x22);
    }
}

} // namespace

QFont EditorTheme::bodyFont(qreal zoom) const
{
    QFont font;
    font.setFamilies(available(bodyFamilies));
    font.setPixelSize(qRound(fontSize * zoom));
    font.setHintingPreference(QFont::PreferNoHinting);
    return font;
}

QFont EditorTheme::headingFont(int level, qreal zoom) const
{
    QFont font;
    font.setFamilies(available(headingFamilies.isEmpty() ? bodyFamilies : headingFamilies));
    font.setPixelSize(qRound(fontSize * zoom * headingScale(level)));
    font.setWeight(QFont::Bold);
    font.setHintingPreference(QFont::PreferNoHinting);
    return font;
}

QFont EditorTheme::monoFont(qreal zoom, qreal relative) const
{
    QFont font;
    font.setFamilies(available(monoFamilies));
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(qRound(fontSize * zoom * relative));
    return font;
}

qreal EditorTheme::headingScale(int level) const
{
    switch (level) {
    case 1: return 2.25;
    case 2: return 1.75;
    case 3: return 1.5;
    case 4: return 1.25;
    default: return 1.0;
    }
}

EditorTheme EditorTheme::github()
{
    EditorTheme t;
    t.name = QStringLiteral("Github");
    t.background = QColor(0xff, 0xff, 0xff);
    t.text = QColor(0x33, 0x33, 0x33);
    t.heading = QColor(0x33, 0x33, 0x33);
    t.muted = QColor(0x77, 0x77, 0x77);
    t.marker = QColor(0xa0, 0xa4, 0xa8);
    t.link = QColor(0x41, 0x83, 0xc4);
    t.selection = QColor(0xb5, 0xd6, 0xfc);
    t.caret = QColor(0x33, 0x33, 0x33);
    t.findMatch = QColor(0xff, 0xf0, 0x8a);
    t.findCurrent = QColor(0xff, 0xb9, 0x4f);
    t.codeBackground = QColor(0xf8, 0xf8, 0xf8);
    t.codeBorder = QColor(0xe7, 0xea, 0xed);
    t.codeText = QColor(0x33, 0x33, 0x33);
    t.inlineCodeBackground = QColor(0xf3, 0xf4, 0xf4);
    t.inlineCodeText = QColor(0x33, 0x33, 0x33);
    t.quoteBar = QColor(0xdf, 0xe2, 0xe5);
    t.quoteText = QColor(0x77, 0x77, 0x77);
    t.rule = QColor(0xe7, 0xe7, 0xe7);
    t.headingRule = QColor(0xee, 0xee, 0xee);
    t.tableBorder = QColor(0xdf, 0xe2, 0xe5);
    t.tableHeaderBackground = QColor(0xff, 0xff, 0xff);
    t.tableStripe = QColor(0xf8, 0xf8, 0xf8);
    t.highlightBackground = QColor(0xff, 0xff, 0x00);
    t.error = QColor(0xcc, 0x33, 0x33);
    setCodeColors(t, false);
    t.sidebarBackground = QColor(0xfa, 0xfa, 0xfa);
    t.sidebarText = QColor(0x33, 0x33, 0x33);
    t.sidebarActive = QColor(0xe6, 0xe6, 0xe6);
    t.sidebarHover = QColor(0xef, 0xef, 0xef);
    t.border = QColor(0xe5, 0xe5, 0xe5);
    t.bodyFamilies = withFallbacks(sans(), false);
    t.monoFamilies = mono();
    return t;
}

EditorTheme EditorTheme::night()
{
    EditorTheme t = github();
    t.name = QStringLiteral("Night");
    t.dark = true;
    t.background = QColor(0x36, 0x3b, 0x40);
    t.text = QColor(0xb8, 0xbf, 0xc6);
    t.heading = QColor(0xde, 0xde, 0xde);
    t.muted = QColor(0x9d, 0xa2, 0xa6);
    t.marker = QColor(0x80, 0x87, 0x8e);
    t.link = QColor(0x6d, 0xc1, 0xe7);
    t.selection = QColor(0x4a, 0x89, 0xdc);
    t.caret = QColor(0xee, 0xee, 0xee);
    t.findMatch = QColor(0x6b, 0x5b, 0x1b);
    t.findCurrent = QColor(0xa0, 0x6d, 0x1a);
    t.codeBackground = QColor(0x33, 0x33, 0x33);
    t.codeBorder = QColor(0x33, 0x33, 0x33);
    t.codeText = QColor(0xd0, 0xd0, 0xd0);
    t.inlineCodeBackground = QColor(0x2f, 0x35, 0x3a);
    t.inlineCodeText = QColor(0xd4, 0xd4, 0xd4);
    t.quoteBar = QColor(0x47, 0x4d, 0x54);
    t.quoteText = QColor(0x9d, 0xa2, 0xa6);
    t.rule = QColor(0x47, 0x4d, 0x54);
    t.headingRule = QColor(0x47, 0x4d, 0x54);
    t.tableBorder = QColor(0x47, 0x4d, 0x54);
    t.tableHeaderBackground = QColor(0x47, 0x4d, 0x54);
    t.tableStripe = QColor(0x3a, 0x40, 0x46);
    t.highlightBackground = QColor(0x9a, 0x86, 0x1e);
    t.error = QColor(0xff, 0x7b, 0x72);
    setCodeColors(t, true);
    t.sidebarBackground = QColor(0x2e, 0x30, 0x33);
    t.sidebarText = QColor(0xb7, 0xb7, 0xb7);
    t.sidebarActive = QColor(0x22, 0x22, 0x22);
    t.sidebarHover = QColor(0x3a, 0x3d, 0x41);
    t.border = QColor(0x25, 0x27, 0x29);
    t.maxWidth = 914;
    t.lineHeight = 1.625;
    return t;
}

EditorTheme EditorTheme::newsprint()
{
    EditorTheme t = github();
    t.name = QStringLiteral("Newsprint");
    t.background = QColor(0xf3, 0xf2, 0xee);
    t.text = QColor(0x1f, 0x09, 0x09);
    t.heading = QColor(0x1f, 0x09, 0x09);
    t.link = QColor(0x06, 0x55, 0x88);
    t.codeBackground = QColor(0xda, 0xda, 0xda);
    t.codeBorder = QColor(0xda, 0xda, 0xda);
    t.inlineCodeBackground = QColor(0xda, 0xda, 0xda);
    t.quoteBar = QColor(0x99, 0x99, 0x99);
    t.quoteText = QColor(0x55, 0x55, 0x55);
    t.tableHeaderBackground = QColor(0xde, 0xdd, 0xd8);
    t.tableStripe = QColor(0xe8, 0xe7, 0xe2);
    t.tableBorder = QColor(0xc0, 0xbe, 0xb6);
    t.sidebarBackground = QColor(0xf3, 0xf2, 0xee);
    t.sidebarActive = QColor(0xdc, 0xda, 0xd2);
    t.sidebarHover = QColor(0xe6, 0xe4, 0xde);
    t.border = QColor(0xdc, 0xda, 0xd2);
    t.bodyFamilies = withFallbacks(serif(), true);
    t.lineHeight = 1.5;
    t.maxWidth = 914;
    t.headingRuleLevels = 0;
    t.headingsSerif = true;
    return t;
}

EditorTheme EditorTheme::whitey()
{
    EditorTheme t = github();
    t.name = QStringLiteral("Whitey");
    t.background = QColor(0xfe, 0xfe, 0xfe);
    t.link = QColor(0x24, 0x84, 0xc1);
    t.codeBackground = QColor(0xf8, 0xf8, 0xf8);
    t.inlineCodeBackground = QColor(0xed, 0xed, 0xed);
    t.bodyFamilies = withFallbacks(QStringList{QStringLiteral("Vollkorn"), QStringLiteral("Palatino")} + serif(), true);
    t.fontSize = 19;
    t.lineHeight = 1.53;
    t.maxWidth = 960;
    t.headingRuleLevels = 0;
    return t;
}

EditorTheme EditorTheme::pixyll()
{
    EditorTheme t = github();
    t.name = QStringLiteral("Pixyll");
    t.bodyFamilies = withFallbacks(QStringList{QStringLiteral("Merriweather")} + serif(), true);
    t.headingFamilies = withFallbacks(QStringList{QStringLiteral("Lato")} + sans(), false);
    t.link = QColor(0x46, 0x3f, 0x5c);
    t.lineHeight = 1.5;
    t.maxWidth = 914;
    t.headingRuleLevels = 0;
    return t;
}

QStringList EditorTheme::names()
{
    return {QStringLiteral("Github"), QStringLiteral("Newsprint"), QStringLiteral("Night"), QStringLiteral("Pixyll"),
            QStringLiteral("Whitey")};
}

EditorTheme EditorTheme::byName(const QString &name)
{
    if (name.compare(QLatin1String("night"), Qt::CaseInsensitive) == 0)
        return night();
    if (name.compare(QLatin1String("newsprint"), Qt::CaseInsensitive) == 0)
        return newsprint();
    if (name.compare(QLatin1String("whitey"), Qt::CaseInsensitive) == 0)
        return whitey();
    if (name.compare(QLatin1String("pixyll"), Qt::CaseInsensitive) == 0)
        return pixyll();
    return github();
}

} // namespace ot
