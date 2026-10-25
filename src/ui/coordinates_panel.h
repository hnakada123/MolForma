#pragma once
#include "editing/coordinates.h"
#include <QStyledItemDelegate>
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
// Coordinates read at a glance with six decimals, right aligned; editing a cell shows and keeps
// the full value, so an unchanged cell never rounds the geometry.
class CoordinateDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QString displayText(const QVariant &value, const QLocale &locale) const override;

  protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override;
};
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
