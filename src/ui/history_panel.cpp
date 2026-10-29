// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/history_panel.h"
#include "model/i18n.h"
#include "model/units.h"
#include "nwchem/nwchem_execution.h"
#include "ui/editor_chrome.h"
#include "ui/main_window.h"
#include "ui/run_progress.h"
#include <QtWidgets>
#include <algorithm>

namespace mf {
namespace {
// Item data: the job folder of a row, the kind of its state (or of its ΔE), the line under a text,
// the length of its ΔE bar (0–1), and whether a row is the heading of a day.
constexpr int DirectoryRole = Qt::UserRole, KindRole = Qt::UserRole + 1, DetailRole = Qt::UserRole + 2,
              BarRole = Qt::UserRole + 3, GroupRole = Qt::UserRole + 4;
enum Column {
    StateColumn,
    NameColumn,
    LevelColumn,
    TaskColumn,
    CreatedColumn,
    DurationColumn,
    EnergyColumn,
    DeltaColumn,
    MemoColumn,
    ColumnCount
};
struct EnergyUnit {
    const char *key, *label;
    double factor; // Per Hartree.
    int digits;
};
constexpr EnergyUnit energyUnits[]{{"kcal", "kcal/mol", HartreeToKcal, 2},
                                   {"kj", "kJ/mol", HartreeToKj, 2},
                                   {"ev", "eV", HartreeToEv, 4},
                                   {"hartree", "Eh", 1, 8}};
// The state of a job in a word, and the kind of label it takes.
std::pair<QString, QString> stateLabel(const HistoryEntry &e, bool owned) {
    if (runStateActive(e.status) && !owned)
        return e.orphaned ? std::pair<QString, QString>{tr("プロセスなし"), "orphan"}
                          : std::pair<QString, QString>{tr("稼働未確認"), "unknown"};
    return {runStateName(e.status), runStateKind(e.status)};
}
// The names of the state filters, as their keys "", active, finished, stopped and prepared.
QStringList stateFilterNames() {
    return {tr("すべて"), tr("実行中"), tr("終了", "state"), tr("停止・失敗"), tr("保存のみ", "state")};
}
// The state filter a job falls under: active, finished, stopped (also failed and orphaned) or prepared.
QString stateGroup(const HistoryEntry &e, bool owned) {
    if (e.status == RunState::Prepared || e.status == RunState::Finished)
        return runStateKey(e.status);
    return runStateActive(e.status) && (owned || !e.orphaned) ? "active" : "stopped";
}
void paintPill(QPainter *p, const QRect &cell, const QString &text, const QString &kind, const QFont &font,
               const ChromeColors &c) {
    const auto colors = pillColors(kind, c);
    auto small = font;
    small.setPointSizeF(font.pointSizeF() * .88);
    small.setBold(true);
    const QFontMetrics metrics(small);
    const int w = std::min(metrics.horizontalAdvance(text) + 18, cell.width()), h = metrics.height() + 6;
    const QRectF pill(cell.left() + .5, cell.center().y() - h / 2. + .5, w - 1, h - 1);
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(colors.line == colors.background ? QPen(Qt::NoPen) : QPen(colors.line, 1));
    p->setBrush(colors.background);
    p->drawRoundedRect(pill, h / 2., h / 2.);
    p->setFont(small);
    p->setPen(colors.text);
    p->drawText(pill, Qt::AlignCenter, metrics.elidedText(text, Qt::ElideRight, w - 10));
}
QString hartree(double value, int digits = 8) {
    return QString::number(value, 'f', digits).replace('-', "−");
}
// "+1.84", "−0.52" or "0.00".
QString signedNumber(double value, int digits) {
    const auto text = QString::number(std::abs(value), 'f', digits);
    if (text.toDouble() == 0)
        return text;
    return (value > 0 ? "+" : "−") + text;
}
// The method in short (the functional names a DFT method), then the basis.
QString levelText(const HistoryEntry &e) {
    if (e.method.isEmpty() && e.basis.isEmpty())
        return "—";
    const auto method = e.method.startsWith("DFT / ") ? e.method.mid(6)
                        : e.method == "Hartree–Fock"  ? QString("HF")
                                                      : e.method;
    return method + " / " + e.basis;
}
// Dates in the language of the interface: "10月25日（日）", or "Sun, Oct 25".
QLocale dateLocale() {
    return english() ? QLocale(QLocale::English, QLocale::UnitedStates)
                     : QLocale(QLocale::Japanese, QLocale::Japan);
}
QString dayTitle(const QDate &day) {
    if (!day.isValid())
        return tr("作成日時なし");
    const auto today = QDate::currentDate();
    const auto date = dateLocale().toString(day, day.year() == today.year() ? tr("M月d日（ddd）")
                                                                            : tr("yyyy年M月d日（ddd）"));
    return day == today               ? tr("今日 · %1").arg(date)
           : day == today.addDays(-1) ? tr("昨日 · %1").arg(date)
                                      : date;
}
// Local times: "10月25日（日）15:20", and in the list "15:20" under a day or "10/25 15:20".
QString dayAndTime(const QDateTime &t) {
    const auto local = t.toLocalTime();
    return dateLocale().toString(local, local.date().year() == QDate::currentDate().year()
                                            ? tr("M月d日（ddd）HH:mm")
                                            : tr("yyyy年M月d日（ddd）HH:mm"));
}
QString listTime(const QDateTime &t, bool underDay) {
    const auto local = t.toLocalTime();
    return local.toString(underDay                                             ? "HH:mm"
                          : local.date().year() == QDate::currentDate().year() ? "M/d HH:mm"
                                                                               : "yyyy/M/d HH:mm");
}
// What the energy of a job is, as the run's log summary recorded it.
QString energySource(const HistoryEntry &e) {
    const auto summary = e.manifest["summary"].toObject();
    if (!summary["energy"].isDouble())
        return tr("ログ末尾の最後の値");
    const auto source = summary["energy_source"].toString();
    return source == "frequency_reference" ? tr("振動数の参照構造")
           : source == "bsse_corrected"    ? tr("BSSE補正後")
                                           : tr("最後のtask");
}
// Why a job has no ΔE against the reference; empty when it has one.
QString comparisonNote(const HistoryEntry &e, const HistoryEntry &reference) {
    if (runStateActive(e.status))
        return e.orphaned ? tr("記録の修復後に表示") : tr("終了後に表示");
    if (!e.energy)
        return tr("エネルギーの記録なし");
    return historyIncomparable(e, reference);
}
QLabel *caption(const QString &text = {}) {
    auto *label = new QLabel(text);
    label->setProperty("caption", true);
    label->setWordWrap(true);
    return label;
}
void setFlag(QWidget *w, const char *name, const QVariant &value) {
    if (w->property(name) == value)
        return;
    w->setProperty(name, value);
    repolish(w);
}
void showText(QPlainTextEdit *edit, const QString &value, bool follow = false) {
    if (edit->toPlainText() == value)
        return;
    const int old = edit->verticalScrollBar()->value();
    edit->setPlainText(value);
    edit->verticalScrollBar()->setValue(follow ? edit->verticalScrollBar()->maximum() : old);
}

// The rows of the list: a state label, the name over the composition, energies in figures, the ΔE
// with a bar, and the headings of the days.
class JobDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const int line = option.fontMetrics.height();
        const auto first = index.siblingAtColumn(StateColumn);
        const int height = first.data(GroupRole).toBool()          ? line + 14
                           : first.data(KindRole) == "placeholder" ? 4 * line
                                                                   : 2 * line + 14;
        return {QStyledItemDelegate::sizeHint(option, index).width(), height};
    }
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const ChromeColors c(option.palette);
        const auto first = index.siblingAtColumn(StateColumn);
        p->save();
        if (first.data(GroupRole).toBool()) { // A day: its date and the number of its jobs.
            p->fillRect(option.rect, c.window);
            p->setPen(c.line);
            p->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
            auto bold = option.font;
            bold.setBold(true);
            const QRect r = option.rect.adjusted(14, 0, -14, 0);
            const auto title = index.data().toString();
            p->setFont(bold);
            p->setPen(c.text);
            p->drawText(r, Qt::AlignVCenter | Qt::AlignLeft, title);
            p->setFont(option.font);
            p->setPen(c.muted);
            p->drawText(r.adjusted(QFontMetrics(bold).horizontalAdvance(title) + 10, 0, 0, 0),
                        Qt::AlignVCenter | Qt::AlignLeft, index.data(DetailRole).toString());
            p->restore();
            return;
        }
        if (first.data(KindRole) == "placeholder") {
            p->setPen(c.muted);
            p->drawText(option.rect.adjusted(14, 0, -14, 0), Qt::AlignCenter | Qt::TextWordWrap,
                        index.data().toString());
            p->restore();
            return;
        }
        QStyleOptionViewItem base = option;
        initStyleOption(&base, index);
        base.text.clear();
        option.widget->style()->drawControl(QStyle::CE_ItemViewItem, &base, p, option.widget);
        p->setPen(c.hover);
        p->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
        const QRect r = option.rect.adjusted(index.column() == StateColumn ? 14 : 6, 0, -6, 0);
        const auto text = index.data().toString(), detail = index.data(DetailRole).toString();
        auto font = option.font, small = option.font;
        small.setPointSizeF(font.pointSizeF() * .9);
        auto mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono.setPointSizeF(font.pointSizeF() * .95);
        const auto elided = [&](const QFont &f, const QString &s, int width) {
            return QFontMetrics(f).elidedText(s, Qt::ElideRight, width);
        };
        switch (index.column()) {
        case StateColumn:
            if (option.state & QStyle::State_Selected)
                p->fillRect(QRect(option.rect.left(), option.rect.top(), 3, option.rect.height()), c.accent);
            paintPill(p, r, text, index.data(KindRole).toString(), font, c);
            break;
        case NameColumn: {
            font.setBold(true);
            const int line = QFontMetrics(font).height(), below = QFontMetrics(small).height();
            p->setFont(font);
            p->setPen(c.text);
            p->drawText(QRect(r.left(), r.center().y() - line, r.width(), line),
                        Qt::AlignLeft | Qt::AlignVCenter, elided(font, text, r.width()));
            p->setFont(small);
            p->setPen(c.muted);
            p->drawText(QRect(r.left(), r.center().y() + 1, r.width(), below),
                        Qt::AlignLeft | Qt::AlignVCenter, elided(small, detail, r.width()));
            break;
        }
        case EnergyColumn: {
            const int suffix = detail.isEmpty() ? 0 : QFontMetrics(small).horizontalAdvance(detail) + 5;
            p->setFont(small);
            p->setPen(c.muted);
            p->drawText(r, Qt::AlignRight | Qt::AlignVCenter, detail);
            p->setFont(mono);
            p->setPen(text == "—" ? c.muted : c.text);
            p->drawText(r.adjusted(0, 0, -suffix, 0), Qt::AlignRight | Qt::AlignVCenter, text);
            break;
        }
        case DeltaColumn: {
            const auto kind = index.data(KindRole).toString();
            if (kind != "reference" && kind != "delta") { // Why there is no ΔE.
                p->setFont(small);
                p->setPen(c.muted);
                p->drawText(r, Qt::AlignLeft | Qt::AlignVCenter,
                            elided(small, detail.isEmpty() ? text : detail, r.width()));
                break;
            }
            auto bold = mono;
            bold.setBold(true);
            const int number = std::min(QFontMetrics(bold).horizontalAdvance("+0000.00"), r.width() / 2);
            p->setFont(bold);
            p->setPen(c.text);
            p->drawText(QRect(r.left(), r.top(), number, r.height()), Qt::AlignRight | Qt::AlignVCenter,
                        text);
            const QRect rest(r.left() + number + 10, r.top(), r.width() - number - 10, r.height());
            if (kind == "reference") {
                paintPill(p, rest, tr("基準"), "reference", font, c);
                break;
            }
            const QRectF track(rest.left(), rest.center().y() - 4, rest.width(), 8);
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(Qt::NoPen);
            p->setBrush(c.segment);
            p->drawRoundedRect(track, 4, 4);
            if (const double fraction = index.data(BarRole).toDouble(); fraction > 0) {
                p->setBrush(c.accent);
                p->drawRoundedRect(
                    QRectF(track.left(), track.top(), std::max(6., track.width() * fraction), 8), 4, 4);
            }
            break;
        }
        case DurationColumn:
            p->setPen(index.data(KindRole) == "running" ? c.accentText : text == "—" ? c.muted : c.text);
            p->drawText(r, Qt::AlignRight | Qt::AlignVCenter, text);
            break;
        case LevelColumn:
            if (!detail.isEmpty()) { // Narrow: the task under the method, in place of its column.
                const int line = QFontMetrics(font).height(), below = QFontMetrics(small).height();
                p->setPen(c.text);
                p->drawText(QRect(r.left(), r.center().y() - line, r.width(), line),
                            Qt::AlignLeft | Qt::AlignVCenter, elided(font, text, r.width()));
                p->setFont(small);
                p->setPen(c.muted);
                p->drawText(QRect(r.left(), r.center().y() + 1, r.width(), below),
                            Qt::AlignLeft | Qt::AlignVCenter, elided(small, detail, r.width()));
                break;
            }
            [[fallthrough]];
        default:
            p->setPen(index.column() == MemoColumn || text == "—" ? c.muted : c.text);
            p->drawText(r, Qt::AlignLeft | Qt::AlignVCenter, elided(font, text, r.width()));
        }
        p->restore();
    }
};
} // namespace

