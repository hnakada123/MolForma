// SPDX-License-Identifier: Apache-2.0
// Native NWChem settings; navigation/reset workflow inspired by KAGAMI advanced_tab.py.
#include "ui/advanced_panel.h"
#include "nwchem/nwchem_output.h"
#include "ui/calculation_panel.h"
#include "ui/diagnostics_list.h"
#include "ui/execution_panel.h"
#include "ui/main_window.h"
#include "ui/theme.h"
#include <QtWidgets>
#include <algorithm>

namespace mf {
AdvancedPanel::AdvancedPanel(MainWindow *owner) : QWidget(owner), window(owner) {
    setObjectName("advancedTab");
    auto *layout = new QVBoxLayout(this);
    auto *toolbar = new QHBoxLayout;
    layout->addLayout(toolbar);
    context = new QLabel;
    context->setObjectName("advancedContext");
    context->setTextFormat(Qt::PlainText);
    context->setWordWrap(true);
    toolbar->addWidget(context, 1);
    auto *execution = new QPushButton("5 実行設定へ");
    execution->setObjectName("advancedExecution");
    toolbar->addWidget(execution);
    connect(execution, &QPushButton::clicked, this,
            [this] { window->workspaceTabs->setCurrentWidget(window->execution); });
    auto *basic = new QPushButton("3 計算条件へ");
    basic->setObjectName("advancedBasic");
    undo = new QPushButton("元に戻す");
    redo = new QPushButton("やり直す");
    resetAll = new QPushButton("詳細設定を既定へ");
    undo->setObjectName("advancedUndo");
    redo->setObjectName("advancedRedo");
    resetAll->setObjectName("advancedResetAll");
    for (auto *b : {basic, undo, redo, resetAll})
        toolbar->addWidget(b);
    connect(basic, &QPushButton::clicked, this,
            [this] { window->workspaceTabs->setCurrentWidget(window->calculation); });
    connect(undo, &QPushButton::clicked, this, [this] { window->history()->undo(); });
    connect(redo, &QPushButton::clicked, this, [this] { window->history()->redo(); });
    connect(resetAll, &QPushButton::clicked, this, [this] { reset({}); });
    auto *hint = new QLabel("空欄・既定は「3 計算条件」またはNWChemの既定値を使用します。"
                            "数値は1e-8のような指数表記に対応し、入力を確定すると反映します。"
                            "保留中の値は保存され、対応する計算に切り替えると再び有効になります。");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *find = new QHBoxLayout;
    layout->addLayout(find);
    category = new QComboBox;
    category->setObjectName("advancedCategory");
    category->addItem("全カテゴリ", "");
    for (const auto &c : nwchemAdvancedGroups())
        category->addItem(c.label, c.value);
    find->addWidget(category);
    search = new QLineEdit;
    search->setObjectName("advancedSearch");
    search->setPlaceholderText("項目名・NWChemキーワードで検索");
    search->setClearButtonEnabled(true);
    find->addWidget(search, 1);
    filter = new QComboBox;
    filter->setObjectName("advancedFilter");
    filter->addItem("現在の計算で使用する項目", "applicable");
    filter->addItem("変更した項目のみ", "changed");
    filter->addItem("全項目（保留も表示）", "all");
    find->addWidget(filter);
    count = new QLabel;
    count->setObjectName("advancedCount");
    find->addWidget(count);
    message = new QLabel;
    message->setWordWrap(true);
    layout->addWidget(message);
    auto *split = new QSplitter;
    layout->addWidget(split, 1);
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 4, 0);
    table = new QTreeWidget;
    table->setObjectName("advancedTable");
    table->setColumnCount(5);
    table->setHeaderLabels({"項目", "値", "状態", "NWChem", ""});
    table->setAlternatingRowColors(true);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auto *header = table->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(State, QHeaderView::Stretch);
    header->setSectionResizeMode(Reset, QHeaderView::Fixed);
    for (const auto &[column, width] : {std::pair{Name, 220}, {Value, 190}, {Keyword, 120}, {Reset, 72}})
        table->setColumnWidth(column, width);
    leftLayout->addWidget(table, 1);
    empty = new QLabel("一致する詳細設定はありません。");
    empty->setObjectName("advancedEmpty");
    leftLayout->addWidget(empty);
    help = new QLabel;
    help->setObjectName("advancedHelp");
    help->setTextFormat(Qt::PlainText);
    help->setWordWrap(true);
    help->setTextInteractionFlags(Qt::TextSelectableByMouse);
    help->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    help->setMinimumHeight(help->fontMetrics().lineSpacing() * 4);
    leftLayout->addWidget(help);
    outputSummary = new QLabel;
    outputSummary->setObjectName("advancedOutputSummary");
    outputSummary->setTextFormat(Qt::PlainText);
    outputSummary->setWordWrap(true);
    outputSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    leftLayout->addWidget(outputSummary);
    for (const auto &g : nwchemAdvancedGroups()) {
        auto *group = new QTreeWidgetItem(table, {g.label});
        group->setData(Name, Qt::UserRole + 1, g.value);
        auto font = group->font(Name);
        font.setBold(true);
        group->setFont(Name, font);
        groups[g.value] = group;
        auto *resetGroup = new QPushButton("既定へ");
        resetGroup->setObjectName("advancedReset_" + g.value);
        resetGroup->setToolTip("「" + g.label + "」の明示指定をすべて解除します。Undoで戻せます。");
        groupResets[g.value] = resetGroup;
        table->setItemWidget(group, Reset, resetGroup);
        group->setSizeHint(Reset, resetGroup->sizeHint());
        connect(resetGroup, &QPushButton::clicked, this, [this, group = g.value] { reset(group); });
        for (const auto &o : nwchemAdvancedOptions()) {
            if (o.group != g.value)
                continue;
            Row row;
            row.item = new QTreeWidgetItem(group, {o.label});
            row.item->setData(Name, Qt::UserRole, o.key);
            row.item->setData(Name, Qt::UserRole + 1, o.group);
            row.item->setText(Keyword, o.keyword);
            const auto tip = o.help + "\nNWChem: " + o.keyword + "\n未指定: " + o.defaultText;
            row.item->setToolTip(Name, tip);
            row.item->setToolTip(Keyword, tip);
            if (o.kind == AdvancedKind::Flag) {
                auto *check = new QCheckBox("有効");
                row.editor = check;
                connect(check, &QCheckBox::toggled, this, [this, key = o.key](bool value) {
                    change(key, value ? QJsonValue(true) : QJsonValue::Undefined);
                });
            } else if (o.kind == AdvancedKind::Choice) {
                auto *choices = new QComboBox;
                choices->addItem(
                    o.group == "visualization" ? "既定（" + o.defaultText + "）" : "既定（指定なし）", "");
                for (const auto &c : o.choices)
                    choices->addItem(c.label, c.value);
                row.editor = choices;
                connect(choices, &QComboBox::currentIndexChanged, this, [this, choices, key = o.key] {
                    const auto value = choices->currentData().toString();
                    change(key, value.isEmpty() ? QJsonValue::Undefined : QJsonValue(value));
                });
            } else {
                auto *edit = new QLineEdit;
                edit->setPlaceholderText(o.defaultText);
                edit->setClearButtonEnabled(true);
                row.editor = edit;
                connect(edit, &QLineEdit::editingFinished, this, [this, edit, o] {
                    if (!busy && edit->isModified())
                        change(o.key, parseNwchemAdvanced(o, edit->text()));
                    edit->setModified(false);
                });
            }
            row.editor->setObjectName("advanced_" + o.key);
            row.editor->setToolTip(tip);
            table->setItemWidget(row.item, Value, row.editor);
            row.item->setSizeHint(Value, row.editor->sizeHint());
            row.reset = new QPushButton("戻す");
            row.reset->setObjectName("advancedResetField_" + o.key);
            row.reset->setToolTip("この項目の明示指定を解除します。Undoで戻せます。");
            table->setItemWidget(row.item, Reset, row.reset);
            connect(row.reset, &QPushButton::clicked, this,
                    [this, key = o.key] { change(key, QJsonValue::Undefined); });
            rows[o.key] = row;
        }
    }
    table->expandAll();
    connect(table, &QTreeWidget::currentItemChanged, this, &AdvancedPanel::describe);
    // Working in an editor selects its row, so the description follows the keyboard too.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        for (auto it = rows.cbegin(); now && it != rows.cend(); ++it)
            if (it->editor == now || it->editor->isAncestorOf(now)) {
                if (table->currentItem() != it->item)
                    table->setCurrentItem(it->item);
                return;
            }
    });
    split->addWidget(left);
    auto *right = new QWidget;
    auto *output = new QVBoxLayout(right);
    output->setContentsMargins(4, 0, 0, 0);
    diagnostics = new DiagnosticsList;
    diagnostics->setObjectName("advancedValidation");
    output->addWidget(diagnostics, 1);
    // A message naming an option (「…」) shows its row; others belong to "3 計算条件".
    connect(diagnostics, &DiagnosticsList::messageClicked, this, [this](const QString &text) {
        for (const auto &o : nwchemAdvancedOptions())
            if (text.contains("「" + o.label + "」") && rows.contains(o.key)) {
                showRow(o.key);
                return;
            }
        const auto key = window->calculation->fieldFor(text);
        if (!key.isEmpty() && key != "advanced") {
            window->workspaceTabs->setCurrentWidget(window->calculation);
            window->calculation->showField(key);
        }
    });
    auto *actions = new QHBoxLayout;
    auto *showInput = new QToolButton;
    showInput->setObjectName("advancedShowInput");
    showInput->setText("入力全文（詳細設定を反映）");
    showInput->setCheckable(true);
    showInput->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    actions->addWidget(showInput);
    actions->addStretch();
    copy = new QPushButton("コピー");
    save = new QPushButton("保存…");
    copy->setObjectName("advancedCopy");
    save->setObjectName("advancedSave");
    actions->addWidget(copy);
    actions->addWidget(save);
    output->addLayout(actions);
    preview = new QPlainTextEdit;
    preview->setObjectName("advancedInput");
    preview->setReadOnly(true);
    preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output->addWidget(preview, 3);
    showInput->setChecked(window->settings.value("advanced/showInput", false).toBool());
    preview->setVisible(showInput->isChecked());
    showInput->setArrowType(showInput->isChecked() ? Qt::DownArrow : Qt::RightArrow);
    connect(showInput, &QToolButton::toggled, this, [this, showInput](bool on) {
        preview->setVisible(on);
        showInput->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        window->settings.setValue("advanced/showInput", on);
    });
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    split->setSizes({880, 460});
    connect(copy, &QPushButton::clicked, this, [this] { window->calculation->exportInput(false); });
    connect(save, &QPushButton::clicked, this, [this] { window->calculation->exportInput(true); });
    connect(search, &QLineEdit::textChanged, this, &AdvancedPanel::filterRows);
    connect(filter, &QComboBox::currentIndexChanged, this, &AdvancedPanel::filterRows);
    connect(category, &QComboBox::currentIndexChanged, this, &AdvancedPanel::filterRows);
}
void AdvancedPanel::change(const QString &key, const QJsonValue &value) {
    if (busy || window->hasCandidate() || window->worker ||
        nwchemAdvanced(window->document()).value(key) == value)
        return;
    window->safe([&] {
        window->edit("NWChem詳細設定を変更", [&](Document &d) { setNwchemAdvanced(d, key, value); });
    });
}
void AdvancedPanel::reset(const QString &group) {
    if (busy || window->hasCandidate() || window->worker)
        return;
    window->safe(
        [&] { window->edit("NWChem詳細設定を既定へ", [&](Document &d) { resetNwchemAdvanced(d, group); }); });
}
void AdvancedPanel::showRow(const QString &key) {
    const auto &row = rows[key];
    search->clear();
    category->setCurrentIndex(0);
    filter->setCurrentIndex(0);
    if (row.item->isHidden()) // An option kept for another kind of calculation.
        filter->setCurrentIndex(filter->findData("all"));
    table->setCurrentItem(row.item);
    table->scrollToItem(row.item, QAbstractItemView::PositionAtCenter);
    row.editor->setFocus(Qt::OtherFocusReason);
}
void AdvancedPanel::describe() {
    const auto *item = table->currentItem();
    const auto key = item ? item->data(Name, Qt::UserRole).toString() : QString();
    const auto group = item ? item->data(Name, Qt::UserRole + 1).toString() : QString();
    QString text = "行を選ぶと、その項目の説明を表示します。";
    for (const auto &o : nwchemAdvancedOptions())
        if (o.key == key) {
            text = o.label + "（NWChem: " + o.keyword + "）\n" + o.help + "\n未指定: " + o.defaultText;
            if (!item->text(State).isEmpty())
                text += "\n状態: " + item->text(State);
        }
    help->setText(text);
    outputSummary->setVisible(group == "visualization");
}
void AdvancedPanel::filterRows() {
    const auto &d = window->displayDocument();
    const auto a = nwchemAdvanced(d);
    const auto query = search->text().trimmed();
    const auto mode = filter->currentData().toString(), selectedGroup = category->currentData().toString();
    QSet<QString> visibleGroups;
    int visible = 0;
    for (const auto &o : nwchemAdvancedOptions()) {
        const bool show =
            (selectedGroup.isEmpty() || selectedGroup == o.group) &&
            (mode != "applicable" || nwchemAdvancedApplies(o, d)) &&
            (mode != "changed" || a.contains(o.key)) &&
            (o.label + ' ' + o.keyword + ' ' + o.key + ' ' + o.help).contains(query, Qt::CaseInsensitive);
        rows[o.key].item->setHidden(!show);
        if (show) {
            visibleGroups.insert(o.group);
            ++visible;
        }
    }
    for (auto it = groups.begin(); it != groups.end(); ++it)
        it.value()->setHidden(!visibleGroups.contains(it.key()));
    empty->setVisible(!visible);
    count->setText(QString("%1 / %2項目").arg(visible).arg(rows.size()));
}
void AdvancedPanel::refresh() {
    if (busy)
        return;
    QScopedValueRollback guard(busy, true);
    const auto &d = window->displayDocument();
    const auto a = nwchemAdvanced(d), s = nwchemSettings(d);
    const bool editable = !window->hasCandidate() && !window->worker;
    int activeCount = 0, pendingCount = 0, changedCount = 0;
    QMap<QString, int> changedGroups;
    const auto text = palette().color(QPalette::Text), dim = palette().color(QPalette::PlaceholderText);
    for (const auto &o : nwchemAdvancedOptions()) {
        auto &row = rows[o.key];
        const auto v = a.value(o.key);
        const bool active = nwchemAdvancedApplies(o, d), changed = !v.isUndefined();
        const QSignalBlocker blocker(row.editor);
        if (auto *line = qobject_cast<QLineEdit *>(row.editor)) {
            const auto value = changed ? nwchemAdvancedText(v) : QString();
            if (line->text() != value)
                line->setText(value);
        } else if (auto *check = qobject_cast<QCheckBox *>(row.editor))
            check->setChecked(v.toBool());
        else if (auto *combo = qobject_cast<QComboBox *>(row.editor)) {
            while (combo->count() > o.choices.size() + 1)
                combo->removeItem(combo->count() - 1);
            int index = changed ? combo->findData(v.toString()) : 0;
            if (index < 0) {
                combo->addItem("未対応: " + nwchemAdvancedText(v), v.toString());
                index = combo->count() - 1;
            }
            combo->setCurrentIndex(index);
        }
        row.editor->setEnabled(editable && active);
        row.reset->setEnabled(editable && changed);
        // Changed options in bold with a mark; options the calculation does not use are dimmed.
        row.item->setText(Name, (changed ? "● " : "") + o.label);
        auto font = row.item->font(Name);
        font.setBold(changed);
        row.item->setFont(Name, font);
        row.item->setForeground(Name, active ? text : dim);
        row.item->setForeground(Keyword, active ? text : dim);
        const auto error = nwchemAdvancedError(o, v);
        const auto state = changed ? (active ? (error.isEmpty() ? "入力へ反映" : "エラー: " + error)
                                             : "保留: 現在の計算では使用しません。")
                                   : QString();
        row.item->setText(State, state);
        row.item->setToolTip(State, state);
        row.item->setForeground(State, !error.isEmpty() ? errorColor(palette()) : active ? text : dim);
        if (changed) {
            ++changedCount;
            active ? ++activeCount : ++pendingCount;
            ++changedGroups[o.group];
        }
    }
    for (auto it = groups.begin(); it != groups.end(); ++it)
        it.value()->setText(State, changedGroups.contains(it.key())
                                       ? QString("変更 %1項目").arg(changedGroups[it.key()])
                                       : QString());
    context->setText(QString("%1 / %2 / %3  ·  詳細指定 %4項目（適用 %5 / 保留 %6）")
                         .arg(s["method"].toString().toUpper(), s["basis"].toString(), s["task"].toString())
                         .arg(changedCount)
                         .arg(activeCount)
                         .arg(pendingCount));
    for (auto it = groupResets.begin(); it != groupResets.end(); ++it)
        it.value()->setEnabled(editable && changedGroups.contains(it.key()));
    resetAll->setEnabled(editable && changedCount);
    undo->setEnabled(editable && window->history()->canUndo());
    redo->setEnabled(editable && window->history()->canRedo());
    message->setText(window->hasCandidate()
                         ? "未確定の分子候補を表示しています。確定・取消後に編集・出力できます。"
                     : window->worker ? "分子の整形中です。終了後に編集・出力できます。"
                                      : QString());
    message->setVisible(!message->text().isEmpty());
    const auto report = validateNwchem(d);
    const auto outputs = nwchemOutputFiles(d, report);
    outputSummary->setText(
        "HF/KSの軌道・密度を、計算後に出力します。保存先は「軌道・ファイル」の永続ディレクトリに従います。\n"
        "この出力設定は計算プリセットの切替後も保持します。\n" +
        (outputs.isEmpty() ? QString("CUBE・MOLDENの出力はオフです。") : "出力予定:\n" + outputs.join('\n')));
    diagnostics->setMessages(
        report.errors, report.warnings,
        "✓ 入力を生成できます。全文は「6 入力プレビュー」または下の「入力全文」で確認できます。");
    preview->setPlainText(report.errors.isEmpty() ? nwchemInput(d)
                                                  : "入力を生成できません。上のエラーを修正してください。");
    copy->setEnabled(editable && report.errors.isEmpty());
    save->setEnabled(editable && report.errors.isEmpty());
    filterRows();
    describe();
}
} // namespace mf
