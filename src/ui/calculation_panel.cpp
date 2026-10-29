// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/calculation_panel.h"
#include "model/geometry.h"
#include "model/i18n.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_output.h"
#include "ui/diagnostics_list.h"
#include "ui/editor_chrome.h"
#include "ui/input_highlighter.h"
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
    presetButton = new QPushButton(tr("プリセット"));
    presetButton->setObjectName("calculationPresets");
    presetButton->setIcon(lineIcon("preset"));
    presetButton->setToolTip(
        tr("組込み・登録したプリセットを適用します。項目のツールチップに変わる条件を表示します。"));
    presetMenu = new QMenu(presetButton);
    presetMenu->setObjectName("calculationPresetMenu");
    presetMenu->setToolTipsVisible(true);
    presetButton->setMenu(presetMenu);
    tools->addWidget(presetButton);
    storeButton = new QPushButton(tr("現在の条件を登録…"));
    storeButton->setObjectName("calculationStorePreset");
    tools->addWidget(storeButton);
    connect(storeButton, &QPushButton::clicked, this, &CalculationPanel::storePreset);
    userPresets = QJsonDocument::fromJson(window->preference("nwchem/presets").toByteArray()).array();
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
    search->setPlaceholderText(tr("設定を検索（項目名・NWChemキーワード）"));
    search->setClearButtonEnabled(true);
    search->addAction(lineIcon("search"), QLineEdit::LeadingPosition);
    search->setFixedWidth(280);
    tools->addWidget(search);
    // All rows or the changed ones, and the reset of everything.
    changeFilter = new QComboBox;
    changeFilter->setObjectName("calculationChangeFilter");
    changeFilter->addItem(tr("すべての項目"), "all");
    changeFilter->addItem(tr("既定から変更した項目のみ"), "changed");
    changeFilters = new ChoiceButtons(changeFilter, ChoiceButtons::Segments, {tr("すべて"), tr("変更済み")});
    changeFilters->setObjectName("calculationChangeFilters");
    tools->addWidget(changeFilters);
    connect(changeFilter, &QComboBox::currentIndexChanged, this, [this] {
        filterRows();
        followScroll();
    });
    resetAll = new QPushButton(tr("すべて既定へ"));
    resetAll->setObjectName("calculationResetAll");
    resetAll->setIcon(lineIcon("reset"));
    resetAll->setToolTip(
        tr("この画面の条件と詳細設定を既定に戻します（計算名・タイトル・座標の単位・追加入力と、"
           "電荷・多重度は保持）。「元に戻す」で戻せます。"));
    tools->addWidget(resetAll);
    connect(resetAll, &QPushButton::clicked, this, [this] {
        QStringList keys;
        for (const auto &key : changedKeys(nwchemSettings(window->document())))
            if (!QStringList{"start", "title", "geometry_units"}.contains(key))
                keys << key;
        reset(keys, tr("計算条件をすべて既定に戻す"), {"*"});
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
    navColumn->setFixedWidth(english() ? 264 : 224); // English names and notes are wider.
    auto *navLayout = new QVBoxLayout(navColumn);
    navLayout->setContentsMargins(6, 10, 6, 10);
    navLayout->setSpacing(8);
    nav = new NavList;
    nav->setObjectName("calculationSections");
    navLayout->addWidget(nav, 1);
    auto *legend =
        caption(tr("「変更 n」は既定から変えた項目の数、薄い字はこの計算で使わないセクションです。"));
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

    nav->addHeading(tr("主な条件"));
    auto *basic = group("basic", tr("基本設定"));
    buttons(basic, tr("計算目的"), "task", true,
            {tr("一点エネルギー"), tr("勾配"), tr("構造最適化"), tr("振動数"), tr("最適化 → 振動数"),
             tr("遷移状態探索"), tr("遷移状態 → 振動数"), "IRC", tr("座標スキャン")});
    buttons(basic, tr("電子状態の手法"), "method", false, {"HF", "DFT", "MP2", "CCSD", "CCSD(T)"});
    choice(basic, tr("DFT汎関数"), "functional");
    auto *customXc = new QLineEdit;
    customXc->setObjectName("nwchem_xc_custom");
    customXc->setPlaceholderText(tr("例: xpbe96 0.75 cpbe96 1.0 hfexch 0.25"));
    customXc->setToolTip(tr("DFTブロックの xc 行です。汎関数で「任意のxc指定」を選んだときに使います。"));
    fields["xc_custom"] = customXc;
    addRow(basic, "xc_custom", tr("任意のxc"), customXc);
    connect(customXc, &QLineEdit::editingFinished, this,
            [this, customXc] { change("xc_custom", customXc->text().simplified()); });
    choice(basic, tr("DFT分散補正"), "dispersion");
    choice(basic, tr("基底関数"), "basis");
    choice(basic, tr("d・f関数の形式"), "basis_functions");
    elementBases = new QTableWidget(0, 2);
    elementBases->setObjectName("nwchem_basis_overrides");
    elementBases->setHorizontalHeaderLabels({tr("元素"), tr("基底")});
    elementBases->verticalHeader()->hide();
    elementBases->horizontalHeader()->setStretchLastSection(true);
    elementBases->setSelectionMode(QAbstractItemView::NoSelection);
    elementBases->setToolTip(
        tr("元素ごとに別の基底を指定します（例: 金属だけdef2-TZVP）。「共通」は上の基底関数です。"));
    addRow(basic, "basis_overrides", tr("元素別の基底"), elementBases);
    help(basic, tr("基底の対応元素を検査します。def2系のRb以降にはdef2-ECP、LANL2DZのNa以降には"
                   "LANL2DZ ECPを自動指定します。"));

    auto *electronic = group("electronic", tr("電荷・スピン"));
    integer(electronic, tr("全電荷"), "charge");
    integer(electronic, tr("スピン多重度 2S+1"), "multiplicity");
    choice(electronic, tr("参照波動関数"), "reference");
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
            [this] { change("charge", window->document().expectedCharge()); });

    auto *convergence = group("convergence", tr("SCF・電子相関"));
    integer(convergence, tr("SCF最大反復"), "scf_maxiter");
    choice(convergence, tr("SCF収束"), "scf_convergence");
    buttons(convergence, tr("DFT積分グリッド"), "grid", false);
    choice(convergence, tr("相対論ハミルトニアン"), "relativistic");
    flag(convergence, tr("RI-J（Coulomb項のフィッティング、DFT）"), "ri_j");
    flag(convergence, tr("相関計算で内殻を凍結（freeze atomic）"), "freeze_core");
    integer(convergence, tr("CC最大反復"), "cc_maxiter");
    auto *opt = group("optimization", tr("最適化・振動数"));
    integer(opt, tr("最適化最大反復"), "opt_maxiter");
    choice(opt, tr("最適化収束"), "opt_convergence");
    flag(opt, tr("数値Hessianを使用"), "numerical_frequencies");
    help(opt, tr("MP2・ROHF・ECP・溶媒・相対論・RI-Jの振動数は数値Hessianを自動選択します。"
                 "固定原子・ゴースト・外部点電荷を含む振動数解析は未対応です。"));

    nav->addHeading(tr("計算目的に応じて"));
    auto *path = group("path", tr("IRC・座標スキャン"));
    integer(path, tr("IRCの点数（各方向）"), "irc_points");
    real(path, tr("IRCの刻み幅 / amu½·Bohr"), "irc_stride");
    choice(path, tr("IRCの方向"), "irc_direction");
    scanCoordinate = new QLabel;
    scanCoordinate->setObjectName("calculationScanCoordinate");
    auto *scanAtoms = new QPushButton(tr("選択中の原子を設定"));
    scanAtoms->setObjectName("calculationScanAtoms");
    scanAtoms->setToolTip(tr("「1 分子」で2〜4原子を順に選ぶと、距離・角度・二面角をスキャンします。"
                             "後に選んだ原子の側を動かします。"));
    auto *scanRow = new QWidget;
    auto *scanLayout = new QHBoxLayout(scanRow);
    scanLayout->setContentsMargins(0, 0, 0, 0);
    scanLayout->addWidget(scanCoordinate, 1);
    scanLayout->addWidget(scanAtoms);
    addRow(path, "scan_atoms", tr("スキャン座標"), scanRow);
    connect(scanAtoms, &QPushButton::clicked, this, &CalculationPanel::setScanCoordinate);
    real(path, tr("開始値（Å・°）"), "scan_start");
    real(path, tr("終了値（Å・°）"), "scan_end");
    integer(path, tr("点の数"), "scan_steps");
    flag(path, tr("各点で他の座標を最適化（緩和スキャン）"), "scan_relaxed");
    help(path, tr("IRCは遷移状態の構造から反応経路を追跡します（既定は両方向）。NWChemは経路の長さが"
                  "1 amu½·Bohrを超えるまで極小に着いたと判定しないため、短い側は点の数の上限で"
                  "「mepgs: could not optimize」となって止まり、残りの方向は計算されません。そのときは"
                  "「IRCの方向」で残りの方向だけを計算します。結果は「結果ビューア」の「経路・スキャン」"
                  "に表示します。"));

    auto *constrained = group("constraints", tr("内部座標の拘束"));
    constraints = new QTableWidget(0, 2);
    constraints->setObjectName("nwchem_constraints");
    constraints->setHorizontalHeaderLabels({tr("座標"), tr("値（Å・°）")});
    constraints->verticalHeader()->hide();
    constraints->horizontalHeader()->setStretchLastSection(true);
    constraints->setSelectionBehavior(QAbstractItemView::SelectRows);
    constraints->setMaximumHeight(130);
    constrained->addRow(constraints);
    rowKeys[constraints] = "constraints";
    rows["constraints"] = constraints;
    labels["constraints"] = tr("内部座標の拘束");
    sectionOf["constraints"] = "constraints";
    sections.last().keys << "constraints";
    auto *constraintButtons = new QWidget;
    auto *constraintRow = new QHBoxLayout(constraintButtons);
    constraintRow->setContentsMargins(0, 0, 0, 0);
    auto *addConstraintButton = new QPushButton(tr("選択中の2〜4原子を追加"));
    addConstraintButton->setObjectName("calculationAddConstraint");
    auto *removeConstraint = new QPushButton(tr("削除"));
    removeConstraint->setObjectName("calculationRemoveConstraint");
    constraintRow->addWidget(addConstraintButton);
    constraintRow->addWidget(removeConstraint);
    constraintRow->addStretch();
    constrained->addRow(constraintButtons);
    rowKeys[constraintButtons] = "constraints";
    help(constrained, tr("距離・角度・二面角を指定値に固定して最適化します（NWChemのzcoord）。"
                         "固定原子・ゴーストとは併用できません。"));
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

    auto *followup = group("followup", tr("後続の一点計算"));
    flag(followup, tr("最適化後に別の手法・基底で一点エネルギーを計算"), "followup");
    choice(followup, tr("手法"), "followup_method");
    choice(followup, tr("DFT汎関数"), "followup_functional");
    choice(followup, tr("DFT分散補正"), "followup_dispersion");
    choice(followup, tr("基底関数（全元素）"), "followup_basis");

    auto *counterpoise = group("bsse", tr("BSSE補正"));
    flag(counterpoise, tr("単量体ごとのcounterpoise補正（BSSE）"), "bsse");
    monomers = new QLabel;
    monomers->setObjectName("calculationMonomers");
    monomers->setProperty("muted", true);
    monomers->setWordWrap(true);
    counterpoise->addRow(QString(), monomers);
    rowKeys[monomers] = "bsse";

    nav->addHeading(tr("追加の計算"));
    auto *properties = group("properties", tr("物性"));
    flag(properties, tr("Mulliken電荷"), "prop_mulliken");
    flag(properties, tr("ESP・RESP電荷"), "prop_esp");
    flag(properties, tr("NMR遮蔽定数（GIAO）"), "prop_nmr");
    flag(properties, tr("静的分極率"), "prop_polarizability");
    help(properties, tr("最後の計算（後続の一点計算があればその条件）の後に計算します。"
                        "結果は「結果ビューア」の「物性」に表示します。"));

    auto *excited = group("excited", tr("励起状態（TDDFT）"));
    flag(excited, tr("TDDFTで励起エネルギー・振動子強度を計算"), "tddft");
    integer(excited, tr("励起状態の数（nroots）"), "tddft_roots");
    flag(excited, tr("三重項も計算（閉殻）"), "tddft_triplets");
    flag(excited, tr("Tamm–Dancoff近似（TDA / cis）"), "tddft_tda");
    help(excited, tr("DFTの一点エネルギーで使えます。結果は「結果ビューア」の「励起状態」に表示します。"));

    nav->addHeading(tr("環境・出力"));
    auto *environment = group("environment", tr("周囲の環境"));
    buttons(environment, tr("溶媒モデル"), "solvent", false, {tr("気相"), "COSMO", "SMD"});
    choice(environment, tr("溶媒"), "solvent_name");
    auto *dielectric = new QDoubleSpinBox;
    dielectric->setObjectName("nwchem_dielectric");
    const auto dielectricSpec = nwchemSettingSpec("dielectric");
    dielectric->setRange(dielectricSpec.minimum, dielectricSpec.maximum);
    dielectric->setDecimals(dielectricSpec.decimals);
    dielectric->setKeyboardTracking(false);
    fields["dielectric"] = dielectric;
    addRow(environment, "dielectric", tr("比誘電率（例: 水 78.4）"), dielectric);
    labels["dielectric"] = tr("比誘電率");
    connect(dielectric, &QDoubleSpinBox::valueChanged, this, [this](double n) { change("dielectric", n); });
    flag(environment, tr("「1 分子」の外部点電荷を含める"), "include_point_charges");

    // Only detailed options: the movecs, the scratch and permanent folders, MOLDEN and CUBE.
    auto *orbitalFiles = group("orbitals", tr("軌道・出力ファイル"));
    help(orbitalFiles,
         tr("分子軌道の形を「結果ビューア」の「軌道」で表示するには、下の「MOLDENファイルを出力」を"
            "オンにします。電子密度・軌道の格子データはCUBEファイルで出力します。"));

    auto *output = group("output", tr("入力ファイル"));
    for (const auto &key : {"start", "title"}) {
        auto *line = new QLineEdit;
        line->setObjectName(QString("nwchem_") + key);
        fields[key] = line;
        addRow(output, key, QString(key) == "start" ? tr("計算名（英数字）") : tr("タイトル"), line);
        connect(line, &QLineEdit::editingFinished, this, [this, line, key] { change(key, line->text()); });
    }
    // The memory is set in "3 実行設定" with the processes it is multiplied by.
    memoryState = new QLabel;
    memoryState->setObjectName("calculationMemoryState");
    auto *memoryLink = linkButton(tr("3 実行設定で変更"), "calculationMemoryLink");
    connect(memoryLink, &QPushButton::clicked, this, [this] { window->showSetting(2, "memory_mb"); });
    auto *memoryRow = readOnlyRow(memoryState, memoryLink);
    output->addRow(tr("メモリ"), memoryRow);
    rows["memory_mb"] = memoryRow;
    rowKeys[memoryRow] = "memory_mb";
    sectionOf["memory_mb"] = "output";
    choice(output, tr("座標の出力単位"), "geometry_units");

    auto *extra = group("extra", tr("追加入力（上級者向け）"));
    help(extra, tr("MolFormaが扱わないNWChemの指示を、検査せずにそのまま入力へ加えます。"
                   "task shell・pythonブロックは使用できません。"));
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
    extraInput->setPlaceholderText(tr("例: set cphf:maxiter 50"));
    extraTasks->setPlaceholderText(tr("例: task dft property"));
    addRow(extra, "extra_input", tr("タスクの前"), extraInput);
    addRow(extra, "extra_tasks", tr("入力の末尾"), extraTasks);
    labels["extra_input"] = tr("追加入力（タスクの前）");
    labels["extra_tasks"] = tr("追加入力（入力の末尾）");
    auto *applyExtra = new QPushButton(tr("追加入力を適用"));
    applyExtra->setObjectName("calculationApplyExtra");
    applyExtra->setProperty("mcpExecutionSetting", true);
    extra->addRow(QString(), applyExtra);
    rowKeys[applyExtra] = "extra_input";
    connect(applyExtra, &QPushButton::clicked, this, [this] {
        const auto before = extraInput->toPlainText(), after = extraTasks->toPlainText();
        if (busy || !editable())
            return;
        window->safe([&] {
            window->edit(tr("NWChemの追加入力を変更"), [&](Document &d) {
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
    summaryCard = new CalculationSummary("calculation", tr("この計算"), [this](const QString &link) {
        if (link == "memory")
            window->showSetting(2, "memory_mb");
    });
    railLayout->addWidget(summaryCard);

    validationCard = new StatusCard;
    validationCard->setObjectName("calculationCheck");
    validationCard->headline()->setObjectName("calculationStatus");
    auto *checkLayout = validationCard->body();
    passed = new QLabel(tr("分子・計算条件・詳細設定の検証を通過しました。"));
    passed->setObjectName("calculationChecksPassed");
    passed->setProperty("muted", true);
    passed->setWordWrap(true);
    checkLayout->addWidget(passed);
    validation = new DiagnosticsList;
    validation->setObjectName("calculationValidation");
    validation->setFrameShape(QFrame::NoFrame);
    validation->setFitContents(220);
    checkLayout->addWidget(validation);
    connect(validation, &DiagnosticsList::diagnosticClicked, this,
            [this](const QString &setting, int tab) { window->showSetting(tab, setting); });
    connect(validation, &DiagnosticsList::fixRequested, this, &CalculationPanel::applyFix);
    railLayout->addWidget(validationCard);

    // The detailed option chosen last: what it does, its values and its lines in the input.
    detailCard = new SettingsCard(tr("選んだ詳細設定"));
    detailCard->setObjectName("calculationDetailCard");
    auto *closeDetail = new QToolButton;
    closeDetail->setObjectName("calculationDetailClose");
    closeDetail->setIcon(lineIcon("close"));
    closeDetail->setProperty("flat", true);
    closeDetail->setToolTip(tr("閉じる"));
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
    auto *facts = new FormLayout;
    facts->setHorizontalSpacing(14);
    facts->setVerticalSpacing(3);
    for (const auto &[name, label, objectName] :
         {std::tuple{QT_TR_NOOP("既定"), &detailDefault, "calculationDetailDefault"},
          {QT_TR_NOOP("現在"), &detailCurrent, "calculationDetailCurrent"},
          {QT_TR_NOOP("使う計算"), &detailScope, "calculationDetailScope"}}) {
        *label = new QLabel;
        (*label)->setObjectName(objectName);
        (*label)->setWordWrap(true);
        auto *term = new QLabel(tr(name));
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
    detailReset = new QPushButton(tr("既定に戻す"));
    detailReset->setObjectName("calculationDetailReset");
    detailReset->setIcon(lineIcon("reset"));
    showInInput = new QPushButton(tr("入力で見る"));
    showInInput->setObjectName("calculationShowInInput");
    showInInput->setIcon(lineIcon("document"));
    showInInput->setToolTip(tr("「入力プレビュー」でこの項目の行を表示します。"));
    detailActions->addWidget(detailReset);
    detailActions->addWidget(showInInput);
    detailActions->addStretch();
    detailCard->body()->addLayout(detailActions);
    connect(detailReset, &QPushButton::clicked, this,
            [this] { changeDetail(selectedDetail, QJsonValue::Undefined); });
    connect(showInInput, &QPushButton::clicked, this, [this] {
        if (const auto line = showInInput->property("line").toInt(); line >= 0)
            window->showInputLine(line);
    });
    detailCard->hide();
    railLayout->addWidget(detailCard);

    auto *changeCard = new SettingsCard(tr("既定から変更した条件"));
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
    connect(changes, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        window->showSetting(1, item->data(0, Qt::UserRole).toString());
    });
    noChanges = new QLabel(tr("なし（HF / STO-3G・一点エネルギー）"));
    noChanges->setObjectName("calculationNoChanges");
    noChanges->setProperty("muted", true);
    changeCard->body()->addWidget(noChanges);
    railLayout->addWidget(changeCard); // As tall as its rows; the link stays at the bottom.
    railLayout->addStretch(1);
    auto *showPreview = linkButton(tr("4 入力プレビューで確認"), "calculationShowPreview");
    showPreview->setToolTip(tr("生成するNWChem入力の全文を「4 入力プレビュー」で表示します。"));
    auto *previewRow = new QHBoxLayout;
    previewRow->addStretch();
    previewRow->addWidget(showPreview);
    railLayout->addLayout(previewRow);
    connect(showPreview, &QPushButton::clicked, this, [this] { window->showInputLine(); });

    // Labels for search and the list of changes: form rows, plus the fields with their own rows.
    QStringList completions = labels.values();
    for (const auto &o : nwchemAdvancedOptions())
        completions << o.label + tr("（詳細）");
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
        return value.toBool() ? tr("オン") : tr("オフ");
    if (value.isDouble())
        return QLocale().toString(value.toDouble());
    if (value.isArray())
        return tr("%n件", nullptr, value.toArray().size());
    if (value.isObject()) {
        QStringList parts;
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            parts << it.key() + ": " + it.value().toString();
        return parts.join(tr("、"));
    }
    const auto text = value.toString();
    return text.contains('\n') || text.size() > 40 ? tr("あり（%n文字）", nullptr, text.size()) : text;
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
void CalculationPanel::applyFix(const QString &code, const QString &setting) {
    const auto report = validateNwchem(window->document());
    for (const auto &diagnostic : report.diagnostics)
        if (diagnostic.code == code && diagnostic.setting == setting && diagnostic.fix) {
            change(diagnostic.fix->setting, diagnostic.fix->value);
            return;
        }
}
void CalculationPanel::searchSettings(const QString &text) {
    const bool detailed = text.endsWith(tr("（詳細）"));
    const auto query = QString(text).remove(tr("（詳細）")).trimmed();
    if (query.isEmpty())
        return;
    if (!detailed)
        for (auto it = labels.begin(); it != labels.end(); ++it)
            if (it.value().contains(query, Qt::CaseInsensitive)) {
                window->showSetting(1, it.key());
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
    window->statusBar()->showMessage(tr("「%1」に一致する設定はありません。").arg(query), 5000);
}
QFormLayout *CalculationPanel::group(const QString &key, const QString &title) {
    auto *card = new SettingsCard(title);
    card->setObjectName("calculationCard_" + key);
    formWidget->layout()->addWidget(card);
    auto *note = new QLabel;
    note->setProperty("muted", true);
    card->header()->insertWidget(1, note);
    auto *resetButton = new QPushButton(tr("このセクションを既定に戻す"));
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
    auto *form = new FormLayout;
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
void CalculationPanel::integer(QFormLayout *form, const QString &label, const QString &key) {
    auto *spin = new QSpinBox;
    spin->setObjectName("nwchem_" + key);
    const auto &spec = nwchemSettingSpec(key);
    spin->setRange(spec.minimum, spec.maximum);
    spin->setKeyboardTracking(false);
    spin->setMaximumWidth(180);
    fields[key] = spin;
    addRow(form, key, label, spin);
    connect(spin, &QSpinBox::valueChanged, this, [this, key](int n) { change(key, n); });
}
void CalculationPanel::real(QFormLayout *form, const QString &label, const QString &key) {
    auto *spin = new QDoubleSpinBox;
    spin->setObjectName("nwchem_" + key);
    const auto &spec = nwchemSettingSpec(key);
    spin->setRange(spec.minimum, spec.maximum);
    spin->setDecimals(spec.decimals);
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
        combo->setItemText(0, tr("共通（%1）").arg(nwchemBasisLabel(QJsonObject{{"basis", s["basis"]}})));
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
    scanCoordinate->setText(scan.isEmpty() ? tr("未設定") : nwchemCoordinateLabel(d, scan));
    QStringList parts;
    for (const auto &atoms : nwchemBsseMonomers(d)) {
        QMap<QString, int> counts;
        for (int i : atoms)
            ++counts[d.atoms[i].element];
        QString formula;
        for (const auto &symbol : counts.keys())
            formula += symbol + (counts[symbol] > 1 ? QString::number(counts[symbol]) : QString());
        parts << formula;
    }
    monomers->setText(parts.size() < 2 ? tr("単量体: 結合していない分子が2個以上必要です。")
                                       : tr("単量体 %1 個: %2（電荷は原子の形式電荷から決めます）")
                                             .arg(parts.size())
                                             .arg(parts.join(tr("、"))));
}
// The internal coordinate of the ordered selection in the editor, with its current value.
static std::pair<Ids, double> selectedCoordinate(MainWindow *window, const Ids &selection) {
    if (selection.size() < 2 || selection.size() > 4)
        fail(tr("「1 分子」で2〜4原子を順に選択してください（距離・角度・二面角）。"));
    return {selection, measure(window->document(), selection)};
}
void CalculationPanel::addConstraint() {
    window->safe([&] {
        const auto [ids, value] = selectedCoordinate(window, window->selection());
        auto list = nwchemSettings(window->document())["constraints"].toArray();
        list.append(QJsonObject{{"atoms", idsToJson(ids)}, {"value", std::round(value * 1e4) / 1e4}});
        change("constraints", list);
    });
}
void CalculationPanel::setScanCoordinate() {
    window->safe([&] {
        const auto [ids, value] = selectedCoordinate(window, window->selection());
        // A first guess for the range: +0.5 Å, +60° or half a turn, kept inside the valid range.
        const double start = std::round(value * 1e4) / 1e4;
        const double end = ids.size() == 2   ? start + 0.5
                           : ids.size() == 3 ? std::min(start + 60, 179.0)
                                             : (start <= 0 ? start + 180 : start - 180);
        if (busy || !editable())
            return;
        window->edit(tr("スキャン座標を設定"), [&](Document &d) {
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
    return !window->hasCandidate() && !window->chemistryRunning();
}
void CalculationPanel::change(const QString &key, const QJsonValue &value) {
    if (busy || !editable())
        return;
    const auto &current = window->document();
    if (nwchemSettingValue(current, nwchemSettings(current), key) == value)
        return;
    window->safe([&] {
        window->edit(tr("NWChem計算条件を変更"), [&](Document &d) { setNwchemSetting(d, key, value); });
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
        title = tr("「%1」を既定に戻す").arg(what);
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
        box.unused->setText(box.showUnused ? tr("この計算で使わない項目を隠す")
                                           : tr("この計算で使わない項目も表示（%1）").arg(unused));
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
        it.value()->setToolTip(changed.contains(it.key()) ? tr("既定値から変更しています。") : QString());
    }
    const auto a = nwchemAdvanced(d);
    for (const auto &section : std::as_const(sections)) {
        int count = 0;
        for (const auto &key : section.keys)
            count += changed.contains(key);
        for (const auto &detail : std::as_const(details))
            count += detail.section == section.key && a.contains(detail.option->key);
        const auto unused = nwchemSectionUnused(d, section.key);
        section.note->setText(unused.isEmpty()           ? QString()
                              : unused == tr("使わない") ? tr("この計算では使いません")
                                                         : tr("この計算では使いません（%1）").arg(unused));
        if (auto *entry = nav->entry(section.key)) {
            entry->setData(NavList::BadgeRole, count ? tr("変更 %1").arg(count) : QString());
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
        b->setToolTip(tr("「%1」を既定に戻す").arg(labels.value(key)));
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
            const auto value =
                used ? detailValueText(o, a[o.key]) : tr("%1（保留）").arg(detailValueText(o, a[o.key]));
            auto *item = new QTreeWidgetItem(changes, {o.label, value});
            item->setData(0, Qt::UserRole, "advanced:" + o.key);
            item->setToolTip(0, tr("%1（%2の詳細設定）").arg(o.label, section.title));
            item->setToolTip(1, value);
            item->setForeground(0, colors.muted);
            if (!used)
                item->setForeground(1, colors.muted);
            auto *b = new QToolButton;
            b->setObjectName("calculationReset_advanced_" + o.key);
            b->setIcon(lineIcon("reset"));
            b->setProperty("flat", true);
            b->setToolTip(tr("「%1」を既定に戻す").arg(o.label));
            b->setEnabled(editable());
            changes->setItemWidget(item, 2, b);
            connect(b, &QToolButton::clicked, this, [this, key = o.key] {
                QTimer::singleShot(0, this, [this, key] { changeDetail(key, QJsonValue::Undefined); });
            });
        }
    changeCount->setText(total ? tr("%n 項目", nullptr, total) : QString());
    // Its rows (up to 12, then it scrolls), not the height of the rail.
    const int row = std::max(changes->sizeHintForRow(0), 26);
    changes->setFixedHeight(std::min(changes->topLevelItemCount(), 12) * row + 2 * changes->frameWidth());
    changes->setVisible(total);
    noChanges->setVisible(!total);
    changeFilters->button(1)->setText(tr("変更済み %1").arg(total));
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
        presetMenu->addSection(tr("登録したプリセット"));
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
    auto *remove = presetMenu->addMenu(lineIcon("delete"), tr("登録を解除"));
    remove->setObjectName("calculationRemovePresets");
    remove->setEnabled(!userPresets.isEmpty());
    for (int i = 0; i < userPresets.size(); ++i) {
        auto *action =
            remove->addAction(tr("「%1」を削除").arg(userPresets[i].toObject()["name"].toString()));
        action->setObjectName(QString("calculationRemovePreset%1").arg(i));
        connect(action, &QAction::triggered, this, [this, i] {
            if (i >= userPresets.size())
                return;
            userPresets.removeAt(i);
            window->setPreference("nwchem/presets",
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
            changes << tr("詳細設定: %n項目", nullptr, selected[key].toObject().size());
            continue;
        }
        changes << labels.value(key, key) + ": " + valueText(key, current[key]) + " → " +
                       valueText(key, selected[key]);
    }
    if (changes.isEmpty())
        return tr("現在の条件と同じです。");
    auto shown = changes.mid(0, 12);
    if (changes.size() > 12)
        shown << "…";
    return tr("適用すると %n 項目が変わります（分子・電荷・多重度・計算名・メモリ・単位は保持）:\n・%1",
              nullptr, changes.size())
        .arg(shown.join(tr("\n・")));
}
void CalculationPanel::applyPreset(const QString &name, const QJsonObject &preset) {
    if (busy || !editable())
        return;
    const auto before = nwchemSettings(window->document());
    window->safe([&] {
        window->edit(tr("NWChemプリセットを適用"), [&](Document &d) { applyNwchemPreset(d, preset); });
    });
    if (nwchemSettings(window->document()) != before)
        window->statusBar()->showMessage(
            tr("プリセット「%1」を適用しました。「元に戻す」で戻せます。").arg(name), 8000);
}
void CalculationPanel::storePreset() {
    if (!editable())
        return;
    const auto settings = nwchemPresetSettings(nwchemSettings(window->document()));
    bool ok;
    auto name =
        QInputDialog::getText(this, tr("NWChemプリセットを登録"), tr("名前"), QLineEdit::Normal, {}, &ok)
            .trimmed();
    if (!ok || name.isEmpty())
        return;
    for (const auto &entry : userPresets)
        if (entry.toObject()["name"].toString() == name) {
            QMessageBox::information(this, tr("登録名"),
                                     tr("同名のプリセットがあります。別の名前で登録してください。"));
            return;
        }
    userPresets << QJsonObject{{"name", name}, {"settings", settings}};
    window->setPreference("nwchem/presets", QJsonDocument(userPresets).toJson(QJsonDocument::Compact));
    reloadPresets();
    window->statusBar()->showMessage(tr("プリセット「%1」を登録しました。").arg(name), 5000);
}
void CalculationPanel::refresh() {
    if (busy)
        return;
    QScopedValueRollback guard(busy, true);
    const auto &d = window->displayDocument();
    const auto s = nwchemSettings(d);
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        const auto value = nwchemSettingValue(d, s, it.key());
        auto *w = it.value();
        const QSignalBlocker blocker(w);
        if (auto *c = qobject_cast<QComboBox *>(w)) {
            while (c->count() > nwchemChoices(it.key()).size())
                c->removeItem(c->count() - 1);
            int index = c->findData(value.toString());
            if (index < 0) {
                c->addItem(tr("未対応: %1").arg(value.toVariant().toString()), value.toString());
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
    memoryState->setText(tr("%1 MB / MPIプロセス").arg(s["memory_mb"].toInt()));
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
    for (auto it = fields.begin(); it != fields.end(); ++it)
        it.value()->setEnabled(nwchemSettingEditable(nwchemSettingSpec(it.key()), s));
    const bool canEdit = editable();
    formWidget->setEnabled(canEdit);
    for (auto *b : {presetButton, storeButton})
        b->setEnabled(canEdit);
    resetAll->setEnabled(canEdit);
    message->setText(window->hasCandidate()
                         ? tr("未確定の分子候補を表示しています。確定・取消後に条件を編集できます。")
                     : window->chemistryRunning() ? tr("分子の整形中です。終了後に条件を編集できます。")
                                                  : QString());
    banner->setVisible(!message->text().isEmpty());
    const auto check = validateNwchem(d);
    const auto method = s["method"].toString();
    const QString referenceName = method == "dft" && check.reference != "rohf"
                                      ? (check.reference == "rhf" ? "RKS" : "UKS")
                                      : check.reference.toUpper();
    reference->setText(
        s["reference"] == "auto" && !referenceName.isEmpty()
            ? (check.reference == "rhf" ? tr("→ %1（閉殻）") : tr("→ %1（開殻）")).arg(referenceName)
            : QString());
    formalCharge->setText(d.hasCenters()
                              ? tr("配位中心の酸化数と形式電荷の合計（%1）を適用").arg(d.expectedCharge())
                              : tr("原子の形式電荷の合計（%1）を適用").arg(d.formalCharge()));
    summaryCard->update(d);
    // Whether the input can be generated, and why not.
    const bool ok = check.errors.isEmpty();
    validationCard->setState(ok ? StatusCard::Ok : StatusCard::Error,
                             ok ? (check.warnings.isEmpty() ? tr("入力を生成できます")
                                                            : tr("入力を生成できます（確認 %n 件）", nullptr,
                                                                 check.warnings.size()))
                                : tr("入力を生成できません（エラー %n 件）", nullptr, check.errors.size()));
    validation->setDiagnostics(check.diagnostics);
    validation->setVisible(validation->count());
    passed->setVisible(ok); // As the checks of "4 入力プレビュー" say it, above the warnings.
    if (auto *outputs = detailBoxes.value("orbitals").outputs) {
        const auto files = nwchemOutputFiles(d, check);
        outputs->setText(files.isEmpty() ? tr("CUBE・MOLDENの出力はオフです。")
                                         : tr("計算後に出力するファイル:\n%1").arg(files.join('\n')));
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
    box.disclosure = new Disclosure(tr("詳細設定"), open);
    box.toggle = box.disclosure->button();
    box.toggle->setObjectName("calculationDetails_" + key);
    box.toggle->setToolTip(tr("NWChemの各ブロックへ直接指定する項目です。空欄の項目は既定値を使います。"));
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
                title = g.value == "visualization" ? tr("CUBE / MOLDEN 出力") : g.label;
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
            const auto tip = tr("%1\nNWChem: %2\n未指定: %3").arg(o.help, o.keyword, o.defaultText);
            detail.row->setToolTip(tip);
            if (o.kind == AdvancedKind::Flag) {
                auto *check = new QCheckBox(tr("オン"));
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
            detail.reset->setToolTip(
                tr("この項目を既定に戻します（明示指定を解除）。「元に戻す」で戻せます。"));
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
    box.empty = caption(tr("この計算で使う詳細設定はありません。"));
    box.empty->setContentsMargins(8, 2, 0, 2);
    layout->addWidget(box.empty);
    box.unused = new QPushButton;
    box.unused->setObjectName("calculationDetailUnused_" + key);
    box.unused->setProperty("link", true);
    box.unused->setCursor(Qt::PointingHandCursor);
    box.unused->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    box.unused->setToolTip(tr("今の計算では入力に入らない項目です。値は保存され、対応する計算に切り替えると"
                              "使います（保留）。"));
    layout->addWidget(box.unused, 0, Qt::AlignLeft);
    if (groups.contains("visualization")) {
        box.outputs = new QLabel;
        box.outputs->setObjectName("calculationOutputFiles");
        box.outputs->setProperty("muted", true);
        box.outputs->setTextFormat(Qt::PlainText);
        box.outputs->setWordWrap(true);
        box.outputs->setTextInteractionFlags(Qt::TextSelectableByMouse);
        box.outputs->setContentsMargins(8, 6, 0, 0);
        box.outputs->setToolTip(
            tr("保存先は「永続ファイルのディレクトリ」に従います。この出力設定はプリセットの"
               "切替後も保持します。"));
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
    window->showPanel(this);
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
        window->showPanel(this);
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
        window->edit(tr("NWChem詳細設定を変更"), [&](Document &d) { setNwchemAdvanced(d, key, value); });
    });
}
QString CalculationPanel::detailValueText(const AdvancedOption &o, const QJsonValue &value) const {
    if (o.kind == AdvancedKind::Flag)
        return value.toBool() ? tr("オン") : tr("オフ");
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
                check->setText(v.toBool() ? tr("オン") : tr("オフ"));
            } else if (auto *combo = qobject_cast<QComboBox *>(detail.editor)) {
                while (combo->count() > o.choices.size() + 1)
                    combo->removeItem(combo->count() - 1);
                combo->setItemText(0, tr("既定（%1）").arg(blank));
                int index = changed ? combo->findData(v.toString()) : 0;
                if (index < 0) {
                    combo->addItem(tr("未対応: %1").arg(nwchemAdvancedText(v)), v.toString());
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
        detail.state->setText(kind == "error"     ? tr("エラー")
                              : kind == "applied" ? tr("適用")
                              : kind == "pending" ? tr("保留")
                                                  : QString());
        detail.state->setToolTip(!error.isEmpty()    ? tr("エラー: %1").arg(error)
                                 : kind == "applied" ? tr("入力へ反映します。")
                                 : kind == "pending" ? tr("保留: 今の計算では使いません。")
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
        it->disclosure->setDetail(changed
                                      ? tr("%n 項目 · 変更 %1", nullptr, totals.value(it.key())).arg(changed)
                                      : tr("%n 項目", nullptr, totals.value(it.key())));
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
    detailHelp->setText(error.isEmpty() ? o.help : tr("%1\nエラー: %2").arg(o.help, error));
    detailDefault->setText(nwchemAdvancedDefault(o, d));
    detailCurrent->setText(a.contains(o.key) ? detailValueText(o, a[o.key]) : tr("空欄（既定）"));
    detailScope->setText(nwchemAdvancedApplies(o, d)
                             ? nwchemAdvancedScope(o)
                             : tr("%1（今の計算では使いません）").arg(nwchemAdvancedScope(o)));
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
    NwchemGeneratedInput generated;
    try {
        generated = generateNwchemInput(d);
    } catch (const std::exception &) {
    }
    const auto &input = generated.text;
    const bool active = nwchemAdvancedApplies(o, d), changed = nwchemAdvanced(d).contains(o.key);
    if (input.isEmpty()) {
        snippet->hide();
        snippetNote->setText(tr("入力を生成できません。上の検査結果を確認してください。"));
        return;
    }
    if (!active) {
        snippet->hide();
        snippetNote->setText(changed ? tr("今の計算では使わないため、入力に入りません（保留）。")
                                     : tr("今の計算では使いません。"));
        return;
    }
    const auto lines = input.split('\n');
    const auto &outline = generated.outline;
    const auto ownLines = outline.settings.value(o.key);
    const auto name = nwchemAdvancedBlock(o, d);
    const NwchemInputBlock *block = nullptr;
    for (const auto &b : outline.blocks) {
        const bool carries = std::any_of(ownLines.begin(), ownLines.end(),
                                         [&](int line) { return line >= b.line && line < b.line + b.lines; });
        if (carries) {
            block = &b;
            break;
        }
        if (!block && b.keyword == name)
            block = &b;
    }
    if (!block) {
        snippet->hide();
        snippetNote->setText(changed ? tr("この値を書き込む行はありません。")
                                     : tr("空欄のため指定しません（既定値を使います）。"));
        return;
    }
    QList<int> own;
    for (int i = block->line; i < block->line + block->lines; ++i)
        if (changed && ownLines.contains(i) && outline.advanced.contains(i))
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
    snippetNote->setText(changed ? QString() : tr("空欄のため、ここには指定しません（既定値を使います）。"));
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
