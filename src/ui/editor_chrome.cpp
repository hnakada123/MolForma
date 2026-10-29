// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/editor_chrome.h"
#include "model/i18n.h"
#include "nwchem/nwchem.h"
#include "ui/molecule_view.h"
#include "ui/theme.h"
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QSlider>
#include <QSpacerItem>
#include <QStyle>
#include <QStyledItemDelegate>
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
    } else if (name == "preset") { // A bookmark.
        line({{6, 3}, {18, 3}, {18, 21}, {12, 17}, {6, 21}}, true);
    } else if (name == "reset") { // Back to the default: an arrow turning counterclockwise.
        const QRectF ring(4.5, 4, 16, 16);
        p.arcMoveTo(ring, 200);
        p.arcTo(ring, 200, 295);
        p.lineTo(3, 10.2);
        line({{3, 4.8}, {3, 10.2}, {8.4, 10.2}});
    } else if (name == "info") {
        circle({12, 12}, 9);
        line({{12, 11}, {12, 16.5}});
        line({{12, 7.8}, {12, 8}});
    } else if (name == "error") {
        circle({12, 12}, 9);
        line({{9, 9}, {15, 15}});
        line({{15, 9}, {9, 15}});
    } else if (name == "arrow") {
        line({{5, 12}, {19, 12}});
        line({{13, 6}, {19, 12}, {13, 18}});
    } else if (name == "stop") {
        filled = true;
        p.addRoundedRect(QRectF(6, 6, 12, 12), 2, 2);
    } else if (name == "new") { // A document with a plus.
        line({{6, 3}, {14, 3}, {19, 8}, {19, 21}, {6, 21}}, true);
        line({{14, 3}, {14, 8}, {19, 8}});
        line({{12.5, 11}, {12.5, 18}});
        line({{9, 14.5}, {16, 14.5}});
    } else if (name == "ring") { // An aromatic ring.
        line({{12, 3.5}, {19.4, 7.75}, {19.4, 16.25}, {12, 20.5}, {4.6, 16.25}, {4.6, 7.75}}, true);
        circle({12, 12}, 4);
    } else if (name == "ringAdd") {
        line({{10, 3}, {16.1, 6.5}, {16.1, 13.5}, {10, 17}, {3.9, 13.5}, {3.9, 6.5}}, true);
        line({{18.5, 15.5}, {18.5, 21.5}});
        line({{15.5, 18.5}, {21.5, 18.5}});
    } else if (name == "recent") { // A clock.
        circle({12, 12}, 8.5);
        line({{12, 7.5}, {12, 12}, {15.5, 14}});
    } else if (name == "history") { // A clock turning back.
        const QRectF ring(4.5, 4, 16, 16);
        p.arcMoveTo(ring, 200);
        p.arcTo(ring, 200, 295);
        line({{3, 4.8}, {3, 10.2}, {8.4, 10.2}});
        line({{12.5, 8}, {12.5, 12}, {15.5, 14}});
    } else if (name == "save") { // A floppy disk.
        line({{4, 4}, {16.5, 4}, {20, 7.5}, {20, 20}, {4, 20}}, true);
        line({{8, 4}, {8, 8.5}, {15, 8.5}, {15, 4}});
        line({{7.5, 20}, {7.5, 13.5}, {16.5, 13.5}, {16.5, 20}});
    } else if (name == "saveAs") { // A floppy disk and a pencil.
        line({{10, 17}, {3.5, 17}, {3.5, 3.5}, {14, 3.5}, {17, 6.5}, {17, 10}});
        line({{7, 3.5}, {7, 7.5}, {13, 7.5}, {13, 3.5}});
        line({{12.5, 21}, {15.8, 20.1}, {21.1, 14.8}, {18.7, 12.4}, {13.4, 17.7}}, true);
    } else if (name == "quit") { // An arrow out of a door.
        line({{13, 4}, {5, 4}, {5, 20}, {13, 20}});
        line({{10, 12}, {20.5, 12}});
        line({{16.5, 8}, {20.5, 12}, {16.5, 16}});
    } else if (name == "selectAll") { // A dashed frame.
        line({{4, 7}, {4, 4}, {7, 4}});
        line({{17, 4}, {20, 4}, {20, 7}});
        line({{20, 17}, {20, 20}, {17, 20}});
        line({{7, 20}, {4, 20}, {4, 17}});
        line({{10.5, 4}, {13.5, 4}});
        line({{10.5, 20}, {13.5, 20}});
        line({{4, 10.5}, {4, 13.5}});
        line({{20, 10.5}, {20, 13.5}});
    } else if (name.startsWith("bond")) {                  // The kinds of bond, at the angle of "bond".
        const QPointF along(6.5, -3.75), across(.5, .866); // Half the bond; one unit to its side.
        const auto at = [&](double t, double k) { return QPointF(12, 12) + along * t + across * k; };
        if (name == "bond1" || name == "bond3" || name == "bondDative")
            line({at(-1, 0), at(1, 0)});
        if (name == "bond2")
            for (const double k : {-2.2, 2.2})
                line({at(-1, k), at(1, k)});
        if (name == "bond3")
            for (const double k : {-3.6, 3.6})
                line({at(-1, k), at(1, k)});
        if (name == "bondAromatic") {
            line({at(-1, -2.2), at(1, -2.2)});
            for (const double t : {-1., -.15, .7})
                line({at(t, 2.2), at(t + .3, 2.2)});
        }
        if (name == "bondDative")
            line({at(.5, -2.5), at(1, 0), at(.5, 2.5)});
    } else if (name == "hash") {
        line({{10, 4}, {8, 20}});
        line({{16, 4}, {14, 20}});
        line({{4.5, 9}, {19.5, 9}});
        line({{4.5, 15}, {19.5, 15}});
    } else if (name == "table") {
        p.addRoundedRect(QRectF(3.5, 4.5, 17, 15), 2, 2);
        line({{3.5, 9.5}, {20.5, 9.5}});
        line({{3.5, 14.5}, {20.5, 14.5}});
        line({{9.5, 9.5}, {9.5, 19.5}});
    } else if (name == "split") { // A view over a table.
        p.addRoundedRect(QRectF(3.5, 4.5, 17, 15), 2, 2);
        line({{3.5, 13}, {20.5, 13}});
    } else if (name == "settings") { // A gear of eight teeth.
        QPolygonF gear;
        for (int i = 0; i < 8; ++i)
            for (const auto &[degrees, radius] : {std::pair{-17., 6.5}, {-9., 9.}, {9., 9.}, {17., 6.5}}) {
                const double angle = (i * 45 + degrees) * M_PI / 180;
                gear << QPointF(12 + radius * std::cos(angle), 12 + radius * std::sin(angle));
            }
        p.addPolygon(gear);
        p.closeSubpath();
        circle({12, 12}, 2.75);
    } else if (name == "inspect") { // A magnifier over a check.
        circle({10.5, 10.5}, 6.5);
        line({{15.5, 15.5}, {20, 20}});
        line({{7.7, 10.6}, {9.7, 12.6}, {13.4, 8.6}});
    } else if (name == "symmetry") { // Two halves on either side of a mirror.
        line({{9, 7}, {9, 18}, {3.5, 18}}, true);
        line({{15, 7}, {15, 18}, {20.5, 18}}, true);
        for (const double y : {3.5, 8.5, 13.5, 18.5})
            line({{12, y}, {12, y + 1.5}});
    } else if (name == "angle") {
        line({{20, 19}, {4.5, 19}, {14, 5}});
        const QRectF arc(-3.5, 11, 16, 16); // Around the vertex.
        p.arcMoveTo(arc, 0);
        p.arcTo(arc, 0, 56);
    } else if (name == "newman") { // A Newman projection: a staggered conformer.
        circle({12, 12}, 5.5);
        for (int i = 0; i < 3; ++i) {
            const double front = (i * 120 - 90) * M_PI / 180, back = front + M_PI;
            line({{12, 12}, {12 + 9 * std::cos(front), 12 + 9 * std::sin(front)}});
            line({{12 + 5.5 * std::cos(back), 12 + 5.5 * std::sin(back)},
                  {12 + 9 * std::cos(back), 12 + 9 * std::sin(back)}});
        }
    } else if (name == "cube") {
        line({{12, 3}, {19.8, 7.5}, {19.8, 16.5}, {12, 21}, {4.2, 16.5}, {4.2, 7.5}}, true);
        line({{4.2, 7.5}, {12, 12}, {19.8, 7.5}});
        line({{12, 12}, {12, 21}});
    } else if (name == "newFolder") {
        line({{3, 6}, {9.5, 6}, {11.5, 8.5}, {21, 8.5}, {21, 19}, {3, 19}}, true);
        line({{12, 10.75}, {12, 16.75}});
        line({{9, 13.75}, {15, 13.75}});
    } else if (name == "chart") {
        line({{4, 4}, {4, 20}, {20, 20}});
        line({{7.5, 16}, {11, 10.5}, {14, 13.5}, {19, 6}});
    } else if (name == "sphere") { // A shaded ball.
        circle({12, 12}, 8.5);
        const QRectF shine(7, 7, 10, 10);
        p.arcMoveTo(shine, 100);
        p.arcTo(shine, 100, 70);
    } else if (name == "eyeOff") {
        p.moveTo(2.5, 12);
        p.quadTo(12, 3.5, 21.5, 12);
        p.quadTo(12, 20.5, 2.5, 12);
        circle({12, 12}, 3);
        line({{4, 4}, {20, 20}});
    } else if (name == "eraser") {
        line({{4, 14.5}, {13, 5.5}, {19, 11.5}, {10, 20.5}}, true);
        line({{8.5, 10}, {14.5, 16}});
        line({{10, 20.5}, {20, 20.5}});
    } else if (name == "tabs") { // A window whose first tab is open.
        p.addRoundedRect(QRectF(3.5, 4.5, 17, 15), 2, 2);
        line({{10, 4.5}, {10, 9.5}, {20.5, 9.5}});
        line({{15.5, 4.5}, {15.5, 9.5}});
    } else if (name == "image") {
        p.addRoundedRect(QRectF(3.5, 4.5, 17, 15), 2, 2);
        line({{3.5, 17}, {9, 11.5}, {13, 15.5}, {15.5, 13}, {20.5, 18}});
        circle({15.5, 9}, 1.5);
    } else if (name == "book") { // An open book.
        for (const double s : {-1, 1}) {
            p.moveTo(12, 7);
            p.quadTo(12 + s * 4, 4.5, 12 + s * 9, 5);
            p.lineTo(12 + s * 9, 18);
            p.quadTo(12 + s * 4, 17.5, 12, 20);
        }
        line({{12, 7}, {12, 20}});
    } else if (name == "keyboard") {
        p.addRoundedRect(QRectF(2.5, 6, 19, 12), 2, 2);
        for (const double x : {6, 10, 14, 18}) {
            line({{x - .5, 9.5}, {x + .5, 9.5}});
            line({{x - .5, 12.5}, {x + .5, 12.5}});
        }
        line({{8, 15}, {16, 15}});
    } else if (name == "unlock") {
        p.addRoundedRect(QRectF(5, 11, 14, 10), 2, 2);
        p.moveTo(8, 11);
        p.lineTo(8, 8);
        p.arcTo(QRectF(8, 4, 8, 8), 180, -150);
    } else if (name == "axes") { // The axes of a frame.
        line({{8, 3.5}, {8, 16}, {20.5, 16}});
        line({{8, 16}, {3.5, 20.5}});
        line({{5.5, 6}, {8, 3.5}, {10.5, 6}});
        line({{18, 13.5}, {20.5, 16}, {18, 18.5}});
    } else if (name == "sort") { // Shorter and shorter lines beside an arrow down.
        line({{4, 6.5}, {13, 6.5}});
        line({{4, 12}, {10.5, 12}});
        line({{4, 17.5}, {8, 17.5}});
        line({{17.5, 5}, {17.5, 19}});
        line({{14.5, 16}, {17.5, 19}, {20.5, 16}});
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

QString settingsRules() {
    return R"(
        QFrame[toolbar="true"] { background: @base; border: none; border-bottom: 1px solid @line; }
        QFrame[banner="true"] { background: @candidate; border: none; border-bottom: 1px solid @candidateLine; }
        QFrame[banner="true"] QLabel { color: @candidateText; }
        QFrame[card="true"] { background: @base; border: 1px solid @line; border-radius: 10px; }
        QFrame[card="true"][state="ok"] { background: @goodTint; }
        QFrame[card="true"][state="warn"] { background: @candidate; border-color: @candidateLine; }
        QFrame[card="true"][state="error"] { background: @badTint; }
        QLabel[headline="ok"] { color: @good; font-weight: bold; }
        QLabel[headline="warn"] { color: @candidateText; font-weight: bold; }
        QLabel[headline="error"] { color: @error; font-weight: bold; }
        QLabel[headline="waiting"] { color: @muted; font-weight: bold; }
        QLabel[cardTitle="true"] { color: @text; font-weight: bold; font-size: 14px; }
        QLabel[caption="true"] { color: @muted; font-size: 11px; }
        QLabel[muted="true"] { color: @muted; }
        QLabel[good="true"] { color: @good; font-weight: bold; }
        QLabel[bad="true"] { color: @error; font-weight: bold; }
        QLabel[mono="true"] { font-family: monospace; }
        QFrame[choice="segments"] { background: @segment; border: none; border-radius: 7px; }
        QFrame[choice="segments"] QToolButton { border: none; border-radius: 5px; padding: 4px 10px; color: @muted;
                                                background: transparent; }
        QFrame[choice="segments"] QToolButton:hover { color: @text; }
        QFrame[choice="segments"] QToolButton:checked { background: @base; color: @accentText; font-weight: bold; }
        QFrame[choice="chips"] { background: transparent; border: none; }
        QFrame[choice="chips"] QToolButton { border: 1px solid @line; border-radius: 13px; padding: 4px 11px;
                                             background: @base; color: @text; }
        QFrame[choice="chips"] QToolButton:hover { background: @hover; }
        QFrame[choice="chips"] QToolButton:checked { background: @tint; border-color: @accent; color: @accentText;
                                                     font-weight: bold; }
        QFrame[choice] QToolButton:disabled { color: @muted; }
        QFrame[choice] QLabel { color: @error; }
        QPushButton { border: 1px solid @line; border-radius: 6px; background: @base; color: @text; padding: 5px 10px; }
        QPushButton:hover { background: @hover; }
        QPushButton:disabled { color: @muted; border-color: @hover; }
        QPushButton[primary="true"] { background: @accent; border-color: @accent; color: @onAccent; font-weight: bold;
                                      padding: 6px 14px; }
        QPushButton[primary="true"]:hover { background: @accentHover; }
        QPushButton[primary="true"]:disabled { background: @line; border-color: @line; color: @muted; }
        QPushButton[danger="true"] { color: @error; border-color: @error; font-weight: bold; }
        QPushButton[danger="true"]:disabled { color: @muted; border-color: @hover; font-weight: normal; }
        QPushButton[link="true"] { border: none; background: transparent; color: @accentText; padding: 2px 0;
                                   text-align: left; }
        QPushButton[link="true"]:hover { text-decoration: underline; }
        QPushButton[flat="true"] { border: none; background: transparent; padding: 3px; }
        QPushButton[flat="true"]:hover { background: @hover; }
        QToolButton[fold="true"] { border: none; border-radius: 5px; padding: 3px; }
        QToolButton[fold="true"]:hover { background: @hover; }
        QToolButton[disclosure="true"] { border: none; background: transparent; color: @accentText;
            font-weight: bold; padding: 6px 2px 2px 0; }
        QLabel[disclosureDetail="true"] { color: @muted; padding-top: 4px; }
        QToolButton[flat="true"] { border: none; border-radius: 5px; padding: 3px 6px; color: @text; }
        QToolButton[flat="true"]:hover { background: @hover; }
        QToolButton[flat="true"]:checked { background: @tint; color: @accentText; font-weight: bold; }
        QLineEdit[search="true"] { border: 1px solid @line; border-radius: 6px; padding: 4px; background: @base;
                                   color: @text; }
        QScrollArea[canvas="true"] { background: @window; border: none; }
        QWidget[canvas="true"] { background: @window; }
        QListWidget[nav="true"] { background: @base; border: none; }
        QTabWidget[tabs="underline"]::pane { background: @base; border: none; border-top: 1px solid @line; }
        QTabWidget[tabs="underline"]::tab-bar { left: 4px; }
        QTabWidget[tabs="underline"] > QTabBar { background: transparent; }
        QTabWidget[tabs="underline"] > QTabBar::tab { background: transparent; border: none; color: @muted;
                                                      border-bottom: 2px solid transparent; padding: 6px 3px;
                                                      margin-right: 14px; }
        QTabWidget[tabs="underline"] > QTabBar::tab:hover { color: @text; }
        QTabWidget[tabs="underline"] > QTabBar::tab:selected { color: @accentText; border-bottom-color: @accent; }
        QTabWidget[tabs="underline"] > QTabBar::tab:disabled { color: @line; }
        QTableView[plain="true"], QTreeView[plain="true"] { background: @base; border: 1px solid @line;
            border-radius: 8px; gridline-color: @hover; selection-background-color: @tint; selection-color: @text;
            alternate-background-color: @window; }
        QTableView[plain="true"]::item:selected, QTreeView[plain="true"]::item:selected { background: @tint;
                                                                                         color: @text; }
        QTableView[plain="true"] QHeaderView::section, QTreeView[plain="true"] QHeaderView::section {
            background: @base; color: @muted; border: none; border-bottom: 1px solid @line; padding: 5px 8px; }
        QTableView[plain="true"] QTableCornerButton::section { background: @base; border: none; }
        QPlainTextEdit[log="true"] { background: @window; border: 1px solid @line; border-radius: 6px; }
    )";
}
QPushButton *linkButton(const QString &text, const QString &objectName) {
    auto *b = new QPushButton(text);
    if (!objectName.isEmpty())
        b->setObjectName(objectName);
    b->setProperty("link", true);
    b->setIcon(lineIcon("arrow"));
    b->setLayoutDirection(Qt::RightToLeft); // The arrow after the text.
    b->setCursor(Qt::PointingHandCursor);
    b->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed); // As wide as its text, also in a form row.
    return b;
}
QPlainTextEdit *logView(const QString &objectName) {
    auto *text = new QPlainTextEdit;
    text->setObjectName(objectName);
    text->setReadOnly(true);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    text->setProperty("log", true);
    return text;
}
QToolButton *copyButton(const QString &objectName, const QString &toolTip) {
    auto *b = new QToolButton;
    b->setObjectName(objectName);
    b->setIcon(lineIcon("copy"));
    b->setProperty("flat", true);
    b->setToolTip(toolTip);
    b->setAccessibleName(toolTip);
    return b;
}
void repolish(QWidget *widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}
QString viewControlsHint() {
    return tr("左ドラッグ: 回転 · 右ドラッグ: 平行移動 · ホイール: 拡大縮小");
}
QString calculationBrief(const Document &d) {
    const auto s = nwchemSettings(d);
    QString task = s["task"].toString(), solvent;
    for (const auto &c : nwchemChoices("task"))
        if (c.value == task)
            task = c.label;
    if (s["solvent"] != "none") {
        // The names without their notes in parentheses (" (" in English).
        const auto opening = tr("（");
        for (const auto &c : nwchemChoices("solvent"))
            if (c.value == s["solvent"].toString())
                solvent = " · " + c.label.section(opening, 0, 0);
        for (const auto &c : nwchemChoices("solvent_name"))
            if (c.value == s["solvent_name"].toString() && s["solvent_name"] != "dielectric")
                solvent = tr("%1（%2）").arg(solvent, c.label.section(opening, 0, 0));
    }
    return (s["method"] == "dft" ? nwchemFunctionalLabel(s) : s["method"].toString().toUpper()) + " / " +
           nwchemBasisLabel(s) + " · " + task + solvent;
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

ChoiceButtons::ChoiceButtons(QComboBox *box, Look look, const QStringList &labels, QWidget *parent)
    : QFrame(parent), combo(box), other(new QLabel) {
    setProperty("choice", look == Segments ? "segments" : "chips");
    QLayout *row;
    if (look == Segments) {
        auto *h = new QHBoxLayout(this);
        h->setContentsMargins(2, 2, 2, 2);
        h->setSpacing(2);
        row = h;
        setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed); // As wide as its buttons.
    } else {
        row = new FlowLayout(this, 6, 6);
        row->setContentsMargins(0, 0, 0, 0);
    }
    combo->setParent(this);
    combo->hide();
    auto *group = new QButtonGroup(this);
    for (int i = 0; i < combo->count(); ++i) {
        auto *b = new QToolButton;
        b->setObjectName(combo->objectName() + '_' + combo->itemData(i).toString());
        b->setText(labels.value(i, combo->itemText(i)));
        if (b->text() != combo->itemText(i))
            b->setToolTip(combo->itemText(i));
        b->setCheckable(true);
        b->setAutoRaise(true);
        row->addWidget(b);
        group->addButton(b, i);
        buttons << b;
    }
    if (look == Segments)
        static_cast<QHBoxLayout *>(row)->addWidget(other);
    else
        row->addWidget(other);
    other->hide();
    connect(group, &QButtonGroup::idClicked, this, [this](int index) {
        if (combo->currentIndex() != index)
            combo->setCurrentIndex(index);
        sync();
    });
    connect(combo, &QComboBox::currentIndexChanged, this, [this] { sync(); });
    combo->installEventFilter(this);
    sync();
}
void ChoiceButtons::sync() {
    const int index = combo->currentIndex();
    for (int i = 0; i < buttons.size(); ++i) {
        buttons[i]->setChecked(i == index);
        buttons[i]->setEnabled(combo->isEnabled());
    }
    other->setVisible(index >= buttons.size());
    other->setText(index >= buttons.size() ? combo->currentText() : QString());
}
bool ChoiceButtons::eventFilter(QObject *watched, QEvent *event) {
    if (watched == combo && event->type() == QEvent::EnabledChange)
        for (auto *b : std::as_const(buttons))
            b->setEnabled(combo->isEnabled());
    return QFrame::eventFilter(watched, event);
}

