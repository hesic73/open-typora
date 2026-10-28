#pragma once

#include <QHash>
#include <QList>
#include <QString>

namespace ot {

enum class InlineType {
    Text,
    Emph,
    Strong,
    Strike,
    Highlight,
    Code,
    Math,
    Link,
    Image,
    AutoLink,
    Escape,
    Entity,
    HtmlTag,
    /// A pair of HTML tags with content between them, such as <u>...</u>.
    HtmlSpan,
    SoftBreak,
    HardBreak,
    FootnoteRef,
};

/// One inline construct. Positions are indices into the parsed text: the
/// whole construct is [start, end), its content [innerStart, innerEnd); what
/// lies outside the content is syntax that is shown only while editing it.
struct InlineNode {
    InlineType type = InlineType::Text;
    int start = 0;
    int end = 0;
    int innerStart = 0;
    int innerEnd = 0;
    /// Link or image target, the LaTeX of a formula, a tag's name, an entity's character.
    QString target;
    /// Link title; for HTML tags the whole tag; for math "display" when written $$...$$.
    QString title;
    QList<InlineNode> children;
    int id = 0;
};

using LinkReferences = QHash<QString, QString>;

/// Parses inline Markdown. `references` resolves [text][label] links.
QList<InlineNode> parseInline(const QString &text, const LinkReferences *references = nullptr);

/// The text a reader sees: syntax dropped, entities decoded, formulas as written.
QString plainText(const QString &text, const QList<InlineNode> &nodes);

/// Normalises a reference label for lookup.
QString referenceKey(const QString &label);

/// Decodes "&amp;" and friends; empty when the entity is unknown.
QString decodeEntity(const QString &entity);

/// The value of an attribute in an HTML tag, empty when absent.
QString htmlAttribute(const QString &tag, const QString &name);

} // namespace ot
