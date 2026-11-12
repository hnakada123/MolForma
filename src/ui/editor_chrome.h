// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QToolButton>
#include <QVector>
#include <QWidget>
#include <utility>
class QGridLayout;

namespace mf {
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