namespace {
class NavDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const int line = option.fontMetrics.height();
        return {120, index.data(NavList::HeadingRole).toBool() ? line + 16 : line + 14};
    }
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const ChromeColors c(option.palette);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = option.rect.adjusted(6, 1, -6, -1);
        auto font = option.font;
        if (index.data(NavList::HeadingRole).toBool()) {
            font.setBold(true);
            font.setPointSizeF(font.pointSizeF() * .88);
            p->setFont(font);
            p->setPen(c.muted);
            p->drawText(r.adjusted(6, 0, 0, -3), Qt::AlignLeft | Qt::AlignBottom, index.data().toString());
            p->restore();
            return;
        }
        const bool selected = option.state & QStyle::State_Selected,
                   hover = option.state & QStyle::State_MouseOver,
                   muted = index.data(NavList::MutedRole).toBool();
        if (selected || hover) {
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? c.tint : c.hover);
            p->drawRoundedRect(r, 6, 6);
        }
        int right = r.right() - 8;
        auto small = option.font;
        small.setPointSizeF(small.pointSizeF() * .86);
        const auto badge = index.data(NavList::BadgeRole).toString(),
                   note = index.data(NavList::NoteRole).toString();
        const QFontMetrics sm(small);
        if (!badge.isEmpty()) {
            p->setFont(small);
            const int w = sm.horizontalAdvance(badge) + 12, h = sm.height() + 2;
            const QRect pill(right - w + 1, r.center().y() - h / 2, w, h);
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? c.base : c.segment);
            p->drawRoundedRect(pill, h / 2., h / 2.);
            p->setPen(selected ? c.accentText : c.muted);
            p->drawText(pill, Qt::AlignCenter, badge);
            right = pill.left() - 6;
        } else if (!note.isEmpty()) {
            p->setFont(small);
            const int w = sm.horizontalAdvance(note);
            p->setPen(c.muted);
            p->drawText(QRect(right - w + 1, r.top(), w, r.height()), Qt::AlignVCenter | Qt::AlignRight,
                        note);
            right -= w + 6;
        }
        if (index.data(NavList::MarkRole).toBool()) {
            p->setPen(Qt::NoPen);
            p->setBrush(c.accent);
            p->drawEllipse(QPointF(right - 3, r.center().y() + .5), 3, 3);
            right -= 12;
        }
        font.setBold(selected);
        p->setFont(font);
        p->setPen(selected ? c.accentText : muted ? c.muted : c.text);
        const QRect text(r.left() + 10, r.top(), std::max(0, right - r.left() - 12), r.height());
        p->drawText(text, Qt::AlignVCenter | Qt::AlignLeft,
                    QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, text.width()));
        p->restore();
    }
};
} // namespace

