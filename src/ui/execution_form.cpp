// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/execution_form.h"
#include "mcp/policy.h"
#include "nwchem/nwchem_execution.h"
#include "ui/editor_chrome.h"
#include <QtWidgets>

namespace mf {
namespace {
QLabel *caption(const QString &text) {
    auto *label = new QLabel(text);
    label->setProperty("caption", true);
    label->setWordWrap(true);
    return label;
}
QLabel *muted(const QString &text) {
    auto *label = new QLabel(text);
    label->setProperty("muted", true);
    label->setWordWrap(true);
    return label;
}
// A note beside a card title, shortened with "…" when the card is narrow.
QLabel *note(const QString &text) {
    auto *label = new ElidedLabel(text);
    label->setProperty("muted", true);
    label->setToolTip(text);
    return label;
}
} // namespace

ExecutionForm::ExecutionForm(Scope scope, const QString &name, QWidget *parent)
    : QWidget(parent), prefix(name) {
    setProperty("canvas", true);
    auto *cards = new QVBoxLayout(this);
    cards->setContentsMargins(0, 0, 0, 0);
    cards->setSpacing(12);
    auto *files = new SettingsCard(tr("実行ファイル・保存先"));
    files->setObjectName(prefix + "Files");
    files->header()->insertWidget(1, note(tr("実行ごとに専用のジョブフォルダーを作ります")), 1);
    cards->addWidget(files);
    auto *paths = new QGridLayout;
    paths->setHorizontalSpacing(12);
    paths->setVerticalSpacing(6);
    paths->setColumnStretch(1, 1);
    files->body()->addLayout(paths);
    pathField(paths, tr("NWChem実行ファイル"), "executable", false);
    mode = new QComboBox;
    mode->setObjectName(prefix + "_mode");
    mode->addItem(tr("MPIで起動"), "mpi");
    mode->addItem(tr("直接実行（対応する逐次版等）"), "direct");
    fields["mode"] = mode;
    modes = new ChoiceButtons(mode, ChoiceButtons::Segments, {tr("MPIで起動"), tr("直接実行（逐次版など）")});
    {
        const int row = paths->rowCount();
        paths->addWidget(new QLabel(tr("起動方式")), row, 0);
        paths->addWidget(modes, row, 1, 1, 2, Qt::AlignLeft);
    }
    connect(mode, &QComboBox::currentIndexChanged, this,
            [this] { emit changed("mode", mode->currentData().toString()); });
    pathField(paths, tr("MPIランチャー"), "launcher", false);
    pathField(paths, tr("ジョブ保存先"), "job_root", true);
    if (scope == Scope::Project) {
        pathField(paths, tr("参照ファイルの基準"), "reference_directory", true);
        qobject_cast<QLineEdit *>(fields["reference_directory"])
            ->setPlaceholderText(tr("空欄: プロジェクトの場所（未保存時は起動時の場所）"));
        auto *pathsHelp = new Disclosure(tr("パスの扱いについて"));
        pathsHelp->button()->setObjectName(prefix + "PathHelp");
        files->body()->addWidget(pathsHelp, 0, Qt::AlignLeft);
        auto *pathHint = muted(
            tr("実行ファイルは名前またはパスで指定します。MPIはmpirun/mpiexecの -np を使用します。"
               "入力movecsの相対パスは参照基準から解決します。scratch・permanentは新しいジョブ"
               "フォルダーが基準です。出力movecs・CUBE・MOLDENはpermanent（未指定ならジョブ）に保存します。"
               "出力movecsを./で始めるとジョブ内、絶対パスなら指定先に保存します。"));
        pathHint->hide();
        files->body()->addWidget(pathHint);
        connect(pathsHelp->button(), &QToolButton::toggled, pathHint, &QWidget::setVisible);
    }

    resources = new SettingsCard(tr("計算資源"));
    resources->setObjectName(prefix + "ResourcesCard");
    cards->addWidget(resources);
    resourceGrid = new QGridLayout;
    resourceGrid->setHorizontalSpacing(12);
    resourceGrid->setVerticalSpacing(10);
    resources->body()->addLayout(resourceGrid);
    resourceGrid->addWidget(integer(tr("MPI起動プロセス数"), "mpi_ranks"), 0, 0);
    // Short names over the narrow columns; the full ones in their tooltips.
    auto *omp = integer(tr("OpenMP / プロセス"), "omp_threads");
    omp->setToolTip(tr("1プロセスあたりのOpenMPスレッド数（OMP_NUM_THREADS）"));
    resourceGrid->addWidget(omp, 0, 1);
    auto *blas = integer(tr("BLAS / プロセス"), "blas_threads");
    blas->setToolTip(tr("1プロセスあたりのBLASスレッド数（OPENBLAS_NUM_THREADS・MKL_NUM_THREADS など）"));
    resourceGrid->addWidget(blas, 0, 2);
    auto *stackCell = new QWidget;
    auto *stackLayout = new QVBoxLayout(stackCell);
    stackLayout->setContentsMargins(0, 0, 0, 0);
    stackLayout->setSpacing(4);
    stackLayout->addWidget(caption(tr("OpenMPスタック")));
    auto *stack = new QLineEdit;
    fields["omp_stacksize"] = stack;
    stack->setObjectName(prefix + "_omp_stacksize");
    stack->setPlaceholderText(tr("空欄: 継承（例: 64M）"));
    stackLayout->addWidget(stack);
    resourceGrid->addWidget(stackCell, 1, 0);
    connect(stack, &QLineEdit::editingFinished, this, [this, stack] {
        if (stack->isModified())
            emit changed("omp_stacksize", stack->text().trimmed());
        stack->setModified(false);
    });
    resourceGrid->addWidget(integer(tr("制限時間（0: なし）"), "timeout_seconds", tr(" 秒")), 1, 2);
    for (int column = 0; column < 3; ++column)
        resourceGrid->setColumnStretch(column, 1);
    resources->body()->addWidget(caption(tr("MPI-PR版では補助プロセスが必要です（1計算プロセスでも起動数2）。"
                                            "起動数は自動補正しません。OpenMP/BLASの既定は各1です。")));

    auto *environmentCard = new SettingsCard(tr("追加の環境変数"));
    environmentCard->setObjectName(prefix + "EnvironmentCard");
    cards->addWidget(environmentCard);
    auto *add = new QPushButton(tr("追加"));
    add->setIcon(lineIcon("plus"));
    removeEnvironment = new QPushButton(tr("選択行を削除"));
    add->setObjectName(prefix + "AddEnvironment");
    removeEnvironment->setObjectName(prefix + "RemoveEnvironment");
    add->setProperty("mcpExecutionSetting", true);
    removeEnvironment->setProperty("mcpExecutionSetting", true);
    environmentCard->header()->addWidget(removeEnvironment);
    environmentCard->header()->addWidget(add);
    environment = new QTableWidget(0, 3);
    environment->setObjectName(prefix + "Environment");
    environment->setProperty("mcpExecutionSetting", true);
    environment->setHorizontalHeaderLabels({tr("有効"), tr("名前"), tr("値")});
    environment->verticalHeader()->hide();
    environment->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    environment->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    environment->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    environment->setMinimumHeight(110);
    environment->setMaximumHeight(200);
    environmentCard->body()->addWidget(environment);
    environmentEmpty = muted(tr("なし。有効な行だけを起動環境へ上書きします（例: NWCHEM_BASIS_LIBRARY）。"));
    environmentEmpty->setObjectName(prefix + "EnvironmentEmpty");
    environmentCard->body()->addWidget(environmentEmpty);
    environmentCard->body()->addWidget(
        caption(tr("スレッド数は「計算資源」で指定してください。値の環境変数展開は行いません。")));
    // A new row starts off, so that it applies only once it is named and switched on. Edits of the
    // table that are still pending come first.
    connect(add, &QPushButton::clicked, this, [this] {
        commitEnvironment();
        auto next = rows;
        next.append(QJsonObject{{"enabled", false}, {"name", ""}, {"value", ""}});
        emit changed("environment", next);
    });
    connect(removeEnvironment, &QPushButton::clicked, this, [this] {
        const int row = environment->currentRow();
        commitEnvironment();
        if (row >= 0 && row < rows.size()) {
            auto next = rows;
            next.removeAt(row);
            emit changed("environment", next);
        }
    });
    // Defer until the delegate's current edit has completed.
    connect(environment, &QTableWidget::itemChanged, this, [this] {
        environmentDirty = true;
        QTimer::singleShot(0, this, &ExecutionForm::commitEnvironment);
    });
}
void ExecutionForm::pathField(QGridLayout *grid, const QString &label, const QString &key, bool directory) {
    const int row = grid->rowCount();
    auto *line = new QLineEdit;
    line->setObjectName(prefix + "_" + key);
    fields[key] = line;
    auto *browse = new QPushButton(tr("参照…"));
    browse->setObjectName(prefix + "Browse_" + key);
    // The program to run is chosen by a person; McpGui refuses to operate these controls.
    if (isProtectedExecutionKey(key)) {
        line->setProperty("mcpExecutionSetting", true);
        browse->setProperty("mcpExecutionSetting", true);
    }
    auto *name = new QLabel(label);
    name->setBuddy(line);
    grid->addWidget(name, row, 0);
    grid->addWidget(line, row, 1);
    grid->addWidget(browse, row, 2);
    if (!directory) { // Where the program was found, under its field.
        auto *where = new QLabel;
        where->setObjectName(prefix + "Found_" + key);
        where->setTextFormat(Qt::PlainText);
        where->setWordWrap(true);
        where->hide();
        found[key] = where;
        grid->addWidget(where, row + 1, 1, 1, 2);
    }
    connect(line, &QLineEdit::editingFinished, this, [this, line, key] {
        if (line->isModified())
            emit changed(key, line->text().trimmed());
        line->setModified(false);
    });
    connect(browse, &QPushButton::clicked, this, [this, line, key, directory] {
        const auto path = directory
                              ? QFileDialog::getExistingDirectory(this, tr("ディレクトリ"), line->text())
                              : QFileDialog::getOpenFileName(this, tr("実行ファイル"), line->text());
        if (!path.isEmpty())
            emit changed(key, path);
    });
}
QWidget *ExecutionForm::integer(const QString &label, const QString &key, const QString &unit) {
    auto *cell = new QWidget;
    auto *layout = new QVBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    layout->addWidget(caption(label));
    auto *spin = new QSpinBox;
    spin->setObjectName(prefix + "_" + key);
    const auto range = executionRange(key);
    spin->setRange(range.minimum, range.maximum);
    spin->setSuffix(unit);
    spin->setKeyboardTracking(false);
    fields[key] = spin;
    layout->addWidget(spin);
    connect(spin, &QSpinBox::valueChanged, this, [this, key](int value) { emit changed(key, value); });
    return cell;
}
void ExecutionForm::setSettings(const QJsonObject &s) {
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        const QSignalBlocker blocker(it.value());
        if (auto *line = qobject_cast<QLineEdit *>(it.value())) {
            if (line->text() != s[it.key()].toString())
                line->setText(s[it.key()].toString());
        } else if (auto *spin = qobject_cast<QSpinBox *>(it.value()))
            spin->setValue(s[it.key()].toInt());
        else if (auto *combo = qobject_cast<QComboBox *>(it.value())) {
            while (combo->count() > 2)
                combo->removeItem(2);
            int index = combo->findData(s[it.key()].toString());
            if (index < 0) {
                combo->addItem(tr("未対応: %1").arg(s[it.key()].toVariant().toString()), s[it.key()]);
                index = 2;
            }
            combo->setCurrentIndex(index);
        }
    }
    modes->sync();
    mpi = s["mode"] == "mpi";
    fields["launcher"]->setEnabled(mpi);
    fields["mpi_ranks"]->setEnabled(mpi);
    if (!mpi)
        found["launcher"]->hide();
    const QSignalBlocker blocker(environment);
    environmentDirty = false;
    rows = s["environment"].toArray();
    environment->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const auto entry = rows[row].toObject();
        for (int column = 0; column < 3; ++column) {
            auto *item = environment->item(row, column);
            if (!item) {
                item = new QTableWidgetItem;
                environment->setItem(row, column, item);
            }
            if (column == 0) {
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
                item->setCheckState(entry["enabled"].toBool() ? Qt::Checked : Qt::Unchecked);
            } else
                item->setText(entry[column == 1 ? "name" : "value"].toString());
        }
    }
    environment->setVisible(!rows.isEmpty());
    removeEnvironment->setVisible(!rows.isEmpty());
    environmentEmpty->setVisible(rows.isEmpty());
}
void ExecutionForm::commitEnvironment() {
    if (!environmentDirty)
        return;
    environmentDirty = false;
    if (rows.size() != environment->rowCount())
        return;
    auto next = rows;
    for (int row = 0; row < next.size(); ++row) {
        auto item = next[row].toObject(); // Unknown fields of a row stay.
        item["enabled"] = environment->item(row, 0)->checkState() == Qt::Checked;
        item["name"] = environment->item(row, 1)->text().trimmed();
        item["value"] = environment->item(row, 2)->text();
        next[row] = item;
    }
    emit changed("environment", next);
}
QWidget *ExecutionForm::field(const QString &key) const {
    return key == "environment" ? environment : fields.value(key);
}
void ExecutionForm::setFound(const QString &key, const QString &text, bool ok) {
    auto *where = found.value(key);
    if (!where)
        return;
    where->setText(text);
    where->setProperty("good", ok);
    where->setProperty("bad", !ok);
    repolish(where);
    where->setVisible(!text.isEmpty() && (key != "launcher" || mpi));
}
} // namespace mf
