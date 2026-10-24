#include "ui/result_charts.h"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace mf {
namespace {
// About five round tick values (1, 2, 5 × 10^n steps) covering [lo, hi].
QVector<double> ticks(double lo, double hi) {
    const double span = hi - lo;
    if (!(span > 0))
        return {lo};
    const double raw = span / 5, magnitude = std::pow(10, std::floor(std::log10(raw)));
    const double step = raw / magnitude < 1.5 ? magnitude
                        : raw / magnitude < 3 ? 2 * magnitude
                        : raw / magnitude < 7 ? 5 * magnitude
                                              : 10 * magnitude;
    QVector<double> values;
    for (double v = std::ceil(lo / step) * step; v <= hi + step * 1e-9 && values.size() < 20; v += step)
        values << (std::abs(v) < step * 1e-9 ? 0 : v);
    return values;
}
QString tickText(double value, bool logY) {
    if (logY)
        return QString("1e%1").arg(int(std::round(value)));
    return QString::number(value, 'g', 4);
}
} // namespace
ChartView::ChartView(QWidget *parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}
void ChartView::setChart(const QString &t, const QString &x, const QString &y, const QVector<ChartSeries> &s,
                         bool log, const QVector<QPair<QString, double>> &l) {
    title = t;
    xLabel = x;
    yLabel = y;
    series = s;
    logY = log;
    levels = l;
    update();
}
void ChartView::clear() {
    setChart({}, {}, {}, {});
}
void ChartView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().base());
    const QColor text = palette().color(QPalette::Text), grid = palette().color(QPalette::Mid);
    const auto transformY = [&](double y) { return logY ? std::log10(y) : y; };
    double x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
    for (const auto &s : series)
        for (const auto &point : s.points) {
            x0 = std::min(x0, point.x()); // Keep the x range even where a log axis cannot show y.
            x1 = std::max(x1, point.x());
            if (logY && !(point.y() > 0))
                continue;
            y0 = std::min(y0, transformY(s.sticks ? std::min(point.y(), 0.) : point.y()));
            y1 = std::max(y1, transformY(point.y()));
        }
    for (const auto &[label, level] : levels)
        if (!logY || level > 0) {
            y0 = std::min(y0, transformY(level));
            y1 = std::max(y1, transformY(level));
        }
    p.setPen(text);
    if (!std::isfinite(x0) || !std::isfinite(y0)) {
        p.drawText(rect(), Qt::AlignCenter, "表示するデータがありません");
        return;
    }
    if (x1 - x0 < 1e-12) {
        x0 -= 0.5;
        x1 += 0.5;
    }
    if (y1 - y0 < 1e-12) {
        y0 -= logY ? 0.5 : std::max(1e-12, std::abs(y0) * 0.1 + 1e-6);
        y1 += logY ? 0.5 : std::max(1e-12, std::abs(y1) * 0.1 + 1e-6);
    }
    if (logY) {
        y0 = std::floor(y0);
        y1 = std::ceil(y1);
    } else {
        const double pad = (y1 - y0) * 0.06;
        y0 -= y0 == 0 ? 0 : pad;
        y1 += pad;
    }
    const auto metrics = fontMetrics();
    const QRectF plot(64, metrics.height() * 1.8, width() - 64 - 14, height() - metrics.height() * 4.2);
    if (plot.width() < 20 || plot.height() < 20)
        return;
    const auto toScreen = [&](double x, double y) {
        return QPointF(plot.left() + (x - x0) / (x1 - x0) * plot.width(),
                       plot.bottom() - (transformY(y) - y0) / (y1 - y0) * plot.height());
    };
    p.drawText(QRectF(0, 0, width(), metrics.height() * 1.5), Qt::AlignCenter, title);
    // Grid and tick labels.
    QVector<double> yTicks;
    if (logY)
        for (int e = int(y0); e <= int(y1) && yTicks.size() < 30; ++e)
            yTicks << e;
    else
        yTicks = ticks(y0, y1);
    for (double v : yTicks) {
        const double y = plot.bottom() - (v - y0) / (y1 - y0) * plot.height();
        p.setPen(QPen(grid, 1, Qt::DotLine));
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        p.setPen(text);
        p.drawText(QRectF(0, y - metrics.height() / 2., plot.left() - 6, metrics.height()),
                   Qt::AlignRight | Qt::AlignVCenter, tickText(v, logY));
    }
    for (double v : ticks(x0, x1)) {
        const double x = plot.left() + (v - x0) / (x1 - x0) * plot.width();
        p.setPen(QPen(grid, 1, Qt::DotLine));
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        p.setPen(text);
        p.drawText(QRectF(x - 40, plot.bottom() + 2, 80, metrics.height()), Qt::AlignCenter,
                   tickText(v, false));
    }
    p.setPen(text);
    p.drawRect(plot);
    p.drawText(QRectF(plot.left(), plot.bottom() + metrics.height() + 2, plot.width(), metrics.height()),
               Qt::AlignCenter, xLabel);
    p.save();
    p.translate(metrics.height() * 0.6, plot.center().y());
    p.rotate(-90);
    p.drawText(QRectF(-plot.height() / 2, -metrics.height() / 2., plot.height(), metrics.height()),
               Qt::AlignCenter, yLabel);
    p.restore();
    p.setClipRect(plot.adjusted(-1, -1, 1, 1));
    for (int i = 0; i < levels.size(); ++i) {
        const auto &[label, level] = levels[i];
        if (logY && !(level > 0))
            continue;
        const double y = toScreen(x0, level).y();
        p.setPen(QPen(text, 1, Qt::DashLine));
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        // Alternate sides so nearby reference lines keep readable labels.
        p.drawText(QRectF(plot.left() + 4, y - metrics.height(), plot.width() - 8, metrics.height()),
                   (i % 2 ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignBottom, label);
    }
    for (const auto &s : series) {
        p.setPen(QPen(s.color, s.sticks ? 1.5 : 2));
        if (s.sticks) {
            for (const auto &point : s.points)
                p.drawLine(toScreen(point.x(), logY ? std::pow(10, y0) : 0), toScreen(point.x(), point.y()));
            continue;
        }
        QPainterPath path;
        bool started = false;
        for (const auto &point : s.points) {
            if (logY && !(point.y() > 0)) {
                started = false;
                continue;
            }
            const auto screen = toScreen(point.x(), point.y());
            if (started)
                path.lineTo(screen);
            else
                path.moveTo(screen);
            started = true;
            if (s.points.size() <= 60)
                p.drawEllipse(screen, 2.5, 2.5);
        }
        p.drawPath(path);
    }
    p.setClipping(false);
    // Legend in the top-right corner of the plot.
    double y = plot.top() + 4;
    for (const auto &s : series) {
        if (s.name.isEmpty())
            continue;
        const double w = metrics.horizontalAdvance(s.name);
        p.setPen(QPen(s.color, 2));
        p.drawLine(QPointF(plot.right() - w - 30, y + metrics.height() / 2.),
                   QPointF(plot.right() - w - 12, y + metrics.height() / 2.));
        p.setPen(text);
        p.drawText(QPointF(plot.right() - w - 6, y + metrics.ascent()), s.name);
        y += metrics.height();
    }
}
} // namespace mf