NavList::NavList(QWidget *parent) : QListWidget(parent) {
    setItemDelegate(new NavDelegate(this));
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setProperty("nav", true);
}
QListWidgetItem *NavList::addHeading(const QString &text) {
    auto *item = new QListWidgetItem(text, this);
    item->setData(HeadingRole, true);
    item->setFlags(Qt::NoItemFlags);
    return item;
}
QListWidgetItem *NavList::addEntry(const QString &text, const QVariant &key) {
    auto *item = new QListWidgetItem(text, this);
    item->setData(Qt::UserRole, key);
    return item;
}
QListWidgetItem *NavList::entry(const QVariant &key) const {
    for (int i = 0; i < count(); ++i)
        if (!item(i)->data(HeadingRole).toBool() && item(i)->data(Qt::UserRole) == key)
            return item(i);
    return nullptr;
}
void NavList::setCurrentKey(const QVariant &key) {
    if (auto *e = entry(key); e && e != currentItem()) {
        const QSignalBlocker blocker(this);
        setCurrentItem(e);
        viewport()->update();
    }
}

SettingsCard::SettingsCard(const QString &title, QWidget *parent) : QFrame(parent) {
    setProperty("card", true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 14);
    layout->setSpacing(10);
    head = new QHBoxLayout;
    head->setSpacing(8);
    heading = new QLabel(title);
    heading->setProperty("cardTitle", true);
    head->addWidget(heading);
    head->addStretch();
    layout->addLayout(head);
    contents = new QWidget;
    content = new QVBoxLayout(contents);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(8);
    layout->addWidget(contents);
    heading->setVisible(!title.isEmpty());
}
QToolButton *SettingsCard::setCollapsible(bool open, const QString &summary) {
    fold = new QToolButton;
    fold->setObjectName(objectName().isEmpty() ? QString() : objectName() + "Toggle");
    fold->setProperty("fold", true);
    fold->setCheckable(true);
    fold->setChecked(open);
    fold->setAccessibleName(heading->text());
    head->insertWidget(0, fold);
    folded = new ElidedLabel(summary); // Shortened in a narrow card, in full as a tooltip.
    folded->setProperty("muted", true);
    folded->setToolTip(summary);
    head->insertWidget(2, folded, 1);
    connect(fold, &QToolButton::toggled, this, [this](bool on) { showOpen(on); });
    showOpen(open);
    return fold;
}
void SettingsCard::setOpen(bool open) {
    if (fold)
        fold->setChecked(open);
}
void SettingsCard::setSummary(const QString &summary) {
    if (!folded)
        return;
    folded->setText(summary);
    folded->setToolTip(summary);
    showOpen(fold->isChecked());
}
void SettingsCard::showOpen(bool open) {
    contents->setVisible(open);
    fold->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    fold->setToolTip(open ? tr("閉じる", "fold") : tr("開く", "fold"));
    folded->setVisible(!open && !folded->text().isEmpty());
}

