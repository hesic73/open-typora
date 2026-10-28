#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace ot {

/// Line icons for the context menu's buttons: bold, italic, code, link, quote,
/// ordered, bullets, task.
QIcon menuIcon(const QString &name, const QColor &color);

} // namespace ot