// The rows of the job list: the jobs that pass the search and the state filter in their order, under
// the headings of their days when the list is in order of creation, or else a line that says none
// matches. The cells are computed as the view paints them, so another search costs no widgets.
class HistoryModel : public QAbstractItemModel {
  public:
    struct Job {
        HistoryEntry entry;
        bool owned = false;          // The run of this window.
        std::optional<double> delta; // ΔE / Hartree against the reference.
        QString note;                // Why there is no ΔE.
    };
    struct Day {
        QString title, detail; // The date, and the number of its jobs.
        QDate date;
        int first = 0, count = 0; // Its jobs in the list.
    };
    struct Context {
        const EnergyUnit *unit = nullptr;
        QString reference;   // The job folder ΔE refers to.
        double largest = 0;  // The largest |ΔE|, the full length of a bar.
        bool narrow = false; // The task under the method, in place of its column.
        QString placeholder; // Shown when no job matches.
    };
    HistoryModel(QObject *parent, QStringList titles);
    void show(QVector<Job> jobs, QVector<Day> days, const Context &context);
    void setNarrow(bool on);
    void update(const HistoryEntry &e); // The same job, read again.
    QModelIndex find(const QString &directory) const;
    // Rows of the top level that span all columns: the days, or the placeholder.
    bool spanned(int row) const;
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

  private:
    QStringList titles;
    QVector<Job> jobs;
    QVector<Day> days; // Empty unless the list is in order of creation.
    Context context;
    const Job *job(const QModelIndex &index) const;
};
HistoryModel::HistoryModel(QObject *parent, QStringList titles)
    : QAbstractItemModel(parent), titles(titles) {}
void HistoryModel::show(QVector<Job> nextJobs, QVector<Day> nextDays, const Context &next) {
    beginResetModel();
    jobs = std::move(nextJobs);
    days = std::move(nextDays);
    context = next;
    endResetModel();
}
void HistoryModel::setNarrow(bool on) {
    if (context.narrow == on)
        return;
    context.narrow = on;
    const auto changed = [this](const QModelIndex &parent) {
        if (const int rows = rowCount(parent); rows > 0)
            emit dataChanged(index(0, LevelColumn, parent), index(rows - 1, LevelColumn, parent));
    };
    if (days.isEmpty())
        changed({});
    for (int day = 0; day < days.size(); ++day)
        changed(index(day, 0));
}
void HistoryModel::update(const HistoryEntry &e) {
    const auto at = find(e.directory);
    if (!at.isValid())
        return;
    jobs[days.isEmpty() ? at.row() : days[at.parent().row()].first + at.row()].entry = e;
    emit dataChanged(at, at.siblingAtColumn(ColumnCount - 1));
}
QModelIndex HistoryModel::find(const QString &directory) const {
    for (int i = 0; !directory.isEmpty() && i < jobs.size(); ++i) {
        if (jobs[i].entry.directory != directory)
            continue;
        for (int day = 0; day < days.size(); ++day)
            if (i < days[day].first + days[day].count)
                return index(i - days[day].first, 0, index(day, 0));
        return index(i, 0);
    }
    return {};
}
bool HistoryModel::spanned(int row) const {
    return jobs.isEmpty() ? row == 0 : !days.isEmpty();
}
const HistoryModel::Job *HistoryModel::job(const QModelIndex &index) const {
    if (!index.isValid() || jobs.isEmpty())
        return nullptr;
    if (index.internalId() == 0)
        return days.isEmpty() ? &jobs[index.row()] : nullptr;
    return &jobs[days[int(index.internalId()) - 1].first + index.row()];
}
QModelIndex HistoryModel::index(int row, int column, const QModelIndex &parent) const {
    if (!hasIndex(row, column, parent))
        return {};
    return createIndex(row, column, parent.isValid() ? quintptr(parent.row() + 1) : quintptr(0));
}
QModelIndex HistoryModel::parent(const QModelIndex &child) const {
    if (!child.isValid() || child.internalId() == 0)
        return {};
    return createIndex(int(child.internalId()) - 1, 0, quintptr(0));
}
int HistoryModel::rowCount(const QModelIndex &parent) const {
    if (!parent.isValid())
        return jobs.isEmpty()   ? int(!context.placeholder.isEmpty())
               : days.isEmpty() ? int(jobs.size())
                                : int(days.size());
    if (parent.internalId() == 0 && parent.column() == 0 && !jobs.isEmpty() && !days.isEmpty())
        return days[parent.row()].count;
    return 0;
}
int HistoryModel::columnCount(const QModelIndex &) const {
    return ColumnCount;
}
QVariant HistoryModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 || section >= ColumnCount)
        return QAbstractItemModel::headerData(section, orientation, role);
    return section == DeltaColumn && context.unit ? QString("ΔE / ") + context.unit->label : titles[section];
}
Qt::ItemFlags HistoryModel::flags(const QModelIndex &index) const {
    if (!index.isValid())
        return Qt::NoItemFlags;
    return job(index) ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::ItemIsEnabled;
}
QVariant HistoryModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid())
        return {};
    const auto *j = job(index);
    if (!j) { // The heading of a day, or the line that says nothing matches.
        if (index.column() != StateColumn)
            return {};
        if (jobs.isEmpty())
            return role == Qt::DisplayRole ? QVariant(context.placeholder)
                   : role == KindRole      ? QVariant("placeholder")
                                           : QVariant();
        const auto &day = days[index.row()];
        return role == Qt::DisplayRole ? QVariant(day.title)
               : role == DetailRole    ? QVariant(day.detail)
               : role == GroupRole     ? QVariant(true)
                                       : QVariant();
    }
    const auto &e = j->entry;
    const int column = index.column();
    if (role == DirectoryRole)
        return e.directory;
    if (role == Qt::ToolTipRole) {
        if (column == MemoColumn && !e.memo.isEmpty())
            return e.memo;
        return historyStatus(e.status, j->owned, e.orphaned) + "\n" + displayPath(e.directory) +
               (e.warnings.isEmpty() ? "" : "\n" + e.warnings.join('\n'));
    }
    const auto &unit = *context.unit;
    switch (column) {
    case StateColumn: {
        const auto [label, kind] = stateLabel(e, j->owned);
        return role == Qt::DisplayRole ? QVariant(label) : role == KindRole ? QVariant(kind) : QVariant();
    }
    case NameColumn:
        return role == Qt::DisplayRole ? QVariant(e.name)
               : role == DetailRole    ? QVariant(e.formula.isEmpty() ? tr("組成不明") : e.formula)
                                       : QVariant();
    case LevelColumn:
        return role == Qt::DisplayRole                ? QVariant(levelText(e))
               : role == DetailRole && context.narrow ? QVariant(e.task.isEmpty() ? QString("—") : e.task)
                                                      : QVariant();
    case TaskColumn:
        return role == Qt::DisplayRole ? QVariant(e.task.isEmpty() ? QString("—") : e.task) : QVariant();
    case CreatedColumn:
        return role == Qt::DisplayRole
                   ? QVariant(e.created.isValid() ? listTime(e.created, !days.isEmpty()) : QString("—"))
                   : QVariant();
    case DurationColumn:
        return role == Qt::DisplayRole ? QVariant(e.seconds < 0 ? QString("—") : elapsedClock(e.seconds))
               : role == KindRole      ? QVariant(j->owned ? "running" : "")
                                       : QVariant();
    case EnergyColumn:
        return role == Qt::DisplayRole ? QVariant(e.energy ? hartree(*e.energy) : QString("—"))
               : role == DetailRole    ? QVariant(e.energy ? historyEnergyMethod(e.energyMethod) : QString())
                                       : QVariant();
    case DeltaColumn:
        if (!j->delta)
            return role == Qt::DisplayRole ? QVariant("—")
                   : role == DetailRole    ? QVariant(j->note)
                                           : QVariant();
        return role == Qt::DisplayRole ? QVariant(signedNumber(*j->delta * unit.factor, unit.digits))
               : role == KindRole      ? QVariant(e.directory == context.reference ? "reference" : "delta")
               : role == BarRole ? QVariant(context.largest > 0 ? std::abs(*j->delta) / context.largest : 0.)
                                 : QVariant();
    case MemoColumn:
        return role == Qt::DisplayRole ? QVariant(e.memo) : QVariant();
    default:
        return {};
    }
}

