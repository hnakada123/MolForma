// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QFormLayout>
#include <QLayout>
#include <QList>
#include <QObject>
#include <functional>
class QAbstractButton;

namespace mf {
// Lays items out left to right and wraps them into further rows when the width runs out. Spacer
// items and horizontally expanding widgets share the width left over in their row.
class FlowLayout : public QLayout {
  public:
    explicit FlowLayout(QWidget *parent = nullptr, int horizontalSpacing = 8, int verticalSpacing = 6);
    ~FlowLayout() override;
    void addItem(QLayoutItem *item) override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QLayoutItem *takeAt(int index) override;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect &rect) override;
    // Rows as tall as the tallest item, shown or hidden, so that showing other items in a row
    // does not change the height of the layout.
    void setUniformRows(bool on) {
        uniform = on;
        invalidate();
    }

  private:
    int rowHeight(const QList<QLayoutItem *> &row) const;
    int arrange(const QRect &rect, bool testOnly) const;
    QList<QLayoutItem *> items;
    int horizontal, vertical;
    bool uniform = false;
};

// A QFormLayout whose rows of wrapping text or buttons get the height they need at its width. QFormLayout
// keeps one set of row heights for the two widths it remembers (its size hint's and the last one asked),
// so after a height asked for its size hint's width it laid rows out at the heights of that wider width,
// too short for its own, and they covered each other.
class FormLayout : public QFormLayout {
  public:
    using QFormLayout::QFormLayout;
    void setGeometry(const QRect &rect) override;
};

// Keeps a toolbar laid out by a FlowLayout on one row while it can: before the row would wrap,
// apply(true) makes the bar compact (names of buttons left to their tooltips, narrower fields).
// Widened or shown again, apply(false) tries the full bar, compact again if it still wraps.
class ToolbarFit : public QObject {
  public:
    ToolbarFit(QWidget *bar, std::function<void(bool compact)> apply);
    // What the bar holds changed (a shorter name, a button hidden): the full bar again if it fits.
    void refit();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QWidget *bar;
    std::function<void(bool)> apply;
    bool compact = false;
    int narrowest = 0; // The width of the bar when it became compact.
    void fit();
};
// A button of a compact toolbar: its icon alone, with its name kept in the tooltip and as its
// accessible name; or back with its name.
void setIconOnly(QAbstractButton *button, bool iconOnly);
} // namespace mf
