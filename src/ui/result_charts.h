// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QVector>
#include <QWidget>

namespace mf {
// The name of an orbital by its place among the occupied or empty orbitals of its spin: "HOMO",
// "HOMO−2", "LUMO", "LUMO+1", …
QString frontierName(const QVector<ResultOrbital> &orbitals, int index);
// The color of a series, taken from the palette when it is drawn, so that it follows the theme.
enum class ChartTone { Accent, Second, Contrast, Muted, Text };
struct ChartSeries {
    QString name;
    QVector<QPointF> points;
    ChartTone tone = ChartTone::Accent;
    bool sticks = false; // Vertical lines from zero (stick spectra) instead of a polyline.
};
// Small QPainter chart for result summaries: optimization convergence and broadened spectra.
// Colors for text and grid follow the widget palette, so light and dark themes both work.
class ChartView : public QWidget {
    Q_OBJECT
  public:
    explicit ChartView(QWidget *parent = nullptr);
    // levels are horizontal reference lines (e.g. convergence thresholds) with labels.
    void setChart(const QString &title, const QString &xLabel, const QString &yLabel,
                  const QVector<ChartSeries> &series, bool logY = false,
                  const QVector<QPair<QString, double>> &levels = {});
    void clear();
    // Larger x on the left, as IR spectra are drawn in wavenumbers.
    void setReversedX(bool on);
    // One stick drawn thicker in the accent color (series and point index; -1 for none).
    void setSelectedStick(int series, int index);
    QSize sizeHint() const override {
        return {420, 220};
    }
    QSize minimumSizeHint() const override {
        return {240, 140};
    }

  signals:
    // A click near a stick of a stick series.
    void stickClicked(int series, int index);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

  private:
    QString title, xLabel, yLabel;
    QVector<ChartSeries> series;
    QVector<QPair<QString, double>> levels;
    bool logY = false, reversed = false;
    int selectedSeries = -1, selectedIndex = -1;
    // The x mapping of the last paint, for clicks.
    QRectF plot;
    double x0 = 0, x1 = 1;
    double screenX(double x) const;
    QColor color(ChartTone tone) const;
};

// The frames of a structure list as a track to click or drag along, with the energy of each frame
// drawn above it where the result knows it (scan and IRC points).
class FrameTimeline : public QWidget {
    Q_OBJECT
  public:
    explicit FrameTimeline(QWidget *parent = nullptr);
    // energies: one per frame, NaN where unknown.
    void setFrames(int count, const QVector<double> &energies = {});
    void setCurrent(int index);
    int current() const {
        return now;
    }
    QSize sizeHint() const override {
        return {240, 36};
    }
    QSize minimumSizeHint() const override {
        return {100, 30};
    }

  signals:
    void frameSelected(int index);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private:
    QRectF track() const;
    double xOf(int index) const;
    void pick(double x);
    int count = 0, now = -1;
    QVector<double> energies;
};

// The energy levels of the orbitals around HOMO and LUMO, one column per spin: occupied levels
// filled with their electrons, empty ones as outlines, degenerate levels side by side.
class OrbitalLevels : public QWidget {
    Q_OBJECT
  public:
    explicit OrbitalLevels(QWidget *parent = nullptr);
    void setOrbitals(const QVector<ResultOrbital> &orbitals, int selected);
    void setSelected(int index);
    QSize sizeHint() const override {
        return {240, 360};
    }
    QSize minimumSizeHint() const override {
        return {190, 220};
    }

  signals:
    void orbitalClicked(int index);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    bool event(QEvent *event) override;

  private:
    int levelAt(const QPointF &position) const;
    QVector<ResultOrbital> orbitals;
    QStringList spins;
    QVector<int> shown; // Indices of the orbitals in the window around the frontier orbitals.
    int below = 0, above = 0, selected = -1;
    QVector<std::pair<int, QRectF>> bars; // Orbital index and bar of the last paint, for clicks.
};
} // namespace mf
