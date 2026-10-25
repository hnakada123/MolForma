#include "ui/diagnostics_list.h"
#include <QResizeEvent>
#include <QStyle>

namespace mf {
DiagnosticsList::DiagnosticsList(QWidget *parent) : QListWidget(parent) {
    setWordWrap(true);
    setTextElideMode(Qt::ElideNone);
    setResizeMode(QListView::Adjust);
    setSelectionMode(QAbstractItemView::SingleSelection);
    connect(this, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (!item->data(MessageRole).toString().isEmpty())
            emit messageClicked(item->data(MessageRole).toString(), item->data(ErrorRole).toBool());
    });
    connect(this, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (!item->data(FixRole).toString().isEmpty())
            emit fixRequested(item->data(MessageRole).toString());
    });
}
void DiagnosticsList::setMessages(const QStringList &errors, const QStringList &warnings, const QString &ok,
                                  const QMap<QString, QString> &fixes) {
    clear();
    // Icons from the style follow the theme; text uses the palette.
    const auto add = [&](const QString &text, QStyle::StandardPixmap icon, const QString &message,
                         bool error) {
        auto *item = new QListWidgetItem(style()->standardIcon(icon), text, this);
        item->setData(MessageRole, message);
        item->setData(ErrorRole, error);
        if (fixes.contains(message)) {
            item->setData(FixRole, fixes[message]);
            item->setText(text + "\n　→ ダブルクリックで修正: " + fixes[message]);
        }
        item->setToolTip(message.isEmpty() ? text : "クリックで該当する設定へ移動します。");
    };
    if (errors.isEmpty() && !ok.isEmpty())
        add(ok, QStyle::SP_DialogApplyButton, {}, false);
    for (const auto &e : errors)
        add("エラー: " + e, QStyle::SP_MessageBoxCritical, e, true);
    for (const auto &w : warnings)
        add("確認: " + w, QStyle::SP_MessageBoxWarning, w, false);
    if (fit)
        updateGeometry();
}
void DiagnosticsList::setFitContents(int maximum) {
    fit = maximum;
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    updateGeometry();
}
QSize DiagnosticsList::sizeHint() const {
    const auto hint = QListWidget::sizeHint();
    if (!fit)
        return hint;
    // Wrapped rows beside their icon, with the delegate's margins.
    const int icon =
        iconSize().isValid() ? iconSize().width() : style()->pixelMetric(QStyle::PM_SmallIconSize);
    const int room = std::max(80, width() - 2 * frameWidth() - icon - 12);
    int height = 2 * frameWidth() + 4;
    for (int i = 0; i < count(); ++i)
        height +=
            std::max(icon, fontMetrics()
                               .boundingRect(QRect(0, 0, room, 100000), Qt::TextWordWrap, item(i)->text())
                               .height()) +
            8;
    return {hint.width(), std::min(fit, height)};
}
void DiagnosticsList::resizeEvent(QResizeEvent *event) {
    QListWidget::resizeEvent(event);
    if (fit && event->size().width() != event->oldSize().width())
        updateGeometry();
}
QStringList DiagnosticsList::messages() const {
    QStringList list;
    for (int i = 0; i < count(); ++i)
        list << item(i)->text();
    return list;
}
} // namespace mf
