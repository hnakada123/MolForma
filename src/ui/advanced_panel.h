#pragma once
#include "nwchem/nwchem_advanced.h"
#include <QMap>
#include <QWidget>
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
namespace mf {
class MainWindow;
class ChoiceButtons;
class DiagnosticsList;
class NavList;
// "4 詳細設定": the categories at the left, the options as a table filtered by what the current
// calculation uses, and at the right the selected option: what it does, the value NWChem uses while
// it is blank, the calculations it takes part in and the lines it writes into the input.
class AdvancedPanel : public QWidget {
    Q_OBJECT
  public:
    explicit AdvancedPanel(MainWindow *owner);
    void refresh();
    // Shows this tab with only the options changed from their defaults.
    void showChanged();
    // Shows this tab with the options of a group ("dft"), or selects one option's row.
    void showGroup(const QString &group);
    void showOption(const QString &key);
    void stopEditing() {
        busy = true;
    }
    void applyChromeStyle();

  private:
    // One table row per option, under its category: name and keyword, editor, state, reset.
    enum Column { Name, Value, State, Reset };
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
    ChoiceButtons *filterButtons;
    NavList *categories;
    QFrame *banner;
    QLabel *context, *empty, *message, *selectedTitle, *keyword, *selectedState, *help, *defaultValue,
        *currentValue, *scope, *snippetNote, *outputSummary, *status;
    QWidget *details;
    QPlainTextEdit *snippet;
    DiagnosticsList *diagnostics;
    QTreeWidget *table;
    QPushButton *resetAll, *resetSelected, *showInInput;
    void change(const QString &key, const QJsonValue &value);
    void reset(const QString &group);
    void filterRows();
    // The selected row at the right: description, values, scope and its lines in the input.
    void describe();
    void showSnippet(const AdvancedOption &option);
};
} // namespace mf
