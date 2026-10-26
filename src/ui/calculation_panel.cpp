// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/calculation_panel.h"
#include "model/geometry.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_output.h"
#include "ui/diagnostics_list.h"
#include "ui/editor_chrome.h"
#include "ui/execution_panel.h"
#include "ui/input_preview_panel.h"
#include "ui/main_window.h"
#include <QtWidgets>
#include <algorithm>

namespace mf {
namespace {
// Takes the width left over in its row of a FlowLayout.
QWidget *gap() {
    auto *w = new QWidget;
    w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return w;
}
QLabel *caption(const QString &text) {
    auto *label = new QLabel(text);
    label->setProperty("caption", true);
    label->setWordWrap(true);
    return label;
}
// The label of a form row, after a dot while its setting differs from the default. The dot keeps
// its room when hidden, so labels do not move.
QString marked(const QString &text, bool changed, const QColor &accent) {
    return QString("<span style=\"color:%1\">●</span>&nbsp;%2")
        .arg(changed ? accent.name() : QString("transparent"), text.toHtmlEscaped());
}
} // namespace

CalculationPanel::CalculationPanel(MainWindow *owner) : QWidget(owner), window(owner) {
    setObjectName("calculationTab");
    setAttribute(Qt::WA_StyledBackground);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Presets, search and the filter of changed settings.
    auto *toolbar = new QFrame;
    toolbar->setObjectName("calculationToolbar");
    toolbar->setProperty("toolbar", true);
    auto *tools = new FlowLayout(toolbar, 8, 6);
    tools->setContentsMargins(12, 8, 12, 8);
    outer->addWidget(toolbar);
    presetButton = new QPushButton("プリセット");
    presetButton->setObjectName("calculationPresets");
    presetButton->setIcon(lineIcon("preset"));
    presetButton->setToolTip(
        "組込み・登録したプリセットを適用します。項目のツールチップに変わる条件を表示します。");
    presetMenu = new QMenu(presetButton);
    presetMenu->setObjectName("calculationPresetMenu");
    presetMenu->setToolTipsVisible(true);
    presetButton->setMenu(presetMenu);
    tools->addWidget(presetButton);
    storeButton = new QPushButton("現在の条件を登録…");
    storeButton->setObjectName("calculationStorePreset");
    tools->addWidget(storeButton);
    connect(storeButton, &QPushButton::clicked, this, &CalculationPanel::storePreset);
    userPresets = QJsonDocument::fromJson(window->settings.value("nwchem/presets").toByteArray()).array();
    reloadPresets();
    connect(presetMenu, &QMenu::aboutToShow, this, [this] {
        // What each preset would change, against the current settings.
        for (auto *action : presetMenu->actions())
            if (action->data().isValid())
                action->setToolTip(describePreset(action->data().toJsonObject()));
    });
    tools->addWidget(gap());
    search = new QLineEdit;
    search->setObjectName("calculationSearch");
    search->setProperty("search", true);
    search->setPlaceholderText("設定を検索（項目名・NWChemキーワード）");
    search->setClearButtonEnabled(true);
    search->addAction(lineIcon("search"), QLineEdit::LeadingPosition);
    search->setFixedWidth(280);
    tools->addWidget(search);
    // All rows or the changed ones, and the reset of everything.
    changeFilter = new QComboBox;
    changeFilter->setObjectName("calculationChangeFilter");
    changeFilter->addItem("すべての項目", "all");
    changeFilter->addItem("既定から変更した項目のみ", "changed");
    changeFilters = new ChoiceButtons(changeFilter, ChoiceButtons::Segments, {"すべて", "変更済み"});
    changeFilters->setObjectName("calculationChangeFilters");
    tools->addWidget(changeFilters);
    connect(changeFilter, &QComboBox::currentIndexChanged, this, [this] {
        filterRows();
        followScroll();
    });
    resetAll = new QPushButton("すべて既定へ");
    resetAll->setObjectName("calculationResetAll");
    resetAll->setIcon(lineIcon("reset"));
    resetAll->setToolTip(
        "この画面の条件と詳細設定を既定に戻します（計算名・タイトル・座標の単位・追加入力と、"
        "電荷・多重度は保持）。「元に戻す」で戻せます。");
    tools->addWidget(resetAll);
    connect(resetAll, &QPushButton::clicked, this, [this] {
        QStringList keys;
        for (const auto &key : changedKeys(nwchemSettings(window->document())))
            if (!QStringList{"start", "title", "geometry_units"}.contains(key))
                keys << key;
        reset(keys, "計算条件をすべて既定に戻す", {"*"});
    });

    // A pending candidate or a running worker: the form waits.
    banner = new QFrame;
    banner->setObjectName("calculationBanner");
    banner->setProperty("banner", true);
    auto *bannerRow = new QHBoxLayout(banner);
    bannerRow->setContentsMargins(12, 6, 12, 6);
    message = new QLabel;
    message->setObjectName("calculationMessage");
    message->setWordWrap(true);
    bannerRow->addWidget(message, 1);
    banner->hide();
    outer->addWidget(banner);

    auto *body = new QHBoxLayout;
    body->setSpacing(0);
    outer->addLayout(body, 1);
    navColumn = new QFrame;
    navColumn->setObjectName("calculationNav");
    navColumn->setFixedWidth(224);
    auto *navLayout = new QVBoxLayout(navColumn);
    navLayout->setContentsMargins(6, 10, 6, 10);
    navLayout->setSpacing(8);
    nav = new NavList;
    nav->setObjectName("calculationSections");
    navLayout->addWidget(nav, 1);
    auto *legend = caption("「変更 n」は既定から変えた項目の数、薄い字はこの計算で使わないセクションです。");
    legend->setContentsMargins(10, 0, 6, 0);
    navLayout->addWidget(legend);
    body->addWidget(navColumn);
    scroll = new QScrollArea;
    scroll->setObjectName("calculationScroll");
    scroll->setProperty("canvas", true);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(380);
    formWidget = new QWidget;
    formWidget->setObjectName("calculationForm");
    formWidget->setProperty("canvas", true);
    auto *forms = new QVBoxLayout(formWidget);
    forms->setContentsMargins(16, 16, 16, 16);
    forms->setSpacing(12);
    scroll->setWidget(formWidget);
    body->addWidget(scroll, 1);
    connect(scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, &CalculationPanel::followScroll);
    connect(nav, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) { showSection(item->data(Qt::UserRole).toString()); });
    connect(nav, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item)
            showSection(item->data(Qt::UserRole).toString());
    });

    nav->addHeading("主な条件");
    auto *basic = group("basic", "基本設定");
    buttons(basic, "計算目的", "task", true,
            {"一点エネルギー", "勾配", "構造最適化", "振動数", "最適化 → 振動数", "遷移状態探索",
             "遷移状態 → 振動数", "IRC", "座標スキャン"});
    buttons(basic, "電子状態の手法", "method", false, {"HF", "DFT", "MP2", "CCSD", "CCSD(T)"});
    choice(basic, "DFT汎関数", "functional");
    auto *customXc = new QLineEdit;
    customXc->setObjectName("nwchem_xc_custom");
    customXc->setPlaceholderText("例: xpbe96 0.75 cpbe96 1.0 hfexch 0.25");
    customXc->setToolTip("DFTブロックの xc 行です。汎関数で「任意のxc指定」を選んだときに使います。");
    fields["xc_custom"] = customXc;
    addRow(basic, "xc_custom", "任意のxc", customXc);
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
    elementBases->setToolTip(
        "元素ごとに別の基底を指定します（例: 金属だけdef2-TZVP）。「共通」は上の基底関数です。");
    addRow(basic, "basis_overrides", "元素別の基底", elementBases);
    help(basic, "基底の対応元素を検査します。def2系のRb以降にはdef2-ECP、LANL2DZのNa以降には"
                "LANL2DZ ECPを自動指定します。");

    auto *electronic = group("electronic", "電荷・スピン");
    integer(electronic, "全電荷", "charge", -1000, 1000);
    integer(electronic, "スピン多重度 2S+1", "multiplicity", 1, 1000);
    choice(electronic, "参照波動関数", "reference");
    reference = new QLabel;
    reference->setObjectName("calculationReference");
    reference->setProperty("muted", true);
    electronic->addRow(QString(), reference);
    rowKeys[reference] = "reference";
    formalCharge = new QPushButton;
    formalCharge->setObjectName("calculationFormalCharge");
    electronic->addRow(QString(), formalCharge);
    rowKeys[formalCharge] = "charge";
    connect(formalCharge, &QPushButton::clicked, this,
            [this] { change("charge", window->document().formalCharge()); });

    auto *convergence = group("convergence", "SCF・電子相関");
    integer(convergence, "SCF最大反復", "scf_maxiter", 1, 10000);
    choice(convergence, "SCF収束", "scf_convergence");
    buttons(convergence, "DFT積分グリッド", "grid", false);
    choice(convergence, "相対論ハミルトニアン", "relativistic");
    flag(convergence, "RI-J（Coulomb項のフィッティング、DFT）", "ri_j");
    flag(convergence, "相関計算で内殻を凍結（freeze atomic）", "freeze_core");
    integer(convergence, "CC最大反復", "cc_maxiter", 1, 10000);
    auto *opt = group("optimization", "最適化・振動数");
    integer(opt, "最適化最大反復", "opt_maxiter", 1, 10000);
    choice(opt, "最適化収束", "opt_convergence");
    flag(opt, "数値Hessianを使用", "numerical_frequencies");
    help(opt, "MP2・ROHF・ECP・溶媒・相対論・RI-Jの振動数は数値Hessianを自動選択します。"
              "固定原子・ゴースト・外部点電荷を含む振動数解析は未対応です。");

    nav->addHeading("計算目的に応じて");
    auto *path = group("path", "IRC・座標スキャン");
    integer(path, "IRCの点数（各方向）", "irc_points", 1, 500);
    real(path, "IRCの刻み幅 / amu½·Bohr", "irc_stride", 0.01, 1, 3);
    scanCoordinate = new QLabel;
    scanCoordinate->setObjectName("calculationScanCoordinate");
    auto *scanAtoms = new QPushButton("選択中の原子を設定");
    scanAtoms->setObjectName("calculationScanAtoms");
    scanAtoms->setToolTip("「1 分子」で2〜4原子を順に選ぶと、距離・角度・二面角をスキャンします。"
                          "後に選んだ原子の側を動かします。");
    auto *scanRow = new QWidget;
    auto *scanLayout = new QHBoxLayout(scanRow);
    scanLayout->setContentsMargins(0, 0, 0, 0);
    scanLayout->addWidget(scanCoordinate, 1);
    scanLayout->addWidget(scanAtoms);
    addRow(path, "scan_atoms", "スキャン座標", scanRow);
    connect(scanAtoms, &QPushButton::clicked, this, &CalculationPanel::setScanCoordinate);
    real(path, "開始値（Å・°）", "scan_start", -180, 180, 4);
    real(path, "終了値（Å・°）", "scan_end", -180, 180, 4);
    integer(path, "点の数", "scan_steps", 2, 50);
    flag(path, "各点で他の座標を最適化（緩和スキャン）", "scan_relaxed");
    help(path, "IRCは遷移状態の構造から両方向へ反応経路を追跡します。結果は「結果ビューア」"
               "の「経路・スキャン」に表示します。");

    auto *constrained = group("constraints", "内部座標の拘束");
    constraints = new QTableWidget(0, 2);
    constraints->setObjectName("nwchem_constraints");
    constraints->setHorizontalHeaderLabels({"座標", "値（Å・°）"});
    constraints->verticalHeader()->hide();
    constraints->horizontalHeader()->setStretchLastSection(true);
    constraints->setSelectionBehavior(QAbstractItemView::SelectRows);
    constraints->setMaximumHeight(130);
    constrained->addRow(constraints);
    rowKeys[constraints] = "constraints";
    rows["constraints"] = constraints;
    labels["constraints"] = "内部座標の拘束";
    sectionOf["constraints"] = "constraints";
    sections.last().keys << "constraints";
    auto *constraintButtons = new QWidget;
    auto *constraintRow = new QHBoxLayout(constraintButtons);
    constraintRow->setContentsMargins(0, 0, 0, 0);
    auto *addConstraintButton = new QPushButton("選択中の2〜4原子を追加");
    addConstraintButton->setObjectName("calculationAddConstraint");
    auto *removeConstraint = new QPushButton("削除");
    removeConstraint->setObjectName("calculationRemoveConstraint");
    constraintRow->addWidget(addConstraintButton);
    constraintRow->addWidget(removeConstraint);
    constraintRow->addStretch();
    constrained->addRow(constraintButtons);
    rowKeys[constraintButtons] = "constraints";
    help(constrained, "距離・角度・二面角を指定値に固定して最適化します（NWChemのzcoord）。"
                      "固定原子・ゴーストとは併用できません。");
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

    auto *followup = group("followup", "後続の一点計算");
    flag(followup, "最適化後に別の手法・基底で一点エネルギーを計算", "followup");
    choice(followup, "手法", "followup_method");
    choice(followup, "DFT汎関数", "followup_functional");
    choice(followup, "DFT分散補正", "followup_dispersion");
    choice(followup, "基底関数（全元素）", "followup_basis");

    auto *counterpoise = group("bsse", "BSSE補正");
    flag(counterpoise, "単量体ごとのcounterpoise補正（BSSE）", "bsse");
    monomers = new QLabel;
    monomers->setObjectName("calculationMonomers");
    monomers->setProperty("muted", true);
    monomers->setWordWrap(true);
    counterpoise->addRow(QString(), monomers);
    rowKeys[monomers] = "bsse";

    nav->addHeading("追加の計算");
    auto *properties = group("properties", "物性");
    flag(properties, "Mulliken電荷", "prop_mulliken");
    flag(properties, "ESP・RESP電荷", "prop_esp");
    flag(properties, "NMR遮蔽定数（GIAO）", "prop_nmr");
    flag(properties, "静的分極率", "prop_polarizability");
    help(properties, "最後の計算（後続の一点計算があればその条件）の後に計算します。"
                     "結果は「結果ビューア」の「物性」に表示します。");

    auto *excited = group("excited", "励起状態（TDDFT）");
    flag(excited, "TDDFTで励起エネルギー・振動子強度を計算", "tddft");
    integer(excited, "励起状態の数（nroots）", "tddft_roots", 1, 100);
    flag(excited, "三重項も計算（閉殻）", "tddft_triplets");
    flag(excited, "Tamm–Dancoff近似（TDA / cis）", "tddft_tda");
    help(excited, "DFTの一点エネルギーで使えます。結果は「結果ビューア」の「励起状態」に表示します。");

    nav->addHeading("環境・出力");
    auto *environment = group("environment", "周囲の環境");
    buttons(environment, "溶媒モデル", "solvent", false, {"気相", "COSMO", "SMD"});
    choice(environment, "溶媒", "solvent_name");
    auto *dielectric = new QDoubleSpinBox;
    dielectric->setObjectName("nwchem_dielectric");
    dielectric->setRange(1, 1000);
    dielectric->setDecimals(4);
    dielectric->setKeyboardTracking(false);
    fields["dielectric"] = dielectric;
    addRow(environment, "dielectric", "比誘電率（例: 水 78.4）", dielectric);
    labels["dielectric"] = "比誘電率";
    connect(dielectric, &QDoubleSpinBox::valueChanged, this, [this](double n) { change("dielectric", n); });
    flag(environment, "「1 分子」の外部点電荷を含める", "include_point_charges");

    // Only detailed options: the movecs, the scratch and permanent folders, MOLDEN and CUBE.
    auto *orbitalFiles = group("orbitals", "軌道・出力ファイル");
    help(orbitalFiles,
         "分子軌道の形を「結果ビューア」の「軌道」で表示するには、下の「MOLDENファイルを出力」を"
         "オンにします。電子密度・軌道の格子データはCUBEファイルで出力します。");

    auto *output = group("output", "入力ファイル");
    for (const auto &key : {"start", "title"}) {
        auto *line = new QLineEdit;
        line->setObjectName(QString("nwchem_") + key);
        fields[key] = line;
        addRow(output, key, QString(key) == "start" ? "計算名（英数字）" : "タイトル", line);
        connect(line, &QLineEdit::editingFinished, this, [this, line, key] { change(key, line->text()); });
    }
    // The memory is set in "3 実行設定" with the processes it is multiplied by.
    memoryState = new QLabel;
    memoryState->setObjectName("calculationMemoryState");
    auto *memoryLink = linkButton("3 実行設定で変更", "calculationMemoryLink");
    connect(memoryLink, &QPushButton::clicked, this, [this] { showField("memory_mb"); });
    auto *memoryRow = readOnlyRow(memoryState, memoryLink);
    output->addRow("メモリ", memoryRow);
    rows["memory_mb"] = memoryRow;
    rowKeys[memoryRow] = "memory_mb";
    sectionOf["memory_mb"] = "output";
    choice(output, "座標の出力単位", "geometry_units");

    auto *extra = group("extra", "追加入力（上級者向け）");
    help(extra, "MolFormaが扱わないNWChemの指示を、検査せずにそのまま入力へ加えます。"
                "task shell・pythonブロックは使用できません。");
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
    addRow(extra, "extra_input", "タスクの前", extraInput);
    addRow(extra, "extra_tasks", "入力の末尾", extraTasks);
    labels["extra_input"] = "追加入力（タスクの前）";
    labels["extra_tasks"] = "追加入力（入力の末尾）";
    auto *applyExtra = new QPushButton("追加入力を適用");
    applyExtra->setObjectName("calculationApplyExtra");
    applyExtra->setProperty("mcpExecutionSetting", true);
    extra->addRow(QString(), applyExtra);
    rowKeys[applyExtra] = "extra_input";
    connect(applyExtra, &QPushButton::clicked, this, [this] {
        const auto before = extraInput->toPlainText(), after = extraTasks->toPlainText();
        if (busy || !editable())
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
    forms->addStretch();
    // The detailed options under the fields of their sections; open where they are all there is.
    addDetails("convergence", {"scf", "dft", "correlation"}, false);
    addDetails("optimization", {"driver", "frequency"}, false);
    addDetails("environment", {"cosmo"}, false);
    addDetails("orbitals", {"files", "visualization"}, true);

    // The calculation in brief, its checks, the detail chosen last and what differs from the defaults;
    // scrolled when the window is too short for all of them.
    auto *railScroll = new QScrollArea;
    railScroll->setObjectName("calculationRailScroll");
    railScroll->setWidgetResizable(true);
    railScroll->setFrameShape(QFrame::NoFrame);
    railScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    railScroll->setFixedWidth(340);
    auto *rail = new QWidget;
    rail->setObjectName("calculationRail");
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(0, 16, 16, 16);
    railLayout->setSpacing(12);
    railScroll->setWidget(rail);
    body->addWidget(railScroll);
    summaryCard = new CalculationSummary("calculation", "この計算", [this](const QString &link) {
        if (link == "memory")
            showField("memory_mb");
    });
    railLayout->addWidget(summaryCard);

    validationCard = new StatusCard;
    validationCard->setObjectName("calculationCheck");
    validationCard->headline()->setObjectName("calculationStatus");
    auto *checkLayout = validationCard->body();
    passed = new QLabel("分子・計算条件・詳細設定の検証を通過しました。");
    passed->setObjectName("calculationChecksPassed");
    passed->setProperty("muted", true);
    passed->setWordWrap(true);
    checkLayout->addWidget(passed);
    validation = new DiagnosticsList;
    validation->setObjectName("calculationValidation");
    validation->setFrameShape(QFrame::NoFrame);
    validation->setFitContents(220);
    checkLayout->addWidget(validation);
    connect(validation, &DiagnosticsList::messageClicked, this,
            [this](const QString &message) { showField(fieldFor(message)); });
    connect(validation, &DiagnosticsList::fixRequested, this, &CalculationPanel::applyFix);
    railLayout->addWidget(validationCard);

    // The detailed option chosen last: what it does, its values and its lines in the input.
    detailCard = new SettingsCard("選んだ詳細設定");
    detailCard->setObjectName("calculationDetailCard");
    auto *closeDetail = new QToolButton;
    closeDetail->setObjectName("calculationDetailClose");
    closeDetail->setIcon(lineIcon("close"));
    closeDetail->setProperty("flat", true);
    closeDetail->setToolTip("閉じる");
    detailCard->header()->addWidget(closeDetail);
    connect(closeDetail, &QToolButton::clicked, this, [this] { selectDetail({}); });
    detailTitle = new QLabel;
    detailTitle->setObjectName("calculationDetailTitle");
    detailTitle->setProperty("cardTitle", true);
    detailTitle->setWordWrap(true);
    detailCard->body()->addWidget(detailTitle);
    auto *keywordRow = new QHBoxLayout;
    detailKeyword = new QLabel;
    detailKeyword->setObjectName("calculationDetailKeyword");
    detailKeyword->setProperty("mono", true);
    detailKeyword->setProperty("muted", true);
    detailKeyword->setTextInteractionFlags(Qt::TextSelectableByMouse);
    keywordRow->addWidget(detailKeyword, 1);
    detailState = new QLabel;
    detailState->setObjectName("calculationDetailState");
    keywordRow->addWidget(detailState);
    detailCard->body()->addLayout(keywordRow);
    detailHelp = new QLabel;
    detailHelp->setObjectName("calculationDetailHelp");
    detailHelp->setTextFormat(Qt::PlainText);
    detailHelp->setWordWrap(true);
    detailCard->body()->addWidget(detailHelp);
    auto *facts = new QFormLayout;
    facts->setHorizontalSpacing(14);
    facts->setVerticalSpacing(3);
    for (const auto &[name, label, objectName] :
         {std::tuple{"既定", &detailDefault, "calculationDetailDefault"},
          {"現在", &detailCurrent, "calculationDetailCurrent"},
          {"使う計算", &detailScope, "calculationDetailScope"}}) {
        *label = new QLabel;
        (*label)->setObjectName(objectName);
        (*label)->setWordWrap(true);
        auto *term = new QLabel(name);
        term->setProperty("muted", true);
        facts->addRow(term, *label);
    }
    detailCard->body()->addLayout(facts);
    snippet = new QPlainTextEdit;
    snippet->setObjectName("calculationSnippet");
    snippet->setReadOnly(true);
    snippet->setLineWrapMode(QPlainTextEdit::NoWrap);
    snippet->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    snippet->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    snippet->setProperty("log", true);
    highlightInput(snippet->document());
    detailCard->body()->addWidget(snippet);
    snippetNote = new QLabel;
    snippetNote->setObjectName("calculationSnippetNote");
    snippetNote->setProperty("muted", true);
    snippetNote->setWordWrap(true);
    detailCard->body()->addWidget(snippetNote);
    auto *detailActions = new QHBoxLayout;
    detailReset = new QPushButton("既定に戻す");
    detailReset->setObjectName("calculationDetailReset");
    detailReset->setIcon(lineIcon("reset"));
    showInInput = new QPushButton("入力で見る");
    showInInput->setObjectName("calculationShowInInput");
    showInInput->setIcon(lineIcon("document"));
    showInInput->setToolTip("「入力プレビュー」でこの項目の行を表示します。");
    detailActions->addWidget(detailReset);
    detailActions->addWidget(showInInput);
    detailActions->addStretch();
    detailCard->body()->addLayout(detailActions);
    connect(detailReset, &QPushButton::clicked, this,
            [this] { changeDetail(selectedDetail, QJsonValue::Undefined); });
    connect(showInInput, &QPushButton::clicked, this, [this] {
        if (const auto line = showInInput->property("line").toInt(); line >= 0)
            window->inputPreview->showLine(line);
    });
    detailCard->hide();
    railLayout->addWidget(detailCard);

    auto *changeCard = new SettingsCard("既定から変更した条件");
    changeCard->setObjectName("calculationChangeCard");
    changeCount = new QLabel;
    changeCount->setObjectName("calculationChangeCount");
    changeCount->setProperty("muted", true);
    changeCard->header()->addWidget(changeCount);
    changes = new QTreeWidget;
    changes->setObjectName("calculationChanges");
    changes->setColumnCount(3);
    changes->setHeaderHidden(true);
    changes->setRootIsDecorated(false);
    changes->setFrameShape(QFrame::NoFrame);
    changes->setSelectionMode(QAbstractItemView::NoSelection);
    changes->setFocusPolicy(Qt::NoFocus);
    changes->setTextElideMode(Qt::ElideRight);
    changes->header()->setStretchLastSection(false);
    changes->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    changes->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    changes->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    changes->setColumnWidth(2, 30);
    changeCard->body()->addWidget(changes, 1);
    connect(changes, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item) { showField(item->data(0, Qt::UserRole).toString()); });
    noChanges = new QLabel("なし（HF / STO-3G・一点エネルギー）");
    noChanges->setObjectName("calculationNoChanges");
    noChanges->setProperty("muted", true);
    changeCard->body()->addWidget(noChanges);
    railLayout->addWidget(changeCard); // As tall as its rows; the link stays at the bottom.
    railLayout->addStretch(1);
    auto *showPreview = linkButton("4 入力プレビューで確認", "calculationShowPreview");
    showPreview->setToolTip("生成するNWChem入力の全文を「4 入力プレビュー」で表示します。");
    auto *previewRow = new QHBoxLayout;
    previewRow->addStretch();
    previewRow->addWidget(showPreview);
    railLayout->addLayout(previewRow);
    connect(showPreview, &QPushButton::clicked, this,
            [this] { window->workspaceTabs->setCurrentWidget(window->inputPreview); });

    // Labels for search and the list of changes: form rows, plus the fields with their own rows.
    QStringList completions = labels.values();
    for (const auto &o : nwchemAdvancedOptions())
        completions << o.label + "（詳細）";
    auto *completer = new QCompleter(completions, search);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    search->setCompleter(completer);
    connect(completer, qOverload<const QString &>(&QCompleter::activated), this,
            &CalculationPanel::searchSettings);
    connect(search, &QLineEdit::returnPressed, this, [this] { searchSettings(search->text()); });
    // Working in the editor of a detail chooses it, so the card at the right follows the keyboard.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        for (auto it = details.cbegin(); now && it != details.cend(); ++it)
            if (it->editor == now || it->editor->isAncestorOf(now)) {
                if (selectedDetail != it.key())
                    selectDetail(it.key());
                return;
            }
    });
    nav->setCurrentKey("basic");
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
    if (key.startsWith("advanced:")) {
        showDetail(key.mid(9));
        return;
    }
    if (key == "advanced") { // The details as a whole: those changed.
        changeFilter->setCurrentIndex(changeFilter->findData("changed"));
        return;
    }
    if (key == "memory_mb") { // Set in "3 実行設定".
        window->workspaceTabs->setCurrentWidget(window->execution);
        if (auto *memory = window->execution->findChild<QWidget *>("execution_memory"))
            memory->setFocus(Qt::OtherFocusReason);
        return;
    }
    QWidget *target = key == "extra_input"   ? static_cast<QWidget *>(extraInput)
                      : key == "extra_tasks" ? static_cast<QWidget *>(extraTasks)
                                             : rows.value(key);
    if (!target)
        return;
    if (changedOnly() && !target->isVisibleTo(formWidget))
        changeFilter->setCurrentIndex(0);
    // Layouts of shown cards settle before the scroll position is taken.
    formWidget->layout()->activate();
    {
        QScopedValueRollback guard(scrolling, true);
        scroll->ensureWidgetVisible(target, 20, 60);
    }
    nav->setCurrentKey(sectionOf.value(key));
    QWidget *focus = target;
    if (auto *buttons = dynamic_cast<ChoiceButtons *>(target))
        for (int i = 0; buttons->button(i); ++i)
            if (buttons->button(i)->isChecked() || !i)
                focus = buttons->button(i);
    focus->setFocus(Qt::OtherFocusReason);
}
QString CalculationPanel::sectionTitle(const QString &key) const {
    for (const auto &section : sections)
        if (section.key == sectionOf.value(key))
            return section.title;
    return {};
}
QString CalculationPanel::fieldFor(const QString &message) const {
    // A detailed option named in brackets ("詳細設定「…」: …"), or the first of those held back.
    for (const auto &o : nwchemAdvancedOptions())
        if (message.contains("「" + o.label + "」") ||
            (message.contains("保留") && message.contains(": " + o.label)))
            return "advanced:" + o.key;
    // The wording of validateNwchem's messages names the setting; the first match wins.
    static const QVector<std::pair<const char *, const char *>> words{
        {"出力movecs", "advanced:vectors_output"},
        {"movecs", "advanced:vectors_input"},
        {"SCF DIIS", "advanced:scf_solver"},
        {"COSMOのideal", "advanced:cosmo_screen"},
        {"Betaの分子軌道", "advanced:cube_spin"},
        {"軌道番号", "advanced:cube_index"},
        {"HOMO", "advanced:cube_orbital"},
        {"格子", "advanced:cube_padding"},
        {"CUBEの範囲", "advanced:cube_padding"},
        {"CUBE", "advanced:cube_output"},
        {"MOLDEN", "advanced:molden_output"},
        {"詳細設定", "advanced"},
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
    const bool detailed = text.endsWith("（詳細）");
    const auto query = QString(text).remove("（詳細）").trimmed();
    if (query.isEmpty())
        return;
    if (!detailed)
        for (auto it = labels.begin(); it != labels.end(); ++it)
            if (it.value().contains(query, Qt::CaseInsensitive)) {
                showField(it.key());
                return;
            }
    // A detailed option by its name, then by its NWChem keyword or key.
    for (const auto &o : nwchemAdvancedOptions())
        if (o.label.contains(query, Qt::CaseInsensitive)) {
            showDetail(o.key);
            return;
        }
    for (const auto &o : nwchemAdvancedOptions())
        if ((o.keyword + ' ' + o.key).contains(query, Qt::CaseInsensitive)) {
            showDetail(o.key);
            return;
        }
    window->statusBar()->showMessage("「" + query + "」に一致する設定はありません。", 5000);
}
QFormLayout *CalculationPanel::group(const QString &key, const QString &title) {
    auto *card = new SettingsCard(title);
    card->setObjectName("calculationCard_" + key);
    formWidget->layout()->addWidget(card);
    auto *note = new QLabel;
    note->setProperty("muted", true);
    card->header()->insertWidget(1, note);
    auto *resetButton = new QPushButton("このセクションを既定に戻す");
    resetButton->setObjectName("calculationResetSection_" + key);
    resetButton->setProperty("link", true);
    resetButton->setCursor(Qt::PointingHandCursor);
    resetButton->hide();
    card->header()->addWidget(resetButton);
    connect(resetButton, &QPushButton::clicked, this, [this, key] {
        for (const auto &section : std::as_const(sections))
            if (section.key == key)
                reset(section.keys, {}, detailBoxes.value(key).groups);
    });
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);
    card->body()->addLayout(form);
    sections << Section{key, title, card, form, {}, note, resetButton};
    nav->addEntry(title, key);
    return form;
}
void CalculationPanel::addRow(QFormLayout *form, const QString &key, const QString &label, QWidget *field) {
    auto *text = new QLabel;
    text->setTextFormat(Qt::RichText);
    text->setBuddy(field);
    text->setProperty("rowLabel", label);
    fieldLabels[key] = text;
    labels[key] = label;
    rows[key] = field;
    rowKeys[field] = key;
    sectionOf[key] = sections.last().key;
    sections.last().keys << key;
    if (!fieldOrder.contains(key))
        fieldOrder << key;
    form->addRow(text, field);
}
void CalculationPanel::choice(QFormLayout *form, const QString &label, const QString &key) {
    auto *combo = new QComboBox;
    combo->setObjectName("nwchem_" + key);
    for (const auto &c : nwchemChoices(key))
        combo->addItem(c.label, c.value);
    fields[key] = combo;
    addRow(form, key, label, combo);
    connect(combo, &QComboBox::currentIndexChanged, this,
            [this, combo, key] { change(key, combo->currentData().toString()); });
}
void CalculationPanel::buttons(QFormLayout *form, const QString &label, const QString &key, bool chips,
                               const QStringList &texts) {
    auto *combo = new QComboBox;
    combo->setObjectName("nwchem_" + key);
    for (const auto &c : nwchemChoices(key))
        combo->addItem(c.label, c.value);
    fields[key] = combo;
    auto *group = new ChoiceButtons(combo, chips ? ChoiceButtons::Chips : ChoiceButtons::Segments, texts);
    group->setObjectName("calculationChoice_" + key);
    choices << group;
    addRow(form, key, label, group);
    rowKeys[combo] = key;
    connect(combo, &QComboBox::currentIndexChanged, this,
            [this, combo, key] { change(key, combo->currentData().toString()); });
}
void CalculationPanel::integer(QFormLayout *form, const QString &label, const QString &key, int minimum,
                               int maximum) {
    auto *spin = new QSpinBox;
    spin->setObjectName("nwchem_" + key);
    spin->setRange(minimum, maximum);
    spin->setKeyboardTracking(false);
    spin->setMaximumWidth(180);
    fields[key] = spin;
    addRow(form, key, label, spin);
    connect(spin, &QSpinBox::valueChanged, this, [this, key](int n) { change(key, n); });
}
void CalculationPanel::real(QFormLayout *form, const QString &label, const QString &key, double minimum,
                            double maximum, int decimals) {
    auto *spin = new QDoubleSpinBox;
    spin->setObjectName("nwchem_" + key);
    spin->setRange(minimum, maximum);
    spin->setDecimals(decimals);
    spin->setKeyboardTracking(false);
    spin->setMaximumWidth(180);
    fields[key] = spin;
    addRow(form, key, label, spin);
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this, key](double n) { change(key, n); });
}
void CalculationPanel::flag(QFormLayout *form, const QString &label, const QString &key) {
    auto *check = new QCheckBox(label);
    check->setObjectName("nwchem_" + key);
    fields[key] = check;
    // The check box carries the text; the label column keeps only the mark of a change.
    addRow(form, key, label, check);
    fieldLabels[key]->setProperty("rowLabel", QString());
    connect(check, &QCheckBox::toggled, this, [this, key](bool b) { change(key, b); });
}
void CalculationPanel::help(QFormLayout *form, const QString &text) {
    form->addRow(caption(text));
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
        combo->setItemText(0, "共通（" + nwchemBasisLabel(QJsonObject{{"basis", s["basis"]}}) + "）");
        const int index = combo->findData(overrides[symbol].toString());
        combo->setCurrentIndex(std::max(0, index));
    }
    // As tall as its rows, up to the maximum.
    int height = elementBases->horizontalHeader()->height() + 2 * elementBases->frameWidth();
    for (int row = 0; row < symbols.size(); ++row)
        height += elementBases->rowHeight(row);
    elementBases->setFixedHeight(std::min(height, 220));
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
        if (busy || !editable())
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
bool CalculationPanel::editable() const {
    return !window->hasCandidate() && !window->worker;
}
void CalculationPanel::change(const QString &key, const QJsonValue &value) {
    if (busy || !editable())
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
QStringList CalculationPanel::changedKeys(const QJsonObject &s) const {
    QStringList result;
    const auto defaults = nwchemDefaults();
    auto order = fieldOrder; // In the order of the form, then the fields with their own rows.
    for (const auto &key : labels.keys())
        if (!order.contains(key))
            order << key;
    for (const auto &key : order)
        if (defaults.contains(key) && s[key] != defaults[key])
            result << key;
    return result;
}
bool CalculationPanel::changedOnly() const {
    return changeFilter->currentData() == "changed";
}
QWidget *CalculationPanel::readOnlyRow(QLabel *value, QPushButton *link) {
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    layout->addWidget(value);
    layout->addWidget(link);
    layout->addStretch();
    return row;
}
void CalculationPanel::reset(const QStringList &keys, const QString &name, const QStringList &groups) {
    if (busy || !editable())
        return;
    const auto defaults = nwchemDefaults();
    const auto changed = changedKeys(nwchemSettings(window->document()));
    const auto a = nwchemAdvanced(window->document());
    QStringList targets, options;
    for (const auto &key : keys)
        if (changed.contains(key) && key != "extra_input" && key != "extra_tasks")
            targets << key;
    for (const auto &o : nwchemAdvancedOptions())
        if (a.contains(o.key) && (groups.contains("*") || groups.contains(o.group)))
            options << o.key;
    if (targets.isEmpty() && options.isEmpty())
        return;
    // Named after the one setting, else after the section of the first.
    QString title = name;
    if (title.isEmpty()) {
        const auto first = targets.isEmpty() ? details.value(options[0]) : Detail{};
        QString what = targets.isEmpty() ? first.option->label : labels.value(targets[0]);
        if (targets.size() + options.size() > 1)
            for (const auto &section : std::as_const(sections))
                if (section.key == (targets.isEmpty() ? first.section : sectionOf.value(targets[0])))
                    what = section.title;
        title = "「" + what + "」を既定に戻す";
    }
    window->safe([&] {
        window->edit(title, [&](Document &d) {
            auto s = d.data["nwchem"].toObject();
            for (const auto &key : targets)
                s[key] = defaults[key];
            d.data["nwchem"] = s;
            for (const auto &key : options)
                setNwchemAdvanced(d, key, QJsonValue::Undefined);
        });
    });
}
void CalculationPanel::showSection(const QString &key) {
    for (const auto &section : std::as_const(sections))
        if (section.key == key) {
            if (!section.card->isVisible() && changedOnly())
                changeFilter->setCurrentIndex(0);
            formWidget->layout()->activate();
            QScopedValueRollback guard(scrolling, true);
            scroll->verticalScrollBar()->setValue(section.card->y() - 16);
            nav->setCurrentKey(key);
            return;
        }
}
void CalculationPanel::followScroll() {
    if (scrolling)
        return;
    // The last card whose top has passed the top of the view; at the bottom, the last one shown.
    const auto *bar = scroll->verticalScrollBar();
    const bool bottom = bar->maximum() > 0 && bar->value() >= bar->maximum();
    QString current;
    for (const auto &section : std::as_const(sections))
        if (section.card->isVisibleTo(formWidget) &&
            (current.isEmpty() || bottom || section.card->y() <= bar->value() + 40))
            current = section.key;
    nav->setCurrentKey(current);
}
void CalculationPanel::filterRows() {
    const bool only = changedOnly();
    const auto &d = window->displayDocument();
    const auto changed = changedKeys(nwchemSettings(d));
    const auto a = nwchemAdvanced(d);
    // The details: what the calculation uses and what is changed, the rest on request.
    QSet<QString> changedSections;
    for (auto it = detailBoxes.begin(); it != detailBoxes.end(); ++it) {
        auto &box = it.value();
        int shown = 0, unused = 0;
        bool changedHere = false;
        QSet<QString> shownGroups;
        for (const auto &detail : std::as_const(details)) {
            if (detail.section != it.key())
                continue;
            const bool mine = a.contains(detail.option->key), used = nwchemAdvancedApplies(*detail.option, d);
            const bool show = only ? mine : used || mine || box.showUnused;
            changedHere |= mine;
            unused += !used && !mine;
            shown += show;
            if (show)
                shownGroups.insert(detail.option->group);
            detail.row->setVisible(show);
        }
        if (changedHere)
            changedSections.insert(it.key());
        for (const auto &group : std::as_const(box.groups))
            groupHeadings[group]->setVisible(shownGroups.contains(group));
        box.disclosure->setVisible(!only || changedHere);
        box.body->setVisible(box.toggle->isChecked() || (only && changedHere));
        box.empty->setVisible(!shown);
        box.unused->setVisible(!only && unused);
        box.unused->setText(box.showUnused ? QString("この計算で使わない項目を隠す")
                                           : QString("この計算で使わない項目も表示（%1）").arg(unused));
        if (box.outputs)
            box.outputs->setVisible(!only);
    }
    for (const auto &section : std::as_const(sections)) {
        bool any = changedSections.contains(section.key);
        for (int row = 0; row < section.form->rowCount(); ++row) {
            QWidget *w = nullptr;
            for (const auto role : {QFormLayout::FieldRole, QFormLayout::SpanningRole})
                if (auto *item = section.form->itemAt(row, role); item && item->widget())
                    w = item->widget();
            const bool mine = changed.contains(rowKeys.value(w));
            any |= mine;
            section.form->setRowVisible(row, !only || mine);
        }
        section.card->setVisible(!only || any);
    }
}
void CalculationPanel::refreshChanges(const QJsonObject &s) {
    const auto &d = window->displayDocument();
    const auto changed = changedKeys(s);
    const ChromeColors colors(palette());
    for (auto it = fieldLabels.begin(); it != fieldLabels.end(); ++it) {
        const auto text =
            marked(it.value()->property("rowLabel").toString(), changed.contains(it.key()), colors.accent);
        if (it.value()->text() != text)
            it.value()->setText(text);
        it.value()->setToolTip(changed.contains(it.key()) ? "既定値から変更しています。" : QString());
    }
    const auto a = nwchemAdvanced(d);
    for (const auto &section : std::as_const(sections)) {
        int count = 0;
        for (const auto &key : section.keys)
            count += changed.contains(key);
        for (const auto &detail : std::as_const(details))
            count += detail.section == section.key && a.contains(detail.option->key);
        const auto unused = nwchemSectionUnused(d, section.key);
        section.note->setText(unused.isEmpty()       ? QString()
                              : unused == "使わない" ? QString("この計算では使いません")
                                                     : "この計算では使いません（" + unused + "）");
        if (auto *entry = nav->entry(section.key)) {
            entry->setData(NavList::BadgeRole, count ? QString("変更 %1").arg(count) : QString());
            entry->setData(NavList::NoteRole, unused);
            entry->setData(NavList::MutedRole, !unused.isEmpty());
            entry->setToolTip(section.note->text());
        }
        section.reset->setVisible(count && section.key != "extra");
    }
    nav->viewport()->update();
    changes->clear();
    for (const auto &key : changed) {
        auto *item = new QTreeWidgetItem(changes, {labels.value(key), valueText(key, s[key])});
        item->setData(0, Qt::UserRole, key);
        item->setToolTip(0, labels.value(key));
        item->setToolTip(1, valueText(key, s[key]));
        item->setForeground(0, colors.muted);
        if (key == "extra_input" || key == "extra_tasks")
            continue; // Protected like the executable: changed in its own card only.
        auto *b = new QToolButton;
        b->setObjectName("calculationReset_" + key);
        b->setIcon(lineIcon("reset"));
        b->setProperty("flat", true);
        b->setToolTip("「" + labels.value(key) + "」を既定に戻す");
        b->setEnabled(editable());
        changes->setItemWidget(item, 2, b);
        // Later: the click may not delete its own button while it is still being handled.
        connect(b, &QToolButton::clicked, this,
                [this, key] { QTimer::singleShot(0, this, [this, key] { reset({key}); }); });
    }
    // The detailed options after the fields, in the order of the sections; held back ones muted.
    int total = changed.size();
    for (const auto &section : std::as_const(sections))
        for (const auto &o : nwchemAdvancedOptions()) {
            if (!a.contains(o.key) || details.value(o.key).section != section.key)
                continue;
            ++total;
            const bool used = nwchemAdvancedApplies(o, d);
            const auto value = detailValueText(o, a[o.key]) + (used ? QString() : "（保留）");
            auto *item = new QTreeWidgetItem(changes, {o.label, value});
            item->setData(0, Qt::UserRole, "advanced:" + o.key);
            item->setToolTip(0, o.label + "（" + section.title + "の詳細設定）");
            item->setToolTip(1, value);
            item->setForeground(0, colors.muted);
            if (!used)
                item->setForeground(1, colors.muted);
            auto *b = new QToolButton;
            b->setObjectName("calculationReset_advanced_" + o.key);
            b->setIcon(lineIcon("reset"));
            b->setProperty("flat", true);
            b->setToolTip("「" + o.label + "」を既定に戻す");
            b->setEnabled(editable());
            changes->setItemWidget(item, 2, b);
            connect(b, &QToolButton::clicked, this, [this, key = o.key] {
                QTimer::singleShot(0, this, [this, key] { changeDetail(key, QJsonValue::Undefined); });
            });
        }
    changeCount->setText(total ? QString("%1 項目").arg(total) : QString());
    // Its rows (up to 12, then it scrolls), not the height of the rail.
    const int row = std::max(changes->sizeHintForRow(0), 26);
    changes->setFixedHeight(std::min(changes->topLevelItemCount(), 12) * row + 2 * changes->frameWidth());
    changes->setVisible(total);
    noChanges->setVisible(!total);
    changeFilters->button(1)->setText(QString("変更済み %1").arg(total));
    filterRows();
}
void CalculationPanel::reloadPresets() {
    presetMenu->clear();
    const auto presets = nwchemPresets();
    for (int i = 0; i < presets.size(); ++i) {
        auto *action = presetMenu->addAction(presets[i].name);
        action->setObjectName(QString("calculationPreset%1").arg(i));
        action->setData(presets[i].settings);
        connect(
            action, &QAction::triggered, this,
            [this, name = presets[i].name, settings = presets[i].settings] { applyPreset(name, settings); });
    }
    if (!userPresets.isEmpty())
        presetMenu->addSection("登録したプリセット");
    for (int i = 0; i < userPresets.size(); ++i) {
        const auto object = userPresets[i].toObject();
        auto *action = presetMenu->addAction(object["name"].toString());
        action->setObjectName(QString("calculationUserPreset%1").arg(i));
        action->setData(object["settings"].toObject());
        connect(action, &QAction::triggered, this,
                [this, name = object["name"].toString(), settings = object["settings"].toObject()] {
                    applyPreset(name, settings);
                });
    }
    presetMenu->addSeparator();
    auto *remove = presetMenu->addMenu("登録を解除");
    remove->setObjectName("calculationRemovePresets");
    remove->setEnabled(!userPresets.isEmpty());
    for (int i = 0; i < userPresets.size(); ++i) {
        auto *action = remove->addAction("「" + userPresets[i].toObject()["name"].toString() + "」を削除");
        action->setObjectName(QString("calculationRemovePreset%1").arg(i));
        connect(action, &QAction::triggered, this, [this, i] {
            if (i >= userPresets.size())
                return;
            userPresets.removeAt(i);
            window->settings.setValue("nwchem/presets",
                                      QJsonDocument(userPresets).toJson(QJsonDocument::Compact));
            QTimer::singleShot(0, this, &CalculationPanel::reloadPresets); // Not inside its own menu.
        });
    }
}
QString CalculationPanel::describePreset(const QJsonObject &selected) const {
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
    return changes.isEmpty()
               ? QString("現在の条件と同じです。")
               : QString("適用すると %1 "
                         "項目が変わります（分子・電荷・多重度・計算名・メモリ・単位は保持）:\n・%2")
                     .arg(changes.size())
                     .arg(changes.mid(0, 12).join("\n・") + (changes.size() > 12 ? "\n・…" : ""));
}
void CalculationPanel::applyPreset(const QString &name, const QJsonObject &preset) {
    if (busy || !editable())
        return;
    const auto before = nwchemSettings(window->document());
    window->safe(
        [&] { window->edit("NWChemプリセットを適用", [&](Document &d) { applyNwchemPreset(d, preset); }); });
    if (nwchemSettings(window->document()) != before)
        window->statusBar()->showMessage("プリセット「" + name + "」を適用しました。「元に戻す」で戻せます。",
                                         8000);
}
void CalculationPanel::storePreset() {
    if (!editable())
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
    window->statusBar()->showMessage("プリセット「" + name + "」を登録しました。", 5000);
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
    for (auto *group : std::as_const(choices))
        group->sync();
    refreshElementBases(s);
    refreshConstraints(s);
    refreshDetails(d);
    memoryState->setText(QString("%1 MB / MPIプロセス").arg(s["memory_mb"].toInt()));
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
    const bool canEdit = editable();
    formWidget->setEnabled(canEdit);
    for (auto *b : {presetButton, storeButton})
        b->setEnabled(canEdit);
    resetAll->setEnabled(canEdit);
    message->setText(window->hasCandidate()
                         ? "未確定の分子候補を表示しています。確定・取消後に条件を編集できます。"
                     : window->worker ? "分子の整形中です。終了後に条件を編集できます。"
                                      : QString());
    banner->setVisible(!message->text().isEmpty());
    const auto check = validateNwchem(d);
    const QString referenceName = method == "dft" && check.reference != "rohf"
                                      ? (check.reference == "rhf" ? "RKS" : "UKS")
                                      : check.reference.toUpper();
    reference->setText(s["reference"] == "auto" && !referenceName.isEmpty()
                           ? "→ " + referenceName + (check.reference == "rhf" ? "（閉殻）" : "（開殻）")
                           : QString());
    formalCharge->setText(QString("原子の形式電荷の合計（%1）を適用").arg(d.formalCharge()));
    summaryCard->update(d);
    // Whether the input can be generated, and why not.
    const bool ok = check.errors.isEmpty();
    validationCard->setState(
        ok ? StatusCard::Ok : StatusCard::Error,
        ok ? (check.warnings.isEmpty()
                  ? QString("入力を生成できます")
                  : QString("入力を生成できます（確認 %1 件）").arg(check.warnings.size()))
           : QString("入力を生成できません（エラー %1 件）").arg(check.errors.size()));
    validation->setMessages(check.errors, check.warnings, {}, fixes(check.errors));
    validation->setVisible(validation->count());
    passed->setVisible(ok); // As the checks of "4 入力プレビュー" say it, above the warnings.
    if (auto *outputs = detailBoxes.value("orbitals").outputs) {
        const auto files = nwchemOutputFiles(d, check);
        outputs->setText(files.isEmpty() ? QString("CUBE・MOLDENの出力はオフです。")
                                         : "計算後に出力するファイル:\n" + files.join('\n'));
    }
    refreshChanges(s);
    if (!selectedDetail.isEmpty())
        describeDetail();
}
void CalculationPanel::addDetails(const QString &key, const QStringList &groups, bool open) {
    const Section *section = nullptr;
    for (const auto &each : std::as_const(sections))
        if (each.key == key)
            section = &each;
    DetailBox box;
    box.groups = groups;
    box.disclosure = new Disclosure("詳細設定", open);
    box.toggle = box.disclosure->button();
    box.toggle->setObjectName("calculationDetails_" + key);
    box.toggle->setToolTip("NWChemの各ブロックへ直接指定する項目です。空欄の項目は既定値を使います。");
    section->card->body()->addWidget(box.disclosure, 0, Qt::AlignLeft);
    box.body = new QWidget;
    box.body->setObjectName("calculationDetailBody_" + key);
    auto *layout = new QVBoxLayout(box.body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(1);
    section->card->body()->addWidget(box.body);
    for (const auto &group : groups) {
        QString title = group;
        for (const auto &g : nwchemAdvancedGroups())
            if (g.value == group)
                title = g.value == "visualization" ? QString("CUBE / MOLDEN 出力") : g.label;
        auto *heading = new QLabel(title);
        heading->setObjectName("calculationDetailGroup_" + group);
        heading->setProperty("detailGroup", true);
        layout->addWidget(heading);
        groupHeadings[group] = heading;
        for (const auto &o : nwchemAdvancedOptions()) {
            if (o.group != group)
                continue;
            Detail detail;
            detail.option = &o;
            detail.section = key;
            // Name and keyword, editor, state and reset, in columns of the same width in every row.
            detail.row = new QWidget;
            detail.row->setObjectName("calculationDetailRow_" + o.key);
            detail.row->setProperty("detailRow", true);
            detail.row->setProperty("detailKey", o.key);
            detail.row->setAttribute(Qt::WA_StyledBackground);
            detail.row->setAttribute(Qt::WA_Hover);
            detail.row->installEventFilter(this);
            auto *row = new QHBoxLayout(detail.row);
            row->setContentsMargins(8, 4, 4, 4);
            row->setSpacing(10);
            auto *names = new QVBoxLayout;
            names->setSpacing(0);
            detail.name = new QLabel;
            detail.name->setTextFormat(Qt::RichText);
            detail.name->setWordWrap(true);
            names->addWidget(detail.name);
            auto *keyword = new QLabel(o.keyword);
            keyword->setProperty("caption", true);
            keyword->setProperty("mono", true);
            names->addWidget(keyword);
            row->addLayout(names, 1);
            const auto tip = o.help + "\nNWChem: " + o.keyword + "\n未指定: " + o.defaultText;
            detail.row->setToolTip(tip);
            if (o.kind == AdvancedKind::Flag) {
                auto *check = new QCheckBox("オン");
                detail.editor = check;
                connect(check, &QCheckBox::toggled, this, [this, key = o.key](bool value) {
                    changeDetail(key, value ? QJsonValue(true) : QJsonValue::Undefined);
                });
            } else if (o.kind == AdvancedKind::Choice) {
                auto *choices = new QComboBox;
                choices->addItem(QString(), "");
                for (const auto &c : o.choices)
                    choices->addItem(c.label, c.value);
                detail.editor = choices;
                connect(choices, &QComboBox::currentIndexChanged, this, [this, choices, key = o.key] {
                    const auto value = choices->currentData().toString();
                    changeDetail(key, value.isEmpty() ? QJsonValue::Undefined : QJsonValue(value));
                });
            } else {
                auto *edit = new QLineEdit;
                edit->setClearButtonEnabled(true);
                detail.editor = edit;
                connect(edit, &QLineEdit::editingFinished, this, [this, edit, &o] {
                    if (!busy && edit->isModified())
                        changeDetail(o.key, parseNwchemAdvanced(o, edit->text()));
                    edit->setModified(false);
                });
            }
            detail.editor->setObjectName("advanced_" + o.key);
            detail.editor->setToolTip(tip);
            detail.editor->setFixedWidth(230);
            row->addWidget(detail.editor);
            detail.state = new QLabel;
            detail.state->setObjectName("calculationDetailState_" + o.key);
            detail.state->setAlignment(Qt::AlignCenter);
            detail.state->setFixedWidth(52);
            row->addWidget(detail.state);
            detail.reset = new QPushButton;
            detail.reset->setObjectName("advancedResetField_" + o.key);
            detail.reset->setIcon(lineIcon("reset"));
            detail.reset->setProperty("flat", true);
            detail.reset->setToolTip("この項目を既定に戻します（明示指定を解除）。「元に戻す」で戻せます。");
            auto policy = detail.reset->sizePolicy();
            policy.setRetainSizeWhenHidden(true);
            detail.reset->setSizePolicy(policy);
            row->addWidget(detail.reset);
            connect(detail.reset, &QPushButton::clicked, this, [this, key = o.key] {
                // Later: the click may not hide its own button while it is still being handled.
                QTimer::singleShot(0, this, [this, key] { changeDetail(key, QJsonValue::Undefined); });
            });
            layout->addWidget(detail.row);
            details[o.key] = detail;
        }
    }
    box.empty = caption("この計算で使う詳細設定はありません。");
    box.empty->setContentsMargins(8, 2, 0, 2);
    layout->addWidget(box.empty);
    box.unused = new QPushButton;
    box.unused->setObjectName("calculationDetailUnused_" + key);
    box.unused->setProperty("link", true);
    box.unused->setCursor(Qt::PointingHandCursor);
    box.unused->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    box.unused->setToolTip("今の計算では入力に入らない項目です。値は保存され、対応する計算に切り替えると"
                           "使います（保留）。");
    layout->addWidget(box.unused, 0, Qt::AlignLeft);
    if (groups.contains("visualization")) {
        box.outputs = new QLabel;
        box.outputs->setObjectName("calculationOutputFiles");
        box.outputs->setProperty("muted", true);
        box.outputs->setTextFormat(Qt::PlainText);
        box.outputs->setWordWrap(true);
        box.outputs->setTextInteractionFlags(Qt::TextSelectableByMouse);
        box.outputs->setContentsMargins(8, 6, 0, 0);
        box.outputs->setToolTip("保存先は「永続ファイルのディレクトリ」に従います。この出力設定はプリセットの"
                                "切替後も保持します。");
        layout->addWidget(box.outputs);
    }
    box.body->setVisible(open);
    connect(box.toggle, &QToolButton::toggled, this, [this] { filterRows(); });
    connect(box.unused, &QPushButton::clicked, this, [this, key] {
        detailBoxes[key].showUnused = !detailBoxes[key].showUnused;
        filterRows();
    });
    detailBoxes[key] = box;
}
void CalculationPanel::openDetails(const QString &key, bool unused) {
    if (!detailBoxes.contains(key))
        return;
    auto &box = detailBoxes[key];
    box.showUnused |= unused;
    if (box.disclosure->isOpen())
        filterRows();
    else
        box.disclosure->setOpen(true); // filterRows() follows.
    // The cards learn their new heights before the position of a row is taken.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    formWidget->layout()->activate();
}
void CalculationPanel::showDetail(const QString &key) {
    if (!details.contains(key))
        return;
    const auto detail = details.value(key);
    const auto &d = window->displayDocument();
    window->workspaceTabs->setCurrentWidget(this);
    if (changedOnly() && !nwchemAdvanced(d).contains(key))
        changeFilter->setCurrentIndex(0);
    openDetails(detail.section, !nwchemAdvancedApplies(*detail.option, d));
    {
        QScopedValueRollback guard(scrolling, true);
        scroll->ensureWidgetVisible(detail.row, 20, 80);
    }
    nav->setCurrentKey(detail.section);
    selectDetail(key);
    detail.editor->setFocus(Qt::OtherFocusReason);
}
void CalculationPanel::showDetailGroup(const QString &group) {
    for (auto it = detailBoxes.cbegin(); it != detailBoxes.cend(); ++it) {
        if (!it->groups.contains(group))
            continue;
        const auto &d = window->displayDocument();
        window->workspaceTabs->setCurrentWidget(this);
        if (changedOnly())
            changeFilter->setCurrentIndex(0);
        bool used = false; // A group the calculation does not use is shown all the same.
        for (const auto &detail : std::as_const(details))
            used |= detail.option->group == group && nwchemAdvancedApplies(*detail.option, d);
        const auto section = it.key();
        openDetails(section, !used);
        {
            QScopedValueRollback guard(scrolling, true);
            scroll->verticalScrollBar()->setValue(groupHeadings[group]->mapTo(formWidget, QPoint()).y() - 16);
        }
        nav->setCurrentKey(section);
        return;
    }
}
void CalculationPanel::changeDetail(const QString &key, const QJsonValue &value) {
    if (busy || !editable() || nwchemAdvanced(window->document()).value(key) == value)
        return;
    window->safe([&] {
        window->edit("NWChem詳細設定を変更", [&](Document &d) { setNwchemAdvanced(d, key, value); });
    });
}
QString CalculationPanel::detailValueText(const AdvancedOption &o, const QJsonValue &value) const {
    if (o.kind == AdvancedKind::Flag)
        return value.toBool() ? "オン" : "オフ";
    for (const auto &c : o.choices)
        if (c.value == value.toString())
            return c.label;
    return nwchemAdvancedText(value);
}
void CalculationPanel::refreshDetails(const Document &d) {
    const auto a = nwchemAdvanced(d);
    const bool canEdit = editable();
    const ChromeColors colors(palette());
    QMap<QString, int> totals, changedCounts;
    for (auto &detail : details) {
        const auto &o = *detail.option;
        const auto v = a.value(o.key);
        const bool used = nwchemAdvancedApplies(o, d), changed = !v.isUndefined();
        const auto blank = nwchemAdvancedDefault(o, d);
        {
            const QSignalBlocker blocker(detail.editor);
            if (auto *line = qobject_cast<QLineEdit *>(detail.editor)) {
                const auto value = changed ? nwchemAdvancedText(v) : QString();
                if (line->text() != value)
                    line->setText(value);
                line->setPlaceholderText(blank);
            } else if (auto *check = qobject_cast<QCheckBox *>(detail.editor)) {
                check->setChecked(v.toBool());
                check->setText(v.toBool() ? "オン" : "オフ");
            } else if (auto *combo = qobject_cast<QComboBox *>(detail.editor)) {
                while (combo->count() > o.choices.size() + 1)
                    combo->removeItem(combo->count() - 1);
                combo->setItemText(0, "既定（" + blank + "）");
                int index = changed ? combo->findData(v.toString()) : 0;
                if (index < 0) {
                    combo->addItem("未対応: " + nwchemAdvancedText(v), v.toString());
                    index = combo->count() - 1;
                }
                combo->setCurrentIndex(index);
            }
        }
        detail.editor->setEnabled(canEdit && used);
        detail.reset->setVisible(changed);
        detail.reset->setEnabled(canEdit && changed);
        const auto text = marked(o.label, changed, colors.accent);
        if (detail.name->text() != text)
            detail.name->setText(text);
        if (detail.name->property("muted").toBool() != !used) {
            detail.name->setProperty("muted", !used);
            repolish(detail.name);
        }
        const auto error = nwchemAdvancedError(o, v);
        const QString kind = !changed           ? QString("none")
                             : !error.isEmpty() ? "error"
                             : used             ? "applied"
                                                : "pending";
        detail.state->setText(kind == "error"     ? "エラー"
                              : kind == "applied" ? "適用"
                              : kind == "pending" ? "保留"
                                                  : QString());
        detail.state->setToolTip(!error.isEmpty()    ? "エラー: " + error
                                 : kind == "applied" ? QString("入力へ反映します。")
                                 : kind == "pending" ? QString("保留: 今の計算では使いません。")
                                                     : QString());
        if (detail.state->property("pill").toString() != kind) {
            detail.state->setProperty("pill", kind);
            repolish(detail.state);
        }
        ++totals[detail.section];
        changedCounts[detail.section] += changed;
    }
    for (auto it = detailBoxes.begin(); it != detailBoxes.end(); ++it) {
        const int changed = changedCounts.value(it.key());
        it->disclosure->setDetail(QString("%1 項目%2")
                                      .arg(totals.value(it.key()))
                                      .arg(changed ? QString(" · 変更 %1").arg(changed) : QString()));
    }
}
void CalculationPanel::selectDetail(const QString &key) {
    for (const auto &name : {selectedDetail, key})
        if (auto *row = details.value(name).row) {
            row->setProperty("selected", name == key);
            repolish(row);
        }
    selectedDetail = details.contains(key) ? key : QString();
    detailCard->setVisible(!selectedDetail.isEmpty());
    if (!selectedDetail.isEmpty())
        describeDetail();
}
void CalculationPanel::describeDetail() {
    if (!details.contains(selectedDetail))
        return;
    const auto &detail = details[selectedDetail];
    const auto &o = *detail.option;
    const auto &d = window->displayDocument();
    const auto a = nwchemAdvanced(d);
    detailTitle->setText(o.label);
    detailKeyword->setText(nwchemAdvancedBlock(o, d) == o.keyword || o.group == "files"
                               ? o.keyword
                               : nwchemAdvancedBlock(o, d) + " › " + o.keyword);
    detailState->setText(detail.state->text());
    detailState->setProperty("pill", detail.state->property("pill"));
    detailState->setVisible(!detail.state->text().isEmpty());
    repolish(detailState);
    const auto error = nwchemAdvancedError(o, a.value(o.key));
    detailHelp->setText(o.help + (error.isEmpty() ? QString() : "\nエラー: " + error));
    detailDefault->setText(nwchemAdvancedDefault(o, d));
    detailCurrent->setText(a.contains(o.key) ? detailValueText(o, a[o.key]) : QString("空欄（既定）"));
    detailScope->setText(nwchemAdvancedScope(o) +
                         (nwchemAdvancedApplies(o, d) ? QString() : "（今の計算では使いません）"));
    showSnippet(o);
    snippetNote->setVisible(!snippetNote->text().isEmpty());
    detailReset->setEnabled(a.contains(o.key) && editable());
    showInInput->setEnabled(showInInput->property("line").toInt() >= 0);
}
void CalculationPanel::showSnippet(const AdvancedOption &o) {
    // The block of the generated input that carries the option, its own lines marked.
    const auto &d = window->displayDocument();
    snippet->clear();
    snippet->setExtraSelections({});
    showInInput->setProperty("line", -1);
    QString input;
    try {
        input = nwchemInput(d);
    } catch (const std::exception &) {
    }
    const bool active = nwchemAdvancedApplies(o, d), changed = nwchemAdvanced(d).contains(o.key);
    if (input.isEmpty()) {
        snippet->hide();
        snippetNote->setText("入力を生成できません。上の検査結果を確認してください。");
        return;
    }
    if (!active) {
        snippet->hide();
        snippetNote->setText(changed ? "今の計算では使わないため、入力に入りません（保留）。"
                                     : "今の計算では使いません。");
        return;
    }
    const auto lines = input.split('\n');
    const auto outline = nwchemInputOutline(d, input);
    const auto name = nwchemAdvancedBlock(o, d);
    const auto last = o.keyword.section(' ', -1).toLower();
    const NwchemInputBlock *block = nullptr;
    for (const auto &b : outline.blocks) {
        bool contains = b.keyword == name;
        for (int i = b.line + 1; !contains && i < b.line + b.lines; ++i)
            contains = lines[i].trimmed().startsWith(name + ' ') || lines[i].trimmed() == name;
        // Of several blocks with the keyword, the one that the details write into.
        const auto marked = [&](const NwchemInputBlock &x) {
            for (int i = x.line; i < x.line + x.lines; ++i)
                if (outline.advanced.contains(i))
                    return true;
            return false;
        };
        if (contains && (!block || (!marked(*block) && marked(b))))
            block = &b;
    }
    if (!block) {
        snippet->hide();
        snippetNote->setText(changed ? "この値を書き込む行はありません。"
                                     : "空欄のため指定しません（既定値を使います）。");
        return;
    }
    QList<int> own;
    for (int i = block->line; i < block->line + block->lines; ++i)
        if (outline.advanced.contains(i) && lines[i].toLower().contains(last))
            own << i;
    // The header, the option's lines with a neighbor each, and the end; "…" for what is left out.
    QList<int> shown{block->line};
    const int end = block->line + block->lines - 1;
    const auto add = [&](int i) {
        if (i > block->line && i <= end && !shown.contains(i))
            shown << i;
    };
    if (own.isEmpty())
        for (int i = block->line + 1; i <= std::min(end, block->line + 5); ++i)
            add(i);
    for (const int i : std::as_const(own))
        for (const int j : {i - 1, i, i + 1})
            add(j);
    add(end);
    std::sort(shown.begin(), shown.end());
    QStringList text;
    QList<int> marks;
    for (int k = 0; k < shown.size(); ++k) {
        if (k && shown[k] > shown[k - 1] + 1)
            text << "  …";
        if (own.contains(shown[k]))
            marks << text.size();
        text << lines[shown[k]];
    }
    snippet->setPlainText(text.join('\n'));
    QList<QTextEdit::ExtraSelection> selections;
    const ChromeColors colors(palette());
    for (const int mark : std::as_const(marks)) {
        QTextEdit::ExtraSelection selection;
        selection.cursor = QTextCursor(snippet->document()->findBlockByNumber(mark));
        selection.format.setBackground(colors.tint);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selections << selection;
    }
    snippet->setExtraSelections(selections);
    // As tall as its lines, with room for the scroll bar of long lines.
    snippet->setFixedHeight(snippet->fontMetrics().lineSpacing() * int(text.size()) +
                            int(2 * snippet->document()->documentMargin()) + 2 * snippet->frameWidth() +
                            style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 2);
    snippet->show();
    showInInput->setProperty("line", own.isEmpty() ? block->line : own.first());
    snippetNote->setText(changed ? QString() : "空欄のため、ここには指定しません（既定値を使います）。");
}
bool CalculationPanel::eventFilter(QObject *watched, QEvent *event) {
    // A click anywhere in the row of a detail chooses it.
    if (event->type() == QEvent::MouseButtonPress)
        if (const auto key = watched->property("detailKey").toString(); !key.isEmpty())
            selectDetail(key);
    return QWidget::eventFilter(watched, event);
}
void CalculationPanel::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    // Narrow windows give the form the room of the section list.
    navColumn->setVisible(width() >= 1040);
}
void CalculationPanel::applyChromeStyle() {
    const auto pal = static_cast<QWidget *>(window)->palette();
    setStyleSheet(chromeRules(settingsRules() + R"(
        #calculationTab, #calculationRailScroll, #calculationRail { background: @window; }
        #calculationNav { background: @base; border: none; border-right: 1px solid @line; }
        QTreeWidget#calculationChanges, QListWidget#calculationValidation { background: transparent; }
        QLabel[detailGroup="true"] { font-weight: bold; padding: 10px 0 3px 8px; }
        QWidget[detailRow="true"] { border-radius: 6px; }
        QWidget[detailRow="true"]:hover { background: @hover; }
        QWidget[detailRow="true"][selected="true"] { background: @tint; }
        QPlainTextEdit#calculationSnippet { background: @window; }
        QLabel[pill] { border-radius: 8px; padding: 1px 7px; font-weight: bold; font-size: 11px; }
        QLabel[pill="applied"] { background: @tint; color: @accentText; }
        QLabel[pill="pending"] { background: @candidate; color: @candidateText; }
        QLabel[pill="error"] { background: @badTint; color: @error; }
    )",
                              pal));
    if (!busy)
        refresh(); // The marks are colored in their markup.
}
} // namespace mf
