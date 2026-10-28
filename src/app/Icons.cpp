#include "Icons.h"

#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace ot {

namespace {

/// 24x24 drawings; "%1" is the colour.
QString drawing(const QString &name)
{
    static const QHash<QString, QString> shapes = {
        {QStringLiteral("bold"),
         QStringLiteral("<path d='M7 5h6a3.5 3.5 0 0 1 0 7H7zM7 12h7a3.5 3.5 0 0 1 0 7H7z' fill='none' stroke='%1' "
                        "stroke-width='2.4' stroke-linejoin='round'/>")},
        {QStringLiteral("italic"),
         QStringLiteral("<path d='M10 5h8M6 19h8M15 5l-6 14' fill='none' stroke='%1' stroke-width='2' "
                        "stroke-linecap='round'/>")},
        {QStringLiteral("code"),
         QStringLiteral("<path d='M9 7l-5 5 5 5M15 7l5 5-5 5' fill='none' stroke='%1' stroke-width='2' "
                        "stroke-linecap='round' stroke-linejoin='round'/>")},
        {QStringLiteral("link"),
         QStringLiteral("<path d='M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 "
                        "5.7 5.7l1-1' fill='none' stroke='%1' stroke-width='2' stroke-linecap='round'/>")},
        {QStringLiteral("quote"),
         QStringLiteral("<path d='M4 18v-5c0-4 2-6.5 5.5-7.5l.7 1.6C8 8 7.2 9.6 7.2 11.5H10V18zM13 18v-5c0-4 2-6.5 "
                        "5.5-7.5l.7 1.6C17 8 16.2 9.6 16.2 11.5H19V18z' fill='%1'/>")},
        {QStringLiteral("ordered"),
         QStringLiteral("<path d='M10 6h10M10 12h10M10 18h10' stroke='%1' stroke-width='2' stroke-linecap='round'/>"
                        "<path d='M4.5 4.5L6 3.8V8.2M3.8 10.8c.4-.6 2.4-.9 2.4.4 0 1-2.4 2-2.4 3h2.6M3.8 16.6c.6-.5 "
                        "2.4-.6 2.4.6 0 .7-.8.9-1.3.9.6 0 1.4.3 1.4 1 0 1.2-2 1.2-2.6.6' fill='none' stroke='%1' "
                        "stroke-width='1.2' stroke-linecap='round' stroke-linejoin='round'/>")},
        {QStringLiteral("bullets"),
         QStringLiteral("<path d='M10 6h10M10 12h10M10 18h10' stroke='%1' stroke-width='2' stroke-linecap='round'/>"
                        "<circle cx='5' cy='6' r='1.6' fill='%1'/><circle cx='5' cy='12' r='1.6' fill='%1'/>"
                        "<circle cx='5' cy='18' r='1.6' fill='%1'/>")},
        {QStringLiteral("task"),
         QStringLiteral("<rect x='4' y='4' width='16' height='16' rx='3' fill='none' stroke='%1' stroke-width='2'/>"
                        "<path d='M8 12.3l2.8 2.7L16 9.3' fill='none' stroke='%1' stroke-width='2' "
                        "stroke-linecap='round' stroke-linejoin='round'/>")},
    };
    return shapes.value(name);
}

QPixmap render(const QString &name, const QColor &color, int size, qreal ratio)
{
    const QString svg = QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>%1</svg>")
                            .arg(drawing(name).arg(color.name()));
    QSvgRenderer renderer(svg.toUtf8());
    QPixmap pixmap(QSize(size, size) * ratio);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(ratio);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter, QRectF(0, 0, size, size));
    return pixmap;
}

} // namespace

QIcon menuIcon(const QString &name, const QColor &color)
{
    QIcon icon;
    // Sharp at the usual screen scales.
    for (const qreal ratio : {1.0, 1.25, 1.5, 2.0, 3.0})
        icon.addPixmap(render(name, color, 16, ratio));
    return icon;
}

} // namespace ot
