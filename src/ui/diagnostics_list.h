#pragma once
#include "model/diagnostic.h"
#include <QListWidget>

namespace mf {
// Errors and warnings of a settings tab, one row each. Clicking a row asks the owner to show the
// related field; rows with a quick fix say so and apply it on double click (see fixRole).
class DiagnosticsList : public QListWidget {
    Q_OBJECT
  public:
    static constexpr int MessageRole = Qt::UserRole, ErrorRole = Qt::UserRole + 1, FixRole = Qt::UserRole + 2;
    explicit DiagnosticsList(QWidget *parent = nullptr);
    static constexpr int CodeRole = Qt::UserRole + 3, SettingRole = Qt::UserRole + 4,
                         TabRole = Qt::UserRole + 5;
    void setDiagnostics(const QVector<Diagnostic> &diagnostics);
    QStringList messages() const;
    // As tall as its rows for the width it gets, up to maximum (then it scrolls).
    void setFitContents(int maximum);
    QSize sizeHint() const override;
  signals:
    void diagnosticClicked(QString setting, int tab);
    void fixRequested(QString code, QString setting);

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    int fit = 0;
};
} // namespace mf
