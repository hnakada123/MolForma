// SPDX-License-Identifier: Apache-2.0
// Derived from KAGAMI tools/kagami_gui/builder.py TemplateDelegate
// (Copyright 2026 Hiroshi Nakada). Adapted to Qt6/C++ with HiDPI-aware caching.
#include "template_delegate.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSet>
#include <QWidget>
#include <algorithm>

namespace mf {
namespace {
const QSize thumbnailSize(60, 44);

QIcon thumbnail(const QJsonObject &t, qreal ratio) {
    QPixmap pixmap(qRound(thumbnailSize.width() * ratio), qRound(thumbnailSize.height() * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    const auto atoms = t["atoms"].toArray();
    QVector<QPointF> points;
    QPointF center;
    for (const auto &v : atoms) {
        auto position = v.toObject()["position"].toArray();
        if (position.size() < 2)
            return QIcon(pixmap);
        QPointF point(position[0].toDouble(), position[1].toDouble());
        points << point;
        center += point;
    }
    if (points.isEmpty())
        return QIcon(pixmap);
    center /= points.size();
    qreal minX = points.first().x(), maxX = minX, minY = points.first().y(), maxY = minY;
    for (const auto &point : points) {
        minX = std::min(minX, point.x());
        maxX = std::max(maxX, point.x());
        minY = std::min(minY, point.y());
        maxY = std::max(maxY, point.y());
    }
    const qreal scale = 35. / std::max({maxX - minX, maxY - minY, 1.});
    for (auto &point : points)
        point = (point - center) * scale + QPointF(30, 22);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#94a3b8"), 1));
    for (const auto &v : t["bonds"].toArray()) {
        const auto bond = v.toObject();
        int a = bond["a"].toInt(-1), b = bond["b"].toInt(-1);
        if (a >= 0 && a < points.size() && b >= 0 && b < points.size())
            painter.drawLine(points[a], points[b]);
    }
    QSet<int> connections;
    for (const auto &v : t["connections"].toArray())
        connections.insert(v.toInt());
    for (int i = 0; i < atoms.size(); ++i) {
        if (atoms[i].toObject()["element"] == "H")
            continue;
        painter.setBrush(QColor(connections.contains(i) ? "#16a34a" : "#475569"));
        painter.drawEllipse(points[i], 3, 3);
    }
    painter.end();
    return QIcon(pixmap);
}
} // namespace

TemplateDelegate::TemplateDelegate(QObject *parent) : QStyledItemDelegate(parent), icons(128) {}

void TemplateDelegate::initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const {
    QStyledItemDelegate::initStyleOption(option, index);
    const auto t = index.data(TemplateRole).toJsonObject();
    if (t["atoms"].toArray().isEmpty())
        return;
    const qreal ratio = option->widget ? option->widget->devicePixelRatioF() : 1.;
    // Include contents so reloading a custom part with the same ID updates its icon.
    const auto key = QJsonDocument(t).toJson(QJsonDocument::Compact) + '/' + QByteArray::number(ratio);
    auto *icon = icons.object(key);
    if (!icon) {
        icon = new QIcon(thumbnail(t, ratio));
        icons.insert(key, icon);
    }
    option->icon = *icon;
    option->decorationSize = thumbnailSize;
    option->features |= QStyleOptionViewItem::HasDecoration;
}

QSize TemplateDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    if (!index.data(TemplateRole).toJsonObject()["atoms"].toArray().isEmpty())
        return {220, std::max(48, option.fontMetrics.height() + 8)};
    return QStyledItemDelegate::sizeHint(option, index);
}
} // namespace mf
