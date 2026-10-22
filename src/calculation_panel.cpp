// SPDX-License-Identifier: Apache-2.0
// Native NWChem counterpart of KAGAMI's job_tab.py (Copyright 2026 Hiroshi Nakada).
#include "calculation_panel.h"
#include "advanced_panel.h"
#include "main_window.h"
#include "nwchem.h"
#include <QtWidgets>
#include <algorithm>

namespace mf {
CalculationPanel::CalculationPanel(MainWindow *owner) : QWidget(owner), window(owner) {
    setObjectName("calculationTab");
    auto *layout = new QVBoxLayout(this);
    auto *toolbar = new QHBoxLayout;
    layout->addLayout(toolbar);
    undo = new QPushButton("元に戻す");
    redo = new QPushButton("やり直す");
    undo->setObjectName("calculationUndo");
    redo->setObjectName("calculationRedo");
    toolbar->addWidget(undo);
    toolbar->addWidget(redo);
    auto *details = new QPushButton("4 詳細設定へ");
    details->setObjectName("calculationAdvanced");
    toolbar->addWidget(details);
    connect(details, &QPushButton::clicked, this,
            [this] { window->workspaceTabs->setCurrentWidget(window->advanced); });
    toolbar->addStretch();
    toolbar->addWidget(new QLabel("NWChem · 分子のGaussian基底計算"));
    connect(undo, &QPushButton::clicked, this, [this] { window->history()->undo(); });
    connect(redo, &QPushButton::clicked, this, [this] { window->history()->redo(); });
    message = new QLabel;
    message->setObjectName("calculationMessage");
    message->setWordWrap(true);
    layout->addWidget(message);
    auto *split = new QSplitter;
    layout->addWidget(split, 1);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(360);
    formWidget = new QWidget;
    auto *forms = new QVBoxLayout(formWidget);
    scroll->setWidget(formWidget);
    split->addWidget(scroll);
    auto *presetForm = group("プリセット", forms);
    presets = new QComboBox;
    presets->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    presets->setMinimumContentsLength(20);
    presets->setObjectName("calculationPresets");
    presetForm->addRow(presets);
    presetSummary = new QLabel;
    presetSummary->setWordWrap(true);
    presetSummary->setObjectName("calculationPresetSummary");
    presetForm->addRow(presetSummary);
    auto *presetButtons = new QHBoxLayout;
    auto *apply = new QPushButton("適用");
    apply->setObjectName("calculationApplyPreset");
    auto *store = new QPushButton("現在の条件を登録…");
    store->setObjectName("calculationStorePreset");
    removePreset = new QPushButton("登録解除");
    removePreset->setObjectName("calculationRemovePreset");
    for (auto *b : {apply, store, removePreset})
        presetButtons->addWidget(b);
    presetForm->addRow(presetButtons);
    userPresets = QJsonDocument::fromJson(window->settings.value("nwchem/presets").toByteArray()).array();
    reloadPresets();
    connect(presets, &QComboBox::currentIndexChanged, this, &CalculationPanel::describePreset);
    connect(apply, &QPushButton::clicked, this, [this] {
        const auto preset = presets->currentData().toJsonObject();
        window->safe([&] {
            window->edit("NWChemプリセットを適用", [&](Document &d) { applyNwchemPreset(d, preset); });
        });
    });
    connect(store, &QPushButton::clicked, this, &CalculationPanel::storePreset);
    connect(removePreset, &QPushButton::clicked, this, [this] {
        int index = presets->currentIndex() - nwchemPresets().size();
        if (index < 0 || index >= userPresets.size())
            return;
        userPresets.removeAt(index);
        window->settings.setValue("nwchem/presets",
                                  QJsonDocument(userPresets).toJson(QJsonDocument::Compact));
        reloadPresets();
        describePreset();
    });

    auto *basic = group("基本設定", forms);
    choice(basic, "計算目的", "task");
    choice(basic, "電子状態の手法", "method");
    choice(basic, "DFT汎関数", "functional");
    choice(basic, "DFT分散補正", "dispersion");
    choice(basic, "基底関数", "basis");
    flag(basic, "球面調和関数（オフ: Cartesian）", "spherical");
    auto *basisHelp = new QLabel("基底の対応元素を検査します。def2系のRb以降にはdef2-ECPを自動指定します。");
    basisHelp->setWordWrap(true);
    basic->addRow(basisHelp);

    auto *electronic = group("電荷・スピン", forms);
    integer(electronic, "全電荷", "charge", -1000, 1000);
    integer(electronic, "スピン多重度 2S+1", "multiplicity", 1, 1000);
    choice(electronic, "参照波動関数", "reference");
    auto *formalCharge = new QPushButton("原子の形式電荷の合計を適用");
    formalCharge->setObjectName("calculationFormalCharge");
    electronic->addRow(formalCharge);
    connect(formalCharge, &QPushButton::clicked, this,
            [this] { change("charge", window->document().formalCharge()); });

    auto *convergence = group("SCF・電子相関", forms);
    integer(convergence, "SCF最大反復", "scf_maxiter", 1, 10000);
    choice(convergence, "SCF収束", "scf_convergence");
    choice(convergence, "DFT積分グリッド", "grid");
    flag(convergence, "相関計算で内殻を凍結（freeze atomic）", "freeze_core");
    integer(convergence, "CC最大反復", "cc_maxiter", 1, 10000);
    auto *opt = group("最適化・振動数", forms);
    integer(opt, "最適化最大反復", "opt_maxiter", 1, 10000);
    choice(opt, "最適化収束", "opt_convergence");
    flag(opt, "数値Hessianを使用", "numerical_frequencies");
    auto *freqHelp = new QLabel("MP2・ROHF・ECP・COSMOの振動数は数値Hessianを自動選択します。"
                                "固定原子・ゴースト・外部点電荷を含む振動数解析は未対応です。");
    freqHelp->setWordWrap(true);
    opt->addRow(freqHelp);

    auto *environment = group("周囲の環境", forms);
    choice(environment, "溶媒モデル", "solvent");
    auto *dielectric = new QDoubleSpinBox;
    dielectric->setObjectName("nwchem_dielectric");
    dielectric->setRange(1, 1000);
    dielectric->setDecimals(4);
    dielectric->setKeyboardTracking(false);
    fields["dielectric"] = dielectric;
    environment->addRow("比誘電率（例: 水 78.4）", dielectric);
    connect(dielectric, &QDoubleSpinBox::valueChanged, this, [this](double n) { change("dielectric", n); });
    flag(environment, "「1 分子」の外部点電荷を含める", "include_point_charges");

    auto *output = group("入力ファイル", forms);
    for (const auto &key : {"start", "title"}) {
        auto *line = new QLineEdit;
        line->setObjectName(QString("nwchem_") + key);
        fields[key] = line;
        output->addRow(QString(key) == "start" ? "計算名（英数字）" : "タイトル", line);
        connect(line, &QLineEdit::editingFinished, this, [this, line, key] { change(key, line->text()); });
    }
    integer(output, "メモリ（MB / MPIプロセス）", "memory_mb", 64, 1048576);
    choice(output, "座標の出力単位", "geometry_units");
    auto *help =
        new QLabel("条件はプロジェクト保存の対象です。計算実行・出力結果の読込は今後の実装対象です。");
    help->setWordWrap(true);
    output->addRow(help);
    forms->addStretch();

    auto *right = new QWidget;
    auto *result = new QVBoxLayout(right);
    result->setContentsMargins(4, 0, 0, 0);
    summary = new QLabel;
    summary->setTextFormat(Qt::PlainText);
    summary->setObjectName("calculationSummary");
    summary->setWordWrap(true);
    result->addWidget(summary);
    validation = new QPlainTextEdit;
    validation->setObjectName("calculationValidation");
    validation->setReadOnly(true);
    validation->setMaximumHeight(175);
    result->addWidget(validation);
    auto *actions = new QHBoxLayout;
    actions->addWidget(new QLabel("NWChem入力（.nw）"));
    actions->addStretch();
    copy = new QPushButton("コピー");
    save = new QPushButton("保存…");
    copy->setObjectName("calculationCopy");
    save->setObjectName("calculationSave");
    actions->addWidget(copy);
    actions->addWidget(save);
    result->addLayout(actions);
    preview = new QPlainTextEdit;
    preview->setObjectName("calculationInput");
    preview->setReadOnly(true);
    preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    result->addWidget(preview, 1);
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    split->setSizes({500, 650});
    connect(copy, &QPushButton::clicked, this, [this] { exportInput(false); });
    connect(save, &QPushButton::clicked, this, [this] { exportInput(true); });
    connect(window->history(), &QUndoStack::indexChanged, this, [this] { refresh(); });
}
QFormLayout *CalculationPanel::group(const QString &title, QVBoxLayout *layout) {
    auto *box = new QGroupBox(title);
    layout->addWidget(box);
    auto *form = new QFormLayout(box);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    return form;
}
void CalculationPanel::choice(QFormLayout *form, const QString &label, const QString &key) {
    auto *combo = new QComboBox;
    combo->setObjectName("nwchem_" + key);
    for (const auto &c : nwchemChoices(key))
        combo->addItem(c.label, c.value);
    fields[key] = combo;
    form->addRow(label, combo);
    connect(combo, &QComboBox::currentIndexChanged, this,
            [this, combo, key] { change(key, combo->currentData().toString()); });
}
void CalculationPanel::integer(QFormLayout *form, const QString &label, const QString &key, int minimum,
                               int maximum) {
    auto *spin = new QSpinBox;
    spin->setObjectName("nwchem_" + key);
    spin->setRange(minimum, maximum);
    spin->setKeyboardTracking(false);
    fields[key] = spin;
    form->addRow(label, spin);
    connect(spin, &QSpinBox::valueChanged, this, [this, key](int n) { change(key, n); });
}
void CalculationPanel::flag(QFormLayout *form, const QString &label, const QString &key) {
    auto *check = new QCheckBox(label);
    check->setObjectName("nwchem_" + key);
    fields[key] = check;
    form->addRow(check);
    connect(check, &QCheckBox::toggled, this, [this, key](bool b) { change(key, b); });
}
void CalculationPanel::change(const QString &key, const QJsonValue &value) {
    if (busy || window->hasCandidate() || window->worker)
        return;
    const bool shared = key == "charge" || key == "multiplicity";
    const QJsonValue current =
        shared ? window->document().job().value(key) : nwchemSettings(window->document()).value(key);
    if (current == value)
        return;
    window->safe([&] {
        window->edit("NWChem計算条件を変更", [&](Document &d) {
            if (shared)
                d.setJobValue(key, value);
            else {
                auto s = d.data["nwchem"].toObject();
                s[key] = value;
                d.data["nwchem"] = s;
            }
        });
    });
}
void CalculationPanel::reloadPresets() {
    const QSignalBlocker blocker(presets);
    presets->clear();
    for (const auto &p : nwchemPresets())
        presets->addItem(p.name, p.settings);
    for (const auto &p : userPresets) {
        const auto object = p.toObject();
        presets->addItem("登録: " + object["name"].toString(), object["settings"].toObject());
    }
}
void CalculationPanel::describePreset() {
    const auto selected = presets->currentData().toJsonObject();
    const auto current = nwchemPresetSettings(nwchemSettings(window->displayDocument()));
    QStringList changes;
    for (auto it = selected.begin(); it != selected.end(); ++it)
        if (it.value() != current[it.key()]) {
            auto valueText = [](const QJsonValue &v) { return nwchemAdvancedText(v); };
            changes << it.key() + ": " + valueText(current[it.key()]) + " → " + valueText(it.value());
        }
    presetSummary->setText(QString("変更 %1 項目。分子・電荷・多重度・計算名・メモリ・単位は保持します。"
                                   "変更内容はここにマウスを置くと表示します。")
                               .arg(changes.size()));
    presetSummary->setToolTip(changes.isEmpty() ? "現在の条件と同じです。" : changes.join('\n'));
    removePreset->setEnabled(presets->currentIndex() >= nwchemPresets().size());
}
void CalculationPanel::storePreset() {
    if (window->hasCandidate() || window->worker)
        return;
    const auto settings = nwchemPresetSettings(nwchemSettings(window->document()));
    bool ok;
    auto name =
        QInputDialog::getText(this, "NWChemプリセットを登録", "名前", QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    for (const auto &entry : userPresets)
        if (entry.toObject()["name"].toString() == name) {
            QMessageBox::information(this, "登録名",
                                     "同名のプリセットがあります。別の名前で登録してください。");
            return;
        }
    userPresets << QJsonObject{{"name", name}, {"settings", settings}};
    window->settings.setValue("nwchem/presets", QJsonDocument(userPresets).toJson(QJsonDocument::Compact));
    reloadPresets();
    presets->setCurrentIndex(presets->count() - 1);
    describePreset();
}
void CalculationPanel::refresh() {
    if (busy)
        return;
    QScopedValueRollback guard(busy, true);
    const auto &d = window->displayDocument();
    const auto s = nwchemSettings(d);
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        const auto value =
            it.key() == "charge" || it.key() == "multiplicity" ? d.job()[it.key()] : s[it.key()];
        auto *w = it.value();
        const QSignalBlocker blocker(w);
        if (auto *c = qobject_cast<QComboBox *>(w)) {
            while (c->count() > nwchemChoices(it.key()).size())
                c->removeItem(c->count() - 1);
            int index = c->findData(value.toString());
            if (index < 0) {
                c->addItem("未対応: " + value.toVariant().toString(), value.toString());
                index = c->count() - 1;
            }
            c->setCurrentIndex(index);
        } else if (auto *spin = qobject_cast<QSpinBox *>(w))
            spin->setValue(value.toInt());
        else if (auto *spin = qobject_cast<QDoubleSpinBox *>(w))
            spin->setValue(value.toDouble());
        else if (auto *check = qobject_cast<QCheckBox *>(w))
            check->setChecked(value.toBool());
        else if (auto *line = qobject_cast<QLineEdit *>(w)) {
            if (line->text() != value.toString())
                line->setText(value.toString());
        }
    }
    const QString method = s["method"].toString(), task = s["task"].toString();
    for (const auto &key : {"functional", "dispersion", "grid"})
        fields[key]->setEnabled(method == "dft");
    fields["freeze_core"]->setEnabled(method == "mp2" || method.startsWith("ccsd"));
    fields["cc_maxiter"]->setEnabled(method.startsWith("ccsd"));
    for (const auto &key : {"opt_maxiter", "opt_convergence"})
        fields[key]->setEnabled(task.startsWith("optimize"));
    fields["numerical_frequencies"]->setEnabled(task.contains("frequencies"));
    fields["dielectric"]->setEnabled(s["solvent"] == "cosmo");
    const bool editable = !window->hasCandidate() && !window->worker;
    formWidget->setEnabled(editable);
    undo->setEnabled(editable && window->history()->canUndo());
    redo->setEnabled(editable && window->history()->canRedo());
    message->setText(window->hasCandidate()
                         ? "未確定の分子候補を表示しています。確定・取消後に条件を編集・出力できます。"
                     : window->worker ? "分子の整形中です。終了後に条件を編集・出力できます。"
                                      : QString());
    message->setVisible(!message->text().isEmpty());
    const auto check = validateNwchem(d);
    summary->setText(
        QString("%1  ·  %2 / %3\n電子数 %4（α %5 / β %6） · 全電荷 %7 · 多重度 %8 · %9%10")
            .arg(d.formula(), method == "dft" ? s["functional"].toString().toUpper() : method.toUpper(),
                 s["basis"].toString())
            .arg(check.electrons)
            .arg(check.alpha)
            .arg(check.beta)
            .arg(d.job()["charge"].toInt())
            .arg(d.job()["multiplicity"].toInt())
            .arg(method == "dft" && check.reference != "rohf" ? (check.reference == "rhf" ? "RKS" : "UKS")
                                                              : check.reference.toUpper(),
                 check.ecpElectrons ? "（ECP適用後）" : ""));
    QStringList diagnostics;
    if (check.errors.isEmpty())
        diagnostics << "✓ 入力を生成できます。";
    for (const auto &error : check.errors)
        diagnostics << "エラー: " + error;
    for (const auto &warning : check.warnings)
        diagnostics << "確認: " + warning;
    validation->setPlainText(diagnostics.join('\n'));
    validation->setFixedHeight(std::clamp(30 + int(diagnostics.size()) * 24, 65, 175));
    preview->setPlainText(check.errors.isEmpty() ? nwchemInput(d)
                                                 : "入力を生成できません。上のエラーを修正してください。");
    copy->setEnabled(editable && check.errors.isEmpty());
    save->setEnabled(editable && check.errors.isEmpty());
    describePreset();
}
void CalculationPanel::exportInput(bool toFile) {
    window->safe([&] {
        if (auto *focused = QApplication::focusWidget())
            focused->clearFocus(); // Commit line edits before capturing a complete, validated input.
        if (window->hasCandidate() || window->worker)
            fail("分子候補の確定・取消、または整形の終了後に出力してください。");
        const auto &d = window->document();
        const auto text = nwchemInput(d);
        if (!toFile) {
            QApplication::clipboard()->setText(text);
            window->statusBar()->showMessage("NWChem入力をコピーしました。", 5000);
            return;
        }
        auto path = QFileDialog::getSaveFileName(
            this, "NWChem入力を保存", nwchemSettings(d)["start"].toString() + ".nw", "NWChem (*.nw)");
        if (path.isEmpty())
            return;
        if (!path.endsWith(".nw", Qt::CaseInsensitive))
            path += ".nw";
        writeFile(path, text.toUtf8());
        window->statusBar()->showMessage("NWChem入力を保存しました。", 5000);
    });
}
} // namespace mf