HistoryPanel::HistoryPanel(MainWindow *owner, const NwchemRun &activeRun)
    : QWidget(owner), window(owner), run(activeRun), scan(this) {
    setObjectName("historyTab");
    setAttribute(Qt::WA_StyledBackground);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // The folder, search and the states, comparing, refresh and the CSV of the list.
    auto *toolbar = new QFrame;
    toolbar->setObjectName("historyToolbar");
    toolbar->setProperty("toolbar", true);
    auto *tools = new FlowLayout(toolbar, 10, 6);
    tools->setContentsMargins(16, 8, 16, 8);
    outer->addWidget(toolbar);
    auto *rootBox = new QWidget;
    auto *rootRow = new QHBoxLayout(rootBox);
    rootRow->setContentsMargins(0, 0, 0, 0);
    rootRow->setSpacing(6);
    auto *rootCaption = caption(tr("ジョブ保存先"));
    rootCaption->setWordWrap(false);
    rootRow->addWidget(rootCaption);
    rootButton = new QToolButton;
    rootButton->setObjectName("historyRootButton");
    rootButton->setProperty("chip", true);
    rootButton->setIcon(lineIcon("folder"));
    rootButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    rootButton->setPopupMode(QToolButton::InstantPopup);
    auto *rootMenu = new QMenu(rootButton);
    rootMenu->setObjectName("historyRootMenu");
    browseAction = rootMenu->addAction(lineIcon("folder"), tr("保存先を選ぶ…"));
    browseAction->setObjectName("historyBrowse");
    // With --mcp-allow-dir, MCP scans only checked paths through history_action.
    browseAction->setProperty("mcpPath", true);
    executionRootAction = rootMenu->addAction(lineIcon("reset"), tr("「3 実行設定」の保存先に戻す"));
    executionRootAction->setObjectName("historyExecutionRoot");
    executionRootAction->setProperty("mcpPath", true);
    rootMenu->addSeparator();
    auto *copyRoot = rootMenu->addAction(lineIcon("copy"), tr("保存先のパスをコピー"));
    copyRoot->setObjectName("historyCopyRoot");
    rootButton->setMenu(rootMenu);
    rootRow->addWidget(rootButton);
    tools->addWidget(rootBox);
    search = new QLineEdit;
    search->setObjectName("historySearch");
    search->setProperty("search", true);
    search->setPlaceholderText(tr("計算名・手法・基底・組成・メモで検索"));
    search->setClearButtonEnabled(true);
    search->addAction(lineIcon("search"), QLineEdit::LeadingPosition);
    search->setFixedWidth(280);
    tools->addWidget(search);
    filter = new QComboBox;
    filter->setObjectName("historyFilter");
    filter->addItem(tr("すべての状態"), "");
    filter->addItem(tr("実行中・稼働未確認"), "active");
    filter->addItem(tr("終了（コード0）"), "finished");
    filter->addItem(tr("停止・失敗・中断・プロセスなし"), "stopped");
    filter->addItem(tr("保存のみ（未実行）"), "prepared");
    filterButtons = new ChoiceButtons(filter, ChoiceButtons::Segments, stateFilterNames());
    filterButtons->setObjectName("historyFilters");
    tools->addWidget(filterButtons);
    auto *gap = new QWidget;
    gap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tools->addWidget(gap);
    compare = new QPushButton(tr("ΔEを比べる"));
    compare->setObjectName("historyCompare");
    compare->setIcon(lineIcon("compare"));
    compare->setCheckable(true);
    compare->setToolTip(tr("計算のエネルギーの差（ΔE）を、基準のジョブに対して一覧に表示します。"));
    tools->addWidget(compare);
    energyUnit = new QComboBox;
    energyUnit->setObjectName("historyEnergyUnit");
    for (const auto &u : energyUnits)
        energyUnit->addItem(u.label, u.key);
    unitButtons = new ChoiceButtons(energyUnit, ChoiceButtons::Segments);
    unitButtons->setObjectName("historyEnergyUnits");
    unitButtons->hide();
    tools->addWidget(unitButtons);
    refreshButton = new QPushButton;
    refreshButton->setObjectName("historyRefresh");
    refreshButton->setIcon(lineIcon("refresh"));
    refreshButton->setToolTip(tr("一覧を更新"));
    refreshButton->setAccessibleName(tr("一覧を更新"));
    tools->addWidget(refreshButton);
    cancel = new QPushButton(tr("読込を中止"));
    cancel->setObjectName("historyCancel");
    cancel->setEnabled(false);
    cancel->hide();
    tools->addWidget(cancel);
    // Narrow, the toolbar keeps one row: "ΔEを比べる" as its icon, a narrower search, no caption and
    // the folder alone.
    toolbarFit = new ToolbarFit(toolbar, [this, rootCaption](bool compact) {
        compactToolbar = compact;
        setIconOnly(compare, compact);
        rootCaption->setVisible(!compact);
        search->setFixedWidth(compact ? 200 : 280);
        updateToolbar();
    });
    // What is seldom needed, so the toolbar stays one row.
    auto *more = new QToolButton;
    more->setObjectName("historyMore");
    more->setText("…");
    more->setToolTip(tr("そのほかの操作"));
    more->setAccessibleName(tr("そのほかの操作"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto *moreMenu = new QMenu(more);
    csv = moreMenu->addAction(lineIcon("export"), tr("一覧をCSVで書き出す…"));
    csv->setObjectName("historyCsv");
    csv->setToolTip(tr("表示中の一覧を、絞り込みと並び順のままCSVに書き出します。"));
    more->setMenu(moreMenu);
    tools->addWidget(more);

    // Jobs recorded as running whose process has ended.
    orphanBanner = new QFrame;
    orphanBanner->setObjectName("historyOrphanBanner");
    orphanBanner->setProperty("banner", true);
    auto *bannerRow = new QHBoxLayout(orphanBanner);
    bannerRow->setContentsMargins(16, 7, 16, 7);
    bannerRow->setSpacing(12);
    orphanText = new QLabel;
    orphanText->setObjectName("historyOrphanText");
    orphanText->setWordWrap(true);
    orphanText->setTextFormat(Qt::PlainText);
    bannerRow->addWidget(orphanText, 1);
    repairAll = new QPushButton(tr("記録を修復"));
    repairAll->setObjectName("historyRepairAll");
    bannerRow->addWidget(repairAll);
    orphanBanner->hide();
    outer->addWidget(orphanBanner);

    pages = new QStackedWidget;
    pages->setObjectName("historyPages");
    outer->addWidget(pages, 1);
    auto *split = new QSplitter(Qt::Horizontal);
    split->setObjectName("historySplit");
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);
    pages->addWidget(split);

    // The list, and the files of the selected job below it.
    auto *left = new QWidget;
    left->setProperty("canvas", true);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(16, 14, 16, 14);
    listSplit = new QSplitter(Qt::Vertical);
    listSplit->setObjectName("historyListSplit");
    listSplit->setChildrenCollapsible(false);
    listSplit->setHandleWidth(12);
    leftLayout->addWidget(listSplit);
    auto *listCard = new QFrame;
    listCard->setObjectName("historyListCard");
    listCard->setProperty("card", true);
    auto *listLayout = new QVBoxLayout(listCard);
    listLayout->setContentsMargins(1, 1, 1, 1);
    listLayout->setSpacing(0);
    table = new QTreeView;
    table->setObjectName("historyTable");
    model = new HistoryModel(this, {tr("状態"), tr("計算名 / 組成"), tr("手法 / 基底"), tr("目的"),
                                    tr("作成"), tr("所要"), tr("計算のE / Eh"), "ΔE", tr("メモ")});
    table->setModel(model);
    table->setRootIsDecorated(false);
    table->setIndentation(0);
    table->setItemsExpandable(false);
    table->setUniformRowHeights(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setFrameShape(QFrame::NoFrame);
    table->setItemDelegate(new JobDelegate(table));
    table->setMouseTracking(true);
    auto *header = table->header();
    header->setSectionsClickable(true);
    header->setSortIndicatorShown(true);
    header->setSortIndicator(sortColumn, sortOrder);
    header->setHighlightSections(false);
    header->setStretchLastSection(false);
    header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    header->setMinimumSectionSize(48);
    const QList<std::pair<int, int>> widths{{StateColumn, 100},   {NameColumn, 150},   {CreatedColumn, 64},
                                            {DurationColumn, 66}, {EnergyColumn, 150}, {DeltaColumn, 180}};
    for (const auto &[column, width] : widths)
        table->setColumnWidth(column, width);
    // The method, the task and the memo share what is left.
    for (const int column : {LevelColumn, TaskColumn, MemoColumn})
        header->setSectionResizeMode(column, QHeaderView::Stretch);
    table->setColumnHidden(DeltaColumn, true);
    listLayout->addWidget(table, 1);
    auto *footer = new QFrame;
    footer->setObjectName("historyListFooter");
    auto *footerRow = new QHBoxLayout(footer);
    footerRow->setContentsMargins(14, 6, 14, 6);
    footerRow->setSpacing(16);
    status = caption();
    status->setObjectName("historyStatus");
    status->setTextFormat(Qt::PlainText);
    footerRow->addWidget(status);
    note = caption();
    note->setObjectName("historyNote");
    note->setTextFormat(Qt::PlainText);
    footerRow->addWidget(note, 1);
    listLayout->addWidget(footer);
    listSplit->addWidget(listCard);

    filesCard = new SettingsCard(tr("ファイル", "job files"));
    filesCard->setObjectName("historyFilesCard");
    // Folded like the other cards; setFilesOpen() also hands its height to the list.
    filesToggle = filesCard->setCollapsible(filesOpen);
    filesToggle->setObjectName("historyFilesToggle");
    fileChoice = new QComboBox;
    fileChoice->setObjectName("historyFile");
    fileChoice->addItem(tr("保存した入力（input.nw）"), "input.nw");
    fileChoice->addItem(tr("標準出力（output.out・末尾128 KiB）"), "output.out");
    fileChoice->addItem(tr("標準エラー（stderr.err・末尾128 KiB）"), "stderr.err");
    fileChoice->addItem(tr("実行記録（run.json）"), "run.json");
    auto *fileButtons = new ChoiceButtons(fileChoice, ChoiceButtons::Segments,
                                          {tr("入力"), tr("標準出力"), tr("標準エラー"), tr("実行記録")});
    fileButtons->setObjectName("historyFiles");
    filesCard->header()->insertWidget(2, fileButtons);
    fileName = caption();
    fileName->setObjectName("historyFileName");
    fileName->setWordWrap(false);
    fileName->setProperty("mono", true);
    filesCard->header()->insertWidget(3, fileName);
    follow = new QCheckBox(tr("追尾"));
    follow->setObjectName("historyFollow");
    follow->setChecked(true);
    follow->setToolTip(tr("表示中は1秒ごとにログの末尾（128 KiB）を読み直し、最後の行を表示します。"));
    filesCard->header()->addWidget(follow);
    copyFile = copyButton("historyCopyFile", tr("表示中のファイルをコピー"));
    filesCard->header()->addWidget(copyFile);
    fileStack = new QStackedWidget;
    const auto pane = [&](const QString &name) {
        auto *p = logView(name);
        fileStack->addWidget(p);
        return p;
    };
    input = pane("historyInput");
    output = pane("historyOutput");
    errors = pane("historyError");
    record = pane("historyRecord");
    fileStack->setMinimumHeight(120);
    filesCard->body()->addWidget(fileStack, 1);
    listSplit->addWidget(filesCard);
    listSplit->setStretchFactor(0, 1);
    listSplit->setStretchFactor(1, 0);
    listSplit->setSizes({600, filesHeight});
    split->addWidget(left);

    // The selected job.
    auto *detailScroll = new QScrollArea;
    detailScroll->setObjectName("historyDetailScroll");
    detailScroll->setWidgetResizable(true);
    detailScroll->setFrameShape(QFrame::NoFrame);
    detailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailScroll->setMinimumWidth(320);
    auto *rail = new QWidget;
    rail->setObjectName("historyRail");
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(20, 18, 20, 18);
    railLayout->setSpacing(0);
    detailEmpty = new QLabel(tr("一覧のジョブを選ぶと、エネルギー・記録・メモと操作をここに表示します。"));
    detailEmpty->setObjectName("historyDetailEmpty");
    detailEmpty->setProperty("muted", true);
    detailEmpty->setWordWrap(true);
    railLayout->addWidget(detailEmpty);
    detail = new QWidget;
    detail->setObjectName("historyDetail");
    railLayout->addWidget(detail, 1);
    detailScroll->setWidget(rail);
    split->addWidget(detailScroll);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    split->setSizes({960, 400});
    auto *d = new QVBoxLayout(detail);
    d->setContentsMargins(0, 0, 0, 0);
    d->setSpacing(14);
    auto *heading = new QVBoxLayout;
    heading->setSpacing(4);
    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(10);
    title = new ElidedLabel;
    title->setObjectName("historyDetailTitle");
    titleRow->addWidget(title, 1);
    statePill = new QLabel;
    statePill->setObjectName("historyDetailState");
    titleRow->addWidget(statePill);
    heading->addLayout(titleRow);
    level = new QLabel;
    level->setObjectName("historyDetailLevel");
    level->setWordWrap(true);
    level->setTextFormat(Qt::PlainText);
    heading->addWidget(level);
    facts = caption();
    facts->setObjectName("historyDetailFacts");
    facts->setTextFormat(Qt::PlainText);
    heading->addWidget(facts);
    d->addLayout(heading);

    energyBox = new QFrame;
    energyBox->setObjectName("historyEnergyBox");
    auto *energyLayout = new QVBoxLayout(energyBox);
    energyLayout->setContentsMargins(14, 12, 14, 12);
    energyLayout->setSpacing(2);
    energyCaption = caption();
    energyCaption->setObjectName("historyEnergyCaption");
    energyLayout->addWidget(energyCaption);
    auto *valueRow = new QHBoxLayout;
    valueRow->setSpacing(8);
    energyValue = new QLabel;
    energyValue->setObjectName("historyEnergyValue");
    energyValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    valueRow->addWidget(energyValue);
    energySuffix = new QLabel;
    energySuffix->setObjectName("historyEnergySuffix");
    valueRow->addWidget(energySuffix, 1, Qt::AlignBaseline);
    energyLayout->addLayout(valueRow);
    energyNote = caption();
    energyNote->setObjectName("historyEnergyNote");
    energyNote->setTextFormat(Qt::PlainText);
    energyLayout->addWidget(energyNote);
    d->addWidget(energyBox);

    // A run of this window: its tasks, steps and criteria as in "3 実行設定".
    progressBox = new QFrame;
    progressBox->setObjectName("historyProgressBox");
    auto *progressLayout = new QVBoxLayout(progressBox);
    progressLayout->setContentsMargins(14, 12, 14, 12);
    progressLayout->setSpacing(10);
    auto *progressHead = new QHBoxLayout;
    auto *progressTitle = new QLabel(tr("進み具合"));
    progressTitle->setProperty("cardTitle", true);
    progressHead->addWidget(progressTitle);
    progressTiming = caption();
    progressTiming->setObjectName("historyProgressTiming");
    progressHead->addWidget(progressTiming, 1);
    progressLayout->addLayout(progressHead);
    progress = new RunProgress("history", 2);
    progressLayout->addWidget(progress);
    progressBox->hide();
    d->addWidget(progressBox);

    factGrid = new QGridLayout;
    factGrid->setHorizontalSpacing(12);
    factGrid->setVerticalSpacing(6);
    factGrid->setColumnMinimumWidth(0, 72);
    factGrid->setColumnStretch(1, 1);
    d->addLayout(factGrid);

    memoBox = new QWidget;
    auto *memoLayout = new QVBoxLayout(memoBox);
    memoLayout->setContentsMargins(0, 0, 0, 0);
    memoLayout->setSpacing(6);
    memoLayout->addWidget(caption(tr("メモ")));
    memo = new QPlainTextEdit;
    memo->setObjectName("historyMemo");
    memo->setPlaceholderText(tr("このジョブについてのメモ（ジョブフォルダーの memo.txt に保存します）"));
    memo->setTabChangesFocus(true);
    memo->setFixedHeight(memo->fontMetrics().lineSpacing() * 3 + 16);
    memo->installEventFilter(this);
    memoLayout->addWidget(memo);
    memoStatus = caption();
    memoStatus->setObjectName("historyMemoStatus");
    memoStatus->setProperty("bad", true);
    memoStatus->hide();
    memoLayout->addWidget(memoStatus);
    d->addWidget(memoBox);

    actionGrid = new QGridLayout;
    actionGrid->setSpacing(8);
    actionGrid->setColumnStretch(0, 1);
    actionGrid->setColumnStretch(1, 1);
    // Placed by arrangeActions() for the selected job; owned by the panel while not placed.
    const auto action = [&](const QString &text, const QString &name, const char *look = nullptr) {
        auto *b = new QPushButton(text, detail);
        b->setObjectName(name);
        if (look)
            b->setProperty(look, true);
        b->hide();
        return b;
    };
    viewer = action(tr("結果ビューアで開く"), "historyResults", "primary");
    reuse = action(tr("分子・条件を再利用"), "historyReuse");
    reuse->setToolTip(tr("保存した分子と条件を、新しい未保存のプロジェクトとして開きます。"));
    folder = action(tr("保存先を開く"), "historyFolder");
    folder->setIcon(lineIcon("folder"));
    stop = action(tr("この計算を停止"), "historyStop", "danger");
    stop->setIcon(lineIcon("stop"));
    setReference = action(tr("このジョブを基準にΔEを比べる"), "historyReference", "link");
    showExecution = action(tr("実行状況とログを表示"), "historyExecution", "link");
    d->addLayout(actionGrid);
    d->addStretch(1);

    repairBox = new QFrame;
    repairBox->setObjectName("historyRepairBox");
    auto *repairLayout = new QVBoxLayout(repairBox);
    repairLayout->setContentsMargins(12, 10, 12, 10);
    repairLayout->setSpacing(8);
    auto *repairText =
        new QLabel(tr("このPCで「実行中」と記録されたまま、計算のプロセスが終了しています。記録を"
                      "「中断」に直し、ログからエネルギーを求めます。"));
    repairText->setWordWrap(true);
    repairLayout->addWidget(repairText);
    repair = new QPushButton(tr("記録を修復"));
    repair->setObjectName("historyRepair");
    repairLayout->addWidget(repair, 0, Qt::AlignLeft);
    repairBox->hide();
    d->addWidget(repairBox);
    stopComparing = action(tr("比較をやめる"), "historyCompareOff", "link");
    d->addWidget(stopComparing, 0, Qt::AlignLeft);
    trashRow = new QFrame;
    trashRow->setObjectName("historyTrashRow");
    auto *trashLayout = new QHBoxLayout(trashRow);
    trashLayout->setContentsMargins(0, 12, 0, 0);
    trashLayout->setSpacing(10);
    trashNote = caption();
    trashNote->setObjectName("historyTrashNote");
    trashLayout->addWidget(trashNote, 1);
    trash = new QPushButton(tr("ごみ箱へ"));
    trash->setObjectName("historyTrash");
    trash->setIcon(lineIcon("delete"));
    trash->setProperty("danger", true);
    trashLayout->addWidget(trash, 0, Qt::AlignVCenter);
    d->addWidget(trashRow);
    detail->hide();

    // A folder without jobs.
    auto *emptyPage = new QWidget;
    emptyPage->setProperty("canvas", true);
    auto *emptyLayout = new QVBoxLayout(emptyPage);
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    auto *emptyCard = new QFrame;
    emptyCard->setObjectName("historyEmptyCard");
    emptyCard->setFixedWidth(560);
    auto *card = new QVBoxLayout(emptyCard);
    card->setContentsMargins(40, 32, 40, 32);
    card->setSpacing(12);
    auto *emptyIcon = new QLabel;
    emptyIcon->setPixmap(lineIcon("folder", ChromeColors(palette()).muted).pixmap(48, 48));
    emptyIcon->setAlignment(Qt::AlignCenter);
    card->addWidget(emptyIcon);
    emptyTitle = new QLabel;
    emptyTitle->setObjectName("historyEmptyTitle");
    emptyTitle->setProperty("cardTitle", true);
    emptyTitle->setAlignment(Qt::AlignCenter);
    emptyTitle->setWordWrap(true);
    card->addWidget(emptyTitle);
    emptyText = new QLabel;
    emptyText->setObjectName("historyEmptyText");
    emptyText->setProperty("muted", true);
    emptyText->setAlignment(Qt::AlignCenter);
    emptyText->setWordWrap(true);
    emptyText->setTextFormat(Qt::PlainText);
    card->addWidget(emptyText);
    auto *emptyButtons = new QHBoxLayout;
    emptyButtons->addStretch();
    auto *emptyBrowse = new QPushButton(tr("保存先を選ぶ…"));
    emptyBrowse->setObjectName("historyEmptyBrowse");
    emptyBrowse->setIcon(lineIcon("folder", Qt::white));
    emptyBrowse->setProperty("primary", true);
    emptyBrowse->setProperty("mcpPath", true);
    emptyButtons->addWidget(emptyBrowse);
    emptyBack = new QPushButton(tr("3 実行設定の保存先に戻す"));
    emptyBack->setObjectName("historyEmptyExecutionRoot");
    emptyBack->setProperty("mcpPath", true);
    emptyButtons->addWidget(emptyBack);
    emptyButtons->addStretch();
    card->addLayout(emptyButtons);
    emptyExecution = caption();
    emptyExecution->setObjectName("historyEmptyExecution");
    emptyExecution->setAlignment(Qt::AlignCenter);
    emptyExecution->setTextFormat(Qt::PlainText);
    card->addWidget(emptyExecution);
    emptyLayout->addStretch();
    emptyLayout->addWidget(emptyCard, 0, Qt::AlignHCenter);
    emptyLayout->addStretch();
    pages->addWidget(emptyPage);

    connect(browseAction, &QAction::triggered, this, [this] {
        const auto p = QFileDialog::getExistingDirectory(this, tr("ジョブ保存先"), root);
        if (!p.isEmpty())
            setRoot(p);
    });
    connect(executionRootAction, &QAction::triggered, this, [this] {
        emit executionCommitRequested();
        setRoot(executionRoot(), false);
    });
    connect(copyRoot, &QAction::triggered, this, [this] { QApplication::clipboard()->setText(root); });
    connect(emptyBrowse, &QPushButton::clicked, browseAction, &QAction::trigger);
    connect(emptyBack, &QPushButton::clicked, executionRootAction, &QAction::trigger);
    // Another search, state or order starts at the top of the list; rescans keep the place.
    connect(search, &QLineEdit::textChanged, this, [this] {
        table->scrollToTop();
        populate();
    });
    connect(filter, &QComboBox::currentIndexChanged, this, [this] {
        table->scrollToTop();
        populate();
    });
    connect(compare, &QPushButton::toggled, this, &HistoryPanel::setComparing);
    connect(energyUnit, &QComboBox::currentIndexChanged, this, [this] { populate(false); });
    connect(refreshButton, &QPushButton::clicked, this, &HistoryPanel::refresh);
    connect(cancel, &QPushButton::clicked, this, [this] {
        scan.cancel();
        cancel->setEnabled(false);
        cancel->hide();
        refreshButton->setEnabled(true);
        status->setText(tr("読込を中止しました。前の一覧を表示しています。"));
    });
    connect(csv, &QAction::triggered, this, [this] {
        auto p = QFileDialog::getSaveFileName(this, tr("表示中の一覧を書き出す"), "nwchem-history.csv",
                                              "CSV (*.csv)");
        if (!p.isEmpty())
            window->safe([&] {
                if (QFileInfo(p).suffix().isEmpty())
                    p += ".csv";
                exportCsv(p);
            });
    });
    connect(repairAll, &QPushButton::clicked, this, [this] { window->safe([this] { repairOrphans(); }); });
    connect(&scan, &HistoryScan::finished, this, [this] {
        entries = scan.entries;
        for (const auto &path : std::as_const(changedJobs)) {
            bool found = false;
            for (auto &e : entries)
                if (e.directory == path) {
                    e = readHistoryEntry(path);
                    found = true;
                    break;
                }
            if (!found && entries.size() < 2000)
                entries << readHistoryEntry(path);
        }
        changedJobs.clear();
        scanned = true;
        cancel->setEnabled(false);
        cancel->hide();
        refreshButton->setEnabled(true);
        toolbarFit->refit(); // Without "読込を中止", and with the folder it scanned.
        populate();
        if (!scan.message.isEmpty())
            status->setText(status->text() + "\n" + scan.message);
    });
    connect(table->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &HistoryPanel::showDetails);
    connect(table, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (!index.data(DirectoryRole).toString().isEmpty())
            window->safe([this] { openResult(); });
    });
    connect(header, &QHeaderView::sortIndicatorChanged, this, [this](int column, Qt::SortOrder order) {
        sortColumn = column;
        sortOrder = order;
        table->scrollToTop();
        populate(false);
    });
    connect(fileChoice, &QComboBox::currentIndexChanged, this, [this](int index) {
        fileStack->setCurrentIndex(index);
        const auto name = fileChoice->currentData().toString();
        fileName->setText(index == 1 || index == 2 ? tr("%1 · 末尾 128 KiB").arg(name) : name);
        follow->setEnabled(index == 1 || index == 2);
        if (!filesOpen)
            setFilesOpen(true);
    });
    fileChoice->setCurrentIndex(1);
    connect(follow, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            updateLogs();
    });
    connect(copyFile, &QToolButton::clicked, this, [this] {
        QApplication::clipboard()->setText(
            static_cast<QPlainTextEdit *>(fileStack->currentWidget())->toPlainText());
    });
    connect(filesToggle, &QToolButton::clicked, this, [this](bool on) { setFilesOpen(on); });
    connect(viewer, &QPushButton::clicked, this, [this] { window->safe([this] { openResult(); }); });
    connect(reuse, &QPushButton::clicked, this, [this] { window->safe([this] { reuseSelected(); }); });
    connect(folder, &QPushButton::clicked, this, [this] {
        window->safe([this] {
            if (selectedDirectory().isEmpty())
                return;
            window->checkMcpPath(selectedDirectory());
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(selectedDirectory())))
                fail(tr("保存先を開けません。"));
        });
    });
    connect(stop, &QPushButton::clicked, this, [this] {
        if (!selectedDirectory().isEmpty() && selectedDirectory() == activeDirectory())
            emit stopRequested();
    });
    connect(setReference, &QPushButton::clicked, this, [this] {
        const auto *e = selectedEntry();
        if (!e || !e->energy || runStateActive(e->status))
            return;
        reference = e->directory;
        table->scrollToTop();
        if (comparing)
            populate(true);
        else
            setComparing(true);
    });
    connect(showExecution, &QPushButton::clicked, this, [this] { emit runPanelRequested(); });
    connect(stopComparing, &QPushButton::clicked, this, [this] { setComparing(false); });
    connect(repair, &QPushButton::clicked, this, [this] { window->safe([this] { repairSelected(); }); });
    connect(trash, &QPushButton::clicked, this, [this] { window->safe([this] { trashSelected(); }); });
    memoTimer.setSingleShot(true);
    memoTimer.setInterval(600);
    connect(&memoTimer, &QTimer::timeout, this, &HistoryPanel::saveMemo);
    connect(memo, &QPlainTextEdit::textChanged, this, [this] {
        if (!loadingMemo)
            memoTimer.start();
    });
    timer.setInterval(1000);
    connect(&timer, &QTimer::timeout, this, [this] {
        if (!isVisible())
            return;
        const auto active = activeDirectory();
        if (!active.isEmpty())
            notifyJob(active);
        if (follow->isChecked())
            updateLogs();
        updateActions();
        updateLive();
    });
    timer.start();
    const auto saved = window->preference("history/root").toString();
    followsExecution = saved.isEmpty();
    root = saved;
    // The files are opened when wanted: the list first.
    setFilesOpen(window->preference("history/files_open", false).toBool(), false);
    updateActions();
}
void HistoryPanel::fitDetail() {
    // A narrow window gives the list the room of an empty detail column, a narrower detail column
    // and one column less: the task goes under the method.
    const bool narrow = width() < 1250;
    if (auto *scroll = findChild<QWidget *>("historyDetailScroll")) {
        scroll->setVisible(!narrow || selectedEntry());
        scroll->setMinimumWidth(narrow ? 280 : 320);
    }
    if (narrow != narrowList) {
        narrowList = narrow;
        // The detail column keeps its width as the window changes: give it less, or back.
        if (auto *split = findChild<QSplitter *>("historySplit"); split && split->width() > 0) {
            const int detail = narrow ? 300 : 400;
            split->setSizes({std::max(0, split->width() - detail), detail});
        }
        table->setColumnHidden(TaskColumn, narrow);
        model->setNarrow(narrow);
    }
}
void HistoryPanel::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    fitDetail();
}
void HistoryPanel::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    setFilesOpen(filesOpen, false); // The splitter has its height now.
    if (followsExecution) {
        const auto next = executionRoot();
        if (next != root) {
            setRoot(next, false);
            return;
        }
    }
    refresh();
}
bool HistoryPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == memo && event->type() == QEvent::FocusOut)
        saveMemo();
    return QWidget::eventFilter(watched, event);
}
QString HistoryPanel::executionRoot() const {
    return historyRoot(window->document(), window->projectPath().isEmpty()
                                               ? QDir::currentPath()
                                               : QFileInfo(window->projectPath()).absolutePath());
}
void HistoryPanel::setRoot(const QString &path, bool remember) {
    auto value = path.trimmed();
    if (value == "~")
        value = QDir::homePath();
    else if (value.startsWith("~/"))
        value = QDir::home().filePath(value.mid(2));
    if (value.isEmpty())
        return;
    value = QFileInfo(value).absoluteFilePath();
    followsExecution = !remember;
    window->setPreference("history/root", remember ? value : QString());
    if (root != value) {
        saveMemo();
        scan.cancel();
        entries.clear();
        changedJobs.clear();
        pendingSelection.clear();
        reference.clear();
        scanned = false;
        root = value;
        populate();
    }
    refresh();
}
void HistoryPanel::refresh() {
    updateToolbar();
    if (root.isEmpty())
        return;
    try { // Showing the tab over MCP must not scan outside --mcp-allow-dir.
        window->checkMcpPath(root);
    } catch (const std::exception &e) {
        status->setText(QString::fromUtf8(e.what()));
        return;
    }
    if (pendingSelection.isEmpty())
        pendingSelection = selectedDirectory();
    cancel->setEnabled(true);
    cancel->show();
    refreshButton->setEnabled(false);
    status->setText(tr("ジョブを読み込んでいます…"));
    scan.start(root);
}
void HistoryPanel::selectDirectory(const QString &path) {
    if (path.isEmpty())
        return;
    const auto parent = QFileInfo(path).absolutePath();
    if (parent != root)
        setRoot(parent);
    pendingSelection = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    refresh();
}
QString HistoryPanel::selectedDirectory() const {
    const auto selected = table->selectionModel()->selectedRows();
    return selected.isEmpty() ? QString() : selected.first().data(DirectoryRole).toString();
}
const HistoryEntry *HistoryPanel::entry(const QString &directory) const {
    if (directory.isEmpty())
        return nullptr;
    for (const auto &e : entries)
        if (e.directory == directory)
            return &e;
    return nullptr;
}
const HistoryEntry *HistoryPanel::selectedEntry() const {
    return entry(selectedDirectory());
}
QString HistoryPanel::activeDirectory() const {
    return run.active() ? run.plan().directory : QString();
}
QVector<HistoryEntry> HistoryPanel::visibleEntries() const {
    QVector<HistoryEntry> visible;
    for (const auto &directory : listed)
        if (const auto *e = entry(directory))
            visible << *e;
    return visible;
}
void HistoryPanel::populate(bool reloadDetails) {
    const auto selected = pendingSelection.isEmpty() ? selectedDirectory() : pendingSelection;
    pendingSelection.clear();
    const auto query = search->text().trimmed();
    const auto state = filter->currentData().toString(), active = activeDirectory();
    const auto *base = entry(reference);
    if (!base || !base->energy || runStateActive(base->status)) {
        reference.clear();
        base = nullptr;
    }
    const auto &unit = energyUnits[std::max(0, energyUnit->currentIndex())];
    // The jobs that match the search, counted by state, and those of the chosen state.
    QMap<QString, int> counts;
    QVector<Row> rows;
    for (const auto &e : std::as_const(entries)) {
        const bool owned = e.directory == active;
        const auto words = QStringList{e.name,    e.method, e.basis,     e.task,
                                       e.formula, e.memo,   e.directory, stateLabel(e, owned).first}
                               .join(' ');
        if (!words.contains(query, Qt::CaseInsensitive))
            continue;
        const auto group = stateGroup(e, owned);
        ++counts[group];
        ++counts[""];
        if (!state.isEmpty() && group != state)
            continue;
        Row row{&e, owned, {}, {}};
        if (comparing && base) {
            row.note = comparisonNote(e, *base);
            if (row.note.isEmpty())
                row.delta = *e.energy - *base->energy;
        }
        rows << row;
    }
    // Values first in the chosen order, then the rows without one; the newest first among equals.
    const auto missing = [&](const Row &r) {
        const auto &e = *r.entry;
        switch (sortColumn) {
        case CreatedColumn:
            return !e.created.isValid();
        case DurationColumn:
            return e.seconds < 0;
        case EnergyColumn:
            return !e.energy.has_value();
        case DeltaColumn:
            return !r.delta.has_value();
        case MemoColumn:
            return e.memo.isEmpty();
        default:
            return false;
        }
    };
    const auto compareValues = [&](const Row &a, const Row &b) -> int {
        const auto &x = *a.entry, &y = *b.entry;
        const auto order = [](auto u, auto v) { return u < v ? -1 : v < u ? 1 : 0; };
        switch (sortColumn) {
        case StateColumn:
            return QString::localeAwareCompare(stateLabel(x, a.owned).first, stateLabel(y, b.owned).first);
        case NameColumn:
            return QString::localeAwareCompare(x.name, y.name);
        case LevelColumn:
            return QString::localeAwareCompare(levelText(x), levelText(y));
        case TaskColumn:
            return QString::localeAwareCompare(x.task, y.task);
        case CreatedColumn:
            return order(x.created.toMSecsSinceEpoch(), y.created.toMSecsSinceEpoch());
        case DurationColumn:
            return order(x.seconds, y.seconds);
        case EnergyColumn:
            return order(*x.energy, *y.energy);
        case DeltaColumn:
            return order(*a.delta, *b.delta);
        default:
            return QString::localeAwareCompare(x.memo, y.memo);
        }
    };
    const auto time = [](const HistoryEntry &e) {
        return e.created.isValid() ? e.created.toMSecsSinceEpoch() : std::numeric_limits<qint64>::min();
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const Row &a, const Row &b) {
        const bool ma = missing(a), mb = missing(b);
        if (ma != mb)
            return mb;
        if (!ma)
            if (const int c = compareValues(a, b))
                return sortOrder == Qt::AscendingOrder ? c < 0 : c > 0;
        return time(*a.entry) > time(*b.entry);
    });
    double largest = 0;
    for (const auto &r : rows)
        if (r.delta)
            largest = std::max(largest, std::abs(*r.delta));
    const bool byDay = sortColumn == CreatedColumn;
    listed.clear();
    QVector<HistoryModel::Job> jobs;
    QVector<HistoryModel::Day> days;
    for (const auto &r : std::as_const(rows)) {
        if (byDay) {
            const auto date = r.entry->created.isValid() ? r.entry->created.toLocalTime().date() : QDate();
            if (days.isEmpty() || days.last().date != date)
                days.append({dayTitle(date), {}, date, int(jobs.size()), 0});
            ++days.last().count;
        }
        jobs.append({*r.entry, r.owned, r.delta, r.note});
        listed << r.entry->directory;
    }
    for (auto &day : days)
        day.detail = tr("%n 件", "jobs", day.count);
    const int scrolled = table->verticalScrollBar()->value();
    {
        QSignalBlocker block(table->selectionModel());
        QSignalBlocker blockHeader(table->header());
        table->header()->setSortIndicator(sortColumn, sortOrder);
        table->setColumnHidden(DurationColumn, comparing);
        table->setColumnHidden(DeltaColumn, !comparing);
        if (byDay != grouped) // Times under a day, or with their date.
            table->setColumnWidth(CreatedColumn, byDay ? 64 : 92);
        grouped = byDay;
        model->show(jobs, days,
                    {&unit, reference, largest, narrowList,
                     rows.isEmpty() && !entries.isEmpty()
                         ? tr("条件に合うジョブはありません。検索語と状態の絞り込みを見直してください。")
                         : QString()});
        for (int row = 0; row < model->rowCount(); ++row)
            if (model->spanned(row))
                table->setFirstColumnSpanned(row, {}, true);
        table->expandAll();
        table->verticalScrollBar()->setValue(scrolled);
        if (const auto at = model->find(selected); at.isValid()) {
            table->setCurrentIndex(at);
            table->scrollTo(at);
        }
    }
    // The states with their counts, and the folder.
    const QStringList keys{"", "active", "finished", "stopped", "prepared"}, names = stateFilterNames();
    for (int i = 0; i < keys.size(); ++i)
        filterButtons->button(i)->setText(QString("%1  %2").arg(names[i]).arg(counts.value(keys[i])));
    updateToolbar();
    if (scan.active())
        status->setText(tr("ジョブを読み込んでいます…"));
    else
        status->setText(tr("%1 / %n 件%2", nullptr, int(entries.size()))
                            .arg(listed.size())
                            .arg(query.isEmpty() ? QString() : tr("（「%1」で絞り込み）").arg(query)));
    note->setText(!comparing
                      ? tr("計算のEは最後のtaskの全エネルギーです。終了コード0は収束の保証ではありません。")
                  : base ? tr("ΔEは基準（%1）と組成・手法・基底・エネルギーの種類が同じジョブだけに"
                              "表示します。熱補正は含みません。")
                               .arg(base->name)
                         : tr("基準にするジョブを選び、右の「このジョブを基準にする」を押してください。"));
    const bool empty = scanned && entries.isEmpty() && !scan.active();
    pages->setCurrentIndex(empty ? 1 : 0);
    if (empty)
        showEmptyPage();
    for (QWidget *w : {static_cast<QWidget *>(search), static_cast<QWidget *>(filterButtons),
                       static_cast<QWidget *>(compare)})
        w->setEnabled(!empty);
    if (reloadDetails || selectedDirectory() != selected)
        showDetails();
    else
        updateActions();
}
void HistoryPanel::updateRow(const HistoryEntry &e) {
    model->update(e);
}
void HistoryPanel::updateToolbar() {
    const auto other = executionRoot();
    const bool same = QDir::cleanPath(other) == QDir::cleanPath(root);
    const auto path = root.isEmpty() ? tr("未設定") : displayPath(root);
    const QString note = same ? tr("（3 実行設定と同じ）") : tr("（選んだ保存先）");
    rootButton->setText(fontMetrics().elidedText(path, Qt::ElideMiddle, compactToolbar ? 200 : 360) +
                        (compactToolbar ? QString() : note));
    rootButton->setToolTip(root + (compactToolbar ? note : QString()));
    executionRootAction->setEnabled(!same || !followsExecution);
    // Jobs recorded as running by MolForma on this PC whose process has ended.
    QStringList orphans;
    for (const auto &e : std::as_const(entries))
        if (e.orphaned && e.directory != activeDirectory())
            orphans << (e.created.isValid()
                            ? dateLocale().toString(e.created.toLocalTime(), tr("M月d日")) + " "
                            : QString()) +
                           e.name;
    orphanBanner->setVisible(!orphans.isEmpty());
    if (!orphans.isEmpty()) {
        const auto names = orphans.size() == 1   ? orphans[0]
                           : orphans.size() == 2 ? tr("%1、%2").arg(orphans[0], orphans[1])
                                                 : tr("%1、%2 ほか").arg(orphans[0], orphans[1]);
        orphanText->setText(tr("このPCで「実行中」と記録されたまま、計算のプロセスが終了しているジョブが %n "
                               "件あります（%1）。記録を「中断」に直し、ログからエネルギーを求めます。",
                               nullptr, int(orphans.size()))
                                .arg(names));
    }
}
void HistoryPanel::showEmptyPage() {
    const QFileInfo info(root);
    const bool readable = info.isDir() && info.isReadable();
    emptyTitle->setText(readable ? tr("この保存先にはジョブがありません")
                                 : tr("この保存先は見つからないか、読めません"));
    emptyText->setText(readable ? tr("「計算を実行」（F5）か「ジョブとして保存」でジョブを作ると、ここに一覧"
                                     "します。別の場所に保存したジョブは、保存先を切り替えて開けます。")
                                : displayPath(root));
    const auto other = executionRoot();
    const bool elsewhere = QDir::cleanPath(other) != QDir::cleanPath(root);
    emptyBack->setVisible(elsewhere);
    emptyExecution->setVisible(elsewhere);
    if (!elsewhere)
        return;
    QString count;
    try { // Over MCP, folders outside --mcp-allow-dir are not counted.
        window->checkMcpPath(other);
        const int n = historyJobCount(other);
        count = n < 0       ? tr("（見つかりません）")
                : n >= 2000 ? tr("（2000 件以上）")
                            : tr("（%n 件）", "jobs", n);
    } catch (const std::exception &) {
    }
    emptyExecution->setText(tr("「3 実行設定」の保存先: %1%2").arg(displayPath(other), count));
}
void HistoryPanel::setComparing(bool on) {
    {
        QSignalBlocker block(compare);
        compare->setChecked(on);
    }
    if (comparing == on)
        return;
    comparing = on;
    unitButtons->setVisible(on);
    if (on) {
        const auto *e = selectedEntry();
        if (reference.isEmpty() && e && e->energy && !runStateActive(e->status))
            reference = e->directory;
        filesOpenBeforeComparing = filesOpen;
        setFilesOpen(false, false);
        sortColumn = DeltaColumn;
        sortOrder = Qt::AscendingOrder;
    } else {
        setFilesOpen(filesOpenBeforeComparing, false);
        sortColumn = CreatedColumn;
        sortOrder = Qt::DescendingOrder;
    }
    table->scrollToTop();
    populate();
}
void HistoryPanel::setFilesOpen(bool open, bool remember) {
    if (filesOpen && !open && listSplit->sizes().value(1) > 80)
        filesHeight = listSplit->sizes().value(1);
    filesOpen = open;
    if (remember)
        window->setPreference("history/files_open", open);
    filesCard->setOpen(open);
    for (QWidget *w :
         {static_cast<QWidget *>(fileName), static_cast<QWidget *>(follow), static_cast<QWidget *>(copyFile)})
        w->setVisible(open);
    const auto m = filesCard->layout()->contentsMargins();
    const int closed = m.top() + m.bottom() + filesCard->header()->sizeHint().height() + 2;
    filesCard->setMaximumHeight(open ? QWIDGETSIZE_MAX : closed);
    // The list takes the room of the files, and gives it back.
    const int total = listSplit->height() > 0 ? listSplit->height() : 600;
    const int files = open ? filesHeight : closed;
    listSplit->setSizes({std::max(160, total - files - listSplit->handleWidth()), files});
}
void HistoryPanel::showDetails() {
    saveMemo(); // The memo of the job selected before.
    const auto *e = selectedEntry();
    try {
        if (e)
            window->checkMcpPath(e->directory);
    } catch (const std::exception &) {
        e = nullptr; // An MCP selection does not read jobs outside --mcp-allow-dir.
    }
    detailEmpty->setVisible(!e);
    detail->setVisible(e != nullptr);
    fitDetail();
    if (!e) {
        memoDirectory.clear();
        for (auto *p : {input, output, errors, record})
            p->clear();
        progress->clear();
        updateActions();
        return;
    }
    const bool owned = e->directory == activeDirectory();
    const auto [label, kind] = stateLabel(*e, owned);
    title->setText(e->name);
    title->setToolTip(e->name);
    statePill->setText(label);
    statePill->setToolTip(historyStatus(e->status, owned, e->orphaned));
    setFlag(statePill, "pill", kind);
    level->setText(levelText(*e) + (e->task.isEmpty() ? "" : " · " + e->task));
    // The molecule as saved with the job (read once per job: the bundle does not change).
    if (!molecules.contains(e->directory)) {
        try {
            molecules[e->directory] = historyMolecule(e->directory);
        } catch (const std::exception &) {
            molecules[e->directory] = std::nullopt;
        }
    }
    QStringList line{e->formula.isEmpty() ? tr("組成不明") : e->formula};
    if (const auto &m = molecules.value(e->directory)) {
        line << tr("%n 原子", nullptr, m->atoms);
        if (!comparing)
            line << tr("電荷 %1 / 多重度 %2").arg(m->charge).arg(m->multiplicity);
    }
    if (comparing) {
        if (e->created.isValid())
            line << dayAndTime(e->created);
        if (e->seconds >= 0)
            line << tr("所要 %1").arg(elapsedClock(e->seconds));
    }
    facts->setText(line.join(" · "));
    showEnergy(*e, owned);
    showFacts(*e, owned);
    // Over MCP with --mcp-allow-dir, memos are written only inside the allowed folders.
    bool outside = false;
    if (window->accessPolicy() && window->accessPolicy()->restricted())
        try {
            window->accessPolicy()->checkPath(e->directory);
        } catch (const std::exception &) {
            outside = true;
        }
    memo->setProperty("mcpPath", outside);
    if (memoDirectory != e->directory || !memo->hasFocus()) {
        loadingMemo = true;
        memo->setPlainText(e->memo);
        loadingMemo = false;
    }
    memoDirectory = e->directory;
    memo->setReadOnly(!e->valid);
    memoStatus->hide();
    memoBox->setVisible(!comparing &&
                        !(owned && runStateActive(e->status))); // A run keeps its stop button in view.
    for (auto pair : {std::pair{input, "input.nw"}, std::pair{record, "run.json"}}) {
        try {
            showText(pair.first, historyText(e->directory, pair.second));
        } catch (const std::exception &error) {
            showText(pair.first, QString::fromUtf8(error.what()));
        }
    }
    updateLogs();
    arrangeActions(e, owned);
    updateActions();
    updateLive();
}
void HistoryPanel::showEnergy(const HistoryEntry &e, bool owned) {
    const bool running = owned && runStateActive(e.status);
    progressBox->setVisible(running);
    energyBox->setVisible(!running);
    if (running)
        return;
    setFlag(energyBox, "compare", comparing);
    const auto &unit = energyUnits[std::max(0, energyUnit->currentIndex())];
    if (!comparing) {
        energyCaption->setText(tr("計算のE（%1）").arg(energySource(e)));
        energyValue->setText(e.energy ? hartree(*e.energy, 10) : QString("—"));
        energySuffix->setText(e.energy ? "Eh · " + historyEnergyMethod(e.energyMethod) : QString());
        energyNote->setText(e.energy ? e.manifest["summary"].toObject()["energy"].isDouble()
                                           ? tr("最後のtaskの値です。収束の保証ではありません。")
                                           : tr("ログ末尾128 KiB内の最後の値です。収束の保証ではありません。")
                            : e.status == RunState::Prepared
                                ? tr("NWChemは起動していません（入力一式の保存のみ）。")
                                : tr("ログに全エネルギーの記録がありません。"));
        return;
    }
    const auto *base = entry(reference);
    if (!base) {
        energyCaption->setText(tr("ΔEの基準"));
        energyValue->setText("—");
        energySuffix->clear();
        energyNote->setText(e.energy && !runStateActive(e.status)
                                ? tr("「このジョブを基準にする」で、このジョブとの差を一覧に表示します。")
                                : tr("エネルギーのある終了したジョブを選び、基準にしてください。"));
        return;
    }
    if (base == &e) {
        energyCaption->setText(tr("このジョブが基準です"));
        energyValue->setText(signedNumber(0, unit.digits));
        energySuffix->setText(unit.label);
        energyNote->setText(tr("ほかのジョブを選ぶと、基準との差をここにも表示します。"));
        return;
    }
    energyCaption->setText(tr("基準 %1 との差").arg(base->name));
    auto why = comparisonNote(e, *base);
    if (!why.isEmpty()) {
        // What differs (as historyIncomparable finds it), this job's then the reference's.
        const auto pair = [&](const QString &a, const QString &b) {
            why = tr("%1（%2、基準は %3）").arg(why, a, b);
        };
        if (!runStateActive(e.status) && e.energy) {
            if (e.formula.isEmpty() || e.formula != base->formula)
                pair(e.formula, base->formula);
            else if (e.method != base->method)
                pair(e.method, base->method);
            else if (e.basis != base->basis)
                pair(e.basis, base->basis);
            else if (e.energyMethod != base->energyMethod)
                pair(historyEnergyMethod(e.energyMethod), historyEnergyMethod(base->energyMethod));
        }
        energyValue->setText("—");
        energySuffix->clear();
        energyNote->setText(tr("%1。差を表示しません。").arg(why));
        return;
    }
    const double delta = *e.energy - *base->energy;
    energyValue->setText(signedNumber(delta * unit.factor, unit.digits));
    energySuffix->setText(unit.label);
    QStringList others;
    for (const auto &u : energyUnits)
        if (&u != &unit)
            others << signedNumber(delta * u.factor, u.digits) + " " + u.label;
    energyNote->setText(others.join(" · "));
}
void HistoryPanel::showFacts(const HistoryEntry &e, bool owned) {
    while (auto *item = factGrid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    int row = 0;
    const auto add = [&](const QString &key, const QString &name, QWidget *value) {
        auto *label = caption(name);
        label->setWordWrap(false);
        factGrid->addWidget(label, row, 0, Qt::AlignTop);
        value->setObjectName("historyFact_" + key);
        factGrid->addWidget(value, row++, 1);
    };
    const auto text = [&](const QString &key, const QString &name, const QString &value, bool mono = false) {
        auto *label = new QLabel(value);
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        if (mono)
            label->setProperty("mono", true);
        add(key, name, label);
    };
    const auto energy = [](const HistoryEntry &x) {
        return x.energy ? tr("%1 Eh（%2）").arg(hartree(*x.energy, 10), historyEnergyMethod(x.energyMethod))
                        : QString("—");
    };
    if (comparing) {
        const auto *base = entry(reference);
        text("energy", tr("計算のE"), energy(e), true);
        if (base && base != &e) {
            text("referenceEnergy", tr("基準のE"), energy(*base), true);
            if (e.energy && comparisonNote(e, *base).isEmpty()) {
                const auto mine = energySource(e), theirs = energySource(*base);
                text("compared", tr("比べた値"),
                     mine == theirs ? tr("どちらも%1の全エネルギー").arg(mine)
                                    : tr("このジョブは%1、基準は%2の全エネルギー").arg(mine, theirs));
            }
        }
        return;
    }
    const auto time = [&](const char *key) {
        return QDateTime::fromString(e.manifest[key].toString(), Qt::ISODateWithMs);
    };
    const auto started = time("started_at"), finished = time("finished_at");
    if (e.status == RunState::Prepared)
        text("created", tr("保存", "state"), e.created.isValid() ? dayAndTime(e.created) : QString("—"));
    else
        text("started", tr("開始"), started.isValid() ? dayAndTime(started) : QString("—"));
    if (owned && runStateActive(e.status))
        text("elapsed", tr("経過"), elapsedClock(run.elapsedSeconds()));
    else if (e.status != RunState::Prepared) {
        QString end = finished.isValid() ? (finished.toLocalTime().date() == started.toLocalTime().date()
                                                ? finished.toLocalTime().toString("HH:mm")
                                                : dayAndTime(finished))
                                         : QString("—");
        if (e.seconds >= 0)
            end += " · " + tr("所要 %1").arg(elapsedClock(e.seconds));
        text("finished", tr("終了", "state"), end);
        const auto summary = e.manifest["summary"].toObject();
        const auto code = QString::number(e.manifest["exit_code"].toInt());
        text("exit", tr("終了コード"),
             !e.manifest.contains("exit_code")        ? tr("未記録")
             : summary.isEmpty()                      ? code
             : summary["normal_termination"].toBool() ? tr("%1 · 正常終了の記録あり").arg(code)
                                                      : tr("%1 · 正常終了の記録なし").arg(code));
    }
    text("integrity", tr("入力照合"), historyIntegrity(e));
    // A repaired record keeps its note in Japanese (see repairHistoryEntry).
    if (!e.manifest["error"].toString().isEmpty())
        text("error", tr("実行エラー"), translated(e.manifest["error"].toString()));
    if (!e.warnings.isEmpty())
        text("warnings", tr("注意"), e.warnings.join('\n'));
    auto *place = new QWidget;
    auto *placeRow = new QHBoxLayout(place);
    placeRow->setContentsMargins(0, 0, 0, 0);
    placeRow->setSpacing(6);
    auto *path = new QLabel(displayPath(e.directory));
    path->setProperty("mono", true);
    path->setWordWrap(true);
    path->setTextFormat(Qt::PlainText);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setToolTip(e.directory);
    placeRow->addWidget(path, 1);
    auto *copy = new QToolButton;
    copy->setObjectName("historyCopyFolder");
    copy->setIcon(lineIcon("copy"));
    copy->setProperty("flat", true);
    copy->setToolTip(tr("フォルダーのパスをコピー"));
    connect(copy, &QToolButton::clicked, this,
            [directory = e.directory] { QApplication::clipboard()->setText(directory); });
    placeRow->addWidget(copy, 0, Qt::AlignTop);
    add("folder", tr("フォルダー"), place);
}
void HistoryPanel::arrangeActions(const HistoryEntry *e, bool owned) {
    for (auto *b : {viewer, reuse, folder, stop, setReference, showExecution}) {
        actionGrid->removeWidget(b);
        b->hide();
    }
    const auto place = [&](QPushButton *b, int row, int column, int span = 1) {
        actionGrid->addWidget(b, row, column, 1, span);
        b->show();
    };
    const bool running = e && owned && runStateActive(e->status);
    viewer->setText(running ? tr("途中のログを開く") : tr("結果ビューアで開く"));
    setFlag(viewer, "primary", !running);
    setReference->setText(comparing ? tr("このジョブを基準にする") : tr("このジョブを基準にΔEを比べる"));
    setFlag(setReference, "link", !comparing);
    if (running) {
        place(stop, 0, 0, 2);
        place(viewer, 1, 0);
        place(folder, 1, 1);
        actionGrid->addWidget(showExecution, 2, 0, 1, 2, Qt::AlignLeft);
        showExecution->show();
    } else if (comparing) {
        place(viewer, 0, 0, 2);
        place(setReference, 1, 0);
        place(reuse, 1, 1);
    } else {
        place(viewer, 0, 0, 2);
        place(reuse, 1, 0);
        place(folder, 1, 1);
        if (e && e->energy && !runStateActive(e->status)) {
            actionGrid->addWidget(setReference, 2, 0, 1, 2, Qt::AlignLeft);
            setReference->show();
        }
    }
    stopComparing->setVisible(comparing);
}
void HistoryPanel::updateLive() {
    const auto *e = selectedEntry();
    if (!e || e->directory != activeDirectory() || !detail->isVisible())
        return;
    progress->showRun(e->directory, run.state());
    const qint64 seconds = run.elapsedSeconds();
    progressTiming->setText(
        tr("経過 %1 · %2 開始 · このウィンドウで実行中")
            .arg(elapsedClock(seconds), QDateTime::currentDateTime().addSecs(-seconds).toString("HH:mm")));
    if (auto *elapsed = findChild<QLabel *>("historyFact_elapsed"))
        elapsed->setText(elapsedClock(seconds));
}
void HistoryPanel::updateLogs() {
    const auto directory = selectedDirectory();
    if (directory.isEmpty() || !detail->isVisible())
        return;
    for (auto pair : {std::pair{output, "output.out"}, std::pair{errors, "stderr.err"}}) {
        try {
            showText(pair.first, historyText(directory, pair.second, true), follow->isChecked());
        } catch (const std::exception &error) {
            showText(pair.first, QString::fromUtf8(error.what()));
        }
    }
}
void HistoryPanel::updateActions() {
    const auto *e = selectedEntry();
    const bool exists = e && QFileInfo(e->directory).isDir();
    const bool owned = e && e->directory == activeDirectory();
    viewer->setEnabled(exists && !resultFiles(e->directory).isEmpty());
    folder->setEnabled(exists);
    reuse->setEnabled(exists && e->valid && !window->hasCandidate() && !window->chemistryRunning());
    stop->setEnabled(exists && owned && run.state() != RunState::Stopping);
    const bool unfinished = e && runStateActive(e->status);
    repair->setEnabled(exists && e->valid && e->orphaned && !owned);
    repairBox->setVisible(e && e->orphaned && !owned && !comparing);
    const bool movable = exists && e->valid && !owned && (!unfinished || e->orphaned);
    trash->setEnabled(movable);
    trash->setVisible(movable);
    trashNote->setText(
        movable      ? tr("ジョブフォルダーごとごみ箱へ移します（戻すときは、ごみ箱から戻します）。")
        : unfinished ? tr("実行中・稼働未確認のジョブはごみ箱へ移せません。停止してから操作して"
                          "ください。")
                     : tr("読めない記録のフォルダーは、ファイルマネージャーで確認してください。"));
    trashRow->setVisible(e && !comparing);
    csv->setEnabled(!listed.isEmpty());
    setReference->setEnabled(e && e->energy && !unfinished && (!comparing || e->directory != reference));
}
void HistoryPanel::notifyJob(const QString &path) {
    if (!isVisible() || QFileInfo(path).absolutePath() != root)
        return;
    if (scan.active()) {
        changedJobs.insert(path);
        return;
    }
    auto next = readHistoryEntry(path);
    if (path == activeDirectory()) {
        next.status = run.state();
        next.seconds = run.elapsedSeconds();
    }
    for (auto &e : entries)
        if (e.directory == path) {
            if (e.status != next.status || e.orphaned != next.orphaned) {
                e = next;
                populate(true);
            } else {
                e = next;
                updateRow(e);
            }
            return;
        }
    entries << next;
    populate();
}
void HistoryPanel::saveMemo() {
    memoTimer.stop();
    if (memoDirectory.isEmpty() || loadingMemo)
        return;
    const auto *e = entry(memoDirectory);
    const auto text = memo->toPlainText().trimmed();
    if (!e || !e->valid || e->memo == text)
        return;
    try {
        window->checkMcpPath(memoDirectory);
        setHistoryMemo(memoDirectory, text);
        for (auto &x : entries)
            if (x.directory == memoDirectory) {
                x.memo = text;
                updateRow(x);
            }
        memoStatus->hide();
    } catch (const std::exception &error) {
        memoStatus->setText(QString::fromUtf8(error.what()));
        memoStatus->show();
    }
}
bool HistoryPanel::containsDirectory(const QString &directory) const {
    return std::any_of(entries.cbegin(), entries.cend(),
                       [&](const auto &entry) { return entry.directory == directory; });
}
QString HistoryPanel::message() const {
    return status->text();
}
void HistoryPanel::reuseSelected() {
    const auto directory = selectedDirectory();
    if (directory.isEmpty())
        return;
    emit reuseRequested(directory);
}
void HistoryPanel::openResult() {
    const auto directory = selectedDirectory();
    if (directory.isEmpty())
        return;
    emit resultsRequested(directory);
}
void HistoryPanel::trashSelected() {
    const auto directory = selectedDirectory();
    if (directory.isEmpty())
        return;
    window->checkMcpPath(directory);
    if (QMessageBox::question(
            this, tr("ジョブをごみ箱へ"),
            tr("入力・プロジェクト・結果を含む次のフォルダーをごみ箱へ移動します。\n%1").arg(directory),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    trashHistoryEntry(directory, root, activeDirectory());
    refresh();
}
void HistoryPanel::repairSelected() {
    const auto directory = selectedDirectory();
    if (directory.isEmpty())
        return;
    window->checkMcpPath(directory);
    if (directory == activeDirectory())
        fail(tr("このウィンドウで実行中の計算です。"));
    repairHistoryEntry(directory);
    refresh();
}
void HistoryPanel::repairOrphans() {
    for (const auto &e : std::as_const(entries))
        if (e.orphaned && e.directory != activeDirectory()) {
            window->checkMcpPath(e.directory);
            repairHistoryEntry(e.directory);
        }
    refresh();
}
void HistoryPanel::exportCsv(const QString &path) const {
    writeFile(path, historyCsv(visibleEntries()).toUtf8());
}
void HistoryPanel::applyChromeStyle() {
    const auto pal = static_cast<QWidget *>(window)->palette();
    setStyleSheet(chromeRules(settingsRules() + R"(
        #historyTab { background: @window; }
        #historyRail { background: @base; }
        QScrollArea#historyDetailScroll { background: @base; border: none; border-left: 1px solid @line; }
        QSplitter#historySplit::handle { background: @line; }
        QToolButton#historyRootButton { border: 1px solid @line; border-radius: 6px; padding: 4px 8px; background: @base;
                                        color: @text; }
        QToolButton#historyRootButton:hover { background: @hover; }
        QToolButton#historyRootButton::menu-indicator { subcontrol-position: right center; right: 6px; }
        QToolButton#historyMore { border: 1px solid @line; border-radius: 6px; padding: 4px 10px; background: @base;
                                  color: @text; font-weight: bold; }
        QToolButton#historyMore:hover { background: @hover; }
        QToolButton#historyMore::menu-indicator { width: 0; }
        QPushButton#historyCompare:checked { background: @tint; border-color: @accent; color: @accentText;
                                             font-weight: bold; }
        QTreeView#historyTable { background: @base; border: none; border-top-left-radius: 9px;
                                   border-top-right-radius: 9px; }
        QTreeView#historyTable::item { border: none; }
        QTreeView#historyTable::item:hover { background: @hover; }
        QTreeView#historyTable::item:selected { background: @tint; color: @text; }
        QTreeView#historyTable QHeaderView::section { background: @base; color: @muted; border: none;
                                                        border-bottom: 1px solid @line; padding: 6px 8px; }
        QFrame#historyListFooter { background: @window; border: none; border-top: 1px solid @line;
                                   border-bottom-left-radius: 9px; border-bottom-right-radius: 9px; }
        QLabel#historyDetailTitle { font-size: 18px; font-weight: bold; }
        QLabel#historyDetailState { border-radius: 10px; padding: 2px 9px; font-weight: bold; }
        QFrame#historyEnergyBox { background: @window; border: none; border-radius: 8px; }
        QFrame#historyEnergyBox[compare="true"] { background: @tint; border: 1px solid @accent; }
        QLabel#historyEnergyValue { font-family: monospace; font-size: 22px; font-weight: 600; }
        QFrame#historyEnergyBox[compare="true"] QLabel#historyEnergyValue,
        QFrame#historyEnergyBox[compare="true"] QLabel#historyEnergySuffix { color: @accentText; font-weight: bold; }
        QFrame#historyProgressBox { background: @base; border: 1px solid @line; border-radius: 8px; }
        QLabel[tile="true"] { background: @window; border: 1px solid @line; border-radius: 6px; padding: 5px 8px; }
        QFrame#historyRepairBox { background: @candidate; border: 1px solid @candidateLine; border-radius: 8px; }
        QFrame#historyRepairBox QLabel { color: @candidateText; }
        QFrame#historyTrashRow { border: none; border-top: 1px solid @hover; }
        QFrame#historyEmptyCard { background: @base; border: 1px dashed @line; border-radius: 12px; }
        QPlainTextEdit#historyMemo { background: @base; border: 1px solid @line; border-radius: 6px; padding: 2px; }
    )",
                              pal) +
                  pillRules("QLabel#historyDetailState", "pill", ChromeColors(pal)));
    setFilesOpen(filesOpen, false); // The height of the closed card follows the fonts of the style.
}
} // namespace mf
