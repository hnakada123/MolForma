#include "ui/result_charts.h"
#include "results/molden_orbitals.h"
#include "ui/editor_chrome.h"
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <algorithm>
#include <cmath>

namespace mf {
namespace {
constexpr double HartreeToEv = 27.211386245988;
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
// The name of an orbital relative to its spin's frontier orbitals: HOMO−2, LUMO+1, …
} // namespace
QString frontierName(const QVector<ResultOrbital> &orbitals, int index) {
    if (index < 0 || index >= orbitals.size())
        return {};
    // Count within the occupied (or the empty) orbitals of the spin, in the order frontierOrbital
    // uses for equal energies: the last of them is HOMO, the first LUMO.
    const auto &o = orbitals[index];
    const bool occupied = o.occupation > 1e-8;
    int rank = 0;
    for (int i = 0; i < orbitals.size(); ++i) {
        const auto &other = orbitals[i];
        if (i == index || other.spin != o.spin || (other.occupation > 1e-8) != occupied)
            continue;
        if (occupied ? other.energy > o.energy || (other.energy == o.energy && i > index)
                     : other.energy < o.energy || (other.energy == o.energy && i < index))
            ++rank;
    }
    if (occupied)
        return rank ? QString("HOMO−%1").arg(rank) : QString("HOMO");
    return rank ? QString("LUMO+%1").arg(rank) : QString("LUMO");
}

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
    selectedSeries = selectedIndex = -1;
    update();
}
void ChartView::clear() {
    setChart({}, {}, {}, {});
}
void ChartView::setReversedX(bool on) {
    reversed = on;
    update();
}
void ChartView::setSelectedStick(int s, int index) {
    if (s == selectedSeries && index == selectedIndex)
        return;
    selectedSeries = s;
    selectedIndex = index;
    update();
}
double ChartView::screenX(double x) const {
    const double share = (x - x0) / (x1 - x0);
    return reversed ? plot.right() - share * plot.width() : plot.left() + share * plot.width();
}
void ChartView::mousePressEvent(QMouseEvent *event) {
    QWidget::mousePressEvent(event);
    if (event->button() != Qt::LeftButton || plot.isEmpty())
        return;
    // The nearest stick within a few pixels of the click.
    int bestSeries = -1, bestIndex = -1;
    double best = 7;
    for (int s = 0; s < series.size(); ++s) {
        if (!series[s].sticks)
            continue;
        for (int i = 0; i < series[s].points.size(); ++i)
            if (const double d = std::abs(screenX(series[s].points[i].x()) - event->position().x());
                d < best) {
                best = d;
                bestSeries = s;
                bestIndex = i;
            }
    }
    if (bestSeries >= 0)
        emit stickClicked(bestSeries, bestIndex);
}
void ChartView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().base());
    const QColor text = palette().color(QPalette::Text), grid = palette().color(QPalette::Mid);
    const auto transformY = [&](double y) { return logY ? std::log10(y) : y; };
    double y0 = INFINITY, y1 = -INFINITY;
    x0 = INFINITY;
    x1 = -INFINITY;
    plot = QRectF();
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
        p.drawText(rect(), Qt::AlignCenter, tr("表示するデータがありません"));
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
    const QRectF area(64, metrics.height() * 1.8, width() - 64 - 14, height() - metrics.height() * 4.2);
    if (area.width() < 20 || area.height() < 20)
        return;
    plot = area;
    const auto toScreen = [&](double x, double y) {
        return QPointF(screenX(x), plot.bottom() - (transformY(y) - y0) / (y1 - y0) * plot.height());
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
        const double x = screenX(v);
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
    for (int index = 0; index < series.size(); ++index) {
        const auto &s = series[index];
        p.setPen(QPen(s.color, s.sticks ? 1.5 : 2));
        if (s.sticks) {
            for (const auto &point : s.points)
                p.drawLine(toScreen(point.x(), logY ? std::pow(10, y0) : 0), toScreen(point.x(), point.y()));
            if (index == selectedSeries && selectedIndex >= 0 && selectedIndex < s.points.size()) {
                const auto &point = s.points[selectedIndex];
                p.setPen(QPen(ChromeColors(palette()).accent, 3.5));
                p.drawLine(toScreen(point.x(), logY ? std::pow(10, y0) : 0), toScreen(point.x(), point.y()));
            }
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

FrameTimeline::FrameTimeline(QWidget *parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("構造列のタイムライン"));
}
void FrameTimeline::setFrames(int n, const QVector<double> &values) {
    count = std::max(0, n);
    energies = values;
    energies.resize(count, std::numeric_limits<double>::quiet_NaN());
    now = std::clamp(now, count ? 0 : -1, count - 1);
    update();
}
void FrameTimeline::setCurrent(int index) {
    index = count ? std::clamp(index, 0, count - 1) : -1;
    if (index == now)
        return;
    now = index;
    setAccessibleDescription(count ? QString("%1 / %2").arg(now + 1).arg(count) : QString());
    update();
}
QRectF FrameTimeline::track() const {
    return QRectF(rect()).adjusted(9, 4, -9, -4);
}
double FrameTimeline::xOf(int index) const {
    const auto area = track();
    return count > 1 ? area.left() + area.width() * index / (count - 1) : area.center().x();
}
void FrameTimeline::pick(double x) {
    if (count < 1 || !isEnabled())
        return;
    const auto area = track();
    const int index =
        count > 1 ? int(std::lround(std::clamp((x - area.left()) / area.width(), 0., 1.) * (count - 1))) : 0;
    if (index != now) {
        setCurrent(index);
        emit frameSelected(index);
    }
}
void FrameTimeline::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton)
        pick(event->position().x());
}
void FrameTimeline::mouseMoveEvent(QMouseEvent *event) {
    if (event->buttons() & Qt::LeftButton)
        pick(event->position().x());
}
void FrameTimeline::keyPressEvent(QKeyEvent *event) {
    const int target = event->key() == Qt::Key_Left || event->key() == Qt::Key_Down  ? now - 1
                       : event->key() == Qt::Key_Right || event->key() == Qt::Key_Up ? now + 1
                       : event->key() == Qt::Key_Home                                ? 0
                       : event->key() == Qt::Key_End                                 ? count - 1
                                                                                     : -2;
    if (target == -2 || count < 1) {
        QWidget::keyPressEvent(event);
        return;
    }
    if (std::clamp(target, 0, count - 1) != now) {
        setCurrent(target);
        emit frameSelected(now);
    }
}
void FrameTimeline::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ChromeColors c(palette());
    const bool on = isEnabled() && count > 0;
    const auto area = track();
    const double y = area.bottom() - 6;
    p.setPen(QPen(c.line, 4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    if (!on)
        return;
    if (now > 0) {
        p.setPen(QPen(c.accent, 4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(area.left(), y), QPointF(xOf(now), y));
    }
    if (count <= 80) {
        p.setPen(Qt::NoPen);
        p.setBrush(c.muted);
        for (int i = 0; i < count; ++i)
            if (i != now)
                p.drawEllipse(QPointF(xOf(i), y), 1.6, 1.6);
    }
    // The known energies as a line above the track (lower is lower on screen).
    double lo = INFINITY, hi = -INFINITY;
    int known = 0;
    for (double e : std::as_const(energies))
        if (std::isfinite(e)) {
            lo = std::min(lo, e);
            hi = std::max(hi, e);
            ++known;
        }
    if (known >= 2) {
        const double top = area.top() + 2, bottom = y - 8;
        const auto at = [&](int i) {
            return QPointF(xOf(i), hi - lo > 1e-12 ? bottom - (energies[i] - lo) / (hi - lo) * (bottom - top)
                                                   : (top + bottom) / 2);
        };
        QPainterPath path;
        bool started = false;
        for (int i = 0; i < count; ++i) {
            if (!std::isfinite(energies[i]))
                continue;
            started ? path.lineTo(at(i)) : path.moveTo(at(i));
            started = true;
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(c.accent, 1.4));
        p.drawPath(path);
        if (std::isfinite(energies.value(now))) {
            p.setBrush(c.accent);
            p.setPen(Qt::NoPen);
            p.drawEllipse(at(now), 2.6, 2.6);
        }
    }
    p.setBrush(c.base);
    p.setPen(QPen(c.accent, 2.5));
    p.drawEllipse(QPointF(xOf(now), y), 5.5, 5.5);
    if (hasFocus()) {
        p.setPen(QPen(c.accent, 1, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
    }
}

OrbitalLevels::OrbitalLevels(QWidget *parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setMouseTracking(false);
    setAccessibleName(tr("軌道エネルギー準位図"));
}
void OrbitalLevels::setOrbitals(const QVector<ResultOrbital> &list, int index) {
    orbitals = list;
    selected = index;
    shown.clear();
    spins.clear();
    below = above = 0;
    for (const auto &o : list)
        if (!spins.contains(o.spin))
            spins << o.spin;
    // Six levels below HOMO and above LUMO of each spin, within 25 eV below HOMO and 15 eV above
    // LUMO (no core orbitals squeezing the valence levels together); the rest only as counts.
    for (const auto &spin : std::as_const(spins)) {
        QVector<int> order;
        for (int i = 0; i < list.size(); ++i)
            if (list[i].spin == spin)
                order << i;
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return list[a].energy < list[b].energy; });
        const int homo = order.indexOf(frontierOrbital(list, spin)),
                  lumo = order.indexOf(frontierOrbital(list, spin, true));
        int lo = 0, hi = int(order.size()) - 1;
        if (homo >= 0 || lumo >= 0) {
            lo = std::max(0, (homo >= 0 ? homo : lumo) - 6);
            hi = std::min(hi, (lumo >= 0 ? lumo : homo) + 6);
        } else
            hi = std::min(hi, 12);
        const double floor = homo >= 0 ? list[order[homo]].energy - 25 / HartreeToEv : -INFINITY,
                     ceiling = lumo >= 0 ? list[order[lumo]].energy + 15 / HartreeToEv : INFINITY;
        while (lo < (homo >= 0 ? homo : lumo) && list[order[lo]].energy < floor)
            ++lo;
        while (hi > (lumo >= 0 ? lumo : homo) && list[order[hi]].energy > ceiling)
            --hi;
        below += lo;
        above += int(order.size()) - 1 - hi;
        for (int k = lo; k <= hi; ++k)
            shown << order[k];
    }
    bars.clear();
    update();
}
void OrbitalLevels::setSelected(int index) {
    if (index != selected) {
        selected = index;
        update();
    }
}
int OrbitalLevels::levelAt(const QPointF &position) const {
    int found = -1;
    double best = INFINITY;
    for (const auto &[index, bar] : bars)
        if (bar.adjusted(-2, -6, 2, 6).contains(position))
            if (const double d = std::abs(bar.center().y() - position.y()); d < best) {
                best = d;
                found = index;
            }
    return found;
}
void OrbitalLevels::mousePressEvent(QMouseEvent *event) {
    QWidget::mousePressEvent(event);
    if (event->button() == Qt::LeftButton)
        if (const int index = levelAt(event->position()); index >= 0)
            emit orbitalClicked(index);
}
bool OrbitalLevels::event(QEvent *event) {
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int index = levelAt(help->pos());
        if (index < 0) {
            QToolTip::hideText();
            event->ignore();
            return true;
        }
        const auto &o = orbitals[index];
        QToolTip::showText(help->globalPos(),
                           tr("%1 %2番 %3\n%4 eV（%5 Eh）、占有数 %6")
                               .arg(o.spin)
                               .arg(o.number > 0 ? o.number : index + 1)
                               .arg(frontierName(orbitals, index))
                               .arg(o.energy * HartreeToEv, 0, 'f', 3)
                               .arg(o.energy, 0, 'g', 8)
                               .arg(o.occupation, 0, 'g', 4),
                           this);
        return true;
    }
    return QWidget::event(event);
}
void OrbitalLevels::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ChromeColors c(palette());
    p.fillRect(rect(), c.base);
    bars.clear();
    const auto metrics = fontMetrics();
    if (shown.isEmpty()) {
        p.setPen(c.muted);
        p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, tr("軌道がありません"));
        return;
    }
    double lo = INFINITY, hi = -INFINITY;
    for (int i : std::as_const(shown)) {
        lo = std::min(lo, orbitals[i].energy * HartreeToEv);
        hi = std::max(hi, orbitals[i].energy * HartreeToEv);
    }
    if (hi - lo < 1) {
        lo -= .5;
        hi += .5;
    }
    const double pad = (hi - lo) * .04;
    lo -= pad;
    hi += pad;
    const double line = metrics.height();
    const QRectF area(42, line * 1.9, width() - 42 - 6, height() - line * 3.8);
    if (area.height() < 40)
        return;
    const auto yOf = [&](double ev) { return area.bottom() - (ev - lo) / (hi - lo) * area.height(); };
    // Energy axis.
    p.setPen(c.line);
    p.drawLine(QPointF(area.left() - 6, area.top()), QPointF(area.left() - 6, area.bottom()));
    for (double v : ticks(lo, hi)) {
        const double y = yOf(v);
        p.setPen(c.line);
        p.drawLine(QPointF(area.left() - 10, y), QPointF(area.left() - 6, y));
        p.setPen(c.muted);
        p.drawText(QRectF(0, y - line / 2, area.left() - 12, line), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(v, 'g', 4).replace('-', QChar(0x2212)));
    }
    p.setPen(c.muted);
    p.drawText(QRectF(0, 0, area.left() - 6, line * 1.4), Qt::AlignRight | Qt::AlignVCenter, "eV");
    if (above)
        p.drawText(QRectF(area.left(), 0, area.width(), line * 1.4), Qt::AlignLeft | Qt::AlignVCenter,
                   tr("▲ ほか %n 個", nullptr, above));
    if (below)
        p.drawText(QRectF(area.left(), height() - line * 1.6, area.width(), line * 1.4),
                   Qt::AlignLeft | Qt::AlignVCenter, tr("▼ ほか %n 個", nullptr, below));
    const double column = area.width() / spins.size();
    const bool labelled = spins.size() == 1; // Names and energies beside the levels of one spin.
    const double barWidth = std::min(64., column * (labelled ? .42 : .7));
    QVector<QRectF> labels;
    for (int s = 0; s < spins.size(); ++s) {
        const double left = area.left() + s * column + 4;
        if (spins.size() > 1) {
            p.setPen(c.muted);
            p.drawText(QRectF(left, line * .4, barWidth, line), Qt::AlignCenter, spins[s]);
        }
        QVector<int> levels;
        for (int i : std::as_const(shown))
            if (orbitals[i].spin == spins[s])
                levels << i;
        // Levels closer than a few pixels (degenerate orbitals) side by side.
        QVector<QVector<int>> groups;
        for (int i : std::as_const(levels)) {
            const double y = yOf(orbitals[i].energy * HartreeToEv);
            if (!groups.isEmpty() &&
                std::abs(yOf(orbitals[groups.last().first()].energy * HartreeToEv) - y) < 4)
                groups.last() << i;
            else
                groups << QVector<int>{i};
        }
        const int homo = frontierOrbital(orbitals, spins[s]),
                  lumo = frontierOrbital(orbitals, spins[s], true);
        for (const auto &group : std::as_const(groups)) {
            const double y = yOf(orbitals[group.first()].energy * HartreeToEv);
            const double w = (barWidth - (group.size() - 1) * 3) / group.size();
            if (group.contains(selected)) {
                p.setPen(Qt::NoPen);
                p.setBrush(c.tint);
                p.drawRoundedRect(QRectF(left - 4, y - 9, column - 4, 18), 4, 4);
            }
            for (int k = 0; k < group.size(); ++k) {
                const int i = group[k];
                const auto &o = orbitals[i];
                const QRectF bar(left + k * (w + 3), y - 1.5, w, 3);
                bars.append({i, bar});
                const QColor color = i == selected ? c.accent : c.text;
                if (o.occupation > 1e-8) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(color);
                    p.drawRect(bar);
                } else {
                    p.setPen(QPen(i == selected ? c.accent : c.muted, 1.2));
                    p.setBrush(c.base);
                    p.drawRect(bar.adjusted(0, -.5, 0, .5));
                }
                // Electrons as arrows: two for a doubly occupied orbital, one for a single electron.
                const int electrons = o.occupation >= 1.5 ? 2 : o.occupation >= .5 ? 1 : 0;
                if (electrons && w >= 14) {
                    p.setPen(QPen(color, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    p.setBrush(Qt::NoBrush);
                    for (int e = 0; e < electrons; ++e) {
                        const double x = bar.center().x() + (electrons == 2 ? (e ? 3.5 : -3.5) : 0);
                        const bool up = e == 0;
                        const double tip = up ? y - 7 : y + 5, tail = up ? y + 5 : y - 7;
                        p.drawLine(QPointF(x, tail), QPointF(x, tip));
                        const double back = up ? 2.5 : -2.5;
                        p.drawLine(QPointF(x - 2.2, tip + back), QPointF(x, tip));
                        p.drawLine(QPointF(x + 2.2, tip + back), QPointF(x, tip));
                    }
                }
            }
            if (!labelled)
                continue;
            int named = -1;
            for (int i : group)
                if (i == homo || i == lumo || (i == selected && named < 0))
                    named = i;
            if (named < 0)
                continue;
            const auto name = frontierName(orbitals, named);
            const QRectF label(left + barWidth + 8, y - line / 2, column - barWidth - 14, line);
            if (std::any_of(labels.begin(), labels.end(),
                            [&](const QRectF &r) { return r.intersects(label); }))
                continue;
            labels << label;
            p.setPen(group.contains(selected) ? c.accentText : c.text);
            p.drawText(label, Qt::AlignLeft | Qt::AlignVCenter, name);
            p.setPen(group.contains(selected) ? c.accentText : c.muted);
            p.drawText(
                label, Qt::AlignRight | Qt::AlignVCenter,
                QString::number(orbitals[named].energy * HartreeToEv, 'f', 2).replace('-', QChar(0x2212)));
        }
        // The HOMO–LUMO gap of one spin, between its two levels.
        if (labelled && homo >= 0 && lumo >= 0 && shown.contains(homo) && shown.contains(lumo)) {
            const double top = yOf(orbitals[lumo].energy * HartreeToEv),
                         bottom = yOf(orbitals[homo].energy * HartreeToEv);
            if (bottom - top > line * 1.6) {
                const double x = left + barWidth / 2;
                p.setPen(QPen(c.muted, 1, Qt::DashLine));
                p.drawLine(QPointF(x, top + 5), QPointF(x, bottom - 9));
                p.setPen(c.text);
                const double gap = (orbitals[lumo].energy - orbitals[homo].energy) * HartreeToEv;
                p.drawText(QRectF(x + 6, (top + bottom) / 2 - line / 2, area.right() - x - 6, line),
                           Qt::AlignLeft | Qt::AlignVCenter, tr("ギャップ %1 eV").arg(gap, 0, 'f', 2));
            }
        }
    }
}
} // namespace mf
