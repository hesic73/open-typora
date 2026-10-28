#include "Highlighter.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace ot {

namespace {

// A small lexer shared by every language. It knows comments, strings, numbers
// and words; a language only says which of each it has. That is far less than
// a real grammar, and enough to make code readable.

struct Language {
    QSet<QString> keywords;
    QSet<QString> literals;
    QStringList lineComments;
    QString blockOpen;
    QString blockClose;
    QString quotes = QStringLiteral("\"'");
    bool backtickStrings = false;
    bool tripleQuotes = false;
    bool caseInsensitive = false;
    /// Words may contain '-' (CSS, shell options are not words, but CSS properties are).
    bool dashedWords = false;
    /// `key:` or `key =` at the start of a line is an attribute (YAML, TOML, JSON keys).
    bool keyValue = false;
    bool markup = false;
    bool diff = false;
};

QSet<QString> words(const char *list)
{
    const QStringList split = QString::fromLatin1(list).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return QSet<QString>(split.cbegin(), split.cend());
}

Language cLike(const char *keywords, const char *literals = "true false null nullptr NULL")
{
    Language language;
    language.keywords = words(keywords);
    language.literals = words(literals);
    language.lineComments = {QStringLiteral("//")};
    language.blockOpen = QStringLiteral("/*");
    language.blockClose = QStringLiteral("*/");
    return language;
}

const QHash<QString, Language> &languages()
{
    static const QHash<QString, Language> table = [] {
        QHash<QString, Language> t;

        const Language cpp = cLike(
            "alignas alignof auto bool break case catch char char8_t char16_t char32_t class co_await co_return "
            "co_yield concept const consteval constexpr constinit const_cast continue decltype default delete do "
            "double dynamic_cast else enum explicit export extern float for friend goto if inline int long mutable "
            "namespace new noexcept operator override final private protected public register reinterpret_cast "
            "requires return short signed sizeof static static_assert static_cast struct switch template this "
            "thread_local throw try typedef typeid typename union unsigned using virtual void volatile wchar_t while "
            "emit signals slots foreach");
        for (const char *name : {"c", "cpp", "c++", "cc", "cxx", "h", "hpp", "objc"})
            t.insert(QString::fromLatin1(name), cpp);

        Language js = cLike(
            "abstract any as asserts async await break case catch class const constructor continue debugger declare "
            "default delete do else enum export extends finally for from function get if implements import in infer "
            "instanceof interface is keyof let module namespace never new of override package private protected "
            "public readonly return satisfies set static super switch this throw try type typeof unique unknown var "
            "void while with yield boolean number string symbol object bigint",
            "true false null undefined NaN Infinity");
        js.backtickStrings = true;
        for (const char *name : {"js", "javascript", "jsx", "ts", "typescript", "tsx", "mjs", "cjs"})
            t.insert(QString::fromLatin1(name), js);

        Language python;
        python.keywords = words(
            "and as assert async await break case class continue def del elif else except finally for from global if "
            "import in is lambda match nonlocal not or pass raise return try while with yield self cls");
        python.literals = words("True False None");
        python.lineComments = {QStringLiteral("#")};
        python.tripleQuotes = true;
        for (const char *name : {"py", "python", "python3"})
            t.insert(QString::fromLatin1(name), python);

        Language rust = cLike(
            "as async await break const continue crate dyn else enum extern fn for if impl in let loop match mod move "
            "mut pub ref return self Self static struct super trait type unsafe use where while i8 i16 i32 i64 i128 "
            "isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String Vec Option Result Box",
            "true false None Some Ok Err");
        rust.quotes = QStringLiteral("\"");
        for (const char *name : {"rust", "rs"})
            t.insert(QString::fromLatin1(name), rust);

        Language go = cLike(
            "break case chan const continue default defer else fallthrough for func go goto if import interface map "
            "package range return select struct switch type var any bool byte error float32 float64 int int8 int16 "
            "int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr",
            "true false nil iota");
        go.backtickStrings = true;
        for (const char *name : {"go", "golang"})
            t.insert(QString::fromLatin1(name), go);

        const Language java = cLike(
            "abstract assert boolean break byte case catch char class const continue default do double else enum "
            "extends final finally float for fun goto if implements import instanceof int interface internal long "
            "native new object open override package private protected public record return sealed short static "
            "strictfp super switch synchronized this throw throws transient try val var void volatile when while "
            "yield string decimal namespace using readonly ref out in is as async await get set");
        for (const char *name : {"java", "kotlin", "kt", "cs", "csharp", "scala", "swift", "dart"})
            t.insert(QString::fromLatin1(name), java);

        Language shell;
        shell.keywords = words(
            "if then else elif fi for while until do done case esac function in select time return exit export local "
            "readonly declare unset shift source alias set cd echo printf read test sudo");
        shell.literals = words("true false");
        shell.lineComments = {QStringLiteral("#")};
        for (const char *name : {"sh", "bash", "zsh", "shell", "console", "fish"})
            t.insert(QString::fromLatin1(name), shell);

        Language json;
        json.literals = words("true false null");
        json.quotes = QStringLiteral("\"");
        json.keyValue = true;
        for (const char *name : {"json", "jsonc", "json5"})
            t.insert(QString::fromLatin1(name), json);

        Language yaml;
        yaml.literals = words("true false null yes no on off ~");
        yaml.lineComments = {QStringLiteral("#")};
        yaml.keyValue = true;
        yaml.dashedWords = true;
        for (const char *name : {"yaml", "yml", "toml", "ini", "conf", "properties", "dockerfile", "makefile"})
            t.insert(QString::fromLatin1(name), yaml);

        Language sql;
        sql.keywords = words(
            "select from where and or not in is null as join inner left right full outer on group by order having "
            "limit offset insert into values update set delete create table alter drop index view primary key "
            "foreign references unique default distinct union all case when then else end exists between like asc "
            "desc with begin commit rollback");
        sql.literals = words("true false null");
        sql.lineComments = {QStringLiteral("--")};
        sql.blockOpen = QStringLiteral("/*");
        sql.blockClose = QStringLiteral("*/");
        sql.quotes = QStringLiteral("'");
        sql.caseInsensitive = true;
        t.insert(QStringLiteral("sql"), sql);

        Language css = cLike("important media import supports keyframes from to", "inherit initial unset none auto");
        css.lineComments.clear();
        css.dashedWords = true;
        css.keyValue = true;
        for (const char *name : {"css", "scss", "less"})
            t.insert(QString::fromLatin1(name), css);

        Language markup;
        markup.markup = true;
        markup.blockOpen = QStringLiteral("<!--");
        markup.blockClose = QStringLiteral("-->");
        for (const char *name : {"html", "xml", "svg", "vue", "xhtml", "qrc", "ui"})
            t.insert(QString::fromLatin1(name), markup);

        Language ruby;
        ruby.keywords = words(
            "alias and begin break case class def defined do else elsif end ensure for if in module next not or redo "
            "rescue retry return self super then undef unless until when while yield require include attr_accessor "
            "puts local function repeat");
        ruby.literals = words("true false nil");
        ruby.lineComments = {QStringLiteral("#"), QStringLiteral("--")};
        for (const char *name : {"ruby", "rb", "lua", "perl", "r", "elixir"})
            t.insert(QString::fromLatin1(name), ruby);

        Language diff;
        diff.diff = true;
        for (const char *name : {"diff", "patch"})
            t.insert(QString::fromLatin1(name), diff);

        return t;
    }();
    return table;
}

class Lexer
{
public:
    Lexer(const QString &code, const Language &language)
        : m_code(code)
        , m_language(language)
    {
    }

