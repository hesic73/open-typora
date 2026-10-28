#include "MathRenderer.h"

#include <QCache>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>

#include "core/formula.h"
#include "core/macro.h"
#include "latex.h"
#include "platform/qt/graphic_qt.h"
#include "render.h"

namespace ot {

namespace {

constexpr int CacheSize = 512;
/// Longer sources, and groups nested deeper, are refused: the typesetter
/// recurses once per level and has no limit of its own.
constexpr qsizetype MaxSourceLength = 4000;
constexpr int MaxNesting = 32;
/// Formulas larger than this (in pixels) are an error, not something to draw.
constexpr qreal MaxExtent = 20000;
/// The typesetter builds as many columns, glyphs or layout letters as a count in
/// the source asks for before any size is known, so the counts are capped here.
constexpr int MaxColumns = 99;
constexpr int MaxRoman = 3999;
constexpr qsizetype MaxLayoutLength = 500;

class TeXFormula final : public Formula
{
public:
    explicit TeXFormula(std::unique_ptr<tex::TeXRender> render)
        : m_render(std::move(render))
        , m_height(m_render->getHeight())
        , m_descent(m_render->getHeight() * (1.0 - m_render->getBaseline()))
    {
    }

    qreal width() const override { return m_render->getWidth(); }
    qreal ascent() const override { return m_height - m_descent; }
    qreal descent() const override { return m_descent; }

    void draw(QPainter *painter, const QPointF &origin, qreal scale) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->translate(origin.x(), origin.y() - ascent() * scale);
        painter->scale(scale, scale);
        tex::Graphics2D_qt graphics(painter);
        m_render->draw(graphics, 0, 0);
        painter->restore();
    }

private:
    std::unique_ptr<tex::TeXRender> m_render;
    qreal m_height;
    qreal m_descent;
};

QString resourceRoot()
{
    const QString overridden = qEnvironmentVariable("OT_MICROTEX_RES");
    const QStringList candidates = {
        overridden,
        QCoreApplication::applicationDirPath() + QStringLiteral("/../share/open-typora/microtex"),
        QStringLiteral(OT_MICROTEX_SOURCE_RES),
    };
    for (const QString &candidate : candidates) {
        if (!candidate.isEmpty() && QFileInfo::exists(candidate + QStringLiteral("/fonts")))
            return QDir(candidate).absolutePath();
    }
    return {};
}

struct Engine {
    bool ready = false;
    QCache<QString, std::shared_ptr<const Formula>> cache{CacheSize};

