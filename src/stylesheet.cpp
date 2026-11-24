#include "stylesheet.h"

#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace Styling {

// ---------------------------------------------------------------------------
// Selectors

struct AttributeTest {
    QString name;       // as written in the XHTML, "epub:type", or a local name
    bool anyPrefix = false;
    QChar op;           // 0 for "has it", or '=', '~', '|', '^', '$', '*'
    QString value;
};

struct Compound {
    QString tag;        // empty for any
    QString id;
    QStringList classes;
    QList<AttributeTest> attributes;
    QStringList pseudos;            // first-child, last-child, only-child, first-of-type, last-of-type
    QList<Compound> negations;      // :not(…)
    bool unsupported = false;       // ::before, :hover and the like never match here
};

struct Selector {
    // Right to left: parts[0] is the element itself, and combinators[i] joins
    // parts[i] to parts[i + 1]: ' ' descendant, '>' child, '+' next sibling,
    // '~' any earlier sibling.
    QList<Compound> parts;
    QList<QChar> combinators;
    int specificity = 0;
};

namespace {

struct Node {
    QString tag;                    // lower-case local name; empty for text
    QList<std::pair<QString, QString>> attributes;   // qualified names
    QString text;
    Node *parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;
    int index = 0;                  // among the parent's element children
    int count = 0;                  // the parent's element children
    int typeIndex = 0, typeCount = 0;
    Node *previous = nullptr;       // the element before it among its siblings
    QString id;
    QStringList classes;