    QList<TokenSpan> run()
    {
        if (m_language.diff)
            return runDiff();
        while (m_position < m_code.size())
            step();
        return m_spans;
    }

private:
    bool startsWith(const QString &text) const
    {
        return !text.isEmpty() && QStringView(m_code).mid(m_position).startsWith(text);
    }

    void add(qsizetype start, qsizetype end, Token token)
    {
        if (end > start)
            m_spans.append({start, end - start, token});
    }

    bool isWordStart(QChar c) const { return c.isLetter() || c == QLatin1Char('_') || c == QLatin1Char('$'); }
    bool isWordPart(QChar c) const
    {
        return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$')
               || (m_language.dashedWords && c == QLatin1Char('-'));
    }

    bool atLineStart() const
    {
        for (qsizetype index = m_position - 1; index >= 0; --index) {
            const QChar c = m_code.at(index);
            if (c == QLatin1Char('\n'))
                return true;
            if (!c.isSpace() && c != QLatin1Char('-') && c != QLatin1Char('{') && c != QLatin1Char(','))
                return false;
        }
        return true;
    }

    void step()
    {
        const QChar c = m_code.at(m_position);

        if (startsWith(m_language.blockOpen)) {
            const qsizetype close = m_code.indexOf(m_language.blockClose, m_position + m_language.blockOpen.size());
            const qsizetype end = close < 0 ? m_code.size() : close + m_language.blockClose.size();
            add(m_position, end, Token::Comment);
            m_position = end;
            return;
        }
        for (const QString &marker : m_language.lineComments) {
            // A shell `#` only starts a comment at the start of a word.
            if (startsWith(marker)
                && (marker != QLatin1String("#") || m_position == 0 || m_code.at(m_position - 1).isSpace())) {
                qsizetype end = m_code.indexOf(QLatin1Char('\n'), m_position);
                if (end < 0)
                    end = m_code.size();
                add(m_position, end, Token::Comment);
                m_position = end;
                return;
            }
        }
        if (m_language.markup && c == QLatin1Char('<')) {
            lexTag();
            return;
        }
        if (m_language.quotes.contains(c) || (m_language.backtickStrings && c == QLatin1Char('`'))) {
            lexString(c);
            return;
        }
        if (c.isDigit() && (m_position == 0 || !isWordPart(m_code.at(m_position - 1)))) {
            const qsizetype start = m_position;
            while (m_position < m_code.size()
                   && (m_code.at(m_position).isLetterOrNumber() || m_code.at(m_position) == QLatin1Char('.')
                       || m_code.at(m_position) == QLatin1Char('_')))
                ++m_position;
            // Hex, binary, a decimal with an exponent or a type suffix. Anything
            // else that merely starts with a digit, such as a folder named
            // "26fall" or a hash, is left uncoloured.
            static const QRegularExpression number(QStringLiteral(
                "^(0[xX][0-9a-fA-F_]+|0[bB][01_]+|0[oO][0-7_]+|[0-9][0-9_]*(\\.[0-9_]*)?([eE][0-9]+)?[uUlLfFnNiI]*)$"));
            if (number.matchView(QStringView(m_code).mid(start, m_position - start)).hasMatch())
                add(start, m_position, Token::Number);
            return;
        }
        if (isWordStart(c)) {
            lexWord();
            return;
        }
        ++m_position;
    }