Disclosure::Disclosure(const QString &text, bool open, QWidget *parent) : QWidget(parent) {
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    toggle = new QToolButton;
    toggle->setText(text);
    toggle->setProperty("disclosure", true);
    toggle->setCheckable(true);
    toggle->setChecked(open);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    toggle->setCursor(Qt::PointingHandCursor);
    row->addWidget(toggle);
    detail = new QLabel;
    detail->setProperty("disclosureDetail", true);
    detail->setCursor(Qt::PointingHandCursor);
    detail->installEventFilter(this); // The count opens it too.
    detail->hide();
    row->addWidget(detail);
    connect(toggle, &QToolButton::toggled, this,
            [this](bool on) { toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow); });
}
bool Disclosure::isOpen() const {
    return toggle->isChecked();
}
void Disclosure::setOpen(bool open) {
    toggle->setChecked(open);
}
void Disclosure::setDetail(const QString &text) {
    detail->setText(text);
    detail->setVisible(!text.isEmpty());
}
bool Disclosure::eventFilter(QObject *watched, QEvent *event) {
    if (watched == detail && event->type() == QEvent::MouseButtonRelease)
        toggle->click();
    return QWidget::eventFilter(watched, event);
}

StatusCard::StatusCard(QWidget *parent) : SettingsCard({}, parent) {
    icon = new QLabel;
    icon->setFixedSize(18, 18);
    text = new QLabel;
    text->setWordWrap(true);
    header()->insertWidget(0, icon, 0, Qt::AlignVCenter);
    header()->insertWidget(1, text, 1);
    setState(Waiting, {});
}
void StatusCard::setState(State state, const QString &headline) {
    static const char *names[]{"ok", "warn", "error", "waiting"};
    current = state;
    text->setText(headline);
    const ChromeColors c(palette());
    const QColor color = state == Ok        ? c.good
                         : state == Warning ? c.candidateText
                         : state == Error   ? c.bad
                                            : c.muted;
    icon->setPixmap(lineIcon(state == Ok        ? "check"
                             : state == Error   ? "error"
                             : state == Warning ? "warning"
                                                : "info",
                             color)
                        .pixmap(QSize(18, 18)));
    if (property("state").toString() != names[state] ||
        text->property("headline").toString() != names[state]) {
        setProperty("state", names[state]);
        text->setProperty("headline", names[state]);
        repolish(this);
        repolish(text);
    }
}
QPushButton *StatusCard::addRecheck(const QString &objectName, const QString &toolTip) {
    auto *b = new QPushButton(tr("再検査"));
    b->setObjectName(objectName);
    b->setIcon(lineIcon("refresh"));
    b->setToolTip(toolTip);
    header()->addWidget(b, 0, Qt::AlignVCenter);
    return b;
}

