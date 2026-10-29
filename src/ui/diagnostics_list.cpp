#include "ui/diagnostics_list.h"
#include "model/i18n.h"
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
            emit diagnosticClicked(item->data(SettingRole).toString(), item->data(TabRole).toInt());
    });
    connect(this, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (!item->data(FixRole).toString().isEmpty())
            emit fixRequested(item->data(CodeRole).toString(), item->data(SettingRole).toString());
    });
}
void DiagnosticsList::setDiagnostics(const QVector<Diagnostic> &diagnostics) {
    clear();
    for (const auto severity : {DiagnosticSeverity::Error, DiagnosticSeverity::Warning})
        for (const auto &d : diagnostics) {
            if (d.severity != severity)
                continue;
            const bool error = severity == DiagnosticSeverity::Error;
            const auto text = (error ? tr("エラー: %1") : tr("確認: %1")).arg(d.message);
            auto *item = new QListWidgetItem(
                style()->standardIcon(error ? QStyle::SP_MessageBoxCritical : QStyle::SP_MessageBoxWarning),
                text, this);
            item->setData(MessageRole, d.message);
            item->setData(ErrorRole, error);
            item->setData(CodeRole, d.code);
            item->setData(SettingRole, d.setting);
            item->setData(TabRole, d.tab);
            if (d.fix) {
                item->setData(FixRole, d.fix->label);
                item->setText(tr("%1\n　→ ダブルクリックで修正: %2").arg(text, d.fix->label));
            }
            item->setToolTip(tr("クリックで該当する設定へ移動します。"));
        }
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
