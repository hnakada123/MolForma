// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QPalette>
#include <QToolButton>
#include <QVector>
#include <utility>
class QGridLayout;
namespace mf {
// Colors of the editor chrome (header, tool rail, option bar, overlays), derived from the widget
// palette so that light and dark themes both stay readable. accentText keeps 4.5:1 on tint.
struct ChromeColors {
    QColor base, window, text, muted, line, hover, segment, accent, accentText, tint, onAccent;
    QColor candidate, candidateLine, candidateText; // The banner of a pending change.
    explicit ChromeColors(const QPalette &palette);
};
// Stroked icons on a 24×24 grid ("select", "move", "undo", …), in the palette's text color, the
// accent when checked and the disabled color when off, or always in `color` if given. "play" is
// filled.
QIcon lineIcon(const QString &name, const QColor &color = {});
// The number of a workflow step in a circle, filled with the accent while its tab is open.
QIcon stepIcon(int number);
// Markers of candidate changes as on the 3D view: "added", "removed" or "moved".
QIcon changeIcon(const QString &kind);

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

// One line of text, shortened with "…" when narrower than the text (the full text in toolTip).
class ElidedLabel : public QLabel {
  public:
    explicit ElidedLabel(const QString &text = {}, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent *event) override;
};

// An editing tool in the rail: the icon over the name, and the tool's key in the corner.
class ToolRailButton : public QToolButton {
  public:
    explicit ToolRailButton(const QString &key, QWidget *parent = nullptr);

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    QString key;
};

// Shows a base widget (the 3D view) with small panels kept in its corners above it.
class OverlayHost : public QWidget {
  public:
    explicit OverlayHost(QWidget *base, QWidget *parent = nullptr);
    void addOverlay(QWidget *overlay, Qt::Alignment corner);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void place();
    QWidget *base;
    QVector<std::pair<QWidget *, Qt::Alignment>> overlays;
};

// The window header: the document on the left, the workflow steps in the middle, and history and
// run on the right. The steps move to a second row when one row would be too narrow.
class HeaderBar : public QWidget {
  public:
    HeaderBar(QWidget *left, QWidget *middle, QWidget *right, QWidget *parent = nullptr);
    bool singleRow() const {
        return single;
    }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    int singleRowWidth() const;
    void arrange(bool oneRow);
    QWidget *left, *middle, *right;
    QGridLayout *grid;
    bool single = false;
};
} // namespace mf
