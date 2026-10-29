// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "editing/coordinates.h"
#include <QAbstractTableModel>
#include <QColor>
#include <QHash>
#include <QIcon>
#include <QStyledItemDelegate>
#include <QWidget>
#include <functional>
class QAction;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTableView;
class QToolButton;
namespace mf {
class MainWindow;
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
// The rows of the coordinate table: the atoms of the document shown, with each cell computed when
// the view asks for it, so that a refresh costs the visible cells only. Editing a cell or its check
// box calls `edit` with the atom, the document column and the text ("1" / "0" for a check).
class CoordinateModel : public QAbstractTableModel {
  public:
    struct View {
        Document document;
        AtomChanges changes; // Of a candidate against the committed document.
        QHash<QString, QString> places;
        CoordinateUnit unit = CoordinateUnit::Angstrom;
        bool editable = true;
        QColor muted, good, changed;
    };
    using Edit = std::function<void(const QString &id, CoordinateColumn column, const QString &text)>;
    CoordinateModel(QObject *parent, Edit edit, QStringList titles, QStringList tips);
    void show(View view);
    void clearIcons(); // After a change of theme.
    QString id(int row) const;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

  private:
    View shown;
    Edit edit;
    QStringList titles, tips;
    // Icons of the atom cells by element ("@C" for a ghost), and of the change column ("#added").
    mutable QHash<QString, QIcon> icons;
    QIcon atomDot(const Atom &atom) const;
    QIcon mark(const QString &kind) const;
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
    // At least the height the bars need as wrapped at the current width, over a table of four rows.
    QSize minimumSizeHint() const override;

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

  private:
    MainWindow *window;
    QTableView *table;
    CoordinateModel *model;
    QStackedWidget *tablePages;
    QFrame *selectionBar;
    QComboBox *units;
    QLineEdit *filter;
    QLabel *message, *selectionHint, *selectionTitle, *moreAtoms, *measurement, *compareNote, *count;
    QList<QLabel *> chips;
    QList<QWidget *> selectionItems, candidateItems; // Shown with a selection, or with a candidate.
    QPushButton *attributes, *exportButton;
    QList<QAction *> exportActions; // Copy and save of the NWChem fragment, then of the XYZ.
    QToolButton *showAll, *showChanged;
    QList<QWidget *> editControls;
    QList<QAction *> editActions;
    bool busy = false;
    QPushButton *button(QLayout *layout, const QString &text, const QString &name, const QString &icon,
                        const std::function<void()> &action, bool editing = false);
    CoordinateUnit tableUnit() const;
    CoordinateUnit exportUnit() const;
    bool editable() const;
    void attempt(const std::function<void()> &action);
    void applyFilter();
    void refreshCount();
    void refreshSelectionBar();
    void refreshExport();
    void addAtom();
    void pasteXyz();
    void importFile();
    void exportText(bool xyz, bool save);
};
} // namespace mf