CalculationSummary::CalculationSummary(const QString &prefix, const QString &caption,
                                       const std::function<void(const QString &)> &openLink, QWidget *parent)
    : SettingsCard({}, parent) {
    setObjectName(prefix + "SummaryCard");
    body()->setSpacing(4);
    auto *kind = new QLabel(caption);
    kind->setProperty("caption", true);
    body()->addWidget(kind);
    title = new QLabel;
    title->setObjectName(prefix + "Summary");
    title->setProperty("cardTitle", true);
    title->setWordWrap(true);
    body()->addWidget(title);
    task = new QLabel;
    task->setObjectName(prefix + "SummaryTask");
    task->setWordWrap(true);
    body()->addWidget(task);
    auto *facts = new FormLayout;
    facts->setContentsMargins(0, 6, 0, 0);
    facts->setHorizontalSpacing(14);
    facts->setVerticalSpacing(3);
    for (const auto &[name, label, key] : {std::tuple{QT_TR_NOOP("組成"), &formula, "Formula"},
                                           {QT_TR_NOOP("電子数"), &electrons, "Electrons"},
                                           {QT_TR_NOOP("電荷・多重度"), &charge, "Charge"},
                                           {QT_TR_NOOP("メモリ"), &memory, "Memory"}}) {
        *label = new QLabel;
        (*label)->setObjectName(prefix + "Summary" + key);
        (*label)->setWordWrap(true);
        auto *term = new QLabel(tr(name));
        term->setProperty("muted", true);
        facts->addRow(term, *label);
    }
    memory->setTextFormat(Qt::RichText);
    connect(memory, &QLabel::linkActivated, this, [openLink](const QString &link) { openLink(link); });
    body()->addLayout(facts);
}
void CalculationSummary::update(const Document &d) {
    const auto s = nwchemSettings(d);
    const auto check = validateNwchem(d);
    const auto brief = calculationBrief(d);
    title->setText(brief.section(" · ", 0, 0));
    task->setText(brief.section(" · ", 1));
    const QString method = s["method"].toString();
    const QString reference = method == "dft" && check.reference != "rohf"
                                  ? (check.reference == "rhf" ? "RKS" : "UKS")
                                  : check.reference.toUpper();
    formula->setText(d.atoms.isEmpty() ? tr("原子なし")
                                       : tr("%1 · %n 原子", nullptr, int(d.atoms.size())).arg(d.formula()));
    electrons->setText((check.ecpElectrons ? tr("%1（α %2 / β %3）ECP適用後") : tr("%1（α %2 / β %3）"))
                           .arg(check.electrons)
                           .arg(check.alpha)
                           .arg(check.beta));
    const auto charges = QString::number(d.charge), multiplicity = QString::number(d.multiplicity);
    charge->setText(reference.isEmpty() ? QString("%1 · %2").arg(charges, multiplicity)
                                        : tr("%1 · %2（%3）").arg(charges, multiplicity, reference));
    // The link on a line of its own, never split.
    memory->setText(tr("%1 MB / MPIプロセス<br><a href=\"memory\" style=\"color:%2; "
                       "text-decoration:none\">3 実行設定で変更</a>")
                        .arg(s["memory_mb"].toInt())
                        .arg(ChromeColors(palette()).accentText.name()));
}

