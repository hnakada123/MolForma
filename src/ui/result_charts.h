#pragma once
#include <QColor>
#include <QPointF>
#include <QVector>
#include <QWidget>

namespace mf {
struct ChartSeries {
    QString name;
    QVector<QPointF> points;
    QColor color;
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
    QSize sizeHint() const override {
        return {420, 220};
    }
    QSize minimumSizeHint() const override {
        return {240, 140};
    }

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    QString title, xLabel, yLabel;
    QVector<ChartSeries> series;
    QVector<QPair<QString, double>> levels;
    bool logY = false;
};
} // namespace mf