    Engine()
    {
        const QString root = resourceRoot();
        if (root.isEmpty())
            return;
        try {
            tex::LaTeX::init(root.toStdString());
            ready = true;
        } catch (const std::exception &) {
            ready = false;
        }
    }
    ~Engine()
    {
        cache.clear();
        if (ready)
            tex::LaTeX::release();
    }
};

Engine &engine()
{
    static Engine instance;
    return instance;
}

enum class Scope { Group, Environment, Delimiters };

struct Frame {
    Scope scope = Scope::Group;
    QString name;
    /// Whether an atom stands at the end of what is written so far, for \limits, ' and the like to act on.
    bool atom = false;
    /// The macros written so far that still wait for arguments, innermost last.
    struct Waiting {
        int arguments;
        /// Whether the macro, once complete, is an atom. \hline and \multicolumn leave none behind.
        bool atom;
    };
    QList<Waiting> waiting;
    /// Tokens to pass over before anything counts: the layout of an array, for one.
    int skip = 0;
    /// A line break was written outside an environment, which turns what follows into rows.
    bool rows = false;
    /// Anything was written in the frame yet.
    bool started = false;
};

/// Commands that define commands or environments (an endless recursion is one
/// line away), that read files and fonts or change the parser's state, or that
/// change the typesetter's settings for every later formula (sizes, colours,
/// column types).
bool isForbiddenCommand(const QString &name)
{
    static const QSet<QString> forbidden = {
        QStringLiteral("newcommand"),     QStringLiteral("renewcommand"),   QStringLiteral("providecommand"),
        QStringLiteral("newenvironment"), QStringLiteral("renewenvironment"), QStringLiteral("DeclareMathOperator"),
        QStringLiteral("def"),            QStringLiteral("gdef"),           QStringLiteral("edef"),
        QStringLiteral("xdef"),           QStringLiteral("let"),            QStringLiteral("newtheorem"),
        QStringLiteral("includegraphics"), QStringLiteral("externalFont"),  QStringLiteral("dynamic"),
        QStringLiteral("GeoGebra"),       QStringLiteral("makeatletter"),   QStringLiteral("makeatother"),
        QStringLiteral("fatalIfCmdConflict"), QStringLiteral("breakEverywhere"), QStringLiteral("input"),
        QStringLiteral("include"),        QStringLiteral("usepackage"),     QStringLiteral("verb"),
        QStringLiteral("above"),          QStringLiteral("overwithdelims"), QStringLiteral("atopwithdelims"),
        QStringLiteral("abovewithdelims"), QStringLiteral("multirow"),    QStringLiteral("magnification"),
        QStringLiteral("DeclareMathSizes"), QStringLiteral("definecolor"), QStringLiteral("arrayrulecolor"),
        QStringLiteral("newcolumntype"),  QStringLiteral("cornersize"),
    };
    return forbidden.contains(name);
}

/// Reads "{name}" at `index` (after spaces); advances past it. Empty when absent.
QString readBracedName(const QString &source, qsizetype &index)
{
    qsizetype at = index;
    while (at < source.size() && source.at(at).isSpace())
        ++at;
    if (at >= source.size() || source.at(at) != QLatin1Char('{'))
        return {};
    const qsizetype close = source.indexOf(QLatin1Char('}'), at);
    if (close < 0 || close - at > 24)
        return {};
    const QString name = source.mid(at + 1, close - at - 1);
    for (const QChar c : name) {
        if (!c.isLetter() && c != QLatin1Char('*'))
            return {};
    }
    index = close + 1;
    return name;
}

/// Steps over "[...]" at `index` (after spaces), an optional argument.
/// Returns false when there is none, or when it holds what a check like this
/// cannot follow (that is a refusal for the caller, see `refused`).
bool skipOptional(const QString &source, qsizetype &index, bool &refused)
{
    qsizetype at = index;
    while (at < source.size() && source.at(at).isSpace())
        ++at;
    if (at >= source.size() || source.at(at) != QLatin1Char('['))
        return false;
    const qsizetype close = source.indexOf(QLatin1Char(']'), at);
    if (close < 0) {
        refused = true;
        return false;
    }
    for (qsizetype i = at + 1; i < close; ++i) {
        const QChar c = source.at(i);
        if (c == QLatin1Char('{') || c == QLatin1Char('}') || c == QLatin1Char('\\') || c == QLatin1Char('&')
            || c == QLatin1Char('%')) {
            refused = true;
            return false;
        }
    }
    index = close + 1;
    return true;
}

/// Whether "{n}" at the start of `text` (after spaces) is a count from 1 to `most`.
bool isCountWithin(QStringView text, int most)
{
    static const QRegularExpression count(QStringLiteral("^\\s*\\{\\s*([0-9]{1,9})\\s*\\}"));
    const QRegularExpressionMatch found = count.matchView(text);
    const int value = found.hasMatch() ? found.capturedView(1).toInt() : 0;
    return value >= 1 && value <= most;
}

/// The length of the group "{...}" that starts `text`, braces included; -1 when it is not closed.
qsizetype groupLength(QStringView text)
{
    if (!text.startsWith(QLatin1Char('{')))
        return -1;
    int depth = 0;
    for (qsizetype at = 0; at < text.size(); ++at) {
        if (text.at(at) == QLatin1Char('\\'))
            ++at;
        else if (text.at(at) == QLatin1Char('{'))
            ++depth;
        else if (text.at(at) == QLatin1Char('}') && --depth == 0)
            return at + 1;
    }
    return -1;
}

/// The length of an array layout once each *{n}{...} in it is written out n times, as the
/// typesetter does; -1 past `most`, or when a repeat is not of that form. (The typesetter
/// also takes time cubic in the length to read a layout, so long ones are refused too.)
qsizetype expandedLayoutLength(QStringView layout, qsizetype most)
{
    static const QRegularExpression repeat(QStringLiteral("^\\*\\s*\\{\\s*([0-9]{1,9})\\s*\\}\\s*"));
    qsizetype total = 0;
    for (qsizetype at = 0; at < layout.size() && total <= most;) {
        const QChar c = layout.at(at);
        const bool argument = (c == QLatin1Char('@') || c == QLatin1Char('>')) && at + 1 < layout.size()
            && layout.at(at + 1) == QLatin1Char('{');
        if (c == QLatin1Char('*')) {
            const QRegularExpressionMatch found = repeat.matchView(layout.mid(at));
            if (!found.hasMatch())
                return -1;
            at += found.capturedLength();
            const qsizetype length = groupLength(layout.mid(at));
            if (length < 0)
                return -1;
            const qsizetype inner = expandedLayoutLength(layout.mid(at + 1, length - 2), most);
            if (inner < 0)
                return -1;
            total += found.capturedView(1).toLongLong() * inner;
            at += length;
        } else if (argument) {
            // @{...} and >{...} are read whole, a * in them included; any other brace is a column.
            const qsizetype length = groupLength(layout.mid(at + 1));
            if (length < 0)
                return -1;
            total += 1 + length;
            at += 1 + length;
        } else {
            ++total;
            ++at;
        }
    }
    return total <= most ? total : -1;
}

/// Takes the style switches (\displaystyle ...) out of the source: the typesetter
/// crashes on some commands that follow one. A switch at the very start still
/// decides between display and text style, which is where models put them.
QString withoutStyleSwitches(const QString &source, bool &display)
{
    static const QSet<QString> styles = {QStringLiteral("displaystyle"), QStringLiteral("textstyle"),
                                         QStringLiteral("scriptstyle"), QStringLiteral("scriptscriptstyle")};
    QString out;
    out.reserve(source.size());
    bool leading = true;
    for (qsizetype index = 0; index < source.size();) {
        const QChar c = source.at(index);
        if (c != QLatin1Char('\\') || index + 1 >= source.size()) {
            leading = leading && c.isSpace();
            out += c;
            ++index;
            continue;
        }
        qsizetype end = index + 2;
        if (source.at(index + 1).isLetter()) {
            while (end < source.size() && source.at(end).isLetter())
                ++end;
        }
        const QString name = source.mid(index + 1, end - index - 1);
        if (styles.contains(name)) {
            if (leading && name == QLatin1String("displaystyle"))
                display = true;
            else if (leading && name == QLatin1String("textstyle"))
                display = false;
        } else {
            leading = false;
            out += QStringView(source).mid(index, end - index);
        }
        index = end;
    }
    return out;
}

/// Whether the typesetter can be trusted with this source. It crashes, rather
/// than throws, on a few constructs (an & or \multicolumn outside an array,
/// \limits with nothing before it or in the place of an argument, \( ... \))
/// and loops on recursive definitions, so what it cannot take is turned away
/// here. To know what stands before a \limits this follows how many arguments
/// each macro takes, as the typesetter does.
bool isSupported(const QString &source)
{
    if (source.size() > MaxSourceLength)
        return false;

    QList<Frame> stack;
    Frame top;
    const auto push = [&](Scope scope, const QString &name) {
        if (stack.size() >= MaxNesting)
            return false;
        stack.append(top);
        top = Frame();
        top.scope = scope;
        top.name = name;
        return true;
    };
    // Something complete has been written: an argument for the macro waiting for one,
    // or else an atom of its own. A macro that this completes is in turn the same.
    const auto finishUnit = [&top]() {
        if (top.skip > 0) {
            --top.skip;
            return;
        }
        while (!top.waiting.isEmpty()) {
            if (--top.waiting.last().arguments > 0)
                return;
            if (!top.waiting.takeLast().atom) {
                top.atom = false;
                return;
            }
        }
        top.atom = true;
    };
    // Whether what is written next is not the argument of a macro.
    const auto freestanding = [&top]() { return top.waiting.isEmpty() && top.skip == 0; };
    // A group, environment or \left...\right pair ends. Each must be complete, and counts as a unit
    // of what it sits in.
    const auto pop = [&]() {
        if (!top.waiting.isEmpty() || top.skip > 0)
            return false;
        // An empty group {} stands for nothing, not even an atom.
        const bool empty = !top.started && top.scope == Scope::Group;
        top = stack.takeLast();
        const bool lone = freestanding();
        const bool had = top.atom;
        finishUnit();
        if (empty && lone)
            top.atom = had;
        return true;
    };
    // \\ and \cr outside an environment turn the rest of the formula into rows, and the typesetter
    // crashes on a further line break inside a group written after one, or inside an environment.
    const auto lineBreak = [&](bool first) {
        // (An empty first row is nothing anyone means, and \begin{cases}\\& crashes it.)
        if (first && top.scope == Scope::Environment)
            return false;
        if (top.scope == Scope::Group) {
            for (const Frame &outer : std::as_const(stack)) {
                if (outer.scope == Scope::Environment || outer.rows)
                    return false;
            }
        }
        if (top.scope != Scope::Environment)
            top.rows = true;
        top.atom = false;
        return true;
    };

    const qsizetype size = source.size();
    // Reads the next token: a command is a backslash and its name, any other character stands alone.
    const auto next = [&source, size](qsizetype &index) {
        QString token(source.at(index++));
        if (token.front() == QLatin1Char('\\')) {
            if (index >= size)
                return QString();
            token += source.at(index++);
            if (token.at(1).isLetter()) {
                while (index < size && source.at(index).isLetter())
                    token += source.at(index++);
            }
        }
        return token;
    };

    bool refused = false;
    for (qsizetype index = 0; index < size;) {
        const QChar c = source.at(index);
        if (c.isSpace()) {
            ++index;
            continue;
        }
        // A comment would hide the rest from the typesetter but not from this check.
        // Only a group can follow what takes a layout.
        if (c == QLatin1Char('%') || (top.skip > 0 && c != QLatin1Char('{')))
            return false;
        // Text mode is a second parser that this check does not follow.
        if (c == QLatin1Char('$'))
            return false;
        const QString token = next(index);
        if (token.isEmpty())
            return false;
        const bool first = !top.started;
        top.started = true;

        if (c == QLatin1Char('{')) {
            if (!push(Scope::Group, {}))
                return false;
        } else if (c == QLatin1Char('}')) {
            if (stack.isEmpty() || top.scope != Scope::Group || !pop())
                return false;
        } else if (c == QLatin1Char('&')) {
            if (top.scope != Scope::Environment || !freestanding())
                return false;
            top.atom = false;
        } else if (c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`') || c == QChar(0x2032)) {
            // A prime is a script of the atom before it.
            if (!top.atom || !freestanding())
                return false;
        } else if (c == QLatin1Char('^') || c == QLatin1Char('_')) {
            // A script takes the next thing, which cannot be something like \limits.
            top.waiting.append({1, true});
        } else if (c != QLatin1Char('\\')) {
            finishUnit();
        } else if (!token.at(1).isLetter()) {
            // \( \) \[ \] open and close math inside math, which the typesetter mishandles.
            if (QStringLiteral("()[]").contains(token.at(1)))
                return false;
            if (token.at(1) == QLatin1Char('\\') && !freestanding())
                return false;
            if (token.at(1) == QLatin1Char('\\')) {
                // A line break starts afresh, and may carry a length: \\[2pt].
                if (!lineBreak(first))
                    return false;
                bool ignored = false;
                skipOptional(source, index, ignored);
            } else if (QStringLiteral("^'\"`~=.").contains(token.at(1))) {
                // An accent, which goes over whatever comes next.
                top.waiting.append({1, true});
            } else {
                finishUnit();
            }
        } else {
            const QString name = token.mid(1);
            if (isForbiddenCommand(name))
                return false;
            if (name == QLatin1String("begin")) {
                const QString env = readBracedName(source, index);
                if (env.isEmpty() || env == QLatin1String("math") || env == QLatin1String("displaymath")
                    || !freestanding())
                    return false;
                if (!push(Scope::Environment, env))
                    return false;
                // These take the column layout, or the number of columns, before the cells.
                static const QSet<QString> withArgument = {QStringLiteral("array"), QStringLiteral("alignat"),
                                                           QStringLiteral("alignedat")};
                top.skip = withArgument.contains(env) ? 1 : 0;
                if (env == QLatin1String("array") || env == QLatin1String("tabular")) {
                    while (index < size && source.at(index).isSpace())
                        ++index;
                    const QStringView rest = QStringView(source).mid(index);
                    const qsizetype length = groupLength(rest);
                    if (length < 0 || expandedLayoutLength(rest.mid(1, length - 2), MaxLayoutLength) < 0)
                        return false;
                }
            } else if (name == QLatin1String("end")) {
                const QString env = readBracedName(source, index);
                if (env.isEmpty() || top.scope != Scope::Environment || top.name != env || !pop())
                    return false;
            } else if (name == QLatin1String("left") || name == QLatin1String("right")) {
                const bool opens = name == QLatin1String("left");
                if (!opens && (top.scope != Scope::Delimiters || !freestanding()))
                    return false;
                // The delimiter after it is part of it.
                while (index < size && source.at(index).isSpace())
                    ++index;
                if (index >= size || QStringLiteral("{}&%").contains(source.at(index)))
                    return false;
                const QString delimiter = next(index);
                if (delimiter.size() > 1 && delimiter.at(1).isLetter() && isForbiddenCommand(delimiter.mid(1)))
                    return false;
                if (opens ? !push(Scope::Delimiters, {}) : !pop())
                    return false;
            } else {
                const tex::MacroInfo *info = tex::MacroInfo::get(name.toStdWString());
                const int arguments = info ? info->_argc : 0;
                // The typesetter cannot take most commands as the argument of another (\mathbf\color{red}...),
                // only symbols, which are not macros, and a few macros that stand for one.
                static const QSet<QString> symbolic = {
                    QStringLiteral("int"),   QStringLiteral("iint"),  QStringLiteral("iiint"), QStringLiteral("iiiint"),
                    QStringLiteral("oint"),  QStringLiteral("vdots"), QStringLiteral("ddots"), QStringLiteral("iddots"),
                    QStringLiteral("surd"),  QStringLiteral("quad"),  QStringLiteral("cong"),  QStringLiteral("doteq"),
                };
                if (info && !freestanding() && !symbolic.contains(name))
                    return false;
                // Options after the first argument are not followed, so such macros are not taken.
                if (info && info->_posOpts == 2)
                    return false;
                if (name == QLatin1String("multicolumn")) {
                    // It reaches into the array being built, so it is only for the cells of one,
                    // and a column count that makes sense.
                    const bool inArray = top.name == QLatin1String("array") || top.name == QLatin1String("tabular");
                    if (top.scope != Scope::Environment || !inArray || !freestanding())
                        return false;
                    if (!isCountWithin(QStringView(source).mid(index), MaxColumns))
                        return false;
                }
                static const QSet<QString> arrayOnly = {QStringLiteral("hline"),    QStringLiteral("hdotsfor"),
                                                        QStringLiteral("rowcolor"), QStringLiteral("cellcolor"),
                                                        QStringLiteral("shoveleft"), QStringLiteral("shoveright")};
                const bool leavesNoAtom = arrayOnly.contains(name) || name == QLatin1String("multicolumn");
                if (arrayOnly.contains(name) && (top.scope != Scope::Environment || !freestanding()))
                    return false;
                static const QSet<QString> infix = {QStringLiteral("over"),  QStringLiteral("atop"),
                                                    QStringLiteral("choose"), QStringLiteral("brace"),
                                                    QStringLiteral("brack"), QStringLiteral("bangle")};
                // \over and its kin take everything written so far as the numerator.
                if (infix.contains(name)) {
                    if (!top.atom || !freestanding() || top.scope == Scope::Environment)
                        return false;
                    top.atom = false;
                    continue;
                }
                // These act on the atom before them.
                const bool acts = name == QLatin1String("limits") || name == QLatin1String("nolimits")
                    || name == QLatin1String("normal");
                if (acts && (!top.atom || !freestanding()))
                    return false;
                if ((name == QLatin1String("mathcumsup") || name == QLatin1String("mathcumsub"))
                    && (!top.atom || !freestanding()))
                    return false;
                if (name == QLatin1String("color") || name == QLatin1String("rowcolor") || name == QLatin1String("cellcolor")) {
                    // These colour what follows; they are no atom, and take a plain name or #rrggbb.
                    static const QRegularExpression colour(QStringLiteral("^\\s*\\{[#A-Za-z0-9]{1,24}\\}"));
                    const QRegularExpressionMatch found = colour.matchView(QStringView(source).mid(index));
                    if (!found.hasMatch() || !freestanding())
                        return false;
                    index += found.capturedLength();
                } else if (name == QLatin1String("cr")) {
                    if (!lineBreak(first))
                        return false;
                } else if (arguments > 0) {
                    if (info->_posOpts == 1)
                        skipOptional(source, index, refused);
                    const bool roman = name == QLatin1String("Roman") || name == QLatin1String("roman");
                    if ((name == QLatin1String("hdotsfor") && !isCountWithin(QStringView(source).mid(index), MaxColumns))
                        || (roman && !isCountWithin(QStringView(source).mid(index), MaxRoman)))
                        return false;
                    top.waiting.append({arguments, !leavesNoAtom});
                } else if (leavesNoAtom) {
                    top.atom = false;
                } else if (!acts) {
                    finishUnit();
                }
            }
        }
        if (refused)
            return false;
    }
    return stack.isEmpty() && top.waiting.isEmpty() && top.skip == 0;
}

} // namespace

