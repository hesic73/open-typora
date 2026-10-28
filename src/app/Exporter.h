#pragma once

#include "Document.h"
#include "EditorTheme.h"

#include <QString>

class QPagedPaintDevice;

namespace ot {

class MarkdownEditor;

/// A standalone HTML page: styles from the theme, formulas as inline SVG.
QString exportToHtml(const QList<Block> &blocks, const EditorTheme &theme, const QString &title, const QString &baseDir);

/// The body of an HTML page for some Markdown, for the clipboard.
QString markdownToHtml(const QString &markdown, const EditorTheme &theme);

/// Paints the document onto pages (a printer, or a PDF writer).
bool printDocument(const MarkdownEditor *editor, QPagedPaintDevice *device, const EditorTheme &theme);

/// A document that shows the Markdown this editor understands.
QString markdownReference();

} // namespace ot
