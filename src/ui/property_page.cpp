// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/property_page.h"
#include "foundation/i18n.h"
#include "ui/chrome_layouts.h"
#include "ui/settings_widgets.h"
#include <QtWidgets>

namespace mf {
// A form in a card or above the cards of a page: its rows, then buttons side by side.
static QFormLayout *formLayout(QWidget *parent) {
    auto *form = new FormLayout(parent);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    return form;
}
PropertyPage::PropertyPage(EditorContext &editorServices, UiServices &uiServices, QWidget *parent)
    : QScrollArea(parent), editorContext(editorServices), uiContext(uiServices) {
    // The cards of section() under what comes before them, on the background of the settings tabs.
    auto *page = new QWidget;
    page->setProperty("canvas", true);
    cards = new QVBoxLayout(page);
    cards->setContentsMargins(10, 10, 10, 12);
    cards->setSpacing(10);
    auto *topWidget = new QWidget;
    top = formLayout(topWidget);
    cards->addWidget(topWidget);
    cards->addStretch();
    setProperty("canvas", true);
    setWidgetResizable(true);
    setWidget(page);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}
QPushButton *PropertyPage::button(QFormLayout *f, const QString &text, const std::function<void()> &slot,
                                  const QString &name) {
    auto *b = new QPushButton(text);
    if (!name.isEmpty())
        b->setObjectName(name);
    // Buttons follow each other in rows that wrap, not each across the whole card.
    auto &row = buttonRows[f];
    if (!row.second || row.first != f->rowCount() - 1) {
        auto *buttons = new QWidget;
        new FlowLayout(buttons, 6, 6);
        buttons->layout()->setContentsMargins(0, 2, 0, 0);
        f->addRow(buttons);
        row = {f->rowCount() - 1, buttons};
    }
    row.second->layout()->addWidget(b);
    connect(b, &QPushButton::clicked, this, [this, slot] { uiContext.safe(slot); });
    return b;
}
QDoubleSpinBox *PropertyPage::number(QFormLayout *f, const QString &label, double v, double min, double max) {
    auto *n = new QDoubleSpinBox;
    n->setDecimals(5);
    n->setRange(min, max);
    n->setValue(v);
    n->setKeyboardTracking(false);
    f->addRow(label, n);
    return n;
}
QComboBox *PropertyPage::choices(QFormLayout *f, const QString &label, const QStringList &labels,
                                 const QStringList &values) {
    auto *box = new QComboBox;
    for (int i = 0; i < labels.size(); ++i)
        box->addItem(labels[i], values.isEmpty() ? labels[i] : values[i]);
    f->addRow(label, box);
    return box;
}
SettingsCard *PropertyPage::section(QFormLayout *&f, const QString &title, int open) {
    // A card per section on the page of the form; the rows that follow go into it. The title is the
    // source text (QT_TR_NOOP): it names the card in every language and is shown translated.
    auto *card = new SettingsCard(translated(title));
    card->setObjectName("moleculeCard_" + title);
    // A long title (English, a wide font) wraps rather than widen the page, which scrolls only down.
    card->titleLabel()->setWordWrap(true);
    auto *body = new QWidget;
    auto *form = formLayout(body);
    card->body()->addWidget(body);
    if (open >= 0)
        card->setCollapsible(open);
    cards->insertWidget(cards->count() - 1, card);
    f = form;
    return card;
}
void PropertyPage::enableWithSelection(QWidget *widget, int minimum, int maximum) {
    selectionControls.add(widget, minimum, maximum);
}
void PropertyPage::refreshSelectionControls() {
    selectionControls.refresh(editorContext.selection().size());
}
Ids PropertyPage::requireSelection(int count) const {
    return requiredSelection(editorContext.selection(), count);
}
} // namespace mf
