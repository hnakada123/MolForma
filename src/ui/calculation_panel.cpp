// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/calculation_panel.h"
#include "model/geometry.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "ui/advanced_panel.h"
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
    search->setPlaceholderText("設定を検索（計算条件と「4 詳細設定」）");
    search->setClearButtonEnabled(true);
    search->addAction(lineIcon("search"), QLineEdit::LeadingPosition);
    search->setFixedWidth(300);
    tools->addWidget(search);
    changedOnly = new QPushButton("変更した項目のみ");
    changedOnly->setObjectName("calculationChangedOnly");
    changedOnly->setCheckable(true);
    changedOnly->setToolTip("既定値から変更した設定の行だけを表示します。");
    tools->addWidget(changedOnly);
    connect(changedOnly, &QPushButton::toggled, this, [this] {
        filterRows();
        followScroll();
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

    auto *orbitalFiles = group("orbitals", "軌道ファイル");
    moldenOutput = new QCheckBox("計算後に分子軌道をMOLDENファイルに出力");
    moldenOutput->setObjectName("calculationMoldenOutput");
    moldenOutput->setToolTip("「結果ビューア」の「軌道」で分子軌道の形を表示できます。"
                             "「4 詳細設定」の「MOLDENファイルを出力」と同じ設定です。");
    orbitalFiles->addRow(QString(), moldenOutput);
    connect(moldenOutput, &QCheckBox::toggled, this, [this](bool on) {
        if (busy || !editable())
            return;
        window->safe([&] {
            window->edit("MOLDEN出力を変更", [&](Document &d) {
                setNwchemAdvanced(d, "molden_output", on ? QJsonValue(true) : QJsonValue::Undefined);
            });
        });
    });
    help(orbitalFiles,
         "電子密度・軌道の格子データ（CUBE）は「4 詳細設定」の「可視化ファイル出力」で設定します。");

    auto *output = group("output", "入力ファイル");
    for (const auto &key : {"start", "title"}) {
        auto *line = new QLineEdit;
        line->setObjectName(QString("nwchem_") + key);
        fields[key] = line;
        addRow(output, key, QString(key) == "start" ? "計算名（英数字）" : "タイトル", line);
        connect(line, &QLineEdit::editingFinished, this, [this, line, key] { change(key, line->text()); });
    }
    integer(output, "メモリ（MB / MPIプロセス）", "memory_mb", 64, 1048576);
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

    // The calculation in brief, its checks and what differs from the defaults.
    auto *rail = new QWidget;
    rail->setObjectName("calculationRail");
    rail->setFixedWidth(340);
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(0, 16, 16, 16);
    railLayout->setSpacing(12);
    body->addWidget(rail);
    auto *summaryCard = new SettingsCard({});
    summaryCard->setObjectName("calculationSummaryCard");
    summaryCard->body()->setSpacing(4);
    summaryCard->body()->addWidget(caption("この計算"));
    summaryTitle = new QLabel;
    summaryTitle->setObjectName("calculationSummary");
    summaryTitle->setProperty("cardTitle", true);
    summaryTitle->setWordWrap(true);
    summaryCard->body()->addWidget(summaryTitle);
    summaryTask = new QLabel;
    summaryTask->setObjectName("calculationSummaryTask");
    summaryTask->setWordWrap(true);
    summaryCard->body()->addWidget(summaryTask);
    auto *facts = new QFormLayout;
    facts->setContentsMargins(0, 6, 0, 0);
    facts->setHorizontalSpacing(14);
    facts->setVerticalSpacing(3);
    for (const auto &[name, label] : {std::pair{"組成", &summaryFormula},
                                      {"電子数", &summaryElectrons},
                                      {"電荷・多重度", &summaryCharge},
                                      {"メモリ", &summaryMemory}}) {
        *label = new QLabel;
        (*label)->setWordWrap(true);
        auto *term = new QLabel(name);
        term->setProperty("muted", true);
        facts->addRow(term, *label);
    }
    summaryCard->body()->addLayout(facts);
    railLayout->addWidget(summaryCard);

    validationCard = new SettingsCard({});
    validationCard->setObjectName("calculationCheck");
    auto *checkLayout = static_cast<SettingsCard *>(validationCard)->body();
    status = new QLabel;
    status->setObjectName("calculationStatus");
    checkLayout->addWidget(status);
    validation = new DiagnosticsList;
    validation->setObjectName("calculationValidation");
    validation->setFrameShape(QFrame::NoFrame);
    validation->setFitContents(220);
    checkLayout->addWidget(validation);
    connect(validation, &DiagnosticsList::messageClicked, this,
            [this](const QString &message) { showField(fieldFor(message)); });
    connect(validation, &DiagnosticsList::fixRequested, this, &CalculationPanel::applyFix);
    railLayout->addWidget(validationCard);

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
    advancedChanges = new QPushButton;
    advancedChanges->setObjectName("calculationAdvancedChanges");
    advancedChanges->setProperty("link", true);
    advancedChanges->setCursor(Qt::PointingHandCursor);
    changeCard->body()->addWidget(advancedChanges);
    connect(advancedChanges, &QPushButton::clicked, this, [this] { window->advanced->showChanged(); });
    railLayout->addWidget(changeCard, 1);
    auto *showPreview = new QPushButton("入力全文を確認（6 入力プレビュー）");
    showPreview->setObjectName("calculationShowPreview");
    showPreview->setProperty("link", true);
    showPreview->setIcon(lineIcon("arrow"));
    showPreview->setLayoutDirection(Qt::RightToLeft); // The arrow after the text.
    showPreview->setCursor(Qt::PointingHandCursor);
    auto *previewRow = new QHBoxLayout;
    previewRow->addStretch();
    previewRow->addWidget(showPreview);
    railLayout->addLayout(previewRow);
    connect(showPreview, &QPushButton::clicked, this,
            [this] { window->workspaceTabs->setCurrentWidget(window->inputPreview); });

    // Labels for search and the list of changes: form rows, plus the fields with their own rows.
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
    if (key == "advanced") {
        window->workspaceTabs->setCurrentWidget(window->advanced);
        return;
    }
    QWidget *target = key == "extra_input"   ? static_cast<QWidget *>(extraInput)
                      : key == "extra_tasks" ? static_cast<QWidget *>(extraTasks)
                                             : rows.value(key);
    if (!target)
        return;
    if (changedOnly->isChecked() && !target->isVisibleTo(formWidget))
        changedOnly->setChecked(false);
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
                reset(section.keys);
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
void CalculationPanel::reset(const QStringList &keys) {
    if (busy || !editable())
        return;
    const auto defaults = nwchemDefaults();
    const auto changed = changedKeys(nwchemSettings(window->document()));
    QStringList targets;
    for (const auto &key : keys)
        if (changed.contains(key) && key != "extra_input" && key != "extra_tasks")
            targets << key;
    if (targets.isEmpty())
        return;
    QString title = "「" + labels.value(targets[0]) + "」を既定に戻す";
    for (const auto &section : std::as_const(sections))
        if (targets.size() > 1 && section.keys.contains(targets[0]))
            title = "「" + section.title + "」を既定に戻す";
    window->safe([&] {
        window->edit(title, [&](Document &d) {
            auto s = d.data["nwchem"].toObject();
            for (const auto &key : targets)
                s[key] = defaults[key];
            d.data["nwchem"] = s;
        });
    });
}
void CalculationPanel::showSection(const QString &key) {
    for (const auto &section : std::as_const(sections))
        if (section.key == key) {
            if (!section.card->isVisible() && changedOnly->isChecked())
                changedOnly->setChecked(false);
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
    const bool only = changedOnly->isChecked();
    const auto changed = changedKeys(nwchemSettings(window->displayDocument()));
    for (const auto &section : std::as_const(sections)) {
        bool any = false;
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
    for (const auto &section : std::as_const(sections)) {
        int count = 0;
        for (const auto &key : section.keys)
            count += changed.contains(key);
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
    changeCount->setText(changed.isEmpty() ? QString() : QString("%1 項目").arg(changed.size()));
    changes->setVisible(!changed.isEmpty());
    noChanges->setVisible(changed.isEmpty());
    const int advancedCount = nwchemAdvanced(d).size();
    advancedChanges->setText(QString("「4 詳細設定」でも %1 項目を変更しています").arg(advancedCount));
    advancedChanges->setVisible(advancedCount);
    changedOnly->setText(QString("変更した項目のみ（%1）").arg(changed.size()));
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
    const bool canEdit = editable();
    formWidget->setEnabled(canEdit);
    for (auto *b : {presetButton, storeButton})
        b->setEnabled(canEdit);
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
    // The calculation in brief.
    QString taskName = task, solvent;
    for (const auto &c : nwchemChoices("task"))
        if (c.value == task)
            taskName = c.label;
    if (s["solvent"] != "none") {
        solvent = " · " + valueText("solvent", s["solvent"]).section("（", 0, 0);
        if (s["solvent_name"] != "dielectric")
            solvent += "（" + valueText("solvent_name", s["solvent_name"]).section("（", 0, 0) + "）";
    }
    summaryTitle->setText((method == "dft" ? nwchemFunctionalLabel(s) : method.toUpper()) + " / " +
                          nwchemBasisLabel(s));
    summaryTask->setText(taskName + solvent);
    summaryFormula->setText(d.atoms.isEmpty() ? QString("原子なし")
                                              : QString("%1 · %2 原子").arg(d.formula()).arg(d.atoms.size()));
    summaryElectrons->setText(QString("%1（α %2 / β %3）%4")
                                  .arg(check.electrons)
                                  .arg(check.alpha)
                                  .arg(check.beta)
                                  .arg(check.ecpElectrons ? "ECP適用後" : ""));
    summaryCharge->setText(QString("%1 · %2%3")
                               .arg(d.job()["charge"].toInt())
                               .arg(d.job()["multiplicity"].toInt())
                               .arg(referenceName.isEmpty() ? QString() : "（" + referenceName + "）"));
    summaryMemory->setText(QString("%1 MB / MPIプロセス").arg(s["memory_mb"].toInt()));
    // Whether the input can be generated, and why not.
    const bool ok = check.errors.isEmpty();
    status->setText(ok ? (check.warnings.isEmpty()
                              ? QString("✓ 入力を生成できます")
                              : QString("✓ 入力を生成できます（確認 %1 件）").arg(check.warnings.size()))
                       : QString("入力を生成できません（エラー %1 件）").arg(check.errors.size()));
    status->setProperty("good", ok);
    status->setProperty("bad", !ok);
    validationCard->setProperty("state", ok ? "ok" : "error");
    for (QWidget *w : {static_cast<QWidget *>(status), static_cast<QWidget *>(validationCard)}) {
        w->style()->unpolish(w);
        w->style()->polish(w);
    }
    validation->setMessages(check.errors, check.warnings, {}, fixes(check.errors));
    validation->setVisible(validation->count());
    refreshChanges(s);
}
void CalculationPanel::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    // Narrow windows give the form the room of the section list.
    navColumn->setVisible(width() >= 1040);
}
void CalculationPanel::applyChromeStyle() {
    const auto pal = static_cast<QWidget *>(window)->palette();
    setStyleSheet(chromeRules(settingsRules() + R"(
        #calculationTab, #calculationRail { background: @window; }
        #calculationNav { background: @base; border: none; border-right: 1px solid @line; }
        QTreeWidget#calculationChanges, QListWidget#calculationValidation { background: transparent; }
        QPushButton:checked { background: @tint; border-color: @accent; color: @accentText; font-weight: bold; }
    )",
                              pal));
    if (!busy)
        refresh(); // The marks are colored in their markup.
}
} // namespace mf
