#pragma once
#include <QCache>
#include <QIcon>
#include <QStyledItemDelegate>

namespace mf {
class TemplateDelegate : public QStyledItemDelegate {
  public:
    static constexpr int TemplateRole = Qt::UserRole + 1;
    static constexpr int ElementRole = Qt::UserRole + 2;
    static constexpr int ElementDetailRole = Qt::UserRole + 3;
    static constexpr int HeadingRole = Qt::UserRole + 4; // A group title in the palette, not a part.
    explicit TemplateDelegate(QObject *parent = nullptr);
    // Tiles (thumbnail over the name) for a list that flows left to right and wraps; a heading
    // and a row without a thumbnail take the whole width.
    void setTiles(bool on) {
        tiles = on;
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

  protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override;

  private:
    void paintTile(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;
    mutable QCache<QByteArray, QIcon> icons;
    bool tiles = false;
};
} // namespace mf
