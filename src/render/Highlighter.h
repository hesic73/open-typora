#pragma once

#include <QList>
#include <QString>

namespace ot {

enum class Token { Plain, Keyword, String, Number, Comment, Title, Attribute, Added, Removed };

struct TokenSpan {
    qsizetype start = 0;
    qsizetype length = 0;
    Token token = Token::Plain;
};

/// Splits source code into colored spans. The spans are in order, do not
/// overlap, and leave plain text uncovered. Unknown languages yield none.
QList<TokenSpan> highlight(const QString &code, const QString &language);

} // namespace ot
