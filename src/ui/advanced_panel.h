#pragma once
#include "nwchem/nwchem_advanced.h"
#include <QMap>
#include <QWidget>
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
namespace mf {
class MainWindow;
class DiagnosticsList;
class AdvancedPanel : public QWidget {
    Q_OBJECT
  public:
    explicit AdvancedPanel(MainWindow *owner);
    void refresh();
    void stopEditing() {
        busy = true;
    }

  private:
    // One table row per option, under its category: name, editor, state, NWChem keyword, reset.
    enum Column { Name, Value, State, Keyword, Reset };
    struct Row {
        QTreeWidgetItem *item;
        QWidget *editor;
        QPushButton *reset;
    };
    MainWindow *window;
    bool busy = false;
    QMap<QString, Row> rows;
    QMap<QString, QTreeWidgetItem *> groups;
    QMap<QString, QPushButton *> groupResets;
    QLineEdit *search;
    QComboBox *category, *filter;
    QLabel *context, *count, *empty, *message, *help, *outputSummary;
    QPlainTextEdit *preview;
    DiagnosticsList *diagnostics;
    QTreeWidget *table;
    QPushButton *undo, *redo, *resetAll, *copy, *save;
    void change(const QString &key, const QJsonValue &value);
    void reset(const QString &group);
    void filterRows();
    // The selected row's description below the table (and the planned CUBE/MOLDEN files).
    void describe();
    void showRow(const QString &key);
};
} // namespace mf