    void lexString(QChar quote)
    {
        const qsizetype start = m_position;
        const QString triple(3, quote);
        if (m_language.tripleQuotes && startsWith(triple)) {
            const qsizetype close = m_code.indexOf(triple, m_position + 3);
            m_position = close < 0 ? m_code.size() : close + 3;
            add(start, m_position, Token::String);
            return;
        }
        ++m_position;
        const bool multiline = quote == QLatin1Char('`');
        while (m_position < m_code.size()) {
            const QChar c = m_code.at(m_position);
            if (c == QLatin1Char('\\')) {
                m_position += 2;
                continue;
            }
            if (c == QLatin1Char('\n') && !multiline)
                break;
            ++m_position;
            if (c == quote)
                break;
        }
        m_position = qMin(m_position, m_code.size());

        // A quoted JSON key is a key, not a value.
        Token token = Token::String;
        if (m_language.keyValue && quote == QLatin1Char('"')) {
            qsizetype after = m_position;
            while (after < m_code.size() && m_code.at(after) == QLatin1Char(' '))
                ++after;
            if (after < m_code.size() && m_code.at(after) == QLatin1Char(':'))
                token = Token::Attribute;
        }
        add(start, m_position, token);
    }

    void lexWord()
    {
        const qsizetype start = m_position;
        const bool lineStart = m_language.keyValue && atLineStart();
        while (m_position < m_code.size() && isWordPart(m_code.at(m_position)))
            ++m_position;
        const QString raw = m_code.mid(start, m_position - start);
        const QString word = m_language.caseInsensitive ? raw.toLower() : raw;

        if (lineStart) {
            qsizetype after = m_position;
            while (after < m_code.size() && m_code.at(after) == QLatin1Char(' '))
                ++after;
            if (after < m_code.size() && (m_code.at(after) == QLatin1Char(':') || m_code.at(after) == QLatin1Char('='))) {
                add(start, m_position, Token::Attribute);
                return;
            }
        }
        if (m_language.keywords.contains(word)) {
            add(start, m_position, Token::Keyword);
        } else if (m_language.literals.contains(word)) {
            add(start, m_position, Token::Number);
        } else if (m_position < m_code.size() && m_code.at(m_position) == QLatin1Char('(')
                   && !m_language.keyValue) {
            add(start, m_position, Token::Title);
        }
    }

