// SPDX-License-Identifier: GPL-2.0-or-later
#include "model/i18n.h"
#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTranslator>

static void initTranslationResources() {
    Q_INIT_RESOURCE(resources);
}
namespace mf {
namespace {
// A text with disambiguation is listed as "disambiguation|text"; a plural one as [one, other].
// Other texts keep their source: Qt's own texts stay English even where the platform theme has
// installed Qt's translation for the system language.
class Catalog : public QTranslator {
  public:
    using QTranslator::QTranslator;
    bool isEmpty() const override {
        return texts().isEmpty();
    }
    QString translate(const char *, const char *source, const char *disambiguation, int n) const override {
        auto key = QString::fromUtf8(source);
        if (disambiguation && *disambiguation)
            key.prepend(QString::fromUtf8(disambiguation) + '|');
        const auto forms = texts().value(key);
        return forms.isEmpty()               ? QString::fromUtf8(source)
               : n == 1 || forms.size() == 1 ? forms.first()
                                             : forms.last();
    }

  private:
    static const QHash<QString, QStringList> &texts() {
        static const auto catalog = [] {
            initTranslationResources();
            QHash<QString, QStringList> result;
            QFile file(":/data/translations/en.json");
            if (!file.open(QIODevice::ReadOnly))
                return result;
            const auto object = QJsonDocument::fromJson(file.readAll()).object();
            for (auto it = object.begin(); it != object.end(); ++it)
                result.insert(it.key(),
                              it->isArray() ? it->toVariant().toStringList() : QStringList{it->toString()});
            return result;
        }();
        return catalog;
    }
};
QTranslator *installed = nullptr;
} // namespace
QString tr(const char *source, const char *disambiguation, int n) {
    return QCoreApplication::translate("MolForma", source, disambiguation, n);
}
QString translated(const QString &source) {
    return tr(source.toUtf8().constData());
}
bool english() {
    return installed;
}
void setEnglish(bool on) {
    if (on == english())
        return;
    if (on) {
        installed = new Catalog(QCoreApplication::instance());
        QCoreApplication::installTranslator(installed);
    } else {
        QCoreApplication::removeTranslator(installed);
        delete installed;
        installed = nullptr;
    }
}
} // namespace mf
