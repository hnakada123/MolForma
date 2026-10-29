// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/run_progress.h"
#include "results/result_summary.h"
#include "ui/editor_chrome.h"
#include <QtWidgets>

namespace mf {
QString elapsedClock(qint64 seconds) {
    return seconds >= 3600 ? QString("%1:%2:%3")
                                 .arg(seconds / 3600)
                                 .arg(seconds / 60 % 60, 2, 10, QChar('0'))
                                 .arg(seconds % 60, 2, 10, QChar('0'))
                           : QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
}
QString displayPath(const QString &path) {
    const auto home = QDir::homePath();
    return path == home ? QString("~") : path.startsWith(home + '/') ? "~" + path.mid(home.size()) : path;
}

RunProgress::RunProgress(const QString &prefix, int columns, QWidget *parent)
    : QWidget(parent), name(prefix) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    stageList = new QVBoxLayout;
    stageList->setSpacing(4);
    layout->addLayout(stageList);
    criteria = new QWidget;
    criteria->setObjectName(name + "Criteria");
    auto *tiles = new QGridLayout(criteria);
    tiles->setContentsMargins(0, 0, 0, 0);
    tiles->setSpacing(columns > 2 ? 8 : 6);
    for (int i = 0; i < 4; ++i) {
        auto *tile = new QLabel;
        tile->setObjectName(QString("%1Criterion%2").arg(name).arg(i));
        tile->setProperty("tile", true);
        tile->setTextFormat(Qt::RichText);
        tiles->addWidget(tile, i / columns, i % columns);
        tiles->setColumnStretch(i % columns, 1);
        criterionLabels << tile;
    }
    criteria->hide();
    layout->addWidget(criteria);
}
void RunProgress::clear() {
    criteria->hide();
    setStages({});
}
void RunProgress::setStages(const QStringList &stages) {
    // Rows in place while their number stays (the views update every second or more often).
    if (stageList->count() != stages.size())
        while (auto *item = stageList->takeAt(0)) {
            delete item->widget();
            delete item;
        }
    for (int i = 0; i < stages.size(); ++i) {
        auto *row = stageList->itemAt(i) ? qobject_cast<QLabel *>(stageList->itemAt(i)->widget()) : nullptr;
        if (!row) {
            row = new QLabel;
            row->setObjectName(QString("%1Stage%2").arg(name).arg(i));
            row->setTextFormat(Qt::RichText);
            row->setWordWrap(true);
            stageList->addWidget(row);
        }
        if (row->text() != stages[i])
            row->setText(stages[i]);
    }
}
void RunProgress::read(const QString &path) {
    if (path != directory) {
        progress = NwchemProgress();
        directory = path;
        offset = 0;
        rest.clear();
        QFile input(QDir(path).filePath("input.nw"));
        const auto text = input.open(QIODevice::ReadOnly) ? QString::fromUtf8(input.readAll()) : QString();
        tasks = NwchemProgress::tasks(text);
        maxSteps = NwchemProgress::driverSteps(text);
    }
    if (path.isEmpty())
        return;
    QFile file(QDir(path).filePath("output.out"));
    if (!file.open(QIODevice::ReadOnly) || file.size() == offset)
        return;
    if (file.size() < offset) { // A rewritten log: read it again.
        progress = NwchemProgress();
        offset = 0;
        rest.clear();
    }
    file.seek(offset);
    const auto chunk = file.read(std::min<qint64>(file.size() - offset, 8 << 20));
    offset += chunk.size();
    rest += chunk;
    const int end = rest.lastIndexOf('\n');
    if (end < 0)
        return;
    for (const auto &line : rest.left(end).split('\n'))
        progress.add(QString::fromUtf8(line));
    rest.remove(0, end + 1);
}
void RunProgress::showRun(const QString &path, RunState state) {
    if (path.isEmpty() || state == RunState::Starting) {
        clear();
        return;
    }
    read(path);
    const ChromeColors c(palette());
    const bool active = runStateActive(state), finished = state == RunState::Finished;
    const int current = std::clamp(progress.started - 1, 0, std::max(0, int(tasks.size()) - 1));
    const auto number = [](double value, int digits) {
        return QString::number(value, 'f', digits).replace('-', "−");
    };
    QStringList stages;
    for (int i = 0; i < tasks.size(); ++i) {
        const bool done = finished || i < current || (i == current && progress.convergedTask == i && !active);
        const bool now = active && i == current;
        const QString status = done ? tr("完了") : now ? tr("実行中") : active ? tr("待機") : tr("未完了");
        const QColor color = done ? c.good : now ? c.accentText : c.muted;
        QString detail;
        if (progress.stepTask == i && !progress.steps.isEmpty()) {
            const auto &last = progress.steps.last();
            detail = tr("ステップ %1%2 · E = %3 Eh · ΔE %4")
                         .arg(last.step)
                         .arg(maxSteps ? tr(" / 最大 %1").arg(maxSteps) : QString())
                         .arg(number(last.energy, 6))
                         .arg(QString::number(last.deltaE, 'e', 1).replace('-', "−"));
            if (progress.convergedTask == i)
                detail = tr("%1 · 収束").arg(detail);
        } else if (now && std::isfinite(progress.energy))
            detail = QString("E = %1 Eh").arg(number(progress.energy, 6));
        stages << tr("<b>%1　%2</b>　<span style=\"color:%3\">%4</span>%5")
                      .arg(i + 1)
                      .arg(tasks[i].toHtmlEscaped(), color.name(), status)
                      .arg(detail.isEmpty() ? QString()
                                            : QString("<br><span style=\"color:%1\">%2</span>")
                                                  .arg(c.muted.name(), detail.toHtmlEscaped()));
    }
    setStages(stages);
    // The four DRIVER criteria of the last step against their thresholds.
    const bool optimizing = progress.stepTask >= 0 && !progress.steps.isEmpty() &&
                            progress.stepTask == current && !progress.thresholds.isEmpty();
    criteria->setVisible(optimizing);
    if (!optimizing)
        return;
    const auto criteria = resultCriteria(progress.steps.last(), progress.thresholds);
    for (int i = 0; i < int(criteria.size()); ++i) {
        const auto &criterion = criteria[i];
        const bool met = criterion.met();
        criterionLabels[i]->setText(
            tr("<b>%1</b>　<span style=\"color:%2\">%3</span><br><span "
               "style=\"font-family:monospace\">%4</span>"
               "<br><span style=\"color:%5\">目標 %6</span>")
                .arg(criterion.name, met ? c.good.name() : c.muted.name(),
                     met                  ? tr("達成")
                     : criterion.judged() ? tr("未達")
                                          : QString("—"))
                .arg(criterion.measured ? QString::number(criterion.value, 'f', 5) : QString("—"),
                     c.muted.name(),
                     std::isfinite(criterion.threshold) ? QString::number(criterion.threshold, 'f', 5)
                                                        : QString("—")));
    }
}
} // namespace mf
