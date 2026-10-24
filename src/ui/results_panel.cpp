// SPDX-License-Identifier: Apache-2.0
// Independent result browsing workflow inspired by KAGAMI results_tab.py; native NWChem readers.
#include "ui/results_panel.h"
#include "editing/coordinates.h"
#include "model/geometry.h"
#include "results/molden_orbitals.h"
#include "ui/main_window.h"
#include "ui/result_charts.h"
#include <QtWidgets>
#include <algorithm>
#include <cmath>

namespace mf {
namespace {
QTableWidget *table(const QStringList &labels, const QString &name) {
    auto *t = new QTableWidget(0, labels.size());
    t->setObjectName(name);
    t->setHorizontalHeaderLabels(labels);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->horizontalHeader()->setStretchLastSection(true);
    return t;
}
void row(QTableWidget *t, int index, const QStringList &values) {
    for (int i = 0; i < values.size(); ++i)
        t->setItem(index, i, new QTableWidgetItem(values[i]));
}
QString real(double value) {
    return std::isfinite(value) ? QString::number(value, 'g', 12) : "—";
}
QString fixed(double value, int decimals) {
    return std::isfinite(value) ? QString::number(value, 'f', decimals) : "—";
}
constexpr double HartreeToKcal = 627.509474, HartreeToEv = 27.211386245988, EvNanometer = 1239.84198;
QString sourceStamp(const QFileInfo &file) {
    if (!file.exists() || !file.isFile())
        return {};
    return QString::fromUtf8(
        QJsonDocument(
            QJsonArray{
                file.absoluteFilePath(), file.canonicalFilePath(), QString::number(file.size()),
                QString::number(file.lastModified().toMSecsSinceEpoch()),
                QString::number(file.fileTime(QFileDevice::FileMetadataChangeTime).toMSecsSinceEpoch())})
            .toJson(QJsonDocument::Compact));
}
// "bond 1 2" (MolForma's scan title) in the words of the calculation tab.
QString coordinateText(const QString &coordinate) {
    const auto words = coordinate.split(' ', Qt::SkipEmptyParts);
    const QString kind = words.value(0) == "bond" ? "距離" : words.value(0) == "angle" ? "角度" : "二面角";
    return kind + ' ' + words.mid(1).join("–");
}
QString solvationText(const QJsonObject &s) {
    QStringList lines;
    if (s.contains("smd_dg"))
        lines << QString("SMD溶媒和自由エネルギー（1 M基準）: %1 kcal/mol、溶液中の全自由エネルギー %2 Eh")
                     .arg(fixed(s["smd_dg"].toDouble() * HartreeToKcal, 2), real(s["smd_total"].toDouble()));
    else if (s.contains("gas") && s.contains("solution"))
        lines << QString("COSMO溶媒和エネルギー（静電項、溶液 − 気相）: %1 kcal/mol")
                     .arg(fixed((s["solution"].toDouble() - s["gas"].toDouble()) * HartreeToKcal, 2));
    return lines.join('\n');
}
// Summary lines for BSSE, solvation and polarizability (also shown in the 物性 tab).
QString resultPropertyLines(const ResultData &data) {
    QString text;
    if (data.properties.contains("bsse")) {
        const auto b = data.properties.value("bsse").toObject();
        text += QString("BSSE（counterpoise）補正: %1 kcal/mol、補正後 %2 Eh（補正前 %3 Eh）\n")
                    .arg(fixed(b["error"].toDouble() * HartreeToKcal, 3), real(b["corrected"].toDouble()),
                         real(b["supermolecular"].toDouble()));
    }
    if (const auto s = solvationText(data.properties.value("solvation").toObject()); !s.isEmpty())
        text += s + '\n';
    if (data.properties.contains("polarizability")) {
        const auto p = data.properties.value("polarizability").toObject();
        constexpr double BohrCubed = 0.1481847; // Å³ per a.u. of polarizability volume.
        text += QString("静的分極率: 等方 %1 a.u.（%2 Å³）、異方性 %3 a.u.\n")
                    .arg(fixed(p["isotropic"].toDouble(), 4), fixed(p["isotropic"].toDouble() * BohrCubed, 4),
                         fixed(p["anisotropy"].toDouble(), 4));
    }
    if (data.properties.contains("scan"))
        text += QString("座標スキャン: %1点（「経路・スキャン」に表示）\n")
                    .arg(data.properties.value("scan").toArray().size());
    if (data.properties.contains("irc")) {
        const auto irc = data.properties.value("irc").toObject();
        text +=
            QString("IRC: %1点%2（「経路・スキャン」に表示）\n")
                .arg(irc["points"].toArray().size())
                .arg(irc["complete"].toBool() ? "、両方向とも収束" : "、収束メッセージは両方向にありません");
    }
    return text;
}
QString orbitalKey(int orbital, double iso, const QString &normalization, int divisions) {
    return QString::fromUtf8(
        QJsonDocument(QJsonArray{orbital, iso, normalization, divisions}).toJson(QJsonDocument::Compact));
}
} // namespace
ResultsPanel::ResultsPanel(MainWindow *owner) : QWidget(owner), window(owner) {
    setObjectName("resultsTab");
    auto *layout = new QVBoxLayout(this);
    auto *actions = new QHBoxLayout;
    layout->addLayout(actions);
    auto *open = new QPushButton("結果ファイルを開く…");
    open->setObjectName("resultsOpen");
    auto *directory = new QPushButton("ジョブフォルダーを開く…");
    directory->setObjectName("resultsDirectory");
    reload = new QPushButton("再読込");
    reload->setObjectName("resultsReload");
    cancel = new QPushButton("読込を中止");
    cancel->setObjectName("resultsCancel");
    for (auto *b : {open, directory, reload, cancel})
        actions->addWidget(b);
    actions->addStretch();
    applyGeometry = new QPushButton("編集中の分子へ適用");
    applyGeometry->setObjectName("resultsApplyStructure");
    applyGeometry->setToolTip("表示中の構造の座標を「1 分子」の分子へ候補として適用します。原子の数と順番が同"
                              "じ計算結果に使えます。結合・設定は変わりません。");
    actions->addWidget(applyGeometry);
    compareGeometry = new QPushButton("編集中の分子と比較");
    compareGeometry->setObjectName("resultsCompare");
    compareGeometry->setToolTip(
        "表示中の構造と「1 分子」の分子を重ね合わせ、RMSDを表示します（同じ原子・同じ順番）。");
    actions->addWidget(compareGeometry);
    saveXyz = new QPushButton("構造を保存…");
    saveXyz->setObjectName("resultsSaveXyz");
    savePng = new QPushButton("画像を保存…");
    savePng->setObjectName("resultsSavePng");
    actions->addWidget(saveXyz);
    actions->addWidget(savePng);
    files = new QComboBox;
    files->setObjectName("resultsFiles");
    files->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    files->setMinimumContentsLength(20);
    layout->addWidget(files);
    source = new QLabel("結果ファイルを開いてください。");
    source->setObjectName("resultsSource");
    source->setTextFormat(Qt::PlainText);
    source->setWordWrap(true);
    layout->addWidget(source);
    status = new QPlainTextEdit;
    status->setObjectName("resultsStatus");
    status->setAccessibleName("結果ビューアのメッセージ");
    status->setReadOnly(true);
    status->setTabChangesFocus(true);
    status->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Keep two lines of status visible without resizing the view during orbital navigation.
    // Longer paths and errors remain selectable and scrollable within this fixed footer.
    status->setFixedHeight(2 * status->fontMetrics().lineSpacing() + 2 * status->frameWidth() +
                           2 * qCeil(status->document()->documentMargin()));
    auto *hint = new QLabel("結果は編集中の分子から独立しています。左ドラッグ: 回転／右ドラッグ: "
                            "移動／ホイール: 拡大縮小。結合は距離から推定します。");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *split = new QSplitter;
    layout->addWidget(split, 1);
    layout->addWidget(status);
    auto *left = new QWidget;
    auto *visual = new QVBoxLayout(left);
    visual->setContentsMargins(0, 0, 0, 0);
    auto *camera = new QHBoxLayout;
    visual->addLayout(camera);
    auto *fit = new QPushButton("表示を合わせる");
    camera->addWidget(fit);
    for (int axis = 0; axis < 3; ++axis) {
        auto *b = new QPushButton(QString("+%1").arg("XYZ"[axis]));
        camera->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, axis] { view->viewFrom(axis); });
    }
    camera->addStretch();
    auto *labelMode = new QComboBox;
    labelMode->setObjectName("resultsLabelMode");
    for (const auto &[label, mode] : {std::pair{"ラベル: すべて", "all"},
                                      {"ラベル: 水素以外", "heavy"},
                                      {"ラベル: 選択した原子", "selection"},
                                      {"ラベル: なし", "none"}})
        labelMode->addItem(label, mode);
    camera->addWidget(labelMode);
    view = new MoleculeView;
    view->setObjectName("resultsView");
    view->setAcceptDrops(false);
    view->setAngles(-25, 25, 0);
    view->setEmptyMessage("NWChem出力・XYZ・CUBE・MOLDENを開いて\n構造・軌道の等値面・振動を確認できます。");
    labelMode->setCurrentIndex(
        std::max(0, labelMode->findData(window->settings.value("results/labels", "all").toString())));
    view->setLabelMode(labelMode->currentData().toString());
    connect(labelMode, &QComboBox::currentIndexChanged, this, [this, labelMode] {
        view->setLabelMode(labelMode->currentData().toString());
        window->settings.setValue("results/labels", labelMode->currentData().toString());
    });
    visual->addWidget(view, 1);
    connect(fit, &QPushButton::clicked, view, &MoleculeView::fit);
    auto *angles = new QHBoxLayout;
    angles->setSpacing(16);
    visual->addLayout(angles);
    QSlider *sliders[3];
    const int initialAngles[] = {-25, 25, 0};
    for (int i = 0; i < 3; ++i) {
        auto *control = new QVBoxLayout;
        control->setSpacing(2);
        auto *label = new QLabel(QStringList{"左右回転", "上下回転", "画面内回転"}[i]);
        control->addWidget(label);
        sliders[i] = new QSlider(Qt::Horizontal);
        sliders[i]->setObjectName(QStringList{"resultsYaw", "resultsPitch", "resultsRoll"}[i]);
        sliders[i]->setRange(-180, 180);
        sliders[i]->setValue(initialAngles[i]);
        label->setBuddy(sliders[i]);
        control->addWidget(sliders[i]);
        angles->addLayout(control, 1);
    }
    for (int i = 0; i < 3; ++i)
        connect(sliders[i], &QSlider::valueChanged, this,
                [this, a = sliders[0], b = sliders[1], c = sliders[2]] {
                    view->setAngles(a->value(), b->value(), c->value());
                });
    // Mouse rotation and +X/+Y/+Z update the sliders, so they continue from the current view.
    connect(view, &MoleculeView::orientationChanged, this,
            [this, a = sliders[0], b = sliders[1], c = sliders[2]] {
                const auto angles = view->angles();
                const QSignalBlocker ba(a), bb(b), bc(c);
                a->setValue(qRound(angles.x()));
                b->setValue(qRound(angles.y()));
                c->setValue(qRound(angles.z()));
            });
    auto *navigation = new QHBoxLayout;
    visual->addLayout(navigation);
    navigation->addWidget(new QLabel("軌道"));
    const QStringList navigationNames{"First", "Back10", "Previous", "Next", "Forward10", "Last"};
    const QStringList navigationLabels{"先頭", "10", "1", "1", "10", "末尾"};
    const QStringList navigationTips{"最初の軌道へ", "10軌道戻る", "1軌道戻る",
                                     "1軌道進む",    "10軌道進む", "最後の軌道へ"};
    const QStyle::StandardPixmap navigationIcons[]{QStyle::SP_MediaSkipBackward, QStyle::SP_MediaSeekBackward,
                                                   QStyle::SP_ArrowLeft,         QStyle::SP_ArrowRight,
                                                   QStyle::SP_MediaSeekForward,  QStyle::SP_MediaSkipForward};
    for (int i = 0; i < 6; ++i) {
        auto *button = orbitalNavigation[i] =
            new QPushButton(style()->standardIcon(navigationIcons[i]), navigationLabels[i]);
        button->setObjectName("resultsOrbital" + navigationNames[i]);
        button->setToolTip(navigationTips[i]);
        button->setAccessibleName(navigationTips[i]);
        navigation->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, i] {
            const int last = data.orbitals.size() - 1;
            if (last < 0)
                return;
            const int offsets[]{0, -10, -1, 1, 10, 0};
            renderOrbital(i == 0   ? 0
                          : i == 5 ? last
                                   : std::clamp(data.selectedOrbital + offsets[i], 0, last));
        });
    }
    orbitalPosition = new QLabel;
    orbitalPosition->setObjectName("resultsOrbitalPosition");
    navigation->addWidget(orbitalPosition);
    navigation->addStretch();
    auto *frames = new QHBoxLayout;
    visual->addLayout(frames);
    frames->addWidget(new QLabel("構造"));
    frame = new QSpinBox;
    frame->setObjectName("resultsFrame");
    frames->addWidget(frame);
    playFrames = new QPushButton("構造列を再生");
    playFrames->setObjectName("resultsPlayFrames");
    frames->addWidget(playFrames);
    frames->addStretch();
    auto *measuring = new QHBoxLayout;
    visual->addLayout(measuring);
    measurementLabel = new QLabel("原子をクリックすると距離・角度・二面角を測定します（2〜4原子）。");
    measurementLabel->setObjectName("resultsMeasurement");
    measurementLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    measuring->addWidget(measurementLabel, 1);
    auto *pin = new QPushButton("計測を画面に表示");
    pin->setObjectName("resultsPinMeasurement");
    auto *clearMeasure = new QPushButton("計測を消去");
    clearMeasure->setObjectName("resultsClearMeasurement");
    measuring->addWidget(pin);
    measuring->addWidget(clearMeasure);
    connect(view, &MoleculeView::atomClicked, this, [this](const QString &id) { pickAtom(id); });
    connect(pin, &QPushButton::clicked, this, [this] {
        const auto &d = view->documentRef();
        Ids ids;
        for (int n : picked)
            if (n < d.atoms.size())
                ids << d.atoms[n].id;
        bool valid = false;
        window->safe([&] {
            if (picked.size() < 2 || ids.size() != picked.size())
                fail("2〜4原子をクリックしてから表示してください。");
            measure(d, ids); // Rejects coincident or collinear atoms before pinning.
            valid = true;
        });
        if (valid) {
            pinned << picked;
            picked.clear();
            setViewDocument(Document(view->documentCopy()));
        }
    });
    connect(clearMeasure, &QPushButton::clicked, this, [this] {
        picked.clear();
        pinned.clear();
        setViewDocument(Document(view->documentCopy()));
    });
    split->addWidget(left);
    auto *tabs = details = new QTabWidget;
    tabs->setObjectName("resultsDetails");
    split->addWidget(tabs);
    summary = new QLabel;
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    summary->setAlignment(Qt::AlignTop);
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(summary);
    tabs->addTab(scroll, "概要");
    auto *energyPage = new QWidget;
    auto *enl = new QVBoxLayout(energyPage);
    auto *energyRow = new QHBoxLayout;
    enl->addLayout(energyRow);
    energyUnit = new QComboBox;
    energyUnit->setObjectName("resultsEnergyUnit");
    for (const auto &[label, unit] : {std::pair{"ΔE: kcal/mol", "kcal"},
                                      {"ΔE: kJ/mol", "kj"},
                                      {"ΔE: eV", "ev"},
                                      {"ΔE: Hartree", "hartree"},
                                      {"ΔE: cm⁻¹", "wavenumber"}})
        energyUnit->addItem(label, unit);
    energyRow->addWidget(energyUnit);
    energyReference = new QPushButton("選択行をΔEの基準にする");
    energyReference->setObjectName("resultsEnergyReference");
    energyRow->addWidget(energyReference);
    energyRow->addStretch();
    energy = table({"手法", "全エネルギー / Eh", "ΔE", "ログ行"}, "resultsEnergy");
    enl->addWidget(energy, 1);
    energyNote = new QLabel;
    energyNote->setObjectName("resultsEnergyNote");
    energyNote->setWordWrap(true);
    enl->addWidget(energyNote);
    tabs->addTab(energyPage, "エネルギー");
    connect(energyUnit, &QComboBox::currentIndexChanged, this, &ResultsPanel::showEnergies);
    connect(energyReference, &QPushButton::clicked, this, [this] {
        if (energy->currentRow() >= 0 && energy->currentRow() < data.energies.size()) {
            referenceEnergy = energy->currentRow();
            showEnergies();
        }
    });
    auto *orbitalPage = new QWidget;
    auto *ol = new QVBoxLayout(orbitalPage);
    auto *orbitalActions = new QHBoxLayout;
    homo = new QPushButton("HOMO");
    lumo = new QPushButton("LUMO");
    homo->setObjectName("resultsHomo");
    lumo->setObjectName("resultsLumo");
    for (auto *b : {homo, lumo})
        orbitalActions->addWidget(b);
    orbitalActions->addStretch();
    ol->addLayout(orbitalActions);
    auto *orbitalOptions = new QFormLayout;
    orbitalIso = new QLineEdit("0.03");
    orbitalIso->setObjectName("resultsOrbitalIso");
    orbitalOptions->addRow("等値 / bohr⁻³ᐟ²（±）", orbitalIso);
    orbitalGrid = new QSpinBox;
    orbitalGrid->setObjectName("resultsOrbitalGrid");
    orbitalGrid->setRange(16, 100);
    orbitalGrid->setValue(40);
    orbitalOptions->addRow("各軸の格子分割数", orbitalGrid);
    orbitalNorm = new QComboBox;
    orbitalNorm->setObjectName("resultsOrbitalNorm");
    orbitalNorm->addItem("自動判定", "auto");
    orbitalNorm->addItem("標準MOLDEN（NWChemのNONE / JANPA）", "standard");
    orbitalNorm->addItem("NWChem（正規化因子を含む係数）", "nwchem");
    orbitalOptions->addRow("基底係数の正規化", orbitalNorm);
    ol->addLayout(orbitalOptions);
    orbitalStatus = new QLabel;
    orbitalStatus->setObjectName("resultsOrbitalStatus");
    orbitalStatus->setTextFormat(Qt::PlainText);
    orbitalStatus->setWordWrap(true);
    ol->addWidget(orbitalStatus);
    orbitals = table({"番号", "軌道", "対称性", "スピン", "エネルギー / Eh", "占有数"}, "resultsOrbitals");
    orbitals->verticalHeader()->hide();
    ol->addWidget(orbitals, 1);
    tabs->addTab(orbitalPage, "軌道");
    connect(orbitals, &QTableWidget::currentCellChanged, this, [this](int row, int, int previousRow, int) {
        if (row != previousRow)
            renderOrbital(row);
        updateActions();
    });
    const auto applyOrbitalOptions = [this] {
        if (loading())
            return;
        bool ok;
        const double iso = orbitalIso->text().toDouble(&ok);
        if (ok && data.selectedOrbital == orbitals->currentRow() && iso == data.isovalue &&
            orbitalGrid->value() == data.orbitalDivisions &&
            orbitalNorm->currentData().toString() == data.normalizationRequest &&
            sourceStamp(QFileInfo(data.source)) == orbitalCacheSource)
            return;
        renderOrbital(orbitals->currentRow());
    };
    connect(orbitalIso, &QLineEdit::editingFinished, this, applyOrbitalOptions);
    connect(orbitalGrid, &QSpinBox::editingFinished, this, applyOrbitalOptions);
    connect(orbitalNorm, &QComboBox::currentIndexChanged, this, applyOrbitalOptions);
    const auto frontier = [this](bool virtualOrbital) {
        const int row = orbitals->currentRow();
        const auto spin = row >= 0 && row < data.orbitals.size() ? data.orbitals[row].spin : QString("Alpha");
        const int target = frontierOrbital(data.orbitals, spin, virtualOrbital);
        if (data.format != "MOLDEN" && target >= 0)
            orbitals->selectRow(target); // Log orbitals have energies only.
        renderOrbital(target);
    };
    connect(homo, &QPushButton::clicked, this, [frontier] { frontier(false); });
    connect(lumo, &QPushButton::clicked, this, [frontier] { frontier(true); });
    auto *vibrations = new QWidget;
    auto *vl = new QVBoxLayout(vibrations);
    modes = table({"振動数 / cm⁻¹", "IR強度 / km mol⁻¹", "種類"}, "resultsModes");
    vl->addWidget(modes, 1);
    auto *modeRow = new QHBoxLayout;
    vl->addLayout(modeRow);
    playMode = new QPushButton("選択モードを再生");
    playMode->setObjectName("resultsPlayMode");
    modeRow->addWidget(playMode);
    amplitude = new QDoubleSpinBox;
    amplitude->setObjectName("resultsAmplitude");
    amplitude->setRange(.01, 2);
    amplitude->setSingleStep(.05);
    amplitude->setValue(.3);
    amplitude->setSuffix(" Å");
    modeRow->addWidget(amplitude);
    arrows = new QCheckBox("変位を矢印で表示");
    arrows->setObjectName("resultsModeArrows");
    modeRow->addWidget(arrows);
    modeRow->addStretch();
    auto *exportRow = new QHBoxLayout;
    vl->addLayout(exportRow);
    saveModeImages = new QPushButton("振動の連番画像を保存…");
    saveModeImages->setObjectName("resultsSaveModeImages");
    saveModeTrajectory = new QPushButton("振動をXYZ（複数構造）で保存…");
    saveModeTrajectory->setObjectName("resultsSaveModeXyz");
    exportRow->addWidget(saveModeImages);
    exportRow->addWidget(saveModeTrajectory);
    exportRow->addStretch();
    auto *irRow = new QHBoxLayout;
    vl->addLayout(irRow);
    irWidth = new QDoubleSpinBox;
    irWidth->setObjectName("resultsIrWidth");
    irWidth->setRange(1, 200);
    irWidth->setValue(20);
    irWidth->setPrefix("IRスペクトルの半値全幅 ");
    irWidth->setSuffix(" cm⁻¹");
    irRow->addWidget(irWidth);
    irRow->addStretch();
    irSpectrum = new ChartView;
    irSpectrum->setObjectName("resultsIrSpectrum");
    vl->addWidget(irSpectrum, 1);
    auto *modeHint = new QLabel(
        "振幅は表示用に規格化した最大変位です。負の振動数は虚振動を表します。振動数の絶対値が最も小さい"
        "3N−6個以外（直線分子は3N−5個以外）を並進・回転とみなし、IRスペクトル（ローレンツ関数で表示用に"
        "広げた曲線）から除きます。アニメーションの速さは実際の振動数を表しません。");
    modeHint->setWordWrap(true);
    vl->addWidget(modeHint);
    tabs->addTab(vibrations, "振動");
    connect(irWidth, &QDoubleSpinBox::valueChanged, this, &ResultsPanel::showIrSpectrum);
    connect(arrows, &QCheckBox::toggled, this, &ResultsPanel::showArrows);
    connect(amplitude, &QDoubleSpinBox::valueChanged, this, &ResultsPanel::showArrows);
    connect(saveModeImages, &QPushButton::clicked, this,
            [this] { window->safe([this] { exportMode(true); }); });
    connect(saveModeTrajectory, &QPushButton::clicked, this,
            [this] { window->safe([this] { exportMode(false); }); });
    auto *optimizationPage = new QWidget;
    auto *opl = new QVBoxLayout(optimizationPage);
    steps = table({"ステップ", "エネルギー / Eh", "ΔE / Eh", "Gmax", "Grms", "Xrms", "Xmax", "経過 / s"},
                  "resultsSteps");
    opl->addWidget(steps, 1);
    energyChart = new ChartView;
    energyChart->setObjectName("resultsEnergyChart");
    opl->addWidget(energyChart, 1);
    gradientChart = new ChartView;
    gradientChart->setObjectName("resultsGradientChart");
    opl->addWidget(gradientChart, 1);
    auto *stepHint =
        new QLabel("NWChem DRIVERの「@」行です。勾配はHartree/Bohr、変位はBohr。破線は収束閾値です。");
    stepHint->setWordWrap(true);
    opl->addWidget(stepHint);
    tabs->addTab(optimizationPage, "最適化");
    auto *excitedPage = new QWidget;
    auto *el = new QVBoxLayout(excitedPage);
    excitations = table({"状態", "スピン", "対称", "励起エネルギー / eV", "波長 / nm", "振動子強度", "<S²>"},
                        "resultsExcitations");
    el->addWidget(excitations, 1);
    auto *spectrumRow = new QHBoxLayout;
    el->addLayout(spectrumRow);
    spectrumAxis = new QComboBox;
    spectrumAxis->setObjectName("resultsSpectrumAxis");
    spectrumAxis->addItem("横軸: 波長 / nm", "nm");
    spectrumAxis->addItem("横軸: 励起エネルギー / eV", "ev");
    spectrumRow->addWidget(spectrumAxis);
    broadening = new QDoubleSpinBox;
    broadening->setObjectName("resultsBroadening");
    broadening->setRange(.05, 2);
    broadening->setSingleStep(.05);
    broadening->setValue(.3);
    broadening->setPrefix("半値全幅 ");
    broadening->setSuffix(" eV");
    spectrumRow->addWidget(broadening);
    spectrumRow->addStretch();
    spectrum = new ChartView;
    spectrum->setObjectName("resultsSpectrum");
    el->addWidget(spectrum, 1);
    auto *spectrumHint =
        new QLabel("各状態の振動子強度（棒）に、エネルギー軸で指定した半値全幅のガウス関数を掛けて重ね"
                   "た表示用の曲線です。三重項など振動子強度0の状態は棒に現れません。");
    spectrumHint->setWordWrap(true);
    el->addWidget(spectrumHint);
    tabs->addTab(excitedPage, "励起状態");
    connect(spectrumAxis, &QComboBox::currentIndexChanged, this, &ResultsPanel::showSpectrum);
    connect(broadening, &QDoubleSpinBox::valueChanged, this, &ResultsPanel::showSpectrum);
    auto *pathPage = new QWidget;
    auto *pl = new QVBoxLayout(pathPage);
    path = table({"点", "座標・方向", "値", "エネルギー / Eh", "ΔE / kcal mol⁻¹"}, "resultsPath");
    pl->addWidget(path, 1);
    pathChart = new ChartView;
    pathChart->setObjectName("resultsPathChart");
    pl->addWidget(pathChart, 1);
    auto *pathHint =
        new QLabel("座標スキャン: 各点の最後のエネルギー（緩和スキャンは最適化後）で、ΔEは最小値基準です。"
                   "IRC: 遷移状態を0とし、前進方向を正・後退方向を負の反応座標（amu½·Bohr）で表示し、"
                   "ΔEは遷移状態基準です。行を選ぶとその構造を表示します。");
    pathHint->setWordWrap(true);
    pl->addWidget(pathHint);
    tabs->addTab(pathPage, "経路・スキャン");
    connect(path, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (row < 0 || !path->item(row, 0))
            return;
        const int index = path->item(row, 0)->data(Qt::UserRole).toInt();
        if (index >= 0 && index < data.frames.size()) {
            stopAnimation();
            frame->setValue(index + 1);
            showFrame();
        }
    });
    auto *propertyPage = new QWidget;
    auto *ppl = new QVBoxLayout(propertyPage);
    propertySummary = new QLabel;
    propertySummary->setObjectName("resultsPropertySummary");
    propertySummary->setTextFormat(Qt::PlainText);
    propertySummary->setWordWrap(true);
    propertySummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ppl->addWidget(propertySummary);
    atomProperties =
        table({"番号", "元素", "Mulliken電荷", "ESP電荷", "RESP電荷", "NMR遮蔽 / ppm", "異方性 / ppm"},
              "resultsAtomProperties");
    atomProperties->verticalHeader()->hide();
    ppl->addWidget(atomProperties, 1);
    tabs->addTab(propertyPage, "物性");
    auto *surfaces = new QWidget;
    auto *sf = new QFormLayout(surfaces);
    isovalue = new QLineEdit("0.03");
    isovalue->setObjectName("resultsIsovalue");
    sf->addRow("等値（ファイルの単位）", isovalue);
    dataset = new QComboBox;
    dataset->setObjectName("resultsDataset");
    sf->addRow("CUBEデータ", dataset);
    applyIso = new QPushButton("等値面を再生成");
    applyIso->setObjectName("resultsApplyIso");
    sf->addRow(applyIso);
    auto *positive = new QCheckBox("正の面（赤）");
    auto *negative = new QCheckBox("負の面（青）");
    positive->setChecked(true);
    negative->setChecked(true);
    sf->addRow(positive);
    sf->addRow(negative);
    auto *opacity = new QSlider(Qt::Horizontal);
    opacity->setRange(20, 255);
    opacity->setValue(150);
    sf->addRow("不透明度", opacity);
    auto style = [this, positive, negative, opacity] {
        view->setSurfaceStyle(positive->isChecked(), negative->isChecked(), opacity->value());
    };
    connect(positive, &QCheckBox::toggled, this, style);
    connect(negative, &QCheckBox::toggled, this, style);
    connect(opacity, &QSlider::valueChanged, this, style);
    auto *surfaceHint = new QLabel("CUBEとMOLDEN軌道の正負・不透明度を変更できます。CUBEの値の意味は生成"
                                   "条件で確認してください。MOLDENの軌道と等値は「軌道」で選択します。");
    surfaceHint->setWordWrap(true);
    sf->addRow(surfaceHint);
    tabs->addTab(surfaces, "CUBE / 等値面");
    log = new QPlainTextEdit;
    log->setObjectName("resultsLog");
    log->setReadOnly(true);
    log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log->setLineWrapMode(QPlainTextEdit::NoWrap);
    tabs->addTab(log, "元ファイル");
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setSizes({780, 540});
    connect(open, &QPushButton::clicked, this, [this] {
        const auto p = QFileDialog::getOpenFileName(
            this, "結果ファイルを開く", folder,
            "NWChem・可視化 (*.out *.log *.xyz *.cube *.cub *.molden *.mld *.molde);;すべて (*)");
        if (!p.isEmpty())
            openFile(p);
    });
    connect(directory, &QPushButton::clicked, this, [this] {
        const auto p = QFileDialog::getExistingDirectory(this, "ジョブフォルダーを開く", folder);
        if (!p.isEmpty())
            openDirectory(p);
    });
    connect(files, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0)
            window->safe([&] { openEntry(files->itemData(index).toString()); });
    });
    connect(reload, &QPushButton::clicked, this, [this] {
        startLoad(data.source, data.isovalue, data.dataset, data.selectedOrbital, data.normalizationRequest,
                  data.orbitalDivisions);
    });
    connect(cancel, &QPushButton::clicked, this, &ResultsPanel::cancelLoad);
    connect(frame, &QSpinBox::valueChanged, this, [this] {
        if (!animation.isActive())
            showFrame();
    });
    connect(playFrames, &QPushButton::clicked, this, [this] {
        if (animation.isActive()) {
            stopAnimation();
            showFrame();
        } else {
            phase = 0;
            animation.setInterval(180);
            animation.start();
            playFrames->setText("再生を停止");
        }
    });
    connect(playMode, &QPushButton::clicked, this, [this] {
        if (animation.isActive()) {
            stopAnimation();
            showFrame();
        } else {
            phase = 0;
            animation.setInterval(40);
            animation.start();
            playMode->setText("再生を停止");
            view->setArrows({}); // Arrows belong to the reference structure, not the moving one.
            showMode();
        }
    });
    connect(modes, &QTableWidget::currentCellChanged, this, [this] {
        stopAnimation();
        updateActions();
        if (modes->currentRow() >= 0 && modes->currentRow() < data.modes.size() &&
            !data.modes[modes->currentRow()].displacement.isEmpty()) {
            modeDisplayed = true;
            setViewDocument(data.vibrationGeometry);
            view->setSurface({});
        }
        showArrows();
    });
    connect(&animation, &QTimer::timeout, this, [this] {
        if (animation.interval() == 180) {
            frame->setValue(frame->value() % data.frames.size() + 1);
            showFrame();
        } else {
            phase += .18;
            showMode();
        }
    });
    connect(applyIso, &QPushButton::clicked, this, [this] {
        bool ok;
        const double n = isovalue->text().toDouble(&ok);
        if (!ok || !std::isfinite(n) || n <= 0 || n > 1e6) {
            error("等値は0より大きい有限の数で指定してください。");
            return;
        }
        startLoad(data.source, n, dataset->currentIndex());
    });
    connect(applyGeometry, &QPushButton::clicked, this,
            [this] { window->safe([this] { applyStructure(); }); });
    connect(compareGeometry, &QPushButton::clicked, this, [this] {
        window->safe([this] {
            const auto fit = superpose(displayedGeometry(), window->document());
            status->setPlainText(
                QString("編集中の分子との比較: RMSD（座標そのまま）%1 Å、重ね合わせ後 %2 Å（水素以外 "
                        "%3 Å）、最大のずれ %4 Å")
                    .arg(fit.raw, 0, 'f', 4)
                    .arg(fit.rmsd, 0, 'f', 4)
                    .arg(fit.heavyRmsd, 0, 'f', 4)
                    .arg(fit.maximum, 0, 'f', 4));
        });
    });
    connect(saveXyz, &QPushButton::clicked, this, [this] {
        window->safe([&] {
            const auto p =
                QFileDialog::getSaveFileName(this, "表示中の基準構造を保存", "result.xyz", "XYZ (*.xyz)");
            if (!p.isEmpty())
                saveGeometry(p.endsWith(".xyz", Qt::CaseInsensitive) ? p : p + ".xyz");
        });
    });
    connect(savePng, &QPushButton::clicked, this, [this] {
        window->safe([&] {
            auto p = QFileDialog::getSaveFileName(this, "結果画像を保存", "result.png", "PNG (*.png)");
            if (p.isEmpty())
                return;
            if (!p.endsWith(".png", Qt::CaseInsensitive))
                p += ".png";
            if (!view->grab().save(p))
                fail("画像を保存できません。");
        });
    });
    updateActions();
}
ResultsPanel::~ResultsPanel() {
    cancelLoad();
}
void ResultsPanel::stopAnimation() {
    animation.stop();
    playFrames->setText("構造列を再生");
    playMode->setText("選択モードを再生");
}
void ResultsPanel::error(const QString &message) {
    status->setPlainText("読込・表示エラー: " + message + "\n前の結果は保持しています。");
    emit loadFailed(message);
}
void ResultsPanel::cancelLoad() {
    if (!process)
        return;
    auto *p = process;
    process = nullptr;
    p->disconnect(this);
    p->kill();
    p->waitForFinished(2000);
    p->deleteLater();
    work.reset();
    pendingPath.clear();
    status->setPlainText("読込を中止しました。前の結果を保持しています。");
    updateActions();
}
void ResultsPanel::openFile(const QString &path) {
    cancelLoad();
    bool ok = true;
    const auto suffix = QFileInfo(path).suffix().toLower();
    const double iso = suffix == "cube" || suffix == "cub" ? isovalue->text().toDouble(&ok) : .03;
    if (!ok || !std::isfinite(iso) || iso <= 0 || iso > 1e6) {
        error("CUBEの等値は0より大きい有限の数で指定してください。");
        return;
    }
    startLoad(path, iso, 0);
}
void ResultsPanel::openDirectory(const QString &path) {
    cancelLoad();
    QDir dir(path);
    if (!dir.exists()) {
        error("フォルダーが見つかりません。");
        return;
    }
    folder = dir.absolutePath();
    fillFiles("output.out");
    if (files->currentIndex() < 0) {
        error("対応する結果ファイルがありません。");
        return;
    }
    window->safe([&] { openEntry(files->currentData().toString()); });
}
void ResultsPanel::fillFiles(const QString &selected) {
    const QSignalBlocker guard(files);
    files->clear();
    animationFrames.clear();
    static const QRegularExpression step(R"(^(.*)\.m-(\d+)\.s-(\d+)\.xyz$)");
    const auto kind = [](const QString &name) {
        const auto suffix = QFileInfo(name).suffix().toLower();
        return suffix == "out" || suffix == "log"    ? 0
               : suffix == "xyz"                     ? 1
               : suffix == "cube" || suffix == "cub" ? 2
                                                     : 3;
    };
    QVector<QStringList> groups(4);
    QMap<QPair<QString, int>, QStringList> modes;
    for (const auto &name : resultFiles(folder)) {
        if (const auto m = step.match(name); m.hasMatch())
            modes[{m.captured(1), m.captured(2).toInt()}] << name;
        else
            groups[kind(name)] << name;
    }
    // Logs, structures, CUBE, MOLDEN, then the animations, separated by kind.
    for (const auto &group : groups) {
        if (group.isEmpty())
            continue;
        if (files->count())
            files->insertSeparator(files->count());
        for (const auto &name : group)
            files->addItem(name, name);
    }
    if (!modes.isEmpty() && files->count())
        files->insertSeparator(files->count());
    for (auto it = modes.begin(); it != modes.end(); ++it) {
        auto list = it.value();
        std::sort(list.begin(), list.end(), [](const QString &a, const QString &b) {
            return step.match(a).captured(3).toInt() < step.match(b).captured(3).toInt();
        });
        const auto key = QString("animation:%1:%2").arg(it.key().first).arg(it.key().second);
        animationFrames[key] = list;
        files->addItem(QString("振動アニメーション %1: モード %2（%3枚）")
                           .arg(QFileInfo(it.key().first).fileName())
                           .arg(it.key().second)
                           .arg(list.size()),
                       key);
    }
    int index = files->findData(selected);
    if (index < 0 && !selected.isEmpty() && !selected.startsWith("animation:")) {
        files->addItem(selected, selected);
        index = files->count() - 1;
    }
    for (int i = 0; index < 0 && i < files->count(); ++i)
        if (!files->itemData(i).toString().isEmpty())
            index = i;
    files->setCurrentIndex(index);
}
void ResultsPanel::openEntry(const QString &entry) {
    if (!entry.startsWith("animation:")) {
        openFile(QDir(folder).filePath(entry));
        return;
    }
    // Join the steps of one mode into a multi-frame XYZ in a private temporary folder.
    const auto list = animationFrames.value(entry);
    if (list.isEmpty() || !animationDir.isValid())
        fail("振動アニメーションのファイルが見つかりません。");
    QByteArray joined;
    for (const auto &name : list) {
        joined += readFile(QDir(folder).filePath(name));
        if (!joined.endsWith('\n'))
            joined += '\n';
        if (joined.size() > 64 * 1024 * 1024)
            fail("振動アニメーションのファイルが大きすぎます。");
    }
    const auto path = animationDir.filePath(QString("animation-%1.xyz").arg(animationSources.size() + 1));
    writeFile(path, joined);
    animationSources[path] = entry;
    openFile(path);
}
void ResultsPanel::renderOrbital(int orbital) {
    if (data.format != "MOLDEN" || orbital < 0 || orbital >= data.orbitals.size())
        return;
    cancelLoad();
    bool ok;
    const double iso = orbitalIso->text().toDouble(&ok);
    if (!ok || !std::isfinite(iso) || iso <= 0 || iso > 1e6) {
        error("軌道の等値は0より大きい有限の数で指定してください。");
        return;
    }
    startLoad(data.source, iso, 0, orbital, orbitalNorm->currentData().toString(), orbitalGrid->value(),
              true);
}
void ResultsPanel::startLoad(const QString &path, double iso, int channel, int orbital,
                             const QString &normalization, int divisions, bool reuseOrbital) {
    cancelLoad();
    stopAnimation();
    if (path.isEmpty())
        return;
    const QFileInfo before(path);
    const auto stamp = sourceStamp(before);
    const bool sameSource = !stamp.isEmpty() && stamp == orbitalCacheSource;
    if (!reuseOrbital || !sameSource) {
        orbitalCache.clear();
        orbitalCacheSource.clear();
    }
    if (reuseOrbital && !stamp.isEmpty()) {
        if (const auto *cached = orbitalCache.object(orbitalKey(orbital, iso, normalization, divisions))) {
            data.surface = cached->triangles;
            data.grid = cached->grid;
            data.minimum = cached->minimum;
            data.maximum = cached->maximum;
            data.orbitalNormalization = cached->normalization;
            data.warnings = cached->warnings;
            data.orbitalError.clear();
            data.selectedOrbital = orbital;
            data.isovalue = iso;
            data.normalizationRequest = normalization;
            data.orbitalDivisions = divisions;
            display(true);
            emit loaded();
            return;
        }
    }
    work = std::make_unique<QTemporaryDir>();
    if (!work->isValid()) {
        error("一時フォルダーを作成できません。");
        return;
    }
    pendingPath = before.absoluteFilePath();
    const QString request = work->filePath("request.json"), response = work->filePath("result.bin");
    try {
        writeFile(request, QJsonDocument(QJsonObject{{"path", pendingPath},
                                                     {"isovalue", iso},
                                                     {"dataset", channel},
                                                     {"orbital", orbital},
                                                     {"normalization", normalization},
                                                     {"orbital_divisions", divisions}})
                               .toJson());
    } catch (const std::exception &e) {
        error(QString::fromUtf8(e.what()));
        work.reset();
        return;
    }
    process = new QProcess(this);
    auto *p = process;
    p->setProgram(QCoreApplication::applicationFilePath());
    p->setArguments({"--result-worker", request, response});
    p->setStandardOutputFile(QProcess::nullDevice());
    p->setStandardErrorFile(QProcess::nullDevice());
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (process != p || e != QProcess::FailedToStart)
            return;
        const auto message = p->errorString();
        process = nullptr;
        p->deleteLater();
        work.reset();
        updateActions();
        error(message);
    });
    connect(
        p, &QProcess::finished, this,
        [this, p, before, stamp, response, reuseOrbital, sameSource](int code, QProcess::ExitStatus state) {
            if (process != p)
                return;
            process = nullptr;
            p->deleteLater();
            QString failure;
            try {
                if (code != 0 || state != QProcess::NormalExit) {
                    const QString e = QFileInfo::exists(response + ".error")
                                          ? QString::fromUtf8(readFile(response + ".error"))
                                          : "結果の読込プロセスが失敗しました。";
                    fail(e);
                }
                const QFileInfo after(before.absoluteFilePath());
                if (stamp.isEmpty() || sourceStamp(after) != stamp)
                    fail("読込中にファイルが更新されました。再読込してください。");
                auto next = loadResultData(response);
                const bool newSource = data.source != next.source;
                data = std::move(next);
                orbitalCacheSource = stamp;
                cacheOrbital();
                display(reuseOrbital && sameSource && !newSource);
                if (newSource && data.format == "MOLDEN" && !data.orbitals.isEmpty())
                    details->setCurrentIndex(2);
                else if (newSource && data.format == "NWChem" && !data.energies.isEmpty())
                    details->setCurrentIndex(1);
                if (newSource) {
                    // The source path and the selected details tab can resize the view.
                    layout()->activate();
                    view->fit();
                }
            } catch (const std::exception &e) {
                failure = QString::fromUtf8(e.what());
            }
            pendingPath.clear();
            work.reset();
            updateActions();
            if (failure.isEmpty())
                emit loaded();
            else
                error(failure);
        });
    status->setPlainText("読込中: " + pendingPath + "\n完了まで前の結果を表示します。");
    updateActions();
    p->start();
}
void ResultsPanel::cacheOrbital() {
    if (data.format != "MOLDEN" || data.selectedOrbital < 0 || orbitalCacheSource.isEmpty())
        return;
    qsizetype cost = sizeof(OrbitalSurface) + data.surface.capacity() * sizeof(SurfaceTriangle) + 1024;
    for (const auto &warning : data.warnings)
        cost += warning.size() * sizeof(QChar);
    if (cost > orbitalCache.maxCost())
        return;
    orbitalCache.insert(
        orbitalKey(data.selectedOrbital, data.isovalue, data.normalizationRequest, data.orbitalDivisions),
        new OrbitalSurface{data.surface, data.grid, data.minimum, data.maximum, data.orbitalNormalization,
                           data.warnings},
        int(cost));
}
void ResultsPanel::updateActions() {
    const bool ready = !loading() && !data.source.isEmpty();
    cancel->setEnabled(loading());
    reload->setEnabled(ready);
    saveXyz->setEnabled(ready && !data.frames.isEmpty());
    applyGeometry->setEnabled(ready && !data.frames.isEmpty());
    compareGeometry->setEnabled(applyGeometry->isEnabled());
    savePng->setEnabled(ready && !data.frames.isEmpty());
    frame->setEnabled(ready && !data.frames.isEmpty());
    playFrames->setEnabled(ready && data.frames.size() > 1);
    const int mode = modes->currentRow();
    playMode->setEnabled(ready && mode >= 0 && mode < data.modes.size() &&
                         !data.modes[mode].displacement.isEmpty());
    saveModeImages->setEnabled(playMode->isEnabled());
    saveModeTrajectory->setEnabled(playMode->isEnabled());
    energyReference->setEnabled(ready && !data.energies.isEmpty());
    applyIso->setEnabled(ready && data.format == "CUBE");
    dataset->setEnabled(applyIso->isEnabled());
    const int selected = orbitals->currentRow();
    const bool orbitalReady = ready && data.format == "MOLDEN" && data.orbitalBasis;
    orbitalIso->setEnabled(orbitalReady);
    orbitalGrid->setEnabled(orbitalReady);
    orbitalNorm->setEnabled(orbitalReady);
    const auto spin =
        selected >= 0 && selected < data.orbitals.size() ? data.orbitals[selected].spin : QString("Alpha");
    homo->setEnabled(orbitalReady && frontierOrbital(data.orbitals, spin) >= 0);
    lumo->setEnabled(orbitalReady && frontierOrbital(data.orbitals, spin, true) >= 0);
    for (int i = 0; i < 6; ++i)
        orbitalNavigation[i]->setEnabled(
            orbitalReady && data.selectedOrbital >= 0 &&
            (i < 3 ? data.selectedOrbital > 0 : data.selectedOrbital < data.orbitals.size() - 1));
    orbitalPosition->setText(data.selectedOrbital >= 0
                                 ? QString("%1 %2番").arg(data.orbitals[data.selectedOrbital].spin,
                                                          orbitals->item(data.selectedOrbital, 0)->text())
                                 : QString());
}
void ResultsPanel::display(bool orbitalOnly) {
    source->setText(data.source);
    status->setPlainText(data.status);
    if (!orbitalOnly) {
        QString selectedFile = animationSources.value(data.source);
        for (int i = 0; selectedFile.isEmpty() && i < files->count(); ++i) {
            const auto entry = files->itemData(i).toString();
            if (!entry.isEmpty() && !entry.startsWith("animation:") &&
                QFileInfo(QDir(folder).filePath(entry)).absoluteFilePath() ==
                    QFileInfo(data.source).absoluteFilePath())
                selectedFile = entry;
        }
        if (selectedFile.isEmpty()) {
            folder = QFileInfo(data.source).absolutePath();
            selectedFile = QFileInfo(data.source).fileName();
        }
        fillFiles(selectedFile);
    }
    QString info = QString("形式: %1\n構造: %2 / エネルギー: %3 / 軌道: %4 / 振動: %5\n")
                       .arg(data.format)
                       .arg(data.frames.size())
                       .arg(data.energies.size())
                       .arg(data.orbitals.size())
                       .arg(data.modes.size());
    if (!data.frames.isEmpty())
        info += QString("最終表示構造: %1（%2原子）\n")
                    .arg(data.frames.last().molecule.formula())
                    .arg(data.frames.last().molecule.atoms.size());
    if (!data.energies.isEmpty())
        info += "最後の記録: " + data.energies.last().method + " " + real(data.energies.last().hartree) +
                " Eh\nエネルギーは出力順です。最適化ステップとの1対1対応ではありません。\n";
    if (data.format == "CUBE" || data.selectedOrbital >= 0)
        info += QString("格子: %1 × %2 × %3\n値の範囲: %4 ～ %5\n等値: ±%6 / 三角形: %7\n")
                    .arg(data.grid[0])
                    .arg(data.grid[1])
                    .arg(data.grid[2])
                    .arg(real(data.minimum), real(data.maximum), real(data.isovalue))
                    .arg(data.surface.size());
    if (!data.steps.isEmpty())
        info += QString("最適化: %1（%2ステップ、最終 %3 Eh）\n")
                    .arg(data.properties.value("optimization") == "converged" ? "収束" : "収束メッセージなし")
                    .arg(data.steps.last().step)
                    .arg(real(data.steps.last().energy));
    for (const auto &value : data.properties.value("thermochemistry").toArray()) {
        const auto t = value.toObject();
        const double temperature = t["temperature"].toDouble(), h = t["enthalpy_kcal"].toDouble(),
                     g = h - temperature * t["entropy_cal"].toDouble() / 1000;
        info +=
            QString("熱化学（%1 K、対称数 %2）: ZPE %3 kcal/mol、内部エネルギー補正 %4 kcal/mol、"
                    "エンタルピー補正 %5 kcal/mol、エントロピー %6 cal/(mol·K)、"
                    "Gibbs補正 G = H − TS %7 kcal/mol（%8 Eh、算出値）\n")
                .arg(fixed(temperature, 2), real(t["symmetry_number"].toDouble()),
                     fixed(t["zpe_kcal"].toDouble(), 3), fixed(t["energy_kcal"].toDouble(), 3), fixed(h, 3),
                     fixed(t["entropy_cal"].toDouble(), 3), fixed(g, 3), fixed(g / HartreeToKcal, 6));
    }
    if (!data.properties.value("thermochemistry").toArray().isEmpty())
        info += "補正は電子エネルギーに加えて使います。対称数はNWChemの値です（MolFormaの入力では1）。\n";
    if (data.properties.contains("dipole")) {
        const auto d = data.properties.value("dipole").toObject();
        info += QString("双極子モーメント: %1 D（x %2 / y %3 / z %4 a.u.）\n")
                    .arg(fixed(d["debye"].toDouble(), 4), real(d["x"].toDouble()), real(d["y"].toDouble()),
                         real(d["z"].toDouble()));
    }
    for (const auto &spin : {QString("Alpha"), QString("Beta")}) {
        const int homo = frontierOrbital(data.orbitals, spin),
                  lumo = frontierOrbital(data.orbitals, spin, true);
        if (homo >= 0 && lumo >= 0)
            info +=
                QString("HOMO–LUMOギャップ（%1）: %2 eV\n")
                    .arg(spin,
                         fixed((data.orbitals[lumo].energy - data.orbitals[homo].energy) * HartreeToEv, 3));
    }
    if (!data.modes.isEmpty()) {
        const auto rigid = resultRigidModes(data);
        int imaginary = 0;
        QStringList values;
        for (int i = 0; i < data.modes.size(); ++i)
            if (!rigid.value(i) && data.modes[i].frequency < -10) {
                ++imaginary;
                values << fixed(data.modes[i].frequency, 1);
            }
        info += QString("虚振動: %1個（並進・回転を除く−10 cm⁻¹未満%2）\n")
                    .arg(imaginary)
                    .arg(values.isEmpty() ? QString() : "、" + values.join(", ") + " cm⁻¹");
    }
    info += resultPropertyLines(data);
    if (!data.excitations.isEmpty()) {
        const auto strongest =
            std::max_element(data.excitations.begin(), data.excitations.end(),
                             [](const auto &a, const auto &b) { return a.oscillator < b.oscillator; });
        info += QString("励起状態: %1件（最大の振動子強度 %2、%3 eV / %4 nm）\n")
                    .arg(data.excitations.size())
                    .arg(fixed(strongest->oscillator, 4), fixed(strongest->ev, 3),
                         fixed(EvNanometer / strongest->ev, 1));
    }
    info += "\n" + data.warnings.join("\n\n");
    summary->setText(info);
    if (!orbitalOnly) {
        log->setPlainText(data.log);
        referenceEnergy = -1;
        picked.clear();
        pinned.clear();
        showEnergies();
    }
    {
        const QSignalBlocker guard(orbitals);
        if (!orbitalOnly) {
            orbitals->setRowCount(data.orbitals.size());
            QMap<QString, int> indices;
            QMap<QString, int> homos, lumos;
            for (const auto &spin : {QString("Alpha"), QString("Beta")}) {
                homos[spin] = frontierOrbital(data.orbitals, spin);
                lumos[spin] = frontierOrbital(data.orbitals, spin, true);
            }
            for (int i = 0; i < data.orbitals.size(); ++i) {
                const auto &o = data.orbitals[i];
                const QString frontier = i == homos[o.spin]   ? "HOMO"
                                         : i == lumos[o.spin] ? "LUMO"
                                                              : QString();
                row(orbitals, i,
                    {QString::number(++indices[o.spin]), frontier, o.symmetry, o.spin, real(o.energy),
                     real(o.occupation)});
            }
        }
        orbitals->clearSelection();
        orbitals->setCurrentCell(data.selectedOrbital, 0);
        if (data.selectedOrbital >= 0)
            orbitals->scrollToItem(orbitals->item(data.selectedOrbital, 0));
    }
    {
        const QSignalBlocker isoGuard(orbitalIso), gridGuard(orbitalGrid), normGuard(orbitalNorm);
        orbitalIso->setText(real(data.isovalue));
        orbitalGrid->setValue(data.orbitalDivisions);
        orbitalNorm->setCurrentIndex(orbitalNorm->findData(data.normalizationRequest));
    }
    orbitalStatus->setText(
        data.format == "NWChem" && !data.orbitals.isEmpty()
            ? "ログに出力された軌道エネルギーです。形状の表示にはMOLDENファイルを開いてください。"
        : data.format != "MOLDEN"      ? "MOLDENファイルを開くと分子軌道を表示します。"
        : !data.orbitalError.isEmpty() ? data.orbitalError
        : data.selectedOrbital >= 0
            ? QString("表示中: %1 %2番 / 等値 ±%3 / 正規化: "
                      "%4\n一覧の行を選ぶと表示を切り替えます。赤: 正 / 青: 負。")
                  .arg(data.orbitals[data.selectedOrbital].spin,
                       orbitals->item(data.selectedOrbital, 0)->text(), real(data.isovalue),
                       data.orbitalNormalization == "nwchem" ? "NWChem" : "標準MOLDEN")
            : "このファイルには表示可能な分子軌道がありません。");
    if (!orbitalOnly) {
        const QSignalBlocker guard(modes);
        modes->setRowCount(data.modes.size());
        rigidMotion = resultRigidModes(data);
        for (int i = 0; i < data.modes.size(); ++i) {
            const auto &m = data.modes[i];
            row(modes, i,
                {real(m.frequency), real(m.intensity),
                 rigidMotion.value(i) ? "並進・回転"
                 : m.frequency < 0    ? "虚振動"
                                      : "振動"});
        }
        modes->clearSelection();
        modes->setCurrentCell(-1, -1);
        showIrSpectrum();
        showPath();
        showProperties();
        steps->setRowCount(data.steps.size());
        QVector<QPointF> relative, gmax, grms;
        for (int i = 0; i < data.steps.size(); ++i) {
            const auto &t = data.steps[i];
            row(steps, i,
                {QString::number(t.step), fixed(t.energy, 8), real(t.deltaE), real(t.gmax), real(t.grms),
                 real(t.xrms), real(t.xmax), real(t.seconds)});
            relative << QPointF(t.step, (t.energy - data.steps.last().energy) * HartreeToKcal);
            gmax << QPointF(t.step, t.gmax);
            grms << QPointF(t.step, t.grms);
        }
        if (data.steps.isEmpty()) {
            energyChart->clear();
            gradientChart->clear();
        } else {
            energyChart->setChart("エネルギー（最終ステップ基準）", "ステップ", "ΔE / kcal mol⁻¹",
                                  {{"エネルギー", relative, QColor("#2f6db5")}});
            const auto thresholds = data.properties.value("thresholds").toObject();
            QVector<QPair<QString, double>> levels;
            if (thresholds.contains("gmax"))
                levels.append({"gmax閾値", thresholds["gmax"].toDouble()});
            if (thresholds.contains("grms"))
                levels.append({"grms閾値", thresholds["grms"].toDouble()});
            gradientChart->setChart("勾配", "ステップ", "Hartree / Bohr",
                                    {{"Gmax", gmax, QColor("#c0392b")}, {"Grms", grms, QColor("#27845a")}},
                                    true, levels);
        }
        excitations->setRowCount(data.excitations.size());
        for (int i = 0; i < data.excitations.size(); ++i) {
            const auto &x = data.excitations[i];
            row(excitations, i,
                {QString::number(x.root),
                 x.spin.isEmpty()      ? "非制限"
                 : x.spin == "singlet" ? "一重項"
                                       : "三重項",
                 x.symmetry, fixed(x.ev, 4), fixed(EvNanometer / x.ev, 1), fixed(x.oscillator, 5),
                 std::isfinite(x.s2) ? fixed(x.s2, 4) : "—"});
        }
        showSpectrum();
    }
    {
        const QSignalBlocker guard(frame);
        frame->setRange(data.frames.isEmpty() ? 0 : 1, data.frames.size());
        frame->setValue(data.selectedOrbital >= 0 ? data.orbitalFrame + 1 : data.frames.size());
        frame->setSuffix(QString(" / %1").arg(data.frames.size()));
    }
    dataset->clear();
    for (int n : data.datasets)
        dataset->addItem(QString::number(n));
    dataset->setCurrentIndex(data.dataset);
    isovalue->setText(real(data.isovalue));
    showFrame();
    updateActions();
}
void ResultsPanel::showSpectrum() {
    const bool nm = spectrumAxis->currentData() == "nm";
    const double fwhm = broadening->value(), sigma = fwhm / (2 * std::sqrt(2 * std::log(2.)));
    QVector<QPointF> sticks, curve;
    double lo = INFINITY, hi = -INFINITY;
    for (const auto &x : data.excitations)
        if (x.ev > 0) {
            lo = std::min(lo, x.ev);
            hi = std::max(hi, x.ev);
            if (x.oscillator > 0)
                sticks << QPointF(nm ? EvNanometer / x.ev : x.ev, x.oscillator);
        }
    if (!(hi > 0) || sticks.isEmpty()) {
        spectrum->clear();
        return;
    }
    lo = std::max(.5, lo - 3 * fwhm);
    hi += 3 * fwhm;
    for (int k = 0; k <= 400; ++k) {
        const double e = lo + (hi - lo) * k / 400;
        double y = 0;
        for (const auto &x : data.excitations)
            y += x.oscillator * std::exp(-.5 * std::pow((e - x.ev) / sigma, 2));
        curve << QPointF(nm ? EvNanometer / e : e, y);
    }
    spectrum->setChart("吸収スペクトル（表示用）", nm ? "波長 / nm" : "励起エネルギー / eV", "振動子強度",
                       {{"広がり", curve, QColor("#2f6db5")}, {"各状態", sticks, QColor("#c0392b"), true}});
}
void ResultsPanel::showEnergies() {
    const auto unit = energyUnit->currentData().toString();
    const double factor = unit == "kcal"         ? HartreeToKcal
                          : unit == "kj"         ? 2625.499639
                          : unit == "ev"         ? HartreeToEv
                          : unit == "wavenumber" ? 219474.6313632
                                                 : 1.;
    const QString label = energyUnit->currentText().section(':', 1).trimmed();
    energy->setHorizontalHeaderItem(2, new QTableWidgetItem("ΔE / " + label));
    const int reference = referenceEnergy >= 0 && referenceEnergy < data.energies.size()
                              ? referenceEnergy
                              : int(data.energies.size()) - 1;
    const QSignalBlocker guard(energy);
    energy->setRowCount(data.energies.size());
    for (int i = 0; i < data.energies.size(); ++i) {
        const auto &e = data.energies[i];
        row(energy, i,
            {e.method, real(e.hartree),
             fixed((e.hartree - data.energies[reference].hartree) * factor, unit == "hartree" ? 8 : 3),
             e.line ? QString::number(e.line) : "—"});
        if (i == reference)
            for (int c = 0; c < 4; ++c)
                energy->item(i, c)->setFont([&] {
                    auto f = energy->font();
                    f.setBold(true);
                    return f;
                }());
    }
    energyNote->setText(
        data.energies.isEmpty()
            ? QString()
            : QString("ΔEは %1 行目（太字）を基準にした差です。手法の異なる行どうしの差には意味がありま"
                      "せん。1 Eh = 627.509 kcal/mol = 2625.50 kJ/mol = 27.2114 eV。")
                  .arg(reference + 1));
}
void ResultsPanel::showPath() {
    const QSignalBlocker guard(path);
    path->setRowCount(0);
    const auto scan = data.properties.value("scan").toArray();
    const auto irc = data.properties.value("irc").toObject();
    struct Point {
        QString label, coordinate;
        double x, energy;
        int frame;
    };
    QVector<Point> points;
    QString xLabel;
    double reference = INFINITY;
    if (!scan.isEmpty()) {
        const auto coordinate = scan[0].toObject()["coordinate"].toString();
        xLabel = coordinate.startsWith("bond") ? "距離 / Å" : "角度 / °";
        for (const auto &value : scan) {
            const auto p = value.toObject();
            points << Point{QString::number(p["point"].toInt()), coordinateText(p["coordinate"].toString()),
                            p["value"].toDouble(), p["energy"].toDouble(), p["frame"].toInt(-1)};
            reference = std::min(reference, p["energy"].toDouble());
        }
    } else if (!irc.isEmpty()) {
        xLabel = "反応座標 s / amu½·Bohr";
        QVector<Point> backward, forward;
        for (const auto &value : irc["points"].toArray()) {
            const auto p = value.toObject();
            const bool ahead = p["direction"] == "forward";
            (ahead ? forward : backward)
                << Point{QString("%1 %2").arg(ahead ? "前進" : "後退").arg(p["point"].toInt()),
                         ahead ? "前進" : "後退", (ahead ? 1 : -1) * p["s"].toDouble(),
                         p["energy"].toDouble(), p["frame"].toInt(-1)};
        }
        std::reverse(backward.begin(), backward.end());
        points = backward;
        if (irc.contains("ts_energy")) {
            reference = irc["ts_energy"].toDouble();
            points << Point{"TS", "遷移状態", 0, reference, irc["ts_frame"].toInt(-1)};
        }
        points += forward;
        if (!std::isfinite(reference) && !points.isEmpty())
            reference = points.first().energy;
    }
    path->setRowCount(points.size());
    QVector<QPointF> curve;
    for (int i = 0; i < points.size(); ++i) {
        const auto &p = points[i];
        const double relative = (p.energy - reference) * HartreeToKcal;
        row(path, i, {p.label, p.coordinate, fixed(p.x, 4), fixed(p.energy, 8), fixed(relative, 3)});
        path->item(i, 0)->setData(Qt::UserRole, p.frame);
        curve << QPointF(p.x, relative);
    }
    if (curve.isEmpty())
        pathChart->clear();
    else
        pathChart->setChart(scan.isEmpty() ? "IRC（遷移状態基準）" : "座標スキャン（最小値基準）", xLabel,
                            "ΔE / kcal mol⁻¹", {{"エネルギー", curve, QColor("#2f6db5")}});
}
void ResultsPanel::showProperties() {
    const auto mulliken = data.properties.value("mulliken").toArray(),
               esp = data.properties.value("esp").toArray(), resp = data.properties.value("resp").toArray(),
               shielding = data.properties.value("shielding").toArray();
    const int count = std::max({mulliken.size(), esp.size(), resp.size(), shielding.size()});
    const Document *molecule = data.frames.isEmpty() ? nullptr : &data.frames.last().molecule;
    atomProperties->setRowCount(count);
    for (int i = 0; i < count; ++i) {
        const auto nmr = i < shielding.size() ? shielding[i].toObject() : QJsonObject();
        const QString symbol =
            molecule && i < molecule->atoms.size() ? molecule->atoms[i].element : nmr["element"].toString();
        const auto charge = [&](const QJsonArray &list) {
            return i < list.size() ? fixed(list[i].toDouble(), 4) : QString("—");
        };
        row(atomProperties, i,
            {QString::number(i + 1), symbol, charge(mulliken), charge(esp), charge(resp),
             nmr.contains("isotropic") ? fixed(nmr["isotropic"].toDouble(), 3) : "—",
             nmr.contains("anisotropy") ? fixed(nmr["anisotropy"].toDouble(), 3) : "—"});
    }
    auto text = resultPropertyLines(data);
    if (data.properties.contains("polarizability")) {
        const auto t = data.properties.value("polarizability").toObject()["tensor"].toArray();
        QStringList rows;
        for (int r = 0; r < 3; ++r)
            rows << QString("%1 %2 %3")
                        .arg(fixed(t[3 * r].toDouble(), 4), fixed(t[3 * r + 1].toDouble(), 4),
                             fixed(t[3 * r + 2].toDouble(), 4));
        text += "分極率テンソル / a.u.: [" + rows.join(" / ") + "]\n";
    }
    if (!shielding.isEmpty())
        text += "NMRは絶対遮蔽定数です。化学シフトは同じ条件で計算した基準物質（TMS等）の値との差です。\n";
    if (text.isEmpty())
        text = "このログには物性の結果がありません。「3 計算条件」の「物性」・溶媒・BSSE補正で計算できます。";
    propertySummary->setText(text.trimmed());
}
void ResultsPanel::showIrSpectrum() {
    QVector<QPointF> sticks, curve;
    double top = 0;
    for (int i = 0; i < data.modes.size(); ++i) {
        const auto &m = data.modes[i];
        if (rigidMotion.value(i) || m.frequency <= 0 || !std::isfinite(m.intensity))
            continue;
        sticks << QPointF(m.frequency, m.intensity);
        top = std::max(top, m.frequency);
    }
    if (sticks.isEmpty()) {
        irSpectrum->clear();
        return;
    }
    // Sample at a quarter of the half width (at most 20000 points) so narrow peaks keep their height.
    const double gamma = irWidth->value() / 2, end = top + 200;
    const int samples = std::clamp(int(std::ceil(end / (gamma / 2))), 1000, 20000);
    for (int k = 0; k <= samples; ++k) {
        const double x = end * k / samples;
        double y = 0;
        for (const auto &s : sticks)
            y += s.y() * gamma * gamma / ((x - s.x()) * (x - s.x()) + gamma * gamma);
        curve << QPointF(x, y);
    }
    irSpectrum->setChart(
        "IRスペクトル（表示用）", "波数 / cm⁻¹", "IR強度 / km mol⁻¹",
        {{"広がり", curve, QColor("#2f6db5")}, {"各モード", sticks, QColor("#c0392b"), true}});
}
void ResultsPanel::showArrows() {
    const int n = modes->currentRow();
    // Only over the reference structure the displacements belong to.
    if (!arrows->isChecked() || !modeDisplayed || n < 0 || n >= data.modes.size() ||
        data.modes[n].displacement.size() != data.vibrationGeometry.atoms.size() || animation.isActive()) {
        view->setArrows({});
        return;
    }
    // Longest arrow: three times the animation amplitude, so small modes stay visible.
    double longest = 0;
    for (const auto &v : data.modes[n].displacement)
        longest = std::max(longest, v.norm());
    QVector<std::pair<Vec, Vec>> list;
    for (int i = 0; i < data.vibrationGeometry.atoms.size() && longest > 0; ++i)
        list.append({data.vibrationGeometry.atoms[i].position,
                     data.modes[n].displacement.value(i) / longest * 3 * amplitude->value()});
    view->setArrows(list);
}
void ResultsPanel::exportMode(bool images) {
    const int n = modes->currentRow();
    if (loading() || n < 0 || n >= data.modes.size() || data.modes[n].displacement.isEmpty())
        fail("振動モードを選択してください。");
    stopAnimation();
    constexpr int steps = 24; // One period.
    const auto name = QString("mode%1_%2cm").arg(n + 1).arg(qRound(data.modes[n].frequency));
    if (images) {
        const auto directory = QFileDialog::getExistingDirectory(this, "連番画像の保存先", folder);
        if (directory.isEmpty())
            return;
        window->checkMcpPath(directory);
        modeDisplayed = true;
        view->setSurface({});
        for (int k = 0; k < steps; ++k) {
            setViewDocument(resultModeGeometry(data, n, 2 * M_PI * k / steps, amplitude->value()));
            view->renderNow();
            const auto file =
                QDir(directory).filePath(QString("%1_%2.png").arg(name).arg(k, 2, 10, QChar('0')));
            if (!view->grab().save(file))
                fail("画像を保存できません: " + file);
        }
        setViewDocument(data.vibrationGeometry);
        window->statusBar()->showMessage(QString("%1枚の画像を保存しました: %2").arg(steps).arg(directory),
                                         8000);
        return;
    }
    auto file = QFileDialog::getSaveFileName(this, "振動をXYZで保存", name + ".xyz", "XYZ (*.xyz)");
    if (file.isEmpty())
        return;
    if (!file.endsWith(".xyz", Qt::CaseInsensitive))
        file += ".xyz";
    window->checkMcpPath(file);
    QString text;
    for (int k = 0; k < steps; ++k) {
        auto frameText = resultModeGeometry(data, n, 2 * M_PI * k / steps, amplitude->value()).xyz();
        // Keep the atom count line and name the frame by its phase.
        auto lines = frameText.split('\n');
        if (lines.size() > 1)
            lines[1] = QString("%1 phase %2/%3").arg(name).arg(k).arg(steps);
        text += lines.join('\n');
        if (!text.endsWith('\n'))
            text += '\n';
    }
    writeFile(file, text.toUtf8());
}
void ResultsPanel::setViewDocument(const Document &d) {
    const auto ids = [&](const QVector<int> &numbers) {
        Ids list;
        for (int n : numbers)
            if (n >= 0 && n < d.atoms.size())
                list << d.atoms[n].id;
        return list.size() == numbers.size() ? list : Ids();
    };
    QVector<Ids> monitors;
    for (const auto &numbers : pinned)
        if (const auto list = ids(numbers); !list.isEmpty())
            monitors << list;
    view->setDocument(d);
    view->setSelection(ids(picked));
    view->setMonitors(monitors);
    QString text = "原子をクリックすると距離・角度・二面角を測定します（2〜4原子）。";
    if (!picked.isEmpty()) {
        QStringList labels;
        for (int n : picked)
            labels << (n < d.atoms.size() ? d.atoms[n].element : QString("?")) + QString::number(n + 1);
        text = "選択: " + labels.join(" → ");
        if (picked.size() >= 2 && !ids(picked).isEmpty())
            try {
                const double value = measure(d, ids(picked));
                text += picked.size() == 2 ? QString("　距離 %1 Å").arg(value, 0, 'f', 4)
                                           : QString("　%1 %2°")
                                                 .arg(picked.size() == 3 ? "角度" : "二面角")
                                                 .arg(value, 0, 'f', 2);
            } catch (const std::exception &e) {
                text += "　" + QString::fromUtf8(e.what());
            }
    }
    measurementLabel->setText(text);
}
void ResultsPanel::pickAtom(const QString &id) {
    const auto &d = view->documentRef();
    if (!d.contains(id))
        return;
    const int n = d.index(id);
    if (picked.contains(n))
        picked.removeAll(n);
    else {
        if (picked.size() == 4)
            picked.clear();
        picked << n;
    }
    setViewDocument(Document(d));
}
void ResultsPanel::showFrame() {
    modeDisplayed = false;
    view->setArrows({});
    const int i = frame->value() - 1;
    setViewDocument(i >= 0 && i < data.frames.size() ? data.frames[i].molecule : Document());
    view->setSurface(data.format != "MOLDEN" || i == data.orbitalFrame ? data.surface
                                                                       : QVector<SurfaceTriangle>{});
}
void ResultsPanel::showMode() {
    modeDisplayed = true;
    const int n = modes->currentRow();
    if (n < 0 || n >= data.modes.size() || data.modes[n].displacement.isEmpty()) {
        stopAnimation();
        return;
    }
    view->setSurface({});
    setViewDocument(resultModeGeometry(data, n, phase, amplitude->value()));
}
const Document &ResultsPanel::displayedGeometry() const {
    if (loading())
        fail("読込完了後に操作してください。");
    const int i = frame->value() - 1;
    if (i < 0 || i >= data.frames.size())
        fail("表示する構造がありません。");
    // During a vibration the reference structure, not a displaced snapshot.
    return modeDisplayed && modes->currentRow() >= 0 ? data.vibrationGeometry : data.frames[i].molecule;
}
void ResultsPanel::saveGeometry(const QString &path) const {
    writeFile(path, displayedGeometry().xyz().toUtf8());
}
void ResultsPanel::applyStructure() {
    const auto result = displayedGeometry();
    std::pair<double, double> moved;
    window->edit("計算結果の構造を適用", [&](Document &d) { moved = applyResultGeometry(d, result); }, true);
    if (!window->hasCandidate())
        fail("編集中の分子と結果の座標は同じです。");
    window->workspaceTabs->setCurrentIndex(0);
    window->statusBar()->showMessage(
        QString("結果の構造を候補にしました（RMSD %1 Å、最大変位 %2 Å）。「変更を確定」"
                "で適用します。")
            .arg(moved.first, 0, 'f', 4)
            .arg(moved.second, 0, 'f', 4),
        15000);
}
} // namespace mf
