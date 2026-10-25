// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/editor_chrome.h"
#include "ui/theme.h"
#include <QApplication>
#include <QGridLayout>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QSpacerItem>
#include <algorithm>
#include <cmath>

namespace mf {
namespace {
QColor mix(const QColor &a, const QColor &b, double share) { // share of a
    return QColor::fromRgbF(float(a.redF() * share + b.redF() * (1 - share)),
                            float(a.greenF() * share + b.greenF() * (1 - share)),
                            float(a.blueF() * share + b.blueF() * (1 - share)));
}
double luminance(const QColor &c) {
    const auto channel = [](double v) { return v <= .03928 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    return .2126 * channel(c.redF()) + .7152 * channel(c.greenF()) + .0722 * channel(c.blueF());
}
double contrast(const QColor &a, const QColor &b) {
    auto x = luminance(a), y = luminance(b);
    if (x < y)
        std::swap(x, y);
    return (x + .05) / (y + .05);
}
// The first color on the way from `from` to `to` that reads at 4.5:1 on the background.
QColor readable(const QColor &from, const QColor &to, const QColor &background) {
    for (int step = 0; step <= 10; ++step)
        if (const auto c = mix(to, from, step / 10.); contrast(c, background) >= 4.5)
            return c;
    return to;
}

QPainterPath iconPath(const QString &name, bool &filled) {
    QPainterPath p;
    const auto line = [&p](std::initializer_list<QPointF> points, bool close = false) {
        auto point = points.begin();
        p.moveTo(*point);
        for (++point; point != points.end(); ++point)
            p.lineTo(*point);
        if (close)
            p.closeSubpath();
    };
    const auto circle = [&p](QPointF center, double radius) { p.addEllipse(center, radius, radius); };
    filled = false;
    if (name == "select")
        line({{6, 3.5}, {18, 10.7}, {12.7, 12.3}, {10.3, 17.5}}, true);
    else if (name == "move") {
        line({{12, 3}, {12, 21}});
        line({{3, 12}, {21, 12}});
        line({{9, 6}, {12, 3}, {15, 6}});
        line({{9, 18}, {12, 21}, {15, 18}});
        line({{6, 9}, {3, 12}, {6, 15}});
        line({{18, 9}, {21, 12}, {18, 15}});
    } else if (name == "bond") {
        circle({5.5, 16}, 2.5);
        circle({18.5, 8}, 2.5);
        line({{7.6, 14.7}, {16.4, 9.3}});
    } else if (name == "delete") {
        line({{4, 7}, {20, 7}});
        line({{10, 11}, {10, 17}});
        line({{14, 11}, {14, 17}});
        line({{6, 7}, {7, 20}, {17, 20}, {18, 7}});
        line({{9, 7}, {9, 4}, {15, 4}, {15, 7}});
    } else if (name == "place") {
        circle({12, 12}, 8);
        line({{12, 8}, {12, 16}});
        line({{8, 12}, {16, 12}});
    } else if (name == "replace") {
        line({{4, 9}, {17, 9}, {13.5, 5.5}});
        line({{20, 15}, {7, 15}, {10.5, 18.5}});
    } else if (name == "charge") {
        circle({11, 14}, 4.5);
        line({{18, 3}, {18, 8}});
        line({{15.5, 5.5}, {20.5, 5.5}});
    } else if (name == "undo") {
        line({{9, 14}, {4, 9}, {9, 4}});
        p.moveTo(4, 9);
        p.lineTo(14, 9);
        p.arcTo(QRectF(8, 9, 12, 12), 90, -180);
        p.lineTo(11, 21);
    } else if (name == "redo") {
        line({{15, 14}, {20, 9}, {15, 4}});
        p.moveTo(20, 9);
        p.lineTo(10, 9);
        p.arcTo(QRectF(4, 9, 12, 12), 90, 180);
        p.lineTo(13, 21);
    } else if (name == "play") {
        filled = true;
        line({{7, 4.5}, {7, 19.5}, {19.5, 12}}, true);
    } else if (name == "fit") {
        line({{4, 9}, {4, 4}, {9, 4}});
        line({{20, 9}, {20, 4}, {15, 4}});
        line({{4, 15}, {4, 20}, {9, 20}});
        line({{20, 15}, {20, 20}, {15, 20}});
        circle({12, 12}, 2.5);
    } else if (name == "rotate") {
        const QRectF ring(5, 5, 14, 14);
        p.arcMoveTo(ring, 60);
        p.arcTo(ring, 60, 270);
        line({{14.4, 17.2}, {18.1, 15.5}, {18.4, 19.5}});
    } else if (name == "eye") {
        p.moveTo(2.5, 12);
        p.quadTo(12, 3.5, 21.5, 12);
        p.quadTo(12, 20.5, 2.5, 12);
        circle({12, 12}, 3);
    } else if (name == "layers") {
        line({{12, 3}, {21, 8}, {12, 13}, {3, 8}}, true);
        line({{3, 13}, {12, 18}, {21, 13}});
    } else if (name == "document") {
        line({{6, 3}, {14, 3}, {19, 8}, {19, 21}, {6, 21}}, true);
        line({{14, 3}, {14, 8}, {19, 8}});
    } else if (name == "search") {
        circle({10.5, 10.5}, 6.5);
        line({{15.5, 15.5}, {20, 20}});
    } else if (name == "folder")
        line({{3, 6}, {9.5, 6}, {11.5, 8.5}, {21, 8.5}, {21, 19}, {3, 19}}, true);
    else if (name == "refresh") {
        const QRectF ring(5, 5, 14, 14);
        p.arcMoveTo(ring, 20);
        p.arcTo(ring, 20, 290);
        line({{19.5, 4.5}, {19.5, 9.5}, {14.5, 9.5}});
    } else if (name == "export") { // An arrow up out of a tray.
        line({{12, 15}, {12, 4}});
        line({{7.5, 8.5}, {12, 4}, {16.5, 8.5}});
        line({{4, 14}, {4, 20}, {20, 20}, {20, 14}});
    } else if (name == "apply") { // An arrow down into a tray.
        line({{12, 4}, {12, 15}});
        line({{7.5, 10.5}, {12, 15}, {16.5, 10.5}});
        line({{4, 14}, {4, 20}, {20, 20}, {20, 14}});
    } else if (name == "compare") {
        circle({9, 12}, 5.5);
        circle({15, 12}, 5.5);
    } else if (name == "pause") {
        filled = true;
        p.addRect(QRectF(6.5, 5, 4, 14));
        p.addRect(QRectF(13.5, 5, 4, 14));
    } else if (name == "first" || name == "last") {
        filled = true;
        const bool first = name == "first";
        const double x = first ? 0 : 24, s = first ? 1 : -1;
        p.addRect(QRectF(first ? 5 : 17, 5, 2, 14));
        line({{x + s * 19, 5}, {x + s * 19, 19}, {x + s * 8.5, 12}}, true);
    } else if (name == "previous")
        line({{15, 5}, {8, 12}, {15, 19}});
    else if (name == "next")
        line({{9, 5}, {16, 12}, {9, 19}});
    else if (name == "back10") {
        line({{12, 5}, {5, 12}, {12, 19}});
        line({{19, 5}, {12, 12}, {19, 19}});
    } else if (name == "forward10") {
        line({{5, 5}, {12, 12}, {5, 19}});
        line({{12, 5}, {19, 12}, {12, 19}});
    } else if (name == "ruler") {
        line({{3, 16}, {16, 3}, {21, 8}, {8, 21}}, true);
        line({{7, 12}, {9, 14}});
        line({{10, 9}, {12, 11}});
        line({{13, 6}, {15, 8}});
    } else if (name == "label") { // The letter A.
        line({{5, 20}, {12, 4}, {19, 20}});
        line({{8, 14}, {16, 14}});
    } else if (name == "warning") {
        line({{12, 3.5}, {21.5, 20}, {2.5, 20}}, true);
        line({{12, 9.5}, {12, 14}});
        line({{12, 17}, {12, 17.2}});
    } else if (name == "check")
        line({{5, 12.5}, {10, 17.5}, {19, 6.5}});
    else if (name == "close") {
        line({{6, 6}, {18, 18}});
        line({{18, 6}, {6, 18}});
    } else if (name == "plus") {
        line({{12, 5}, {12, 19}});
        line({{5, 12}, {19, 12}});
    } else if (name == "lock") {
        p.addRoundedRect(QRectF(5, 11, 14, 10), 2, 2);
        p.moveTo(8, 11);
        p.lineTo(8, 8);
        p.arcTo(QRectF(8, 4, 8, 8), 180, -180);
        p.lineTo(16, 11);
    } else if (name == "ghost") {
        p.moveTo(4, 21);
        p.lineTo(4, 10);
        p.arcTo(QRectF(4, 2, 16, 16), 180, -180);
        p.lineTo(20, 21);
        p.lineTo(17, 18.5);
        p.lineTo(14.5, 21);
        p.lineTo(12, 18.5);
        p.lineTo(9.5, 21);
        p.lineTo(7, 18.5);
        p.closeSubpath();
        line({{9, 10}, {9, 10.2}});
        line({{15, 10}, {15, 10.2}});
    } else if (name == "copy") {
        p.addRoundedRect(QRectF(9, 9, 12, 12), 2, 2);
        line({{5, 15}, {3, 15}, {3, 3}, {15, 3}, {15, 5}});
    } else if (name == "paste") {
        p.addRoundedRect(QRectF(8, 2.5, 8, 4), 1, 1);
        line({{16, 4.5}, {19, 4.5}, {19, 21}, {5, 21}, {5, 4.5}, {8, 4.5}});
    } else if (name == "import") { // A document with an arrow down into it.
        line({{6, 3}, {14, 3}, {19, 8}, {19, 21}, {6, 21}}, true);
        line({{12, 10}, {12, 17}});
        line({{9, 14}, {12, 17}, {15, 14}});
    } else if (name == "origin") {
        circle({12, 12}, 6.5);
        line({{12, 2}, {12, 7}});
        line({{12, 17}, {12, 22}});
        line({{2, 12}, {7, 12}});
        line({{17, 12}, {22, 12}});
    }
    return p;
}

class LineIconEngine : public QIconEngine {
  public:
    LineIconEngine(const QString &name, const QColor &fixed) : name(name), fixed(fixed) {}
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
        bool filled;
        const auto path = iconPath(name, filled);
        const auto palette = QApplication::palette();
        const ChromeColors colors(palette);
        const QColor color = fixed.isValid()           ? fixed
                             : mode == QIcon::Disabled ? palette.color(QPalette::Disabled, QPalette::Text)
                             : mode == QIcon::Selected ? palette.color(QPalette::HighlightedText)
                             : state == QIcon::On      ? colors.accentText
                                                       : colors.text;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const double scale = std::min(rect.width(), rect.height()) / 24.;
        painter->translate(QRectF(rect).center() - QPointF(12 * scale, 12 * scale));
        painter->scale(scale, scale);
        if (filled) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(color);
        } else {
            painter->setPen(QPen(color, 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush);
        }
        painter->drawPath(path);
        painter->restore();
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1);
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QPixmap pixmap(size * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return pixmap;
    }
    QIconEngine *clone() const override {
        return new LineIconEngine(*this);
    }

  private:
    QString name;
    QColor fixed;
};

class StepIconEngine : public QIconEngine {
  public:
    explicit StepIconEngine(int number) : number(number) {}
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
        const ChromeColors colors(QApplication::palette());
        const QRectF circle = QRectF(rect).adjusted(1, 1, -1, -1);
        const bool on = state == QIcon::On && mode != QIcon::Disabled;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(on ? Qt::NoPen : QPen(colors.muted, 1));
        painter->setBrush(on ? colors.accent : QColor(Qt::transparent));
        painter->drawEllipse(circle);
        QFont font = QApplication::font();
        font.setPixelSize(std::max(8, qRound(circle.height() * .62)));
        font.setBold(on);
        painter->setFont(font);
        painter->setPen(on ? colors.onAccent : colors.muted);
        painter->drawText(circle, Qt::AlignCenter, QString::number(number));
        painter->restore();
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1);
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QPixmap pixmap(size * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return pixmap;
    }
    QIconEngine *clone() const override {
        return new StepIconEngine(*this);
    }

