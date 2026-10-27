// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/coordination.h"
#include "editing/library.h"
#include "model/i18n.h"
#include "ui/main_window.h"
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDirIterator>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QtTest>
using namespace mf;
namespace {
bool japanese(const QString &text) {
    static const QRegularExpression pattern("[\\x{3000}-\\x{30ff}\\x{4e00}-\\x{9fff}\\x{ff00}-\\x{ffef}]");
    return text.contains(pattern);
}
QStringList placeholders(const QString &text) {
    static const QRegularExpression pattern("%(?:L?[0-9]{1,2}|n)");
    QStringList found;
    for (const auto &match : pattern.globalMatch(text))
        found << match.captured();
    found.sort();
    return found;
}
struct Token {
    enum Kind { Identifier, String, Punctuation } kind;
    QString text;
    int line;
    bool marked = false;
};
struct Source {
    QString file;
    QList<Token> tokens;
    QSet<int> exempt; // Lines marked "i18n-exempt", or between "i18n-exempt-begin" and "-end".
};
// A tokenizer for the subset of C++ that decides which literals are marked: comments and character
// literals are skipped, string literals (also raw and prefixed ones) are decoded.
Source tokenize(const QString &file, const QString &text) {
    Source source{file, {}, {}};
    int line = 1;
    bool region = false;
    const auto lines = text.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].contains("i18n-exempt-begin"))
            region = true;
        if (region || lines[i].contains("i18n-exempt"))
            source.exempt.insert(i + 1);
        if (lines[i].contains("i18n-exempt-end"))
            region = false;
    }
    const auto identifierChar = [](QChar c) { return c.isLetterOrNumber() || c == '_'; };
    for (qsizetype i = 0; i < text.size();) {
        const QChar c = text[i];
        if (c == '\n') {
            ++line;
            ++i;
        } else if (c.isSpace()) {
            ++i;
        } else if (text.mid(i, 2) == "//") {
            i = text.indexOf('\n', i);
            if (i < 0)
                break;
        } else if (text.mid(i, 2) == "/*") {
            const auto end = text.indexOf("*/", i + 2);
            line += text.mid(i, end - i).count('\n');
            i = end + 2;
        } else if (c == '\'') {
            if (i > 0 && text[i - 1].isLetterOrNumber()) { // A digit separator.
                ++i;
                continue;
            }
            for (++i; i < text.size() && text[i] != '\''; ++i)
                if (text[i] == '\\')
                    ++i;
            ++i;
        } else if (c == '"' || (c == 'R' && text.mid(i, 2) == "R\"")) {
            Token token{Token::String, {}, line};
            if (c == 'R') {
                const auto open = text.indexOf('(', i);
                const auto close = ')' + text.mid(i + 2, open - i - 2) + '"';
                const auto end = text.indexOf(close, open);
                token.text = text.mid(open + 1, end - open - 1);
                line += token.text.count('\n');
                i = end + close.size();
            } else {
                for (++i; i < text.size() && text[i] != '"'; ++i) {
                    if (text[i] != '\\') {
                        token.text += text[i];
                        continue;
                    }
                    const QChar e = text[++i];
                    if (e == 'n')
                        token.text += '\n';
                    else if (e == 't')
                        token.text += '\t';
                    else if (e == 'r')
                        token.text += '\r';
                    else if (e == '0')
                        token.text += QChar(0);
                    else if (e == 'x') {
                        qsizetype end = i + 1;
                        while (end < text.size() && QString("0123456789abcdefABCDEF").contains(text[end]))
                            ++end;
                        token.text += QChar(text.mid(i + 1, end - i - 1).toInt(nullptr, 16));
                        i = end - 1;
                    } else if (e == 'u') {
                        token.text += QChar(text.mid(i + 1, 4).toInt(nullptr, 16));
                        i += 4;
                    } else
                        token.text += e;
                }
                ++i;
            }
            source.tokens << token;
        } else if (identifierChar(c)) {
            const auto start = i;
            while (i < text.size() && identifierChar(text[i]))
                ++i;
            const auto word = text.mid(start, i - start);
            // String prefixes: u8"…", u"…", U"…", L"…" and their raw forms.
            if (i < text.size() && (text[i] == '"' || text.mid(i, 2) == "R\"") &&
                QStringList{"u8", "u", "U", "L"}.contains(word))
                continue;
            source.tokens << Token{Token::Identifier, word, line};
        } else {
            source.tokens << Token{Token::Punctuation, QString(c), line};
            ++i;
        }
    }
    return source;
}
struct Marked {
    QString key, where;
};
// tr("…" "…"[, "disambiguation"…]) and QT_TR_NOOP("…"): the literals of their first argument (and
// the disambiguation of tr) are marked; the key is "text" or "disambiguation|text".
QList<Marked> markedTexts(Source &source) {
    QList<Marked> result;
    auto &t = source.tokens;
    for (qsizetype i = 0; i + 2 < t.size(); ++i) {
        if (t[i].kind != Token::Identifier || (t[i].text != "tr" && t[i].text != "QT_TR_NOOP") ||
            t[i + 1].text != "(" || t[i + 2].kind != Token::String)
            continue;
        QString text;
        qsizetype j = i + 2;
        for (; j < t.size() && t[j].kind == Token::String; ++j) {
            text += t[j].text;
            t[j].marked = true;
        }
        if (t[i].text == "tr" && j + 1 < t.size() && t[j].text == "," && t[j + 1].kind == Token::String) {
            QString disambiguation;
            for (++j; j < t.size() && t[j].kind == Token::String; ++j) {
                disambiguation += t[j].text;
                t[j].marked = true;
            }
            text.prepend(disambiguation + '|');
        }
        result << Marked{text, QString("%1:%2").arg(source.file).arg(t[i].line)};
    }
    return result;
}
QList<Source> sources() {
    QList<Source> result;
    QDirIterator it(SOURCE_DIR "/src", {"*.cpp", "*.h"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto path = it.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        result << tokenize(QDir(SOURCE_DIR).relativeFilePath(path), QString::fromUtf8(file.readAll()));
    }
    return result;
}
QJsonObject catalog() {
    QFile file(SOURCE_DIR "/assets/translations/en.json");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
// The levels of the headings of a Markdown file, outside code blocks.
QString headingLevels(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QString levels;
    bool code = false;
    for (const auto &line : QString::fromUtf8(file.readAll()).split('\n')) {
        code ^= line.startsWith("```");
        if (!code && line.startsWith('#'))
            levels += QString::number(line.indexOf(' '));
    }
    return levels;
}
} // namespace
static void initManual() {
    Q_INIT_RESOURCE(manual);
}
class TranslationsTest : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDir;
  private slots:
    void initTestCase() {
        QVERIFY(settingsDir.isValid());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        qputenv("XDG_CONFIG_HOME", settingsDir.path().toUtf8());
        qputenv("XDG_DATA_HOME", settingsDir.path().toUtf8());
        qputenv("HOME", settingsDir.path().toUtf8()); // No jobs of the user in the history.
    }
    void cleanup() {
        setEnglish(false);
    }
    // Japanese text in the sources is marked for translation, or exempt on purpose.
    void japaneseTextIsMarked() {
        QStringList unmarked;
        for (auto &source : sources()) {
            markedTexts(source);
            for (const auto &token : source.tokens)
                if (token.kind == Token::String && !token.marked && japanese(token.text) &&
                    !source.exempt.contains(token.line))
                    unmarked << QString("%1:%2: %3").arg(source.file).arg(token.line).arg(token.text);
        }
        QVERIFY2(unmarked.isEmpty(), qPrintable("Unmarked Japanese text:\n" + unmarked.join('\n')));
    }
    // Every marked text has an English translation with the same placeholders; the catalog holds
    // no translation that the sources no longer use.
    void catalogMatchesSources() {
        const auto english = catalog();
        QVERIFY(!english.isEmpty());
        QMap<QString, QString> used;
        for (auto &source : sources())
            for (const auto &marked : markedTexts(source))
                used.insert(marked.key, marked.where);
        QStringList problems;
        for (auto it = used.begin(); it != used.end(); ++it) {
            if (!english.contains(it.key())) {
                problems << QString("%1: missing: %2").arg(it.value(), it.key());
                continue;
            }
            const auto value = english[it.key()];
            const auto forms =
                value.isArray() ? value.toVariant().toStringList() : QStringList{value.toString()};
            auto source = it.key();
            source = source.mid(source.indexOf('|') + 1);
            for (const auto &form : forms) {
                if (form.isEmpty() || japanese(form))
                    problems << QString("%1: not English: %2").arg(it.value(), form);
                // A plural's singular may spell out the count.
                if (placeholders(form) != placeholders(source) &&
                    !(forms.size() == 2 && form == forms.first() &&
                      placeholders(form) ==
                          QStringList(placeholders(source)).filter(QRegularExpression("^%[^n]"))))
                    problems << QString("%1: placeholders differ: %2 -> %3").arg(it.value(), source, form);
            }
            if (value.isArray() && (forms.size() != 2 || !source.contains("%n")))
                problems << QString("%1: plural forms need %n and [one, other]: %2").arg(it.value(), source);
        }
        for (const auto &key : english.keys())
            if (!used.contains(key))
                problems << "unused: " + key;
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }
    void englishCatalog() {
        QCOMPARE(tr("原子"), QString("原子"));
        QVERIFY(!english());
        setEnglish(true);
        QVERIFY(english());
        QCOMPARE(tr("原子"), QString("Atom"));
        QCOMPARE(translated("原子"), QString("Atom"));
        QCOMPARE(element("O").name(), QString("Oxygen"));
        QCOMPARE(templateName(findTemplate("skeleton:water")), QString("Water"));
        setEnglish(false);
        QCOMPARE(tr("原子"), QString("原子"));
        QCOMPARE(element("O").name(), QString("酸素"));
        QCOMPARE(templateName(findTemplate("skeleton:water")), QString("水"));
    }
    // The English manuals, shown by the English interface, follow the Japanese ones section by section.
    void englishManual() {
        initManual();
        QStringList names{"features.md", "mcp.md", "recipes.md"};
        for (const auto &page : QDir(":/docs/features").entryList({"*.md"}))
            names << "features/" + page;
        for (const auto &recipe : QDir(":/docs/recipes").entryList({"*.md"}))
            names << "recipes/" + recipe;
        QCOMPARE(names.size(), 3 + 12 + 16);
        for (const auto &name : std::as_const(names)) {
            QVERIFY2(QFile::exists(":/docs/en/" + name), qPrintable(name));
            const auto japaneseLevels = headingLevels(":/docs/" + name);
            QVERIFY(!japaneseLevels.isEmpty());
            QCOMPARE(headingLevels(":/docs/en/" + name), japaneseLevels);
        }
        // Every page links and shows only what is embedded: other pages and the images beside them.
        static const QRegularExpression link(R"(\]\(([^)#]+)(#[^)]*)?\))");
        for (const auto &language : {QString(":/docs/"), QString(":/docs/en/")})
            for (const auto &name : std::as_const(names)) {
                QFile file(language + name);
                QVERIFY(file.open(QIODevice::ReadOnly));
                const auto text = QString::fromUtf8(file.readAll());
                // The pages are also the Jekyll site of GitHub Pages, where these start Liquid tags.
                QVERIFY2(!text.contains("{{") && !text.contains("{%"), qPrintable(language + name));
                for (auto it = link.globalMatch(text); it.hasNext();) {
                    const auto target = it.next().captured(1);
                    if (target.startsWith("http"))
                        continue;
                    const auto path = QFileInfo(language + name).dir().filePath(target);
                    QVERIFY2(QFile::exists(QDir::cleanPath(path)),
                             qPrintable(language + name + ": " + target));
                }
            }
    }
    // Built in English, every tab and page shows no Japanese: catches texts that are marked but
    // not passed through tr()/translated() where shown, and Japanese names of elements or parts.
    void englishWindow() {
        setEnglish(true);
        MainWindow w;
        w.loadPath(FIXTURE_DIR "/ala_gly_ser.kgui.json");
        w.show();
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        auto *pages = w.findChild<QTabWidget *>("propertyTabs");
        QVERIFY(tabs && pages);
        QStringList found;
        const auto check = [&](const QObject *o, const QString &what, const QString &text) {
            if (japanese(text))
                found << QString("%1 %2 (%3): %4")
                             .arg(o->metaObject()->className(), o->objectName(), what, text.left(120));
        };
        const auto scan = [&] {
            for (auto *a : w.findChildren<QAction *>()) {
                check(a, "text", a->text());
                check(a, "toolTip", a->toolTip());
            }
            for (auto *widget : w.findChildren<QWidget *>()) {
                check(widget, "toolTip", widget->toolTip());
                check(widget, "accessibleName", widget->accessibleName());
                if (auto *b = qobject_cast<QAbstractButton *>(widget))
                    check(b, "text", b->text());
                else if (auto *l = qobject_cast<QLabel *>(widget))
                    check(l, "text", l->text());
                else if (auto *e = qobject_cast<QLineEdit *>(widget))
                    check(e, "placeholder", e->placeholderText());
                else if (auto *c = qobject_cast<QComboBox *>(widget))
                    for (int i = 0; i < c->count(); ++i)
                        check(c, "item", c->itemText(i));
                else if (auto *t = qobject_cast<QTabWidget *>(widget))
                    for (int i = 0; i < t->count(); ++i)
                        check(t, "tab", t->tabText(i));
                else if (auto *v = qobject_cast<QAbstractItemView *>(widget); v && v->model()) {
                    const auto *m = v->model();
                    for (int c = 0; c < m->columnCount(); ++c)
                        check(v, "header", m->headerData(c, Qt::Horizontal).toString());
                    for (int r = 0; r < std::min(m->rowCount(), 200); ++r)
                        for (int c = 0; c < m->columnCount(); ++c) {
                            check(v, "cell", m->index(r, c).data().toString());
                            check(v, "cell toolTip", m->index(r, c).data(Qt::ToolTipRole).toString());
                        }
                }
            }
        };
        for (int i = 0; i < tabs->count(); ++i) {
            tabs->setCurrentIndex(i);
            QTest::qWait(50);
            scan();
        }
        tabs->setCurrentIndex(0);
        for (int i = 0; i < pages->count(); ++i) {
            pages->setCurrentIndex(i);
            QTest::qWait(20);
            scan();
        }
        found.removeDuplicates();
        QVERIFY2(found.isEmpty(), qPrintable("Japanese in the English window:\n" + found.join('\n')));
        // Longer English text must not widen the right-hand pages beyond their column (the page would be
        // cut off at the right, as there is no horizontal scroll bar); a center brings the longest labels.
        w.edit("配位中心を配置",
               [](Document &d) { addCenter(d, "Na", "tetrahedral", Vec(8, 0, 0), 2.4, 1); });
        w.setSelection({w.document().atoms.last().id});
        for (int i = 0; i < pages->count(); ++i) {
            pages->setCurrentIndex(i);
            QTest::qWait(20);
            auto *scroll = qobject_cast<QScrollArea *>(pages->widget(i));
            QVERIFY(scroll && scroll->widget());
            QVERIFY2(scroll->widget()->minimumSizeHint().width() <= scroll->viewport()->width(),
                     qPrintable(QString("page %1 needs %2 px of %3")
                                    .arg(i)
                                    .arg(scroll->widget()->minimumSizeHint().width())
                                    .arg(scroll->viewport()->width())));
        }
        // The longer English buttons of the selection card wrap into a second row: the card grows with
        // them, and the check boxes below do not cover the wrapped button.
        pages->setCurrentIndex(0);
        QTest::qWait(20);
        auto *card = w.findChild<QFrame *>("selectionCard");
        auto *refine = w.findChild<QWidget *>("refineSelectionButton");
        auto *keep = w.findChild<QWidget *>("keepSelection");
        QVERIFY(card && refine && keep);
        QVERIFY(refine->y() > w.findChild<QWidget *>("clearSelection")->y()); // Wrapped.
        QVERIFY2(refine->geometry().bottom() < keep->y(),
                 qPrintable(QString("button ends at %1, check box starts at %2")
                                .arg(refine->geometry().bottom())
                                .arg(keep->y())));
        QVERIFY(w.findChild<QWidget *>("deleteTerminate")->geometry().bottom() < card->height());
    }
};
QTEST_MAIN(TranslationsTest)
#include "test_translations.moc"
