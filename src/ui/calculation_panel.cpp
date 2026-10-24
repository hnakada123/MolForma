// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/calculation_panel.h"
#include "model/geometry.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "ui/advanced_panel.h"
#include "ui/diagnostics_list.h"
#include "ui/execution_panel.h"
#include "ui/input_preview_panel.h"
#include "ui/main_window.h"
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
    // Like the other setting tabs: Undo/Redo on the left, the other tabs in their order on the right.
    toolbar->addWidget(undo);
    toolbar->addWidget(redo);
    toolbar->addStretch();
    auto *details = new QPushButton("4 詳細設定へ");
    details->setObjectName("calculationAdvanced");
    toolbar->addWidget(details);
    auto *execution = new QPushButton("5 実行設定へ");
    execution->setObjectName("calculationExecution");
    toolbar->addWidget(execution);
    connect(execution, &QPushButton::clicked, this,
            [this] { window->workspaceTabs->setCurrentWidget(window->execution); });
    connect(details, &QPushButton::clicked, this,
            [this] { window->workspaceTabs->setCurrentWidget(window->advanced); });
    auto *inputPreview = new QPushButton("6 入力プレビューへ");
    inputPreview->setObjectName("calculationPreview");
    toolbar->addWidget(inputPreview);
    connect(inputPreview, &QPushButton::clicked, this,
            [this] { window->workspaceTabs->setCurrentWidget(window->inputPreview); });
    connect(undo, &QPushButton::clicked, this, [this] { window->history()->undo(); });
    connect(redo, &QPushButton::clicked, this, [this] { window->history()->redo(); });
    message = new QLabel;
    message->setObjectName("calculationMessage");
    message->setWordWrap(true);
    layout->addWidget(message);
    auto *split = new QSplitter;
    layout->addWidget(split, 1);
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    search = new QLineEdit;
    search->setObjectName("calculationSearch");
    search->setPlaceholderText("設定を検索（計算条件と「4 詳細設定」）…");
    search->setClearButtonEnabled(true);
    leftLayout->addWidget(search);
    scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(360);
    leftLayout->addWidget(scroll, 1);
    formWidget = new QWidget;
    formWidget->setObjectName("calculationForm");
    auto *forms = new QVBoxLayout(formWidget);
    scroll->setWidget(formWidget);
    split->addWidget(left);
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
    auto *customXc = new QLineEdit;
    customXc->setObjectName("nwchem_xc_custom");
    customXc->setPlaceholderText("例: xpbe96 0.75 cpbe96 1.0 hfexch 0.25");
    customXc->setToolTip("DFTブロックの xc 行です。汎関数で「任意のxc指定」を選んだときに使います。");
    fields["xc_custom"] = customXc;
    basic->addRow("任意のxc", customXc);
    connect(customXc, &QLineEdit::editingFinished, this,
            [this, customXc] { change("xc_custom", customXc->text().simplified()); });
    choice(basic, "DFT分散補正", "dispersion");
    choice(basic, "基底関数", "basis");
    choice(basic, "d・f関数の形式", "basis_functions");
    elementBases = new QTableWidget(0, 2);
    elementBases->setObjectName("nwchem_basis_overrides");
    elementBases->setHorizontalHeaderLabels({"元素", "基底"});
    elementBases->verticalHeader()->hide();
    elementBases->horizontalHeader()->setStretchLastSection(true);
    elementBases->setSelectionMode(QAbstractItemView::NoSelection);
    elementBases->setMaximumHeight(140);
    elementBases->setToolTip(
        "元素ごとに別の基底を指定します（例: 金属だけdef2-TZVP）。「共通」は上の基底関数です。");
    basic->addRow("元素別の基底", elementBases);
    auto *basisHelp = new QLabel("基底の対応元素を検査します。def2系のRb以降にはdef2-ECP、LANL2DZのNa以降には"
                                 "LANL2DZ ECPを自動指定します。");
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
    choice(convergence, "相対論ハミルトニアン", "relativistic");
    flag(convergence, "RI-J（Coulomb項のフィッティング、DFT）", "ri_j");
    flag(convergence, "相関計算で内殻を凍結（freeze atomic）", "freeze_core");
    integer(convergence, "CC最大反復", "cc_maxiter", 1, 10000);
    auto *opt = group("最適化・振動数", forms);
    integer(opt, "最適化最大反復", "opt_maxiter", 1, 10000);
    choice(opt, "最適化収束", "opt_convergence");
    flag(opt, "数値Hessianを使用", "numerical_frequencies");
    auto *freqHelp = new QLabel("MP2・ROHF・ECP・溶媒・相対論・RI-Jの振動数は数値Hessianを自動選択します。"
                                "固定原子・ゴースト・外部点電荷を含む振動数解析は未対応です。");
    freqHelp->setWordWrap(true);
    opt->addRow(freqHelp);

    auto *path = group("IRC・座標スキャン", forms);
    integer(path, "IRCの点数（各方向）", "irc_points", 1, 500);
    real(path, "IRCの刻み幅 / amu½·Bohr", "irc_stride", 0.01, 1, 3);
    scanCoordinate = new QLabel;
    scanCoordinate->setObjectName("calculationScanCoordinate");
    auto *scanAtoms = new QPushButton("選択中の原子を設定");
    scanAtoms->setObjectName("calculationScanAtoms");
    scanAtoms->setToolTip("「1 分子」で2〜4原子を順に選ぶと、距離・角度・二面角をスキャンします。"
                          "後に選んだ原子の側を動かします。");
    auto *scanRow = new QHBoxLayout;
    scanRow->addWidget(scanCoordinate, 1);
    scanRow->addWidget(scanAtoms);
    path->addRow("スキャン座標", scanRow);
    connect(scanAtoms, &QPushButton::clicked, this, &CalculationPanel::setScanCoordinate);
    real(path, "開始値（Å・°）", "scan_start", -180, 180, 4);
    real(path, "終了値（Å・°）", "scan_end", -180, 180, 4);
    integer(path, "点の数", "scan_steps", 2, 50);
    flag(path, "各点で他の座標を最適化（緩和スキャン）", "scan_relaxed");
    auto *pathHelp = new QLabel("IRCは遷移状態の構造から両方向へ反応経路を追跡します。結果は「結果ビューア」"
                                "の「経路・スキャン」に表示します。");
    pathHelp->setWordWrap(true);
    path->addRow(pathHelp);

    auto *constrained = group("内部座標の拘束（最適化）", forms);
    constraints = new QTableWidget(0, 2);
    constraints->setObjectName("nwchem_constraints");
    constraints->setHorizontalHeaderLabels({"座標", "値（Å・°）"});
    constraints->verticalHeader()->hide();
    constraints->horizontalHeader()->setStretchLastSection(true);
    constraints->setSelectionBehavior(QAbstractItemView::SelectRows);
    constraints->setMaximumHeight(130);
    constrained->addRow(constraints);
    auto *constraintButtons = new QHBoxLayout;
    auto *addConstraintButton = new QPushButton("選択中の2〜4原子を追加");
    addConstraintButton->setObjectName("calculationAddConstraint");
    auto *removeConstraint = new QPushButton("削除");
    removeConstraint->setObjectName("calculationRemoveConstraint");
    constraintButtons->addWidget(addConstraintButton);
    constraintButtons->addWidget(removeConstraint);
    constraintButtons->addStretch();
    constrained->addRow(constraintButtons);
    auto *constraintHelp = new QLabel("距離・角度・二面角を指定値に固定して最適化します（NWChemのzcoord）。"
                                      "固定原子・ゴーストとは併用できません。");
    constraintHelp->setWordWrap(true);
    constrained->addRow(constraintHelp);
    connect(addConstraintButton, &QPushButton::clicked, this, &CalculationPanel::addConstraint);
    connect(removeConstraint, &QPushButton::clicked, this, [this] {
        const int row = constraints->currentRow();
        auto list = nwchemSettings(window->document())["constraints"].toArray();
        if (row < 0 || row >= list.size())
            return;
        list.removeAt(row);
        change("constraints", list);
    });
    connect(constraints, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (busy || item->column() != 1)
            return;
        auto list = nwchemSettings(window->document())["constraints"].toArray();
        if (item->row() >= list.size())
            return;
        bool ok;
        const double value = QLocale().toDouble(item->text(), &ok);
        auto c = list[item->row()].toObject();
        if (!ok || !std::isfinite(value)) {
            refresh();
            return;
        }
        c["value"] = value;
        list[item->row()] = c;
        change("constraints", list);
    });

    auto *followup = group("後続の一点計算", forms);
    flag(followup, "最適化後に別の手法・基底で一点エネルギーを計算", "followup");
    choice(followup, "手法", "followup_method");
    choice(followup, "DFT汎関数", "followup_functional");
    choice(followup, "DFT分散補正", "followup_dispersion");
    choice(followup, "基底関数（全元素）", "followup_basis");

    auto *counterpoise = group("BSSE補正", forms);
    flag(counterpoise, "単量体ごとのcounterpoise補正（BSSE）", "bsse");
    monomers = new QLabel;
    monomers->setObjectName("calculationMonomers");
    monomers->setWordWrap(true);
    counterpoise->addRow(monomers);

    auto *properties = group("物性", forms);
    flag(properties, "Mulliken電荷", "prop_mulliken");
    flag(properties, "ESP・RESP電荷", "prop_esp");
    flag(properties, "NMR遮蔽定数（GIAO）", "prop_nmr");
    flag(properties, "静的分極率", "prop_polarizability");
    auto *propertyHelp = new QLabel("最後の計算（後続の一点計算があればその条件）の後に計算します。"
                                    "結果は「結果ビューア」の「物性」に表示します。");
    propertyHelp->setWordWrap(true);
    properties->addRow(propertyHelp);

    auto *orbitalFiles = group("軌道ファイル", forms);
    moldenOutput = new QCheckBox("計算後に分子軌道をMOLDENファイルに出力");
    moldenOutput->setObjectName("calculationMoldenOutput");
    moldenOutput->setToolTip("「結果ビューア」の「軌道」で分子軌道の形を表示できます。"
                             "「4 詳細設定」の「MOLDENファイルを出力」と同じ設定です。");
    orbitalFiles->addRow(moldenOutput);
    connect(moldenOutput, &QCheckBox::toggled, this, [this](bool on) {
        if (busy || window->hasCandidate() || window->worker)
            return;
        window->safe([&] {
            window->edit("MOLDEN出力を変更", [&](Document &d) {
                setNwchemAdvanced(d, "molden_output", on ? QJsonValue(true) : QJsonValue::Undefined);
            });
        });
    });
    auto *orbitalHelp =
        new QLabel("電子密度・軌道の格子データ（CUBE）は「4 詳細設定」の「可視化ファイル出力」で"
                   "設定します。");
    orbitalHelp->setWordWrap(true);
    orbitalFiles->addRow(orbitalHelp);

    auto *excited = group("励起状態（TDDFT）", forms);
    flag(excited, "TDDFTで励起エネルギー・振動子強度を計算", "tddft");
    integer(excited, "励起状態の数（nroots）", "tddft_roots", 1, 100);
    flag(excited, "三重項も計算（閉殻）", "tddft_triplets");
    flag(excited, "Tamm–Dancoff近似（TDA / cis）", "tddft_tda");
    auto *excitedHelp =
        new QLabel("DFTの一点エネルギーで使えます。結果は「結果ビューア」の「励起状態」に表示します。");
    excitedHelp->setWordWrap(true);
    excited->addRow(excitedHelp);

    auto *environment = group("周囲の環境", forms);
    choice(environment, "溶媒モデル", "solvent");
    choice(environment, "溶媒", "solvent_name");
    auto *dielectric = new QDoubleSpinBox;
    dielectric->setObjectName("nwchem_dielectric");
    dielectric->setRange(1, 1000);
    dielectric->setDecimals(4);
    dielectric->setKeyboardTracking(false);
    fields["dielectric"] = dielectric;
    environment->addRow("比誘電率（例: 水 78.4）", dielectric);
    connect(dielectric, &QDoubleSpinBox::valueChanged, this, [this](double n) { change("dielectric", n); });
    flag(environment, "「1 分子」の外部点電荷を含める", "include_point_charges");

    auto *extra = group("追加入力（上級者向け）", forms);
    auto *extraHelp = new QLabel("MolFormaが扱わないNWChemの指示を、検査せずにそのまま入力へ加えます。"
                                 "task shell・pythonブロックは使用できません。");
    extraHelp->setWordWrap(true);
    extra->addRow(extraHelp);
    for (auto **edit : {&extraInput, &extraTasks}) {
        *edit = new QPlainTextEdit;
        (*edit)->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        (*edit)->setMaximumHeight(90);
        (*edit)->setLineWrapMode(QPlainTextEdit::NoWrap);
        // Unchecked input can start programs through NWChem: protected like the executable.
        (*edit)->setProperty("mcpExecutionSetting", true);
    }
    extraInput->setObjectName("nwchem_extra_input");
    extraTasks->setObjectName("nwchem_extra_tasks");
    extraInput->setPlaceholderText("例: set cphf:maxiter 50");
    extraTasks->setPlaceholderText("例: task dft property");
    extra->addRow("タスクの前", extraInput);
    extra->addRow("入力の末尾", extraTasks);
    auto *applyExtra = new QPushButton("追加入力を適用");
    applyExtra->setObjectName("calculationApplyExtra");
    applyExtra->setProperty("mcpExecutionSetting", true);
    extra->addRow(applyExtra);
    connect(applyExtra, &QPushButton::clicked, this, [this] {
        const auto before = extraInput->toPlainText(), after = extraTasks->toPlainText();
        if (busy || window->hasCandidate() || window->worker)
            return;
        window->safe([&] {
            window->edit("NWChemの追加入力を変更", [&](Document &d) {
                auto s = d.data["nwchem"].toObject();
                s["extra_input"] = before;
                s["extra_tasks"] = after;
                d.data["nwchem"] = s;
            });
        });
    });

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
    auto *help = new QLabel("条件はプロジェクトに保存します。「5 実行設定」で計算を実行し、"
                            "「6 入力プレビュー」で完成した入力を確認できます。");
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
    validation = new DiagnosticsList;
    validation->setObjectName("calculationValidation");
    result->addWidget(validation, 1);
    connect(validation, &DiagnosticsList::messageClicked, this,
            [this](const QString &message) { showField(fieldFor(message)); });
    connect(validation, &DiagnosticsList::fixRequested, this, &CalculationPanel::applyFix);
    result->addWidget(new QLabel("既定値から変更した条件"));
    changesFromDefaults = new QLabel;
    changesFromDefaults->setObjectName("calculationChanges");
    changesFromDefaults->setWordWrap(true);
    changesFromDefaults->setTextFormat(Qt::PlainText);
    changesFromDefaults->setTextInteractionFlags(Qt::TextSelectableByMouse);
    result->addWidget(changesFromDefaults);
    auto *actions = new QHBoxLayout;
    // The complete input is in "6 入力プレビュー"; here it is folded unless asked for.
    showInput = new QToolButton;
    showInput->setObjectName("calculationShowInput");
    showInput->setText("入力全文（.nw）");
    showInput->setCheckable(true);
    showInput->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    showInput->setArrowType(Qt::RightArrow);
    actions->addWidget(showInput);
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
    result->addWidget(preview, 3);
    showInput->setChecked(window->settings.value("calculation/showInput", false).toBool());
    preview->setVisible(showInput->isChecked());
    showInput->setArrowType(showInput->isChecked() ? Qt::DownArrow : Qt::RightArrow);
    connect(showInput, &QToolButton::toggled, this, [this](bool on) {
        preview->setVisible(on);
        showInput->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        window->settings.setValue("calculation/showInput", on);
    });
    split->addWidget(right);
    // The form needs the room; the checks and changes beside it are short.
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setSizes({720, 480});
    connect(copy, &QPushButton::clicked, this, [this] { exportInput(false); });
    connect(save, &QPushButton::clicked, this, [this] { exportInput(true); });
    // Labels for search and the list of changes: form rows, plus the fields with their own rows.
    for (const auto &[key, text] : {std::pair{"xc_custom", "任意のxc"},
                                    {"dielectric", "比誘電率"},
                                    {"basis_overrides", "元素別の基底"},
                                    {"constraints", "内部座標の拘束"},
                                    {"scan_atoms", "スキャン座標"},
                                    {"extra_input", "追加入力（タスクの前）"},
                                    {"extra_tasks", "追加入力（入力の末尾）"}})
        labels[key] = text;
    QStringList completions = labels.values();
    for (const auto &o : nwchemAdvancedOptions())
        completions << o.label + "（詳細設定）";
    auto *completer = new QCompleter(completions, search);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    search->setCompleter(completer);
    connect(completer, qOverload<const QString &>(&QCompleter::activated), this,
            &CalculationPanel::searchSettings);
    connect(search, &QLineEdit::returnPressed, this, [this] { searchSettings(search->text()); });
}
QString CalculationPanel::valueText(const QString &key, const QJsonValue &value) const {
    for (const auto &c : nwchemChoices(key))
        if (c.value == value.toString())
            return c.label;
    if (value.isBool())
        return value.toBool() ? "オン" : "オフ";
    if (value.isDouble())
        return QLocale().toString(value.toDouble());
    if (value.isArray())
        return QString("%1件").arg(value.toArray().size());
    if (value.isObject()) {
        QStringList parts;
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            parts << it.key() + ": " + it.value().toString();
        return parts.join("、");
    }
    const auto text = value.toString();
    return text.contains('\n') || text.size() > 40 ? QString("あり（%1文字）").arg(text.size()) : text;
}
void CalculationPanel::showField(const QString &key) {
    QWidget *target = key == "constraints"       ? static_cast<QWidget *>(constraints)
                      : key == "scan_atoms"      ? static_cast<QWidget *>(scanCoordinate)
                      : key == "extra_input"     ? static_cast<QWidget *>(extraInput)
                      : key == "extra_tasks"     ? static_cast<QWidget *>(extraTasks)
                      : key == "basis_overrides" ? static_cast<QWidget *>(elementBases)
                                                 : fields.value(key);
    if (key == "advanced") {
        window->workspaceTabs->setCurrentWidget(window->advanced);
        return;
    }
    if (!target)
        return;
    scroll->ensureWidgetVisible(target, 20, 60);
    target->setFocus(Qt::OtherFocusReason);
}
QString CalculationPanel::fieldFor(const QString &message) const {
    // The wording of validateNwchem's messages names the setting; the first match wins.
    static const QVector<std::pair<const char *, const char *>> words{{"詳細設定", "advanced"},
                                                                      {"後続の一点計算", "followup"},
                                                                      {"追加入力", "extra_input"},
                                                                      {"拘束", "constraints"},
                                                                      {"スキャン", "scan_atoms"},
                                                                      {"IRC", "irc_points"},
                                                                      {"BSSE", "bsse"},
                                                                      {"物性", "prop_mulliken"},
                                                                      {"NMR", "prop_nmr"},
                                                                      {"ESP", "prop_esp"},
                                                                      {"TDDFT", "tddft"},
                                                                      {"任意のxc", "xc_custom"},
                                                                      {"分散補正", "dispersion"},
                                                                      {"D3", "dispersion"},
                                                                      {"SMD", "solvent"},
                                                                      {"COSMO", "solvent"},
                                                                      {"溶媒", "solvent_name"},
                                                                      {"誘電率", "dielectric"},
                                                                      {"相対論", "relativistic"},
                                                                      {"ZORA", "relativistic"},
                                                                      {"RI-J", "ri_j"},
                                                                      {"フィッティング", "ri_j"},
                                                                      {"元素別の基底", "basis_overrides"},
                                                                      {"ECP", "basis"},
                                                                      {"基底", "basis"},
                                                                      {"多重度", "multiplicity"},
                                                                      {"電子数", "multiplicity"},
                                                                      {"全電荷", "charge"},
                                                                      {"RHF", "reference"},
                                                                      {"ROHF", "reference"},
                                                                      {"UHF", "reference"},
                                                                      {"内殻凍結", "freeze_core"},
                                                                      {"CCSD", "method"},
                                                                      {"計算名", "start"},
                                                                      {"タイトル", "title"},
                                                                      {"メモリ", "memory_mb"},
                                                                      {"振動数", "task"},
                                                                      {"最適化", "task"},
                                                                      {"固定原子", "task"},
                                                                      {"点電荷", "include_point_charges"}};
    for (const auto &[word, key] : words)
        if (message.contains(QString::fromUtf8(word)))
            return key;
    for (auto it = fields.begin(); it != fields.end(); ++it)
        if (message.contains(it.key()))
            return it.key();
    return {};
}
QMap<QString, QString> CalculationPanel::fixes(const QStringList &errors) const {
    QMap<QString, QString> result;
    for (const auto &e : errors) {
        if (e.contains("と多重度") && e.contains("整合しません"))
            result[e] = "電子数に合う多重度にする";
        else if (e.contains("分散補正") || e.contains("パラメータがありません") || e.contains("一致しないか"))
            result[e] = "分散補正を「なし」にする";
        else if (e.contains("RHF / RKSは多重度1"))
            result[e] = "参照波動関数を「自動」にする";
        else if (e.contains("TDDFTは計算目的"))
            result[e] = "計算目的を「一点エネルギー」にする";
        else if (e.contains("SMDでは溶媒名"))
            result[e] = "溶媒を水にする";
        else if (e.contains("SMDはDFT専用"))
            result[e] = "溶媒モデルをCOSMOにする";
        else if (e.contains("ZORAはDFT専用"))
            result[e] = "相対論をDKHにする";
        else if (e.contains("RI-J（Coulombフィッティング）はDFT"))
            result[e] = "RI-Jをオフにする";
    }
    return result;
}
void CalculationPanel::applyFix(const QString &message) {
    const auto &d = window->document();
    const auto fix = fixes({message}).value(message);
    if (fix.startsWith("電子数")) {
        const int electrons = validateNwchem(d).electrons;
        change("multiplicity", electrons % 2 ? 2 : 1);
    } else if (fix.startsWith("分散補正"))
        change("dispersion", "none");
    else if (fix.startsWith("参照波動関数"))
        change("reference", "auto");
    else if (fix.startsWith("計算目的"))
        change("task", "energy");
    else if (fix.startsWith("溶媒を水"))
        change("solvent_name", "h2o");
    else if (fix.startsWith("溶媒モデル"))
        change("solvent", "cosmo");
    else if (fix.startsWith("相対論"))
        change("relativistic", "dkh");
    else if (fix.startsWith("RI-J"))
        change("ri_j", false);
}
void CalculationPanel::searchSettings(const QString &text) {
    const auto query = QString(text).remove("（詳細設定）").trimmed();
    if (query.isEmpty())
        return;
    for (auto it = labels.begin(); it != labels.end(); ++it)
        if (it.value().contains(query, Qt::CaseInsensitive)) {
            showField(it.key());
            return;
        }
    // Not a field of this tab: show it in "4 詳細設定", filtered by the same words.
    window->workspaceTabs->setCurrentWidget(window->advanced);
    if (auto *field = window->advanced->findChild<QLineEdit *>("advancedSearch"))
        field->setText(query);
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
    labels[key] = label;
    fieldOrder << key;
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
    labels[key] = label;
    fieldOrder << key;
    form->addRow(label, spin);
    connect(spin, &QSpinBox::valueChanged, this, [this, key](int n) { change(key, n); });
}
void CalculationPanel::real(QFormLayout *form, const QString &label, const QString &key, double minimum,
                            double maximum, int decimals) {
    auto *spin = new QDoubleSpinBox;
    spin->setObjectName("nwchem_" + key);
    spin->setRange(minimum, maximum);
    spin->setDecimals(decimals);
    spin->setKeyboardTracking(false);
    fields[key] = spin;
    labels[key] = label;
    fieldOrder << key;
    form->addRow(label, spin);
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this, key](double n) { change(key, n); });
}
void CalculationPanel::flag(QFormLayout *form, const QString &label, const QString &key) {
    auto *check = new QCheckBox(label);
    check->setObjectName("nwchem_" + key);
    fields[key] = check;
    labels[key] = label;
    fieldOrder << key;
    form->addRow(check);
    connect(check, &QCheckBox::toggled, this, [this, key](bool b) { change(key, b); });
}
void CalculationPanel::refreshElementBases(const QJsonObject &s) {
    QStringList symbols;
    for (const auto &a : window->displayDocument().atoms)
        if (!symbols.contains(a.element))
            symbols << a.element;
    std::sort(symbols.begin(), symbols.end(),
              [](const auto &a, const auto &b) { return element(a).number < element(b).number; });
    const auto overrides = s["basis_overrides"].toObject();
    const QSignalBlocker blocker(elementBases);
    elementBases->setRowCount(symbols.size());
    for (int row = 0; row < symbols.size(); ++row) {
        const auto symbol = symbols[row];
        auto *item = new QTableWidgetItem(symbol);
        item->setFlags(Qt::ItemIsEnabled);
        elementBases->setItem(row, 0, item);
        auto *combo = qobject_cast<QComboBox *>(elementBases->cellWidget(row, 1));
        if (!combo || combo->property("element") != symbol) {
            combo = new QComboBox;
            combo->setObjectName("nwchem_basis_" + symbol);
            combo->setProperty("element", symbol);
            combo->addItem({}, QString());
            for (const auto &c : nwchemChoices("basis"))
                combo->addItem(c.label, c.value);
            elementBases->setCellWidget(row, 1, combo);
            connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, symbol] {
                auto next = nwchemSettings(window->document())["basis_overrides"].toObject();
                if (combo->currentData().toString().isEmpty())
                    next.remove(symbol);
                else
                    next[symbol] = combo->currentData().toString();
                change("basis_overrides", next);
            });
        }
        const QSignalBlocker comboBlocker(combo);
        combo->setItemText(0, "共通（" + s["basis"].toString() + "）");
        const int index = combo->findData(overrides[symbol].toString());
        combo->setCurrentIndex(std::max(0, index));
    }
}
void CalculationPanel::refreshConstraints(const QJsonObject &s) {
    const auto &d = window->displayDocument();
    const auto list = s["constraints"].toArray();
    const QSignalBlocker blocker(constraints);
    constraints->setRowCount(list.size());
    // Update items in place: this also runs from the table's own itemChanged signal.
    const auto cell = [this](int row, int column) {
        auto *item = constraints->item(row, column);
        if (!item) {
            item = new QTableWidgetItem;
            if (column == 0)
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            constraints->setItem(row, column, item);
        }
        return item;
    };
    for (int row = 0; row < list.size(); ++row) {
        const auto c = list[row].toObject();
        cell(row, 0)->setText(nwchemCoordinateLabel(d, idsFromJson(c["atoms"])));
        cell(row, 1)->setText(QLocale().toString(c["value"].toDouble(), 'f', 4));
    }
    const auto scan = idsFromJson(s["scan_atoms"]);
    scanCoordinate->setText(scan.isEmpty() ? QString("未設定") : nwchemCoordinateLabel(d, scan));
    QStringList parts;
    for (const auto &ids : nwchemBsseMonomers(d)) {
        QMap<QString, int> counts;
        for (const auto &id : ids)
            ++counts[d.atom(id).element];
        QString formula;
        for (const auto &symbol : counts.keys())
            formula += symbol + (counts[symbol] > 1 ? QString::number(counts[symbol]) : QString());
        parts << formula;
    }
    monomers->setText(parts.size() < 2 ? "単量体: 結合していない分子が2個以上必要です。"
                                       : QString("単量体 %1 個: %2（電荷は原子の形式電荷から決めます）")
                                             .arg(parts.size())
                                             .arg(parts.join("、")));
}
// The internal coordinate of the ordered selection in the editor, with its current value.
static std::pair<Ids, double> selectedCoordinate(MainWindow *window, const Ids &selection) {
    if (selection.size() < 2 || selection.size() > 4)
        fail("「1 分子」で2〜4原子を順に選択してください（距離・角度・二面角）。");
    return {selection, measure(window->document(), selection)};
}
void CalculationPanel::addConstraint() {
    window->safe([&] {
        const auto [ids, value] = selectedCoordinate(window, window->selected);
        auto list = nwchemSettings(window->document())["constraints"].toArray();
        list.append(QJsonObject{{"atoms", idsToJson(ids)}, {"value", std::round(value * 1e4) / 1e4}});
        change("constraints", list);
    });
}
void CalculationPanel::setScanCoordinate() {
    window->safe([&] {
        const auto [ids, value] = selectedCoordinate(window, window->selected);
        // A first guess for the range: +0.5 Å, +60° or half a turn, kept inside the valid range.
        const double start = std::round(value * 1e4) / 1e4;
        const double end = ids.size() == 2   ? start + 0.5
                           : ids.size() == 3 ? std::min(start + 60, 179.0)
                                             : (start <= 0 ? start + 180 : start - 180);
        if (busy || window->hasCandidate() || window->worker)
            return;
        window->edit("スキャン座標を設定", [&](Document &d) {
            auto s = d.data["nwchem"].toObject();
            s["scan_atoms"] = idsToJson(ids);
            s["scan_start"] = start;
            s["scan_end"] = end;
            s["task"] = "scan";
            d.data["nwchem"] = s;
        });
    });
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
    auto keys = fieldOrder; // In the order of the form.
    for (const auto &key : selected.keys())
        if (!keys.contains(key))
            keys << key;
    for (const auto &key : keys) {
        if (!selected.contains(key) || selected[key] == current[key])
            continue;
        if (key == "advanced") {
            changes << "詳細設定: " + QString::number(selected[key].toObject().size()) + "項目";
            continue;
        }
        changes << labels.value(key, key) + ": " + valueText(key, current[key]) + " → " +
                       valueText(key, selected[key]);
    }
    presetSummary->setText(
        changes.isEmpty()
            ? QString("現在の条件と同じです。")
            : QString(
                  "適用すると %1 項目が変わります（分子・電荷・多重度・計算名・メモリ・単位は保持）:\n・%2")
                  .arg(changes.size())
                  .arg(changes.mid(0, 8).join("\n・") + (changes.size() > 8 ? "\n・…" : "")));
    presetSummary->setToolTip(changes.join('\n'));
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
    refreshElementBases(s);
    refreshConstraints(s);
    {
        const QSignalBlocker blocker(moldenOutput);
        moldenOutput->setChecked(nwchemAdvanced(d).value("molden_output").toBool());
    }
    for (int i = 0; i < 2; ++i) {
        const auto key = i ? "extra_tasks" : "extra_input";
        auto *edit = i ? extraTasks : extraInput;
        // Keep unapplied typing unless the stored text itself changed (undo, project load).
        if (s[key].toString() != appliedExtra[i] || edit->toPlainText() == appliedExtra[i]) {
            appliedExtra[i] = s[key].toString();
            if (edit->toPlainText() != appliedExtra[i])
                edit->setPlainText(appliedExtra[i]);
        }
    }
    const QString method = s["method"].toString(), task = s["task"].toString();
    for (const auto &key : {"functional", "dispersion", "grid", "ri_j"})
        fields[key]->setEnabled(method == "dft");
    fields["relativistic"]->setEnabled(method == "hf" || method == "dft" || s["relativistic"] != "none");
    for (const auto &key : {"irc_points", "irc_stride"})
        fields[key]->setEnabled(task == "irc");
    for (const auto &key : {"scan_start", "scan_end", "scan_steps", "scan_relaxed"})
        fields[key]->setEnabled(task == "scan");
    const bool followup = s["followup"].toBool();
    fields["followup"]->setEnabled(task.startsWith("optimize") || task.startsWith("saddle") || followup);
    fields["followup_method"]->setEnabled(followup);
    for (const auto &key : {"followup_functional", "followup_dispersion"})
        fields[key]->setEnabled(followup && s["followup_method"] == "dft");
    fields["followup_basis"]->setEnabled(followup);
    fields["solvent_name"]->setEnabled(s["solvent"] != "none");
    fields["xc_custom"]->setEnabled(method == "dft" && s["functional"] == "custom");
    // Stay enabled while checked, so an invalid combination can always be switched off.
    fields["tddft"]->setEnabled((method == "dft" && task == "energy") || s["tddft"].toBool());
    for (const auto &key : {"tddft_roots", "tddft_triplets", "tddft_tda"})
        fields[key]->setEnabled(method == "dft" && s["tddft"].toBool());
    fields["freeze_core"]->setEnabled(method == "mp2" || method.startsWith("ccsd"));
    fields["cc_maxiter"]->setEnabled(method.startsWith("ccsd"));
    for (const auto &key : {"opt_maxiter", "opt_convergence"})
        fields[key]->setEnabled(nwchemDriverTask(s) || task == "irc");
    fields["numerical_frequencies"]->setEnabled(task.contains("frequencies"));
    fields["dielectric"]->setEnabled(s["solvent"] == "cosmo" && s["solvent_name"] == "dielectric");
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
    summary->setText(QString("%1  ·  %2 / %3\n電子数 %4（α %5 / β %6） · 全電荷 %7 · 多重度 %8 · %9%10")
                         .arg(d.formula(), method == "dft" ? nwchemFunctionalLabel(s) : method.toUpper(),
                              nwchemBasisLabel(s))
                         .arg(check.electrons)
                         .arg(check.alpha)
                         .arg(check.beta)
                         .arg(d.job()["charge"].toInt())
                         .arg(d.job()["multiplicity"].toInt())
                         .arg(method == "dft" && check.reference != "rohf"
                                  ? (check.reference == "rhf" ? "RKS" : "UKS")
                                  : check.reference.toUpper(),
                              check.ecpElectrons ? "（ECP適用後）" : ""));
    validation->setMessages(
        check.errors, check.warnings,
        "入力を生成できます。全文は「6 入力プレビュー」または下の「入力全文」で確認できます。",
        fixes(check.errors));
    QStringList changed;
    const auto defaults = nwchemDefaults();
    auto order = fieldOrder; // In the order of the form, then the fields with their own rows.
    for (const auto &key : labels.keys())
        if (!order.contains(key))
            order << key;
    for (const auto &key : order)
        if (defaults.contains(key) && s[key] != defaults[key])
            changed << labels[key] + ": " + valueText(key, s[key]);
    const int advancedCount = nwchemAdvanced(d).size();
    if (advancedCount)
        changed << QString("「4 詳細設定」: %1項目").arg(advancedCount);
    changesFromDefaults->setText(changed.isEmpty() ? QString("なし（HF / STO-3G・一点エネルギー）")
                                                   : changed.join("\n"));
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