  private:
    int number;
};
} // namespace

ChromeColors::ChromeColors(const QPalette &p) {
    base = p.color(QPalette::Base);
    window = p.color(QPalette::Window);
    text = p.color(QPalette::Text);
    const bool dark = window.lightness() < 128;
    // MolForma's own accent, the cobalt of the redesign, whatever the desktop's selection color
    // (a pale one would wash out the chosen tool and step); a little lighter on dark themes.
    accent = QColor(dark ? "#2f6fe4" : "#1d5fd1");
    onAccent = QColor(Qt::white);
    line = mix(text, base, .17);
    hover = mix(text, base, .07);
    segment = mix(text, window, .08);
    tint = mix(accent, base, .16);
    muted = readable(mix(text, base, .55), text, base);
    accentText = readable(accent, text, tint);
    candidate = QColor(dark ? "#3b2a12" : "#fff4e5");
    candidateLine = QColor(dark ? "#8a5a1c" : "#f3c98b");
    candidateText = QColor(dark ? "#fcd9a8" : "#5c2e04");
    good = goodColor(p);
    goodTint = mix(good, base, .13);
    bad = errorColor(p);
    badTint = mix(bad, base, .12);
}

QString chromeRules(QString rules, const QPalette &palette) {
    const ChromeColors c(palette);
    const auto accentHover = c.accent.lightness() > 128 ? c.accent.darker(112) : c.accent.lighter(115);
    // Longer names first, so that "@accentText" is not taken for "@accent" followed by "Text".
    const QList<std::pair<const char *, QColor>> tokens{{"@candidateLine", c.candidateLine},
                                                        {"@candidateText", c.candidateText},
                                                        {"@accentHover", accentHover},
                                                        {"@accentText", c.accentText},
                                                        {"@candidate", c.candidate},
                                                        {"@goodTint", c.goodTint},
                                                        {"@onAccent", c.onAccent},
                                                        {"@badTint", c.badTint},
                                                        {"@segment", c.segment},
                                                        {"@accent", c.accent},
                                                        {"@window", c.window},
                                                        {"@error", c.bad},
                                                        {"@hover", c.hover},
                                                        {"@muted", c.muted},
                                                        {"@good", c.good},
                                                        {"@base", c.base},
                                                        {"@line", c.line},
                                                        {"@tint", c.tint},
                                                        {"@text", c.text}};
    for (const auto &[token, color] : tokens)
        rules.replace(token, color.name());
    return rules;
}

