#pragma once
#include "editing/coordinates.h"
#include <QHash>
#include <QIcon>
#include <QStyledItemDelegate>
#include <QWidget>
#include <functional>
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTableWidgetItem;
class QToolButton;
namespace mf {
class MainWindow;
class MoleculeView;
class ViewControls;
// Cells of the coordinate table. The atom cell holds the element and shows the atom as the 3D view
// labels it ("C14"), with a lock when the atom is fixed. Coordinates read at a glance with six
// decimals, right aligned; editing a cell shows and keeps the full value, so an unchanged cell
// never rounds the geometry. A blank mass reads "既定".
class CoordinateDelegate : public QStyledItemDelegate {
  public:
    enum Kind { AtomCell, CoordinateCell, MassCell };
    explicit CoordinateDelegate(QObject *parent = nullptr, Kind kind = CoordinateCell)
        : QStyledItemDelegate(parent), kind(kind) {}
    QString displayText(const QVariant &value, const QLocale &locale) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

  protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override;

  private:
    Kind kind;
};
class CoordinatesPanel : public QWidget {
    Q_OBJECT
  public:
    explicit CoordinatesPanel(MainWindow *window);
    void refresh();
    void refreshSelection();
    // Shows the columns of the atom attributes (formal charge, unpaired electrons, atom name).
    void showAttributes();
    void previewStructure(const Document &structure);
    // Reads a structure file into a replacement candidate, asking for the PDB MODEL and ALTLOC.
    void importPath(const QString &path);
    void applyChromeStyle();

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

  private:
    MainWindow *window;
    QTableWidget *table;
    QStackedWidget *tablePages;
    QFrame *selectionBar;
    QComboBox *units;
    QLabel *outputUnit; // The unit of the input, a setting of "3 計算条件".
    QLineEdit *filter;
    QLabel *message, *selectionHint, *selectionTitle, *moreAtoms, *measurement, *compareNote, *count,
        *previewLabel, *xyzNote, *exportNote;
    QList<QLabel *> chips;
    QList<QWidget *> selectionItems, candidateItems; // Shown with a selection, or with a candidate.
    QWidget *unitRow, *viewCandidate;
    QPlainTextEdit *preview;
    MoleculeView *view;
    QPushButton *originButton, *fixButton, *ghostButton, *copyButton, *saveButton, *xyzCopy, *xyzSave,
        *attributes;
    ViewControls *viewControls;
    QToolButton *showAll, *showChanged, *xyzFormat;
    QList<QWidget *> editControls;
    // Icons of the atom cells by element ("@C" for a ghost), and of the change column ("#added").
    QHash<QString, QIcon> icons;
    bool busy = false;
    QStringList displayedIds;
    QPushButton *button(QLayout *layout, const QString &text, const QString &name, const QString &icon,
                        const std::function<void()> &action, bool editing = false);
    CoordinateUnit tableUnit() const;
    CoordinateUnit exportUnit() const;
    bool editable() const;
    void attempt(const std::function<void()> &action);
    void cellChanged(QTableWidgetItem *item);
    void applyFilter();
    void refreshCount();
    void refreshSelectionBar();
    void refreshExport();
    void setSelectionFlag(CoordinateColumn column);
    QIcon atomDot(const Atom &atom);
    void addAtom();
    void pasteXyz();
    void importFile();
    void exportText(bool xyz, bool save);
};
} // namespace mf