namespace math {

bool available()
{
    return engine().ready;
}

bool isComplete(const QString &latex)
{
    int braces = 0;
    for (qsizetype index = 0; index < latex.size(); ++index) {
        const QChar c = latex.at(index);
        if (c == QLatin1Char('\\')) {
            ++index; // \{ and \} are literal braces, and \\ is a line break.
            continue;
        }
        if (c == QLatin1Char('{'))
            ++braces;
        else if (c == QLatin1Char('}') && --braces < 0)
            return false;
    }
    if (braces != 0)
        return false;
    if (!latex.contains(QLatin1Char('\\')))
        return true;
    static const QRegularExpression begin(QStringLiteral("\\\\begin(?![a-zA-Z])"));
    static const QRegularExpression end(QStringLiteral("\\\\end(?![a-zA-Z])"));
    static const QRegularExpression left(QStringLiteral("\\\\left(?![a-zA-Z])"));
    static const QRegularExpression right(QStringLiteral("\\\\right(?![a-zA-Z])"));
    return latex.count(begin) == latex.count(end) && latex.count(left) == latex.count(right);
}

std::shared_ptr<const Formula> typeset(const QString &latex, bool display, qreal pixelSize, const QColor &color)
{
    Engine &state = engine();
    if (latex.size() > MaxSourceLength)
        return nullptr;
    // Checked before the source is: checking it takes longer than looking it up.
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(display).arg(pixelSize).arg(color.rgba()).arg(latex);
    if (const auto *cached = state.cache.object(key))
        return *cached;
    const QString source = withoutStyleSwitches(latex.trimmed(), display).trimmed();
    if (!state.ready || source.isEmpty() || !isSupported(source) || !isComplete(source)) {
        state.cache.insert(key, new std::shared_ptr<const Formula>());
        return nullptr;
    }

    std::shared_ptr<const Formula> result;
    try {
        tex::Formula formula(source.toStdWString());
        tex::TeXRenderBuilder builder;
        std::unique_ptr<tex::TeXRender> render(builder.setStyle(display ? tex::TexStyle::display : tex::TexStyle::text)
                                                   .setTextSize(static_cast<float>(pixelSize))
                                                   .setForeground(color.rgba())
                                                   .build(formula));
        const qreal width = render ? render->getWidth() : 0;
        const qreal height = render ? render->getHeight() : 0;
        if (width > 0 && width < MaxExtent && height > 0 && height < MaxExtent)
            result = std::make_shared<TeXFormula>(std::move(render));
    } catch (...) {
        // Not valid LaTeX: the caller shows the source instead.
    }
    // Failures are cached too, so a broken formula is not parsed on every redraw.
    state.cache.insert(key, new std::shared_ptr<const Formula>(result));
    return result;
}

} // namespace math

} // namespace ot
