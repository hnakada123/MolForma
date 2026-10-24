#include "ui/diagnostics_list.h"
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
    if (errors.isEmpty())
        add(ok, QStyle::SP_DialogApplyButton, {}, false);
    for (const auto &e : errors)
        add("エラー: " + e, QStyle::SP_MessageBoxCritical, e, true);
    for (const auto &w : warnings)
        add("確認: " + w, QStyle::SP_MessageBoxWarning, w, false);
}
QStringList DiagnosticsList::messages() const {
    QStringList list;
    for (int i = 0; i < count(); ++i)
        list << item(i)->text();
    return list;
}
} // namespace mf