    void lexTag()
    {
        // `<name attr="value">`: the name is a keyword, attributes are attributes.
        ++m_position;
        if (m_position < m_code.size() && m_code.at(m_position) == QLatin1Char('/'))
            ++m_position;
        qsizetype start = m_position;
        while (m_position < m_code.size()
               && (isWordPart(m_code.at(m_position)) || m_code.at(m_position) == QLatin1Char('-')
                   || m_code.at(m_position) == QLatin1Char(':')))
            ++m_position;
        add(start, m_position, Token::Keyword);

        while (m_position < m_code.size() && m_code.at(m_position) != QLatin1Char('>')) {
            const QChar c = m_code.at(m_position);
            if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                start = m_position++;
                while (m_position < m_code.size() && m_code.at(m_position) != c)
                    ++m_position;
                m_position = qMin(m_position + 1, m_code.size());
                add(start, m_position, Token::String);
            } else if (isWordStart(c)) {
                start = m_position;
                while (m_position < m_code.size()
                       && (isWordPart(m_code.at(m_position)) || m_code.at(m_position) == QLatin1Char('-')
                           || m_code.at(m_position) == QLatin1Char(':')))
                    ++m_position;
                add(start, m_position, Token::Attribute);
            } else {
                ++m_position;
            }
        }
        if (m_position < m_code.size())
            ++m_position;
    }

    QList<TokenSpan> runDiff()
    {
        qsizetype start = 0;
        while (start < m_code.size()) {
            qsizetype end = m_code.indexOf(QLatin1Char('\n'), start);
            if (end < 0)
                end = m_code.size();
            const QStringView line = QStringView(m_code).mid(start, end - start);
            if (line.startsWith(QLatin1String("+++")) || line.startsWith(QLatin1String("---"))
                || line.startsWith(QLatin1String("diff ")) || line.startsWith(QLatin1String("index ")))
                add(start, end, Token::Comment);
            else if (line.startsWith(QLatin1String("@@")))
                add(start, end, Token::Title);
            else if (line.startsWith(QLatin1Char('+')))
                add(start, end, Token::Added);
            else if (line.startsWith(QLatin1Char('-')))
                add(start, end, Token::Removed);
            start = end + 1;
        }
        return m_spans;
    }

    const QString &m_code;
    const Language &m_language;
    qsizetype m_position = 0;
    QList<TokenSpan> m_spans;
};

} // namespace

QList<TokenSpan> highlight(const QString &code, const QString &language)
{
    const auto found = languages().constFind(language.trimmed().toLower());
    if (found == languages().constEnd())
        return {};
    return Lexer(code, *found).run();
}

} // namespace ot
