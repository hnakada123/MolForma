#pragma once
#include "coordinates.h"
#include <QWidget>
#include <functional>
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class QHBoxLayout;
namespace mf {
class MainWindow;
class MoleculeView;
class CoordinatesPanel : public QWidget {
    Q_OBJECT
  public:
    explicit CoordinatesPanel(MainWindow *window);
    void refresh();
    void refreshSelection();
    void previewStructure(const Document &structure);

  private:
    MainWindow *window;
    QTableWidget *table;
    QComboBox *units, *outputUnits;
    QLabel *message, *selectionLabel, *previewLabel;
    QPlainTextEdit *preview;
    MoleculeView *view;
    QPushButton *undoButton, *redoButton, *copyButton, *saveButton, *xyzCopy, *xyzSave;
    QList<QWidget *> editControls;
    bool busy = false;
    QStringList displayedIds;
    QPushButton *button(QHBoxLayout *layout, const QString &text, const QString &name,
                        const std::function<void()> &action, bool editing = false);
    CoordinateUnit tableUnit() const;
    CoordinateUnit exportUnit() const;
    bool editable() const;
    void attempt(const std::function<void()> &action);
    void cellChanged(QTableWidgetItem *item);
    void addAtom();
    void pasteXyz();
    void importFile();
    void exportText(bool xyz, bool save);
};
} // namespace mf
