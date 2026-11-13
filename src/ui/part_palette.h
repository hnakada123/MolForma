// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ui/editor_context.h"
#include "ui/editor_tool.h"
#include "ui/selection_controls.h"
#include <QJsonArray>
#include <QWidget>
class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
namespace mf {
// The palette of parts at the left of "1 分子": the search, the categories, the common elements with
// the periodic table, and the parts as tiles to click or drag onto the 3D view. It also is the store of
// the user's parts ("マイ部品"): saved from the selection of the editor context, read from part files,
// taken off the list and kept in the preferences.
// The palette places nothing itself: it says what was chosen, and the molecular workspace
// (MoleculePanel) takes the tool and makes the edit.
class PartPalette : public QWidget {
    Q_OBJECT
  public:
    PartPalette(EditorContext &editorContext, UiServices &uiContext, QWidget *parent = nullptr);
    // Lists the parts of the category that the search names again; the chosen row stays chosen.
    void refresh();
    // The category shown: "all", "atom", "skeleton", "substituent", "center", "ligand", "amino" or "my".
    QString category() const;
    void setCategory(const QString &code);
    // What the chosen row places; without a row, an atom without an element.
    PaletteEntry choice() const;
    bool hasChoice() const;
    QString choiceText() const; // The name on the chosen row.
    // The part of the chosen row, built in or the user's; fails without a row.
    QJsonObject currentTemplate() const;
    // The chosen row is shown as chosen only while a tool places it.
    void showChoice(bool shown);
    // Makes the row of a part the chosen one by its key (as a drop names it). A part the list does not
    // show brings the whole palette; fails for a part that does not exist.
    void select(const QString &key);
    // Takes the chosen row as a click on it does: says chosen().
    void choose();
    // "アミノ酸の連続追加": whether the next amino acid chosen goes on at the end of the chain.
    bool growing() const;
    void stopGrowing();
    // Reads a part file (.molforma-part.json) into the user's parts and shows them.
    void loadPartFile(const QString &path);
    void savePreferences();
    // Enables "選択原子を保存…" by the selection of the editor context.
    void refreshSelection();
  signals:
    // A part was chosen: a click on its row, Enter in the search, an element button or the periodic
    // table. A SMILES of the search is given once, as the search is cleared.
    void chosen(const mf::PaletteEntry &entry);
    // Another row is the chosen one, or none.
    void choiceChanged();
    void refreshed();
    // A part was read into the user's parts: the palette shall be seen.
    void showRequested();

  private:
    EditorContext &editorContext;
    UiServices &uiContext;
    QButtonGroup *categoryGroup = nullptr;
    QLineEdit *search = nullptr;
    QListWidget *palette = nullptr;
    QLabel *myPartsEmpty = nullptr; // What to do while the list is empty.
    QWidget *myPartsBar = nullptr;  // Saving, reading and removing the user's parts.
    QCheckBox *growth = nullptr;
    QPushButton *periodicTableButton = nullptr;
    SelectionControls selectionControls;
    QString paletteElement = "C";
    // A center element chosen outside the listed metals (S, P, Xe, ...) with the element buttons,
    // the periodic table or the search; listed first under "配位中心".
    QString centerElement;
    QJsonArray customParts;
    void selectFirstPart(); // The first palette row that is a part, not a group heading.
    // Chooses an element for the atom tool, as from the periodic table or the common-element buttons.
    void chooseElement(const QString &symbol);
    void savePart();
    void loadPart();
    void removePart();
};
} // namespace mf
