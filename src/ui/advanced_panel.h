#pragma once
#include "nwchem/nwchem_advanced.h"
#include <QMap>
#include <QWidget>
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
namespace mf {
class MainWindow;
class AdvancedPanel : public QWidget {
    Q_OBJECT
  public:
    explicit AdvancedPanel(MainWindow *owner);
    void refresh();
    void stopEditing() {
        busy = true;
    }

  private:
    struct Row {
        QWidget *container, *editor;
        QLabel *label, *state;
        QPushButton *reset;
    };
    MainWindow *window;
    bool busy = false;
    QMap<QString, Row> rows;
    QMap<QString, QWidget *> groups;
    QMap<QString, QPushButton *> groupResets;
    QLineEdit *search;
    QComboBox *category, *filter;
    QLabel *context, *count, *empty, *message, *outputSummary;
    QPlainTextEdit *diagnostics, *preview;
    QPushButton *undo, *redo, *resetAll, *copy, *save;
    void change(const QString &key, const QJsonValue &value);
    void reset(const QString &group);
    void filterRows();
};
} // namespace mf
