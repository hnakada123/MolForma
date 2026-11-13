// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ui/editor_context.h"
#include "ui/selection_controls.h"
#include <QHash>
#include <QScrollArea>
#include <functional>
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QPushButton;
class QVBoxLayout;
namespace mf {
class SettingsCard;
// A page of the properties at the right of "1 分子": a column of cards on the background of the settings
// tabs, each a form whose buttons follow one another in rows that wrap. A page made of this class alone
// is filled by the workspace (MoleculePanel); the pages that need only the editor context and the
// selection are classes of their own (TransformPage, AttributesPage, ChargePage).
class PropertyPage : public QScrollArea {
  public:
    PropertyPage(EditorContext &editorContext, UiServices &uiContext, QWidget *parent = nullptr);
    // The form for what comes before the cards of the page.
    QFormLayout *form() const {
        return top;
    }
    // A card on the page, which `form` then adds to. The title is the source text (QT_TR_NOOP): it names
    // the card in every language and is shown translated. open: 1 or 0 for a card that opens and closes
    // (open or closed at first), -1 for one always open.
    SettingsCard *section(QFormLayout *&form, const QString &title, int open = -1);
    // A button in the row of buttons at the end of the form; its slot runs through UiServices::safe.
    QPushButton *button(QFormLayout *form, const QString &text, const std::function<void()> &slot,
                        const QString &objectName = {});
    QDoubleSpinBox *number(QFormLayout *form, const QString &label, double value = 0, double minimum = -10000,
                           double maximum = 10000);
    QComboBox *choices(QFormLayout *form, const QString &label, const QStringList &labels,
                       const QStringList &values = {});
    // A control that needs minimum to maximum atoms selected (maximum 0: any number from minimum).
    void enableWithSelection(QWidget *widget, int minimum, int maximum = 0);
    // Enables those controls by the selection of the editor context.
    void refreshSelectionControls();
    // The selection, for an edit that needs one (of `count` atoms if given): fails with what to select.
    Ids requireSelection(int count = 0) const;

  protected:
    EditorContext &editorContext;
    UiServices &uiContext;

  private:
    QFormLayout *top;
    QVBoxLayout *cards;
    // The row of buttons that button() adds to, by form (its row in the form).
    QHash<QFormLayout *, std::pair<int, QWidget *>> buttonRows;
    SelectionControls selectionControls;
};
} // namespace mf
