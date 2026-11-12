// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/editor_chrome.h"
#include "ui/theme.h"
#include <QGridLayout>
#include <QPainter>
#include <QResizeEvent>
#include <algorithm>

namespace mf {
ToolRailButton::ToolRailButton(const QString &key, QWidget *parent) : QToolButton(parent), key(key) {
    setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    setCheckable(true);
    setAutoRaise(true);
    setIconSize(QSize(20, 20));
    setFixedSize(60, 54);
    setFocusPolicy(Qt::TabFocus);
}
void ToolRailButton::paintEvent(QPaintEvent *event) {
    QToolButton::paintEvent(event);
    QPainter p(this);
    QFont small = font();
    small.setPixelSize(10);
    p.setFont(small);
    const ChromeColors colors(palette());
    p.setPen(isChecked() ? colors.accentText : colors.muted);
    p.drawText(rect().adjusted(0, 3, -6, 0), Qt::AlignTop | Qt::AlignRight, key);
}

OverlayHost::OverlayHost(QWidget *base, QWidget *parent) : QWidget(parent), base(base) {
    base->setParent(this);
}
void OverlayHost::addOverlay(QWidget *overlay, Qt::Alignment corner) {
    overlay->setParent(this);
    overlays.append({overlay, corner});
    overlay->installEventFilter(this);
    place();
}
QSize OverlayHost::sizeHint() const {
    return base->sizeHint();
}
QSize OverlayHost::minimumSizeHint() const {
    return base->minimumSizeHint();
}
void OverlayHost::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    place();
}
bool OverlayHost::eventFilter(QObject *watched, QEvent *event) {
    // A panel that grows, shrinks or appears keeps its corner.
    if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show)
        place();
    return QWidget::eventFilter(watched, event);
}
void OverlayHost::place() {
    base->setGeometry(rect());
    const int margin = 10;
    // Panels on the right first; one on the left of the same edge gets the width they leave. A
    // panel takes its maximum width when it has one (a wrapping text), else its size hint.
    int taken[2] = {0, 0}, height[2] = {0, 0};
    for (const bool rightSide : {true, false})
        for (const auto &[overlay, corner] : std::as_const(overlays)) {
            if (overlay->isHidden() || bool(corner & Qt::AlignRight) != rightSide)
                continue;
            const int edge = corner & Qt::AlignBottom ? 1 : 0;
            int available = std::max(0, width() - 2 * margin - (rightSide ? 0 : taken[edge]));
            // Buttons with an icon show only the icon (their name in the tooltip) when narrow.
            if (overlay->property("compactButtons").toBool()) {
                const auto buttons = overlay->findChildren<QToolButton *>();
                if (!overlay->property("wideWidth").isValid())
                    overlay->setProperty("wideWidth", overlay->sizeHint().width());
                const bool compact = overlay->property("wideWidth").toInt() > available;
                for (auto *b : buttons)
                    if (!b->icon().isNull()) {
                        b->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly
                                                      : Qt::ToolButtonTextBesideIcon);
                        if (compact && !b->toolTip().startsWith(b->text()))
                            b->setToolTip(b->text() + ": " + b->toolTip());
                    }
                overlay->layout()->activate();
            }
            const int preferred = overlay->maximumWidth() < QWIDGETSIZE_MAX ? overlay->maximumWidth()
                                                                            : overlay->sizeHint().width();
            int shift = 0;
            // Too little room beside the panel on the right: below (or above) it instead.
            if (!rightSide && available < std::min(preferred, 160)) {
                available = std::max(0, width() - 2 * margin);
                shift = height[edge] + margin;
            }
            const int w = std::min(preferred, available);
            const int h =
                overlay->hasHeightForWidth() ? overlay->heightForWidth(w) : overlay->sizeHint().height();
            if (rightSide) {
                taken[edge] = std::max(taken[edge], w + margin);
                height[edge] = std::max(height[edge], h);
            }
            overlay->setGeometry(
                rightSide ? this->width() - margin - w : margin,
                corner & Qt::AlignBottom ? this->height() - margin - h - shift : margin + shift, w, h);
            overlay->raise();
        }
}

HeaderBar::HeaderBar(QWidget *left, QWidget *middle, QWidget *right, QWidget *parent)
    : QWidget(parent), left(left), middle(middle), right(right) {
    grid = new QGridLayout(this);
    grid->setContentsMargins(14, 8, 14, 8);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(6);
    arrange(true);
}
int HeaderBar::singleRowWidth() const {
    const auto m = grid->contentsMargins();
    return m.left() + m.right() + 2 * grid->horizontalSpacing() + left->sizeHint().width() +
           middle->sizeHint().width() + right->sizeHint().width();
}
QSize HeaderBar::sizeHint() const {
    return {singleRowWidth(), grid->sizeHint().height()};
}
QSize HeaderBar::minimumSizeHint() const {
    // Two rows fit any window that the steps alone fit.
    const auto m = grid->contentsMargins();
    const int sides = left->minimumSizeHint().width() + grid->horizontalSpacing() + right->sizeHint().width();
    return {m.left() + m.right() + std::max(sides, middle->sizeHint().width()), grid->minimumSize().height()};
}
void HeaderBar::resizeEvent(QResizeEvent *event) {
    arrange(event->size().width() >= singleRowWidth());
    QWidget::resizeEvent(event);
}
void HeaderBar::arrange(bool oneRow) {
    if (oneRow == single && grid->count())
        return;
    single = oneRow;
    for (auto *w : {left, middle, right})
        grid->removeWidget(w);
    grid->addWidget(left, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);
    grid->addWidget(right, 0, 2, Qt::AlignRight | Qt::AlignVCenter);
    if (oneRow)
        grid->addWidget(middle, 0, 1, Qt::AlignCenter);
    else
        grid->addWidget(middle, 1, 0, 1, 3, Qt::AlignLeft | Qt::AlignVCenter);
    grid->setColumnStretch(1, 1);
    updateGeometry();
}
} // namespace mf
