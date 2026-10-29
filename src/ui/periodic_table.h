// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>

class QLabel;
class QLineEdit;
class QAbstractButton;
namespace mf {
class PeriodicTableDialog : public QDialog {
    Q_OBJECT
  public:
    explicit PeriodicTableDialog(const QString &current, const QString &query = {},
                                 QWidget *parent = nullptr);
    QString selectedElement() const {
        return selected;
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QString selected;
    QLabel *details, *matches;
    QLineEdit *search;
    QList<QAbstractButton *> buttons;
    void filter();
    void describe(const QString &symbol);
};
} // namespace mf
