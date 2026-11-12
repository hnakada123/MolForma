// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/chrome_layouts.h"
#include "foundation/i18n.h"
#include <QAbstractButton>
#include <QEvent>
#include <QSpacerItem>
#include <QToolButton>
#include <QWidget>
#include <algorithm>
#include <utility>

namespace mf {
FlowLayout::FlowLayout(QWidget *parent, int horizontalSpacing, int verticalSpacing)
    : QLayout(parent), horizontal(horizontalSpacing), vertical(verticalSpacing) {}
FlowLayout::~FlowLayout() {
    while (auto *item = takeAt(0))
        delete item;
}
void FlowLayout::addItem(QLayoutItem *item) {
    items.append(item);
}
int FlowLayout::count() const {
    return items.size();
}
QLayoutItem *FlowLayout::itemAt(int index) const {
    return items.value(index);
}
QLayoutItem *FlowLayout::takeAt(int index) {
    return index >= 0 && index < items.size() ? items.takeAt(index) : nullptr;
}
Qt::Orientations FlowLayout::expandingDirections() const {
    return {};
}
bool FlowLayout::hasHeightForWidth() const {
    return true;
}
ToolbarFit::ToolbarFit(QWidget *bar, std::function<void(bool)> apply)
    : QObject(bar), bar(bar), apply(std::move(apply)) {
    bar->installEventFilter(this);
}
bool ToolbarFit::eventFilter(QObject *watched, QEvent *event) {
    if (watched != bar)
        return QObject::eventFilter(watched, event);
    const auto type = event->type();
    // Wider, or shown again with what it holds changed meanwhile: the full bar, if it fits (the
    // layout request that follows makes it compact again otherwise). Layout requests only make it
    // compact, so the two never take turns.
    if (compact && (type == QEvent::Show || (type == QEvent::Resize && bar->width() > narrowest)))
        refit();
    else if (type == QEvent::Resize || type == QEvent::LayoutRequest)
        fit();
    return QObject::eventFilter(watched, event);
}
void ToolbarFit::refit() {
    if (!compact)
        return;
    compact = false;
    apply(false); // Compact again at the layout request that follows, if it wraps.
}
void ToolbarFit::fit() {
    const auto *layout = bar->layout();
    // The layout's size hint is its one row: wider than the bar, the row wraps.
    if (compact || !layout || bar->width() <= 0 || layout->sizeHint().width() <= bar->width())
        return;
    compact = true;
    narrowest = bar->width();
    apply(true);
}
void setIconOnly(QAbstractButton *button, bool iconOnly) {
    if (!button->property("fullText").isValid()) {
        button->setProperty("fullText", button->text());
        button->setProperty("fullTip", button->toolTip());
    }
    const auto name = button->property("fullText").toString();
    const auto tip = button->property("fullTip").toString();
    if (auto *tool = qobject_cast<QToolButton *>(button))
        tool->setToolButtonStyle(iconOnly ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    else
        button->setText(iconOnly ? QString() : name);
    button->setToolTip(!iconOnly ? tip : tip.isEmpty() ? name : tr("%1：%2").arg(name, tip));
    button->setAccessibleName(name);
}
int FlowLayout::heightForWidth(int width) const {
    return arrange(QRect(0, 0, width, 0), true);
}
QSize FlowLayout::minimumSize() const {
    QSize size;
    for (auto *item : items)
        if (!item->isEmpty())
            size = size.expandedTo(item->minimumSize());
    const auto m = contentsMargins();
    return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}
int FlowLayout::rowHeight(const QList<QLayoutItem *> &row) const {
    int height = 0;
    for (auto *item : uniform ? items : row)
        if (!item->spacerItem())
            height = std::max(height, item->widget() ? item->widget()->sizeHint().height()
                                                     : item->sizeHint().height());
    return height;
}
QSize FlowLayout::sizeHint() const {
    // One row: the width that needs no wrapping.
    int width = 0, shown = 0;
    QList<QLayoutItem *> row;
    for (auto *item : items) {
        if (item->isEmpty())
            continue;
        width += item->sizeHint().width();
        row << item;
        ++shown;
    }
    const int height = shown ? rowHeight(row) : 0;
    const auto m = contentsMargins();
    return {width + std::max(0, shown - 1) * horizontal + m.left() + m.right(),
            height + m.top() + m.bottom()};
}
void FlowLayout::setGeometry(const QRect &rect) {
    QLayout::setGeometry(rect);
    arrange(rect, false);
}
int FlowLayout::arrange(const QRect &rect, bool testOnly) const {
    const auto m = contentsMargins();
    const QRect area = rect.marginsRemoved(m);
    // Spacer items (empty by definition) take part only to share out the room left in a row.
    const auto spacer = [](QLayoutItem *item) { return item->spacerItem() != nullptr; };
    QList<QList<QLayoutItem *>> rows{QList<QLayoutItem *>()};
    int x = 0;
    for (auto *item : items) {
        if (item->isEmpty() && !spacer(item))
            continue;
        const int w = spacer(item) ? 0 : item->sizeHint().width();
        if (!rows.last().isEmpty() && x + horizontal + w > area.width()) {
            rows.append(QList<QLayoutItem *>());
            x = 0;
        }
        x += (rows.last().isEmpty() || spacer(item) ? 0 : horizontal) + w;
        rows.last().append(item);
    }
    // Widths first (the room left in a row goes to spacers and expanding items), then heights,
    // as a wrapping text is as tall as the width it gets.
    struct Row {
        QList<std::pair<QLayoutItem *, int>> items;
        int height = 0;
    };
    QList<Row> laid;
    int total = 0;
    for (const auto &row : std::as_const(rows)) {
        int used = 0, stretch = 0, shown = 0;
        for (auto *item : row) {
            if (spacer(item) || item->expandingDirections() & Qt::Horizontal)
                ++stretch;
            if (!spacer(item)) {
                used += item->sizeHint().width();
                ++shown;
            }
        }
        if (!shown)
            continue;
        used += (shown - 1) * horizontal;
        const int extra = std::max(0, area.width() - used);
        Row out;
        out.height = uniform ? rowHeight(row) : 0;
        int given = 0;
        for (auto *item : row) {
            int w = spacer(item) ? 0 : std::min(item->sizeHint().width(), area.width());
            if (stretch && (spacer(item) || item->expandingDirections() & Qt::Horizontal))
                w += extra / stretch + (given++ < extra % stretch ? 1 : 0);
            if (!spacer(item))
                out.height = std::max(out.height, item->hasHeightForWidth() ? item->heightForWidth(w)
                                                                            : item->sizeHint().height());
            out.items.append({item, w});
        }
        total += (laid.isEmpty() ? 0 : vertical) + out.height;
        laid.append(out);
    }
    if (!testOnly) {
        // Rows centered in a taller area, such as a stack sized by a taller page.
        int y = area.y() + std::max(0, area.height() - total) / 2;
        for (const auto &row : std::as_const(laid)) {
            int left = area.x();
            for (const auto &[item, w] : row.items) {
                if (spacer(item)) {
                    left += w; // Room shared out, without spacing of its own.
                    continue;
                }
                const int h = std::min(row.height, item->hasHeightForWidth() ? item->heightForWidth(w)
                                                                             : item->sizeHint().height());
                item->setGeometry(QRect(left, y + (row.height - h) / 2, w, h));
                left += w + horizontal;
            }
            y += row.height + vertical;
        }
    }
    return total + m.top() + m.bottom();
}
void FormLayout::setGeometry(const QRect &rect) {
    // A neighboring width takes the place of the remembered row heights, so that QFormLayout computes them
    // again for this width rather than reusing those of another one.
    if (hasHeightForWidth() && rect.isValid())
        heightForWidth(rect.width() + 1);
    QFormLayout::setGeometry(rect);
}
} // namespace mf
