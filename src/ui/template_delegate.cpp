// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/template_delegate.h"
#include "model/document.h"
#include "ui/editor_chrome.h"
#include <QApplication>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListView>
#include <QPainter>
#include <QSet>
#include <QWidget>
#include <algorithm>

namespace mf {
namespace {
const QSize thumbnailSize(60, 44);
constexpr int minimumTile = 70;

bool fullWidth(const QModelIndex &index) {
    return index.data(TemplateDelegate::HeadingRole).toBool() ||
           (index.data(TemplateDelegate::ElementRole).toString().isEmpty() &&
            index.data(TemplateDelegate::TemplateRole).toJsonObject()["atoms"].toArray().isEmpty());
}
// The width a row of tiles may use: the list's viewport less the spacing at both ends, and less
// the room QListView keeps for a vertical scroll bar when it wraps items into rows.
int rowWidth(const QStyleOptionViewItem &option) {
    const auto *view = qobject_cast<const QListView *>(option.widget);
    if (!view)
        return 228;
    const int bar = view->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, view);
    return std::max(minimumTile, view->viewport()->width() - 2 * view->spacing() - bar - 3);
}
QFont smaller(QFont font, double factor) {
    if (font.pointSizeF() > 0)
        font.setPointSizeF(font.pointSizeF() * factor);
    else
        font.setPixelSize(std::max(1, qRound(font.pixelSize() * factor)));
    return font;
}

QIcon elementThumbnail(const Element &e, const QFont &font, qreal ratio) {
    QPixmap pixmap(qRound(thumbnailSize.width() * ratio), qRound(thumbnailSize.height() * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF tile(10, 3, 40, 38);
    painter.setPen(QPen(e.color.lightness() > 225 ? QColor("#cbd5e1") : e.color.darker(125), 1));
    painter.setBrush(e.color);
    painter.drawRoundedRect(tile, 8, 8);
    QFont symbolFont(font);
    symbolFont.setPixelSize(20);
    symbolFont.setWeight(QFont::DemiBold);
    painter.setFont(symbolFont);
    painter.setPen(e.color.lightness() < 135 ? QColor(Qt::white) : QColor("#1e293b"));
    painter.drawText(tile, Qt::AlignCenter, e.symbol);
    painter.end();
    return QIcon(pixmap);
}

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
    const qreal ratio = option->widget ? option->widget->devicePixelRatioF() : 1.;
    const auto symbol = index.data(ElementRole).toString();
    if (!symbol.isEmpty()) {
        const auto key = "element/" + symbol.toUtf8() + '/' + option->font.toString().toUtf8() + '/' +
                         QByteArray::number(ratio);
        auto *icon = icons.object(key);
        if (!icon) {
            icon = new QIcon(elementThumbnail(element(symbol), option->font, ratio));
            icons.insert(key, icon);
        }
        option->icon = *icon;
        option->decorationSize = thumbnailSize;
        option->features |= QStyleOptionViewItem::HasDecoration;
        return;
    }
    const auto t = index.data(TemplateRole).toJsonObject();
    if (t["atoms"].toArray().isEmpty())
        return;
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

void TemplateDelegate::paintTile(QPainter *painter, const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const {
    QStyleOptionViewItem styled(option);
    initStyleOption(&styled, index);
    const ChromeColors colors(styled.palette);
    const bool selected = styled.state & QStyle::State_Selected,
               hovered = styled.state & QStyle::State_MouseOver;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(selected ? colors.accent : colors.line, selected ? 1.5 : 1));
    painter->setBrush(selected ? colors.tint : hovered ? colors.hover : colors.base);
    painter->drawRoundedRect(QRectF(styled.rect).adjusted(.75, .75, -.75, -.75), 7, 7);
    const QRect icon(styled.rect.center().x() - thumbnailSize.width() / 2, styled.rect.top() + 5,
                     thumbnailSize.width(), thumbnailSize.height());
    styled.icon.paint(painter, icon);
    // The name in up to two lines under the thumbnail, shortened with "…" when longer.
    const QFont font = smaller(styled.font, .9);
    const QFontMetrics metrics(font);
    const QRect text(styled.rect.left() + 4, icon.bottom() + 3, styled.rect.width() - 8,
                     2 * metrics.height());
    const int flags = Qt::AlignHCenter | Qt::AlignTop | Qt::TextWrapAnywhere;
    auto name = index.data().toString();
    while (name.size() > 1 && metrics.boundingRect(text, flags, name).height() > text.height())
        name = name.left(name.size() - 2) + "…";
    painter->setFont(font);
    painter->setPen(selected ? colors.accentText : colors.text);
    painter->drawText(text, flags, name);
    painter->restore();
}

void TemplateDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                             const QModelIndex &index) const {
    if (tiles && !fullWidth(index)) {
        paintTile(painter, option, index);
        return;
    }
    if (index.data(HeadingRole).toBool()) {
        // A bold group title over a thin rule.
        painter->save();
        QFont font(option.font);
        font.setBold(true);
        painter->setFont(font);
        painter->setPen(option.palette.color(QPalette::Active, QPalette::Text)); // Not greyed out.
        const auto text = option.rect.adjusted(6, 4, -4, -2);
        painter->drawText(
            text, Qt::AlignLeft | Qt::AlignBottom,
            QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, text.width()));
        painter->setPen(option.palette.color(QPalette::Mid));
        painter->drawLine(option.rect.bottomLeft() + QPoint(4, 0), option.rect.bottomRight() - QPoint(4, 0));
        painter->restore();
        return;
    }
    const auto symbol = index.data(ElementRole).toString();
    if (symbol.isEmpty()) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    QStyleOptionViewItem styled(option);
    initStyleOption(&styled, index);
    const auto *style = styled.widget ? styled.widget->style() : QApplication::style();
    const auto textRect =
        style->subElementRect(QStyle::SE_ItemViewItemText, &styled, styled.widget).adjusted(2, 0, -4, 0);
    styled.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &styled, painter, styled.widget);
    const auto &e = element(symbol);
    auto detail = index.data(ElementDetailRole).toString();
    if (detail.isEmpty())
        detail = QString("原子番号 %1").arg(e.number);
    QFont detailFont(styled.font);
    if (detailFont.pointSizeF() > 0)
        detailFont.setPointSizeF(detailFont.pointSizeF() * .8);
    else
        detailFont.setPixelSize(std::max(1, qRound(detailFont.pixelSize() * .8)));
    const QFontMetrics detailMetrics(detailFont);
    const int height = styled.fontMetrics.height() + detailMetrics.height() + 2;
    const int top = textRect.top() + (textRect.height() - height) / 2;
    const bool selected = styled.state & QStyle::State_Selected;
    const auto group = !(styled.state & QStyle::State_Enabled) ? QPalette::Disabled
                       : styled.state & QStyle::State_Active   ? QPalette::Active
                                                               : QPalette::Inactive;
    painter->save();
    painter->setClipRect(textRect);
    painter->setFont(styled.font);
    painter->setPen(styled.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text));
    painter->drawText(QRect(textRect.left(), top, textRect.width(), styled.fontMetrics.height()),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      styled.fontMetrics.elidedText(e.nameJa, Qt::ElideRight, textRect.width()));
    painter->setFont(detailFont);
    painter->setPen(
        styled.palette.color(group, selected ? QPalette::HighlightedText : QPalette::PlaceholderText));
    painter->drawText(QRect(textRect.left(), top + styled.fontMetrics.height() + 2, textRect.width(),
                            detailMetrics.height()),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      detailMetrics.elidedText(detail, Qt::ElideRight, textRect.width()));
    painter->restore();
}

QSize TemplateDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    if (tiles) {
        const int width = rowWidth(option);
        if (index.data(HeadingRole).toBool())
            return {width, option.fontMetrics.height() + 12};
        if (fullWidth(index))
            return {width, std::max(40, option.fontMetrics.height() + 12)};
        const auto *view = qobject_cast<const QListView *>(option.widget);
        const int spacing = view ? view->spacing() : 0;
        const int columns = std::max(1, (width + spacing) / (minimumTile + spacing));
        return {(width - (columns - 1) * spacing) / columns,
                5 + thumbnailSize.height() + 3 + 2 * QFontMetrics(smaller(option.font, .9)).height() + 6};
    }
    if (index.data(HeadingRole).toBool())
        return {220, option.fontMetrics.height() + 10};
    if (!index.data(ElementRole).toString().isEmpty())
        return {220, std::max(48, 2 * option.fontMetrics.height() + 8)};
    // Parts and the other rows (such as "SMILES … から3Dで作成") share the thumbnail height.
    return {220, std::max(48, option.fontMetrics.height() + 8)};
}
} // namespace mf