ViewControls::ViewControls(MoleculeView *view, const Names &names, bool rotate, bool menu, QWidget *parent)
    : QFrame(parent), view(view) {
    setProperty("viewControls", true);
    setProperty("compactButtons", true); // OverlayHost shows only the icons when narrow.
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(3, 3, 3, 3);
    row->setSpacing(1);
    for (int i = 0; i < 3; ++i) {
        auto *b = new QToolButton;
        b->setObjectName(names.axes[i]);
        b->setText(QString("+") + QString("XYZ")[i]);
        b->setToolTip(tr("%1軸の正の側から見て、分子全体を表示します。").arg(QString("XYZ")[i]));
        row->addWidget(b);
        parts << b;
        connect(b, &QToolButton::clicked, this, [view, i] { view->viewFrom(i); });
    }
    auto *divider = new QFrame;
    divider->setFrameShape(QFrame::VLine);
    divider->setFrameShadow(QFrame::Sunken);
    row->addWidget(divider);
    const auto button = [&](const QString &text, const QString &icon, const QString &name) {
        auto *b = new QToolButton;
        b->setObjectName(name);
        b->setText(text);
        b->setIcon(lineIcon(icon));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        row->addWidget(b);
        parts << b;
        return b;
    };
    fit = button(tr("全体を表示"), "fit", names.fit);
    fit->setToolTip(tr("向きを変えずに、分子全体が収まるように表示します。"));
    connect(fit, &QToolButton::clicked, view, &MoleculeView::fit);
    if (rotate) {
        rotation = button(tr("回転"), "rotate", names.rotate);
        rotation->setToolTip(tr("左右・上下・画面内の回転角をスライダーで指定します。"));
        // The angles as sliders in a popup under "回転"; the mouse rotation moves them too.
        popup = new QFrame(this, Qt::Popup);
        popup->setObjectName(names.popup);
        auto *angles = new FormLayout(popup);
        angles->setContentsMargins(14, 12, 14, 12);
        QSlider *sliders[3];
        const auto start = view->angles();
        for (int i = 0; i < 3; ++i) {
            sliders[i] = new QSlider(Qt::Horizontal);
            sliders[i]->setObjectName(names.sliders[i]);
            sliders[i]->setRange(-180, 180);
            sliders[i]->setValue(qRound(start[i]));
            sliders[i]->setMinimumWidth(200);
            angles->addRow(QStringList{tr("左右回転"), tr("上下回転"), tr("画面内回転")}[i], sliders[i]);
            parts << sliders[i];
        }
        connect(rotation, &QToolButton::clicked, this, [this] {
            popup->adjustSize();
            popup->move(
                rotation->mapToGlobal(QPoint(rotation->width() - popup->width(), rotation->height() + 4)));
            popup->show();
        });
        for (int i = 0; i < 3; ++i)
            connect(sliders[i], &QSlider::valueChanged, this,
                    [view, a = sliders[0], b = sliders[1], c = sliders[2]] {
                        view->setAngles(a->value(), b->value(), c->value());
                    });
        // Mouse rotation and +X/+Y/+Z move the sliders, so touching one continues from the view.
        connect(view, &MoleculeView::orientationChanged, this,
                [view, a = sliders[0], b = sliders[1], c = sliders[2]] {
                    const auto angles = view->angles();
                    const QSignalBlocker ba(a), bb(b), bc(c);
                    a->setValue(qRound(angles.x()));
                    b->setValue(qRound(angles.y()));
                    c->setValue(qRound(angles.z()));
                });
    }
    if (menu) {
        display = button(tr("表示"), "eye", names.menu);
        display->setToolTip(tr("ラベル・水素・表示スタイルなどの表示を切り替えます。"));
        display->setPopupMode(QToolButton::InstantPopup);
    }
}
void ViewControls::applyStyle(const QPalette &palette) {
    setStyleSheet(chromeRules(R"(
        QFrame[viewControls="true"] { background: @base; border: 1px solid @line; border-radius: 8px; }
        QToolButton { border: none; border-radius: 5px; padding: 3px 7px; color: @text; background: transparent; }
        QToolButton:hover { background: @hover; }
        QToolButton:disabled { color: @muted; }
        QToolButton::menu-indicator { width: 0; }
    )",
                              palette));
    if (popup)
        popup->setStyleSheet(chromeRules("QFrame { background: @base; border: 1px solid @line; }", palette));
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