QIcon lineIcon(const QString &name, const QColor &color) {
    return QIcon(new LineIconEngine(name, color));
}
QIcon stepIcon(int number) {
    return QIcon(new StepIconEngine(number));
}
QIcon changeIcon(const QString &kind) {
    // The colors of the markers that the 3D view draws for a candidate.
    const QColor color(kind == "added" ? "#25a35a" : kind == "moved" ? "#e08a1e" : "#d23c3c");
    QPixmap pixmap(QSize(28, 28));
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 3.5, Qt::SolidLine, Qt::RoundCap));
    if (kind == "removed") {
        p.drawLine(QPointF(7, 7), QPointF(21, 21));
        p.drawLine(QPointF(21, 7), QPointF(7, 21));
    } else
        p.drawEllipse(QPointF(14, 14), 9, 9);
    p.end();
    return QIcon(pixmap);
}

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

ElidedLabel::ElidedLabel(const QString &text, QWidget *parent) : QLabel(text, parent) {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}
QSize ElidedLabel::minimumSizeHint() const {
    return {0, fontMetrics().height()};
}
QSize ElidedLabel::sizeHint() const {
    return {fontMetrics().horizontalAdvance(text()), fontMetrics().height()};
}
void ElidedLabel::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setPen(palette().color(foregroundRole()));
    const auto shown = fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width());
    p.drawText(contentsRect(), int(alignment()) | Qt::TextSingleLine, shown);
}

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
