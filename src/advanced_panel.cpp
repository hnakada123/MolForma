// SPDX-License-Identifier: Apache-2.0
// Native NWChem settings; navigation/reset workflow inspired by KAGAMI advanced_tab.py.
#include "advanced_panel.h"
#include "calculation_panel.h"
#include "main_window.h"
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
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto *form = new QWidget;
    scroll->setWidget(form);
    auto *forms = new QVBoxLayout(form);
    empty = new QLabel("一致する詳細設定はありません。");
    empty->setObjectName("advancedEmpty");
    forms->addWidget(empty);
    for (const auto &g : nwchemAdvancedGroups()) {
        auto *box = new QGroupBox(g.label);
        groups[g.value] = box;
        forms->addWidget(box);
        auto *entries = new QVBoxLayout(box);
        auto *resetGroup = new QPushButton("このカテゴリを既定へ");
        resetGroup->setObjectName("advancedReset_" + g.value);
        groupResets[g.value] = resetGroup;
        entries->addWidget(resetGroup, 0, Qt::AlignRight);
        connect(resetGroup, &QPushButton::clicked, this, [this, group = g.value] { reset(group); });
        for (const auto &o : nwchemAdvancedOptions()) {
            if (o.group != g.value)
                continue;
            Row row;
            row.container = new QWidget;
            row.container->setObjectName("advancedRow_" + o.key);
            auto *vertical = new QVBoxLayout(row.container);
            vertical->setContentsMargins(0, 4, 0, 6);
            auto *line = new QHBoxLayout;
            row.label = new QLabel(o.label);
            row.label->setTextFormat(Qt::PlainText);
            row.label->setWordWrap(true);
            line->addWidget(row.label, 2);
            if (o.kind == AdvancedKind::Flag) {
                auto *check = new QCheckBox("有効");
                row.editor = check;
                connect(check, &QCheckBox::toggled, this, [this, key = o.key](bool value) {
                    change(key, value ? QJsonValue(true) : QJsonValue::Undefined);
                });
            } else if (o.kind == AdvancedKind::Choice) {
                auto *choices = new QComboBox;
                choices->addItem("既定（指定なし）", "");
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
            row.label->setBuddy(row.editor);
            row.editor->setToolTip(o.help + "\nNWChem: " + o.keyword + "\n未指定: " + o.defaultText);
            line->addWidget(row.editor, 2);
            row.reset = new QPushButton("戻す");
            row.reset->setObjectName("advancedResetField_" + o.key);
            row.reset->setToolTip("この項目の明示指定を解除します。Undoで戻せます。");
            line->addWidget(row.reset);
            connect(row.reset, &QPushButton::clicked, this,
                    [this, key = o.key] { change(key, QJsonValue::Undefined); });
            vertical->addLayout(line);
            auto *help = new QLabel(o.keyword + "  ·  " + o.help + "\n未指定: " + o.defaultText);
            help->setTextFormat(Qt::PlainText);
            help->setWordWrap(true);
            vertical->addWidget(help);
            row.state = new QLabel;
            row.state->setObjectName("advancedState_" + o.key);
            row.state->setTextFormat(Qt::PlainText);
            row.state->setWordWrap(true);
            vertical->addWidget(row.state);
            rows[o.key] = row;
            entries->addWidget(row.container);
        }
    }
    forms->addStretch();
    split->addWidget(scroll);
    auto *right = new QWidget;
    auto *output = new QVBoxLayout(right);
    output->setContentsMargins(4, 0, 0, 0);
    diagnostics = new QPlainTextEdit;
    diagnostics->setObjectName("advancedValidation");
    diagnostics->setReadOnly(true);
    output->addWidget(diagnostics);
    auto *actions = new QHBoxLayout;
    actions->addWidget(new QLabel("NWChem入力（詳細設定を反映）"));
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
    output->addWidget(preview, 1);
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    split->setSizes({680, 640});
    connect(copy, &QPushButton::clicked, this, [this] { window->calculation->exportInput(false); });
    connect(save, &QPushButton::clicked, this, [this] { window->calculation->exportInput(true); });
    connect(search, &QLineEdit::textChanged, this, &AdvancedPanel::filterRows);
    connect(filter, &QComboBox::currentIndexChanged, this, &AdvancedPanel::filterRows);
    connect(category, &QComboBox::currentIndexChanged, this, &AdvancedPanel::filterRows);
    connect(window->history(), &QUndoStack::indexChanged, this, &AdvancedPanel::refresh);
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
        rows[o.key].container->setVisible(show);
        if (show) {
            visibleGroups.insert(o.group);
            ++visible;
        }
    }
    for (auto it = groups.begin(); it != groups.end(); ++it)
        it.value()->setVisible(visibleGroups.contains(it.key()));
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
    QSet<QString> changedGroups;
    for (const auto &o : nwchemAdvancedOptions()) {
        auto &row = rows[o.key];
        const auto v = a.value(o.key);
        const bool active = nwchemAdvancedApplies(o, d), changed = !v.isUndefined();
        const QSignalBlocker blocker(row.editor);
        if (auto *line = qobject_cast<QLineEdit *>(row.editor)) {
            const auto text = changed ? nwchemAdvancedText(v) : QString();
            if (line->text() != text)
                line->setText(text);
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
        row.label->setText((changed ? "● " : "") + o.label);
        const auto error = nwchemAdvancedError(o, v);
        row.state->setText(changed ? (active ? (error.isEmpty() ? "入力へ反映" : "エラー: " + error)
                                             : "保留: 現在の計算では使用しません。")
                                   : QString());
        row.state->setVisible(changed);
        if (changed) {
            ++changedCount;
            active ? ++activeCount : ++pendingCount;
            changedGroups.insert(o.group);
        }
    }
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
    QStringList messages;
    if (report.errors.isEmpty())
        messages << "✓ 入力を生成できます。";
    for (const auto &error : report.errors)
        messages << "エラー: " + error;
    for (const auto &warning : report.warnings)
        messages << "確認: " + warning;
    diagnostics->setPlainText(messages.join('\n'));
    diagnostics->setFixedHeight(std::clamp(30 + int(messages.size()) * 24, 65, 170));
    preview->setPlainText(report.errors.isEmpty() ? nwchemInput(d)
                                                  : "入力を生成できません。上のエラーを修正してください。");
    copy->setEnabled(editable && report.errors.isEmpty());
    save->setEnabled(editable && report.errors.isEmpty());
    filterRows();
}
} // namespace mf
