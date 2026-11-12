// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/line_icons.h"
#include "ui/theme.h"
#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace mf {
namespace {
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
    } else if (name == "expand") { // Arrows out to two corners.
        line({{14, 4}, {20, 4}, {20, 10}});
        line({{10, 20}, {4, 20}, {4, 14}});
        line({{20, 4}, {13.5, 10.5}});
        line({{4, 20}, {10.5, 13.5}});
    } else if (name == "collapse") { // Arrows in from two corners.
        line({{13.5, 4.5}, {13.5, 10.5}, {19.5, 10.5}});
        line({{10.5, 19.5}, {10.5, 13.5}, {4.5, 13.5}});
        line({{13.5, 10.5}, {20, 4}});
        line({{10.5, 13.5}, {4, 20}});
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
    else if (name == "up")
        line({{5, 15}, {12, 8}, {19, 15}});
    else if (name == "down")
        line({{5, 9}, {12, 16}, {19, 9}});
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
} // namespace mf
