// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/translations.h"
#include <QCoreApplication>
#include <QHash>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

namespace mf {
namespace {
// The standard buttons of message boxes, input and file dialogs (QPlatformTheme) and the help
// options of the command line, used only for texts that no installed Qt translation provides.
class Fallback : public QTranslator {
  public:
    using QTranslator::QTranslator;
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char *context, const char *source, const char *, int) const override {
        static const QHash<QString, QString> texts{
            {"QPlatformTheme|OK", "OK"},
            {"QPlatformTheme|Save", "保存"},
            {"QPlatformTheme|Save All", "すべて保存"},
            {"QPlatformTheme|Open", "開く"},
            {"QPlatformTheme|&Yes", "はい(&Y)"},
            {"QPlatformTheme|Yes to &All", "すべてはい(&A)"},
            {"QPlatformTheme|&No", "いいえ(&N)"},
            {"QPlatformTheme|N&o to All", "すべていいえ(&O)"},
            {"QPlatformTheme|Abort", "中止"},
            {"QPlatformTheme|Retry", "再試行"},
            {"QPlatformTheme|Ignore", "無視"},
            {"QPlatformTheme|Close", "閉じる"},
            {"QPlatformTheme|Cancel", "キャンセル"},
            {"QPlatformTheme|Discard", "変更を破棄"},
            {"QPlatformTheme|Help", "ヘルプ"},
            {"QPlatformTheme|Apply", "適用"},
            {"QPlatformTheme|Reset", "リセット"},
            {"QPlatformTheme|Restore Defaults", "既定に戻す"},
            {"QCommandLineParser|Displays help on commandline options.", "オプションの説明を表示する。"},
            {"QCommandLineParser|Displays help, including generic Qt options.",
             "Qtの共通オプションを含めて説明を表示する。"},
            {"QCommandLineParser|Displays version information.", "バージョン情報を表示する。"}};
        return texts.value(QString::fromUtf8(context) + '|' + QString::fromUtf8(source));
    }
};
} // namespace
void installTranslations(QCoreApplication &app) {
    // Qt asks the last installed translator first: Qt's own translation, then the fallback.
    app.installTranslator(new Fallback(&app));
    auto *qt = new QTranslator(&app);
    if (qt->load(QLocale(QLocale::Japanese, QLocale::Japan), "qtbase", "_",
                 QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(qt);
    else
        delete qt;
}
} // namespace mf
