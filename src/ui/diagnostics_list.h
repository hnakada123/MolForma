#pragma once
#include <QListWidget>

namespace mf {
// Errors and warnings of a settings tab, one row each. Clicking a row asks the owner to show the
// related field; rows with a quick fix say so and apply it on double click (see fixRole).
class DiagnosticsList : public QListWidget {
    Q_OBJECT
  public:
    static constexpr int MessageRole = Qt::UserRole, ErrorRole = Qt::UserRole + 1, FixRole = Qt::UserRole + 2;
    explicit DiagnosticsList(QWidget *parent = nullptr);
    // fixes: message → description of the quick fix offered for it.
    void setMessages(const QStringList &errors, const QStringList &warnings, const QString &ok,
                     const QMap<QString, QString> &fixes = {});
    QStringList messages() const;
  signals:
    void messageClicked(QString message, bool error);
    void fixRequested(QString message);
};
} // namespace mf
