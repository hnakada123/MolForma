// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "editing/chemistry.h"
#include "results/result_loader.h"
#include <QDialog>
class QTableWidget;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QLabel;
namespace mf {
class MainWindow;
class ConformerSetDialog : public QDialog {
    Q_OBJECT
  public:
    using Enqueue = std::function<QStringList(const QVector<Document> &, bool)>;
    ConformerSetDialog(MainWindow *window, Enqueue enqueue);
    void setConformers(const Document &original, const QVector<Conformer> &conformers);
    QJsonObject specification() const;
    void setSpecification(const QJsonObject &specification);
    void applySelected();
    void queueSelected();
    void refreshResults();

  private:
    MainWindow *window;
    Enqueue enqueue;
    Document original, pendingOriginal;
    QJsonArray entries;
    ResultLoader loader;
    QVector<int> pending;
    int reading = -1;
    QTableWidget *table;
    QDoubleSpinBox *temperature, *tolerance;
    QComboBox *metric;
    QCheckBox *frequencies;
    QLabel *message;
    void refresh();
    void readNext();
    Document geometry(int index) const;
    void checkMolecule() const;
};
} // namespace mf