    bool isText() const { return tag.isEmpty(); }
    QString attribute(const QString &name) const {
        for (const auto &[key, value] : attributes)
            if (key == name)
                return value;
        return {};
    }
    bool hasAttribute(const AttributeTest &test, QString *value) const {
        for (const auto &[key, v] : attributes) {
            const QString local = key.section(':', -1);
            if (test.anyPrefix ? local == test.name : key == test.name) {
                *value = v;
                return true;
            }
        }
        return false;
    }
};

QString stripComments(QString css) {
    static const QRegularExpression comment("/\\*.*?\\*/", QRegularExpression::DotMatchesEverythingOption);
    return css.remove(comment);
}

// Splits at separator outside brackets, parentheses and quotes.
QStringList splitTop(const QString &text, QChar separator) {
    QStringList out;
    int depth = 0;
    QChar quote;
    QString current;
    for (const QChar c : text) {
        if (!quote.isNull()) {
            if (c == quote)
                quote = QChar();
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(' || c == '[') {
            ++depth;
        } else if (c == ')' || c == ']') {
            depth = std::max(0, depth - 1);
        } else if (c == separator && depth == 0) {
            out << current;
            current.clear();
            continue;
        }
        current += c;
    }
    out << current;
    return out;
}

bool isNameChar(QChar c) { return c.isLetterOrNumber() || c == '-' || c == '_' || c.unicode() > 127; }

std::optional<Compound> parseCompound(const QString &text, int *specificity) {
    Compound compound;
    int i = 0;
    const int n = int(text.size());
    auto name = [&] {
        QString out;
        while (i < n && (isNameChar(text[i]) || text[i] == '\\')) {
            if (text[i] == '\\' && i + 1 < n)
                ++i;
            out += text[i++];
        }
        return out;
    };
    if (i < n && text[i] == '*') {
        ++i;
        if (i < n && text[i] == '|')    // *|tag
            ++i;
    }
    if (i < n && isNameChar(text[i])) {
        QString tag = name();
        if (i < n && text[i] == '|') {  // ns|tag
            ++i;
            tag = name();
        }
        compound.tag = tag.toLower();
        *specificity += 1;
    }
    while (i < n) {
        const QChar c = text[i];
        if (c == '#') {
            ++i;
            compound.id = name();
            *specificity += 10000;
        } else if (c == '.') {
            ++i;
            compound.classes << name();
            *specificity += 100;
        } else if (c == '[') {
            const int close = int(text.indexOf(']', i));
            if (close < 0)
                return std::nullopt;
            QString inner = text.mid(i + 1, close - i - 1).trimmed();
            i = close + 1;
            AttributeTest test;
            static const QRegularExpression pattern(
                "^([*\\w-]*\\|)?([\\w:-]+)\\s*(?:([~|^$*]?=)\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s\\]]+)))?(?:\\s+[iIsS])?$");
            const auto match = pattern.match(inner);
            if (!match.hasMatch())
                return std::nullopt;
            const QString prefix = match.captured(1);
            if (prefix == "*|") {
                test.name = match.captured(2);
                test.anyPrefix = true;
            } else if (!prefix.isEmpty() && prefix != "|") {
                test.name = prefix.chopped(1) + ':' + match.captured(2);
            } else {
                test.name = match.captured(2);
            }
            const QString op = match.captured(3);
            test.op = op.isEmpty() ? QChar() : op.size() == 1 ? QChar('=') : op[0];
            test.value = !match.captured(4).isNull() ? match.captured(4)
                       : !match.captured(5).isNull() ? match.captured(5) : match.captured(6);
            compound.attributes << test;
            *specificity += 100;
        } else if (c == ':') {
            ++i;
            if (i < n && text[i] == ':') {  // a pseudo-element: never laid out here
                ++i;
                name();
                compound.unsupported = true;
                continue;
            }
            const QString pseudo = name().toLower();
            if (i < n && text[i] == '(') {
                int depth = 0, start = i + 1;
                for (; i < n; ++i) {
                    if (text[i] == '(')
                        ++depth;
                    else if (text[i] == ')' && --depth == 0)
                        break;
                }
                const QString argument = text.mid(start, i - start).trimmed();
                ++i;
                if (pseudo == "not") {
                    for (const QString &part : splitTop(argument, ',')) {
                        int ignored = 0;
                        auto negation = parseCompound(part.trimmed(), &ignored);
                        if (!negation)
                            return std::nullopt;
                        compound.negations << *negation;
                    }
                    *specificity += 100;
                } else if (pseudo == "nth-child" && (argument == "1")) {
                    compound.pseudos << "first-child";
                    *specificity += 100;
                } else {
                    compound.unsupported = true;
                }
                continue;
            }
            static const QStringList supported = {"first-child", "last-child", "only-child", "first-of-type",
                                                  "last-of-type", "root"};
            if (supported.contains(pseudo))
                compound.pseudos << pseudo;
            else if (pseudo == "first-letter" || pseudo == "first-line" || pseudo == "before" || pseudo == "after")
                compound.unsupported = true;    // CSS 2's single-colon pseudo-elements
            else
                compound.unsupported = true;    // :hover, :link and the rest
            *specificity += 100;
        } else {
            return std::nullopt;
        }
    }
    return compound;
}

std::shared_ptr<Selector> parseSelector(const QString &text) {
    // Puts spaces around combinators, so they split like descendant gaps.
    QString spaced;
    int depth = 0;
    for (const QChar c : text) {
        if (c == '(' || c == '[')
            ++depth;
        else if (c == ')' || c == ']')
            --depth;
        if (depth == 0 && (c == '>' || c == '+' || c == '~'))
            spaced += QString(" ") + c + " ";
        else
            spaced += c;
    }
    QStringList tokens;
    for (const QString &token : splitTop(spaced.simplified(), ' '))
        if (!token.isEmpty())
            tokens << token;
    if (tokens.isEmpty())
        return nullptr;
    auto selector = std::make_shared<Selector>();
    QList<Compound> leftToRight;
    QList<QChar> joins;
    QChar pending = ' ';
    for (const QString &token : tokens) {
        if (token == ">" || token == "+" || token == "~") {
            pending = token[0];
            continue;
        }
        auto compound = parseCompound(token, &selector->specificity);
        if (!compound)
            return nullptr;
        if (!leftToRight.isEmpty())
            joins << pending;
        leftToRight << *compound;
        pending = ' ';
    }
    for (qsizetype i = leftToRight.size() - 1; i >= 0; --i)
        selector->parts << leftToRight[i];
    for (qsizetype i = joins.size() - 1; i >= 0; --i)
        selector->combinators << joins[i];
    return selector;
}

bool matchesCompound(const Compound &compound, const Node *node) {
    if (compound.unsupported)
        return false;
    if (!compound.tag.isEmpty() && compound.tag != node->tag)
        return false;
    if (!compound.id.isEmpty() && node->id != compound.id)
        return false;
    for (const QString &name : compound.classes)
        if (!node->classes.contains(name))
            return false;
    for (const AttributeTest &test : compound.attributes) {
        QString value;
        if (!node->hasAttribute(test, &value))
            return false;
        bool ok = true;
        switch (test.op.unicode()) {
        case 0: break;
        case '=': ok = value == test.value; break;
        case '~': ok = value.split(' ', Qt::SkipEmptyParts).contains(test.value); break;
        case '|': ok = value == test.value || value.startsWith(test.value + '-'); break;
        case '^': ok = !test.value.isEmpty() && value.startsWith(test.value); break;
        case '$': ok = !test.value.isEmpty() && value.endsWith(test.value); break;
        case '*': ok = !test.value.isEmpty() && value.contains(test.value); break;
        }
        if (!ok)
            return false;
    }
    for (const QString &pseudo : compound.pseudos) {
        if ((pseudo == "first-child" && node->index != 0) || (pseudo == "last-child" && node->index != node->count - 1)
            || (pseudo == "only-child" && node->count != 1) || (pseudo == "first-of-type" && node->typeIndex != 0)
            || (pseudo == "last-of-type" && node->typeIndex != node->typeCount - 1)
            || (pseudo == "root" && node->parent))
            return false;
    }
    for (const Compound &negation : compound.negations)
        if (matchesCompound(negation, node))
            return false;
    return true;
}

bool matchesFrom(const Selector &selector, int part, const Node *node) {
    if (!matchesCompound(selector.parts[part], node))
        return false;
    if (part + 1 >= selector.parts.size())
        return true;
    switch (selector.combinators[part].unicode()) {
    case '>':
        return node->parent && matchesFrom(selector, part + 1, node->parent);
    case '+':
        return node->previous && matchesFrom(selector, part + 1, node->previous);
    case '~':
        for (const Node *sibling = node->previous; sibling; sibling = sibling->previous)
            if (matchesFrom(selector, part + 1, sibling))
                return true;
        return false;
    default:
        for (const Node *ancestor = node->parent; ancestor; ancestor = ancestor->parent)
            if (matchesFrom(selector, part + 1, ancestor))
                return true;
        return false;
    }
}

QList<Declaration> parseDeclarations(const QString &block) {
    QList<Declaration> out;
    for (const QString &part : splitTop(block, ';')) {
        const qsizetype colon = part.indexOf(':');
        if (colon <= 0)
            continue;
        Declaration declaration{part.left(colon).trimmed().toLower(), part.mid(colon + 1).trimmed(), false};
        static const QRegularExpression important("\\s*!\\s*important\\s*$", QRegularExpression::CaseInsensitiveOption);
        if (declaration.value.contains(important)) {
            declaration.value.remove(important);
            declaration.important = true;
        }
        if (!declaration.property.isEmpty() && !declaration.value.isEmpty())
            out << declaration;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Values

// A length in em of the element's own text, or of the root's for rem. Percentages
// and auto have no meaning without a width here, so they are left out.
std::optional<double> length(const QString &value, double *rootEm = nullptr) {
    static const QRegularExpression pattern("^([-+]?(?:\\d+\\.?\\d*|\\.\\d+))\\s*(em|rem|ex|ch|px|pt|pc|in|cm|mm)?$",
                                            QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(value.trimmed());
    if (!match.hasMatch())
        return std::nullopt;
    const double number = match.captured(1).toDouble();
    const QString unit = match.captured(2).toLower();
    if (rootEm)
        *rootEm = 0;
    if (unit == "em")
        return number;
    if (unit == "rem") {
        if (rootEm)
            *rootEm = number;
        return number;
    }
    if (unit == "ex" || unit == "ch")
        return number / 2;
    if (unit == "px")
        return number / 16;
    if (unit == "pt")
        return number / 12;
    if (unit == "pc")
        return number;
    if (unit == "in")
        return number * 6;
    if (unit == "cm")
        return number * 6 / 2.54;
    if (unit == "mm")
        return number * 0.6 / 2.54;
    return number == 0 ? std::optional<double>(0) : std::nullopt;
}

// A font size relative to the parent's.
std::optional<double> fontScale(const QString &value) {
    const QString v = value.trimmed().toLower();
    static const QHash<QString, double> keywords = {
        {"xx-small", 0.6}, {"x-small", 0.75}, {"small", 0.89}, {"medium", 1.0},   {"large", 1.2},
        {"x-large", 1.5},  {"xx-large", 2.0}, {"smaller", 0.83}, {"larger", 1.2}, {"inherit", 1.0}};
    if (keywords.contains(v))
        return keywords.value(v);
    if (v.endsWith('%')) {
        bool ok = false;
        const double percent = v.chopped(1).toDouble(&ok);
        return ok ? std::optional<double>(percent / 100) : std::nullopt;
    }
    return length(v);
}

QStringList values(const QString &value) { return value.simplified().split(' ', Qt::SkipEmptyParts); }

// The four sides of a margin or padding shorthand.
QStringList sides(const QString &value) {
    const QStringList v = values(value);
    switch (v.size()) {
    case 1: return {v[0], v[0], v[0], v[0]};
    case 2: return {v[0], v[1], v[0], v[1]};
    case 3: return {v[0], v[1], v[2], v[1]};
    case 4: return v;
    default: return {};
    }
}

struct Computed {
    QHash<QString, QString> declared;   // the cascade's winners for this element
    double fontScale = 1;               // relative to the reading text size
    QString textAlign, textIndent;      // inherited, as CSS inherits them
    double indentEm = 0;
    bool hasIndent = false;
};

const QStringList &blockTags() {
    static const QStringList tags = {"p",  "div", "h1",  "h2", "h3", "h4", "h5", "h6", "blockquote", "li", "ul",
                                     "ol", "dl",  "dt",  "dd", "pre", "table", "tr", "td", "th", "hr", "center"};
    return tags;
}

// Tags Qt doesn't know that are blocks in a browser.
const QStringList &sectioningTags() {
    static const QStringList tags = {"section", "article", "header", "footer", "aside", "nav", "main", "figure",
                                     "figcaption", "hgroup", "address", "details", "summary", "body"};
    return tags;
}

const QStringList &voidTags() {
    static const QStringList tags = {"br", "hr", "img", "wbr", "col", "area", "source", "image"};
    return tags;
}

QString number(double value) {
    QString text = QString::number(std::round(value * 1000) / 1000, 'f', 3);
    while (text.endsWith('0'))
        text.chop(1);
    if (text.endsWith('.'))
        text.chop(1);
    return text == "-0" ? "0" : text;
}

QString escapeText(const QString &text) {
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out += c;
    }
    return out;
}

// What an element is, by EPUB 3's epub:type and ARIA's role.
QStringList kinds(const Node *node) {
    return (node->attribute("epub:type") + ' ' + node->attribute("role")).split(' ', Qt::SkipEmptyParts);
}

// Text a book keeps for screen readers but hides from the eye: invisible, or
// moved far outside the page, as headings over a title page often are.
bool isHidden(const QHash<QString, QString> &declared) {
    const QString visibility = declared.value("visibility").toLower();
    if (visibility == "hidden" || visibility == "collapse")
        return true;
    const QString position = declared.value("position").toLower();
    if (position != "absolute" && position != "fixed")
        return false;
    for (const QString edge : {"left", "top", "right"}) {
        const auto em = length(declared.value(edge));
        if (em && *em <= -20)
            return true;
    }
    return false;
}

} // namespace

class Rules {
public:
    struct Matched {
        const Selector *selector;
        const QList<Declaration> *declarations;
        int order;
    };
    // Every rule, and by the last part of its selector: an id, a class, a
    // tag, or anything at all.
    std::vector<Matched> all;
    QHash<QString, QList<int>> byId, byClass, byTag;
    QList<int> anything;
    // The sheets they came from, kept alive as long as the rules are.
    QList<const Sheet *> sheets;

    // The rules that could match an element, in no particular order: the
    // cascade orders them itself.
    QList<int> candidates(const Node *node) const {
        QList<int> out = anything;
        out += byTag.value(node->tag);
        if (!node->id.isEmpty())
            out += byId.value(node->id);
        for (const QString &name : node->classes)
            out += byClass.value(name);
        return out;
    }
};

namespace {

class Restyler {
public:
    explicit Restyler(const Rules &rules) : m_rules(rules) {}

    QString render(const Node *body, QHash<QString, QString> *notes) {
        Computed root;
        QString out;
        for (const auto &child : body->children)
            if (auto styled = build(child.get(), root)) {
                moveMargins(styled.get());
                write(styled.get(), &out);
                if (notes)
                    collectNotes(styled.get(), notes);
            }
        return out;
    }

private:
    const Rules &m_rules;
    // Each element's cascade, worked out once.
    QHash<const Node *, QHash<QString, QString>> m_declared;

    QHash<QString, QString> cascade(const Node *node) {
        const auto known = m_declared.constFind(node);
        if (known != m_declared.cend())
            return *known;
        const QHash<QString, QString> found = computeCascade(node);
        m_declared.insert(node, found);
        return found;
    }

    QHash<QString, QString> computeCascade(const Node *node) const {
        struct Winner {
            bool important;
            int specificity;
            int order;
            QString value;
        };
        QHash<QString, Winner> winners;
        auto offer = [&](const QString &property, const QString &value, bool important, int specificity, int order) {
            const auto it = winners.constFind(property);
            if (it != winners.cend()) {
                const Winner &w = *it;
                if (std::tie(w.important, w.specificity, w.order) > std::tie(important, specificity, order))
                    return;
            }
            winners.insert(property, {important, specificity, order, value});
        };
        auto offerExpanded = [&](const Declaration &d, int specificity, int order) {
            if (d.property == "margin" || d.property == "padding") {
                const QStringList four = sides(d.value);
                static const QStringList names = {"-top", "-right", "-bottom", "-left"};
                for (int i = 0; i < four.size(); ++i)
                    offer(d.property + names[i], four[i], d.important, specificity, order);
            } else if (d.property == "font-variant-caps") {
                offer("font-variant", d.value, d.important, specificity, order);
            } else if (d.property == "text-decoration-line") {
                offer("text-decoration", d.value, d.important, specificity, order);
            } else if (d.property == "font") {
                // Only the parts that shape text: style, weight and small caps.
                for (const QString &part : values(d.value)) {
                    const QString p = part.toLower();
                    if (p == "italic" || p == "oblique")
                        offer("font-style", p, d.important, specificity, order);
                    else if (p == "bold" || p == "bolder")
                        offer("font-weight", p, d.important, specificity, order);
                    else if (p == "small-caps")
                        offer("font-variant", p, d.important, specificity, order);
                }
            } else {
                offer(d.property, d.value, d.important, specificity, order);
            }
        };
        QList<int> tried = m_rules.candidates(node);
        // A rule listed under two of the element's classes is tried once.
        std::sort(tried.begin(), tried.end());
        tried.erase(std::unique(tried.begin(), tried.end()), tried.end());
        for (const int index : std::as_const(tried)) {
            const Rules::Matched &rule = m_rules.all[index];
            if (!matchesFrom(*rule.selector, 0, node))
                continue;
            for (const Declaration &d : *rule.declarations)
                offerExpanded(d, rule.selector->specificity, rule.order);
        }
        // The element's own style attribute outranks every rule.
        for (const Declaration &d : parseDeclarations(node->attribute("style")))
            offerExpanded(d, 1000000, 1000000);
        QHash<QString, QString> out;
        for (auto it = winners.cbegin(); it != winners.cend(); ++it)
            out.insert(it.key(), it->value.trimmed());
        return out;
    }

    // An element as it will be written: its tag for Qt, its style, and its
    // top and bottom margins apart, so they can move to the blocks inside it.
    struct Styled {
        const Node *node = nullptr;     // null for text
        QString text;
        QString tag;
        QStringList style;
        std::optional<double> top, bottom;
        bool block = false;
        QString hrWidth;
        std::vector<std::unique_ptr<Styled>> children;

        bool blank() const { return !node && text.trimmed().isEmpty(); }
    };

    std::unique_ptr<Styled> build(const Node *node, const Computed &parent) {
        auto styled = std::make_unique<Styled>();
        if (node->isText()) {
            styled->text = node->text;
            return styled;
        }
        static const QStringList skipped = {"script", "style", "head", "title", "link", "meta", "noscript"};
        if (skipped.contains(node->tag))
            return nullptr;
        Computed self;
        self.declared = cascade(node);
        const QString display = self.declared.value("display").toLower();
        if (display == "none" || isHidden(self.declared))
            return nullptr;
        styled->node = node;

        QString tag = node->tag;
        if (sectioningTags().contains(tag) || ((display == "block" || display == "list-item") && !blockTags().contains(tag)
                                                && tag != "img" && tag != "svg" && tag != "image"))
            tag = "div";
        styled->tag = tag;
        styled->block = blockTags().contains(tag);
        QStringList &style = styled->style;

        // A note's number is raised, as every reader sets it, at the size Qt
        // gives a superscript, which the book's own smaller size would shrink
        // again past reading; an underline beneath it reads as a stray line.
        const bool noteref = tag == "a" && (kinds(node).contains("noteref") || kinds(node).contains("doc-noteref"));
        if (noteref) {
            self.declared.remove("font-size");
            self.declared.insert("text-decoration", "none");
            if (!self.declared.contains("vertical-align"))
                self.declared.insert("vertical-align", "super");
        }

        // Text size, relative to the reading size, so it scales with it.
        self.fontScale = parent.fontScale;
        if (self.declared.contains("font-size"))
            if (const auto scale = fontScale(self.declared.value("font-size")); scale && *scale > 0)
                self.fontScale = std::clamp(parent.fontScale * *scale, 0.5, 3.0);
        if (!qFuzzyCompare(self.fontScale, parent.fontScale))
            style << "font-size: " + number(self.fontScale) + "qe";

        auto emLength = [&](const QString &value, double lo, double hi) -> std::optional<double> {
            double rem = 0;
            const auto em = length(value, &rem);
            if (!em)
                return std::nullopt;
            return std::clamp(rem != 0 ? rem : *em * self.fontScale, lo, hi);
        };

        // Inherited as CSS inherits them, and written on every block, since
        // Qt's blocks don't take them from the block around them.
        self.textAlign = parent.textAlign;
        if (const QString align = self.declared.value("text-align").toLower(); !align.isEmpty()) {
            static const QHash<QString, QString> aligns = {{"left", "left"},   {"start", "left"},     {"right", "right"},
                                                           {"end", "right"},   {"center", "center"},  {"justify", "justify"},
                                                           {"-webkit-center", "center"}};
            if (aligns.contains(align))
                self.textAlign = aligns.value(align);
        }
        self.hasIndent = parent.hasIndent;
        self.indentEm = parent.indentEm;
        if (self.declared.contains("text-indent"))
            if (const auto em = emLength(self.declared.value("text-indent"), -6, 6)) {
                self.indentEm = *em;
                self.hasIndent = true;
            }
        if (styled->block) {
            if (!self.textAlign.isEmpty())
                style << "text-align: " + self.textAlign;
            if (self.hasIndent)
                style << "text-indent: " + number(self.indentEm) + "qe";
            // Margins, with padding added in: Qt has no padding on a paragraph,
            // and a poem's indented lines are often written as padding.
            // Lists are indented by Qt, a level at a time; the book's own
            // indents on top of that pile up until a deep list is a column a
            // letter wide.
            static const QStringList lists = {"ul", "ol", "li", "dl", "dd"};
            const bool list = lists.contains(tag);
            for (const QString edge : {"top", "right", "bottom", "left"}) {
                if (list && (edge == QLatin1String("left") || edge == QLatin1String("right")))
                    continue;
                std::optional<double> total;
                if (self.declared.contains(QString("margin-") + edge))
                    total = emLength(self.declared.value(QString("margin-") + edge), -10, 12);
                if (self.declared.contains(QString("padding-") + edge))
                    if (const auto padding = emLength(self.declared.value(QString("padding-") + edge), 0, 12))
                        total = total.value_or(0) + *padding;
                if (!total)
                    continue;
                if (edge == QLatin1String("top"))
                    styled->top = total;
                else if (edge == QLatin1String("bottom"))
                    styled->bottom = total;
                else
                    style << QString("margin-") + edge + ": " + number(*total) + "qe";
            }
        }

        const QString fontStyle = self.declared.value("font-style").toLower();
        if (fontStyle == "italic" || fontStyle == "oblique")
            style << "font-style: italic";
        else if (fontStyle == "normal")
            style << "font-style: normal";
        const QString weight = self.declared.value("font-weight").toLower();
        if (!weight.isEmpty()) {
            bool numeric = false;
            const int w = weight.toInt(&numeric);
            const bool bold = weight == "bold" || weight == "bolder" || (numeric && w >= 600);
            const bool normal = weight == "normal" || weight == "lighter" || (numeric && w < 600);
            if (bold || normal)
                style << QString("font-weight: ") + (bold ? "bold" : "normal");
        }
        const QString variant = self.declared.value("font-variant").toLower();
        if (variant.contains("small-caps"))
            style << "font-variant: small-caps";
        else if (variant == "normal")
            style << "font-variant: normal";
        const QString transform = self.declared.value("text-transform").toLower();
        if (transform == "uppercase" || transform == "lowercase" || transform == "capitalize" || transform == "none")
            style << "text-transform: " + transform;
        const QString decoration = self.declared.value("text-decoration").toLower();
        if (decoration.contains("underline"))
            style << "text-decoration: underline";
        else if (decoration.contains("line-through"))
            style << "text-decoration: line-through";
        else if (decoration == "none")
            style << "text-decoration: none";
        const QString vertical = self.declared.value("vertical-align").toLower();
        if (vertical == "super" || vertical == "sub")
            style << "vertical-align: " + vertical;
        const QString space = self.declared.value("white-space").toLower();
        if (space == "pre" || space == "pre-wrap" || space == "pre-line")
            style << "white-space: pre-wrap";
        // A scene break drawn short, as a book sets it, rather than across the page.
        if (tag == "hr") {
            const QString width = self.declared.value("width").trimmed();
            if (width.endsWith('%'))
                styled->hrWidth = width;
        }

        for (const auto &child : node->children)
            if (auto built = build(child.get(), self))
                styled->children.push_back(std::move(built));

        // Qt Quick draws an image alone on a centered line at the line's
        // start, so such a block is centered by a left margin instead, which
        // Book::fitImages() works out once the column's width is known.
        if (styled->block && self.textAlign == "center") {
            int images = 0, others = 0;
            for (const auto &child : styled->children)
                if (!child->blank())
                    ++(child->tag == "img" ? images : others);
            if (images == 1 && others == 0)
                style << "margin-left: centered";
        }
        return styled;
    }

    // Qt gives a block's top margin to the first block inside it, and drops
    // its bottom margin altogether, so the space after an epigraph or a poem
    // was lost. Both move to the blocks inside, as the larger of the two, the
    // way adjoining margins collapse in CSS.
    static void moveMargins(Styled *styled) {
        auto edgeChild = [&](bool first) -> Styled * {
            auto &children = styled->children;
            if (first) {
                for (auto &child : children)
                    if (!child->blank())
                        return child->block ? child.get() : nullptr;
            } else {
                for (auto it = children.rbegin(); it != children.rend(); ++it)
                    if (!(*it)->blank())
                        return (*it)->block ? it->get() : nullptr;
            }
            return nullptr;
        };
        if (styled->block) {
            if (Styled *first = edgeChild(true); first && styled->top) {
                first->top = std::max(first->top.value_or(0), *styled->top);
                styled->top.reset();
            }
            if (Styled *last = edgeChild(false); last && styled->bottom) {
                last->bottom = std::max(last->bottom.value_or(0), *styled->bottom);
                styled->bottom.reset();
            }
        }
        for (auto &child : styled->children)
            moveMargins(child.get());
    }

    // A footnote or endnote, as EPUB 3 marks them, or as ARIA's roles do.
    static bool isNote(const Node *node) {
        static const QStringList notes = {"footnote", "endnote", "rearnote", "note", "doc-footnote", "doc-endnote"};
        if (node->attribute("id").isEmpty())
            return false;
        for (const QString &kind : kinds(node))
            if (notes.contains(kind))
                return true;
        return false;
    }

    static void collectNotes(const Styled *styled, QHash<QString, QString> *notes) {
        if (!styled->node)
            return;
        if (isNote(styled->node)) {
            QString html;
            for (const auto &child : styled->children)
                write(child.get(), &html, true);
            notes->insert(styled->node->attribute("id"), html.trimmed());
            return;
        }
        for (const auto &child : styled->children)
            collectNotes(child.get(), notes);
    }

    // In a note shown in place, the link back to where it was referred to
    // leads nowhere useful, so it's left out.
    static void write(const Styled *styled, QString *out, bool inNote = false) {
        if (inNote && styled->node) {
            const QStringList k = kinds(styled->node);
            if (k.contains("backlink") || k.contains("doc-backlink"))
                return;
        }
        if (!styled->node) {
            *out += escapeText(styled->text);
            return;
        }
        *out += '<' + styled->tag;
        for (const auto &[name, value] : styled->node->attributes) {
            if (name == "class" || name == "style" || name.startsWith("xmlns"))
                continue;
            *out += ' ' + name + "=\"" + escapeText(value).replace('"', "&quot;") + '"';
        }
        if (!styled->hrWidth.isEmpty())
            *out += " width=\"" + styled->hrWidth + '"';
        QStringList style = styled->style;
        if (styled->top)
            style << "margin-top: " + number(*styled->top) + "qe";
        if (styled->bottom)
            style << "margin-bottom: " + number(*styled->bottom) + "qe";
        if (!style.isEmpty())
            *out += " style=\"" + style.join("; ") + '"';
        if (voidTags().contains(styled->tag) && styled->children.empty()) {
            *out += " />";
            return;
        }
        *out += '>';
        for (const auto &child : styled->children)
            write(child.get(), out, inNote);
        *out += "</" + styled->tag + '>';
    }
};

void index(Node *node) {
    std::vector<Node *> elements;
    for (const auto &child : node->children)
        if (!child->isText())
            elements.push_back(child.get());
    QHash<QString, int> typeCounts;
    for (Node *element : elements)
        ++typeCounts[element->tag];
    QHash<QString, int> typeSeen;
    Node *previous = nullptr;
    for (int i = 0; i < int(elements.size()); ++i) {
        Node *element = elements[i];
        element->index = i;
        element->count = int(elements.size());
        element->typeIndex = typeSeen[element->tag]++;
        element->typeCount = typeCounts.value(element->tag);
        element->previous = previous;
        previous = element;
        index(element);
    }
}

} // namespace

Sheet Sheet::parse(const QString &source) {
    Sheet sheet;
    const QString css = stripComments(source);
    int order = 0;
    // Walks the text, taking rules at the top level and inside @supports;
    // @media, @font-face, @page and the rest are left out.
    std::function<void(const QString &)> walk = [&](const QString &text) {
        int i = 0;
        const int n = int(text.size());
        while (i < n) {
            const int open = int(text.indexOf('{', i));
            if (open < 0)
                break;
            QString prelude = text.mid(i, open - i).trimmed();
            // A statement at-rule (@charset, @import, @namespace) ends at ';'.
            while (prelude.startsWith('@') && prelude.contains(';')) {
                prelude = prelude.mid(prelude.indexOf(';') + 1).trimmed();
            }
            int depth = 1, j = open + 1;
            for (; j < n && depth > 0; ++j) {
                if (text[j] == '{')
                    ++depth;
                else if (text[j] == '}')
                    --depth;
            }
            const QString body = text.mid(open + 1, j - open - 2);
            i = j;
            if (prelude.startsWith('@')) {
                if (prelude.startsWith("@supports", Qt::CaseInsensitive))
                    walk(body);
                continue;
            }
            Rule rule;
            rule.order = order++;
            for (const QString &part : splitTop(prelude, ','))
                if (auto selector = parseSelector(part.trimmed()))
                    rule.selectors << selector;
            rule.declarations = parseDeclarations(body);
            if (!rule.selectors.isEmpty() && !rule.declarations.isEmpty())
                sheet.rules << rule;
        }
    };
    walk(css);
    return sheet;
}

void linkedSheets(const QByteArray &xhtml, QStringList *hrefs, QStringList *inline_) {
    QXmlStreamReader xml(xhtml);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        if (xml.name() == u"body")
            return;
        if (xml.name() == u"link") {
            const auto a = xml.attributes();
            if (a.value("rel").toString().toLower().split(' ').contains("stylesheet") && !a.value("href").isEmpty())
                *hrefs << a.value("href").toString();
        } else if (xml.name() == u"style") {
            *inline_ << xml.readElementText();
        }
    }
}

std::shared_ptr<const Rules> compile(const QList<const Sheet *> &sheets) {
    auto rules = std::make_shared<Rules>();
    rules->sheets = sheets;
    int order = 0;
    for (const Sheet *sheet : sheets)
        for (const Rule &rule : sheet->rules) {
            for (const auto &selector : rule.selectors) {
                const int index = int(rules->all.size());
                rules->all.push_back({selector.get(), &rule.declarations, order});
                const Compound &last = selector->parts.first();
                if (!last.id.isEmpty())
                    rules->byId[last.id] << index;
                else if (!last.classes.isEmpty())
                    rules->byClass[last.classes.first()] << index;
                else if (!last.tag.isEmpty())
                    rules->byTag[last.tag] << index;
                else
                    rules->anything << index;
            }
            ++order;
        }
    return rules;
}

QString apply(const QByteArray &xhtml, const QList<const Sheet *> &sheets, QHash<QString, QString> *notes) {
    return apply(xhtml, *compile(sheets), notes);
}

QString apply(const QByteArray &xhtml, const Rules &rules, QHash<QString, QString> *notes) {
    QXmlStreamReader xml(xhtml);
    Node root;
    Node *current = &root;
    Node *body = nullptr;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            auto node = std::make_unique<Node>();
            node->tag = xml.name().toString().toLower();
            for (const auto &a : xml.attributes())
                node->attributes.push_back({a.qualifiedName().toString(), a.value().toString()});
            node->id = node->attribute("id");
            node->classes = node->attribute("class").split(' ', Qt::SkipEmptyParts);
            node->parent = current;
            Node *raw = node.get();
            current->children.push_back(std::move(node));
            current = raw;
            if (raw->tag == "body" && !body)
                body = raw;
        } else if (token == QXmlStreamReader::EndElement) {
            if (current->parent)
                current = current->parent;
        } else if (token == QXmlStreamReader::Characters && !xml.isWhitespace()) {
            auto text = std::make_unique<Node>();
            text->text = xml.text().toString();
            text->parent = current;
            current->children.push_back(std::move(text));
        } else if (token == QXmlStreamReader::Characters) {
            // Whitespace between blocks stays a single space, as HTML has it.
            auto text = std::make_unique<Node>();
            text->text = " ";
            text->parent = current;
            current->children.push_back(std::move(text));
        }
    }
    if (xml.hasError() || !body)
        return {};
    index(&root);
    return Restyler(rules).render(body, notes).trimmed();
}

QString sized(const QString &html, int pixelSize) {
    static const QRegularExpression style(" style=\"[^\"]*qe[^\"]*\"");
    static const QRegularExpression unit("(-?\\d+(?:\\.\\d+)?)qe\\b");
    QString out;
    qsizetype last = 0;
    for (auto it = style.globalMatch(html); it.hasNext();) {
        const auto match = it.next();
        QString attribute = match.captured();
        QString replaced;
        qsizetype at = 0;
        for (auto u = unit.globalMatch(attribute); u.hasNext();) {
            const auto m = u.next();
            replaced += attribute.mid(at, m.capturedStart() - at)
                        + QString::number(std::lround(m.captured(1).toDouble() * pixelSize)) + "px";
            at = m.capturedEnd();
        }
        replaced += attribute.mid(at);
        out += html.mid(last, match.capturedStart() - last) + replaced;
        last = match.capturedEnd();
    }
    return out + html.mid(last);
}

} // namespace Styling
