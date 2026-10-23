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
    explicit TemplateDelegate(QObject *parent = nullptr);
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

  protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override;

  private:
    mutable QCache<QByteArray, QIcon> icons;
};
} // namespace mf
