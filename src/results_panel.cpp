// SPDX-License-Identifier: Apache-2.0
// Independent result browsing workflow inspired by KAGAMI results_tab.py; native NWChem readers.
#include "results_panel.h"
#include "main_window.h"
#include "molden_orbitals.h"
#include <QtWidgets>
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
    saveXyz = new QPushButton("構造を保存…");
    saveXyz->setObjectName("resultsSaveXyz");
    savePng = new QPushButton("画像を保存…");
    savePng->setObjectName("resultsSavePng");
    actions->addWidget(saveXyz);
    actions->addWidget(savePng);
    auto *fileRow = new QHBoxLayout;
    layout->addLayout(fileRow);
    files = new QComboBox;
    files->setObjectName("resultsFiles");
    files->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    files->setMinimumContentsLength(20);
    fileRow->addWidget(files, 1);
    auto *selected = new QPushButton("選択した結果を開く");
    selected->setObjectName("resultsOpenSelected");
    fileRow->addWidget(selected);
    source = new QLabel("結果ファイルを開いてください。");
    source->setObjectName("resultsSource");
    source->setTextFormat(Qt::PlainText);
    source->setWordWrap(true);
    layout->addWidget(source);
    status = new QLabel;
    status->setObjectName("resultsStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *hint = new QLabel("結果は編集中の分子から独立しています。左ドラッグ: 回転／右ドラッグ: "
                            "移動／ホイール: 拡大縮小。結合は距離から推定します。");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *split = new QSplitter;
    layout->addWidget(split, 1);
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
    view = new MoleculeView;
    view->setObjectName("resultsView");
    view->setAcceptDrops(false);
    view->setAngles(-25, 25, 0);
    view->setEmptyMessage("NWChem出力・XYZ・CUBE・MOLDENを開いて\n構造・軌道の等値面・振動を確認できます。");
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
    energy = table({"手法", "全エネルギー / Eh", "ログ行"}, "resultsEnergy");
    tabs->addTab(energy, "エネルギー");
    auto *orbitalPage = new QWidget;
    auto *ol = new QVBoxLayout(orbitalPage);
    auto *orbitalActions = new QHBoxLayout;
    homo = new QPushButton("HOMO");
    lumo = new QPushButton("LUMO");
    showOrbital = new QPushButton("選択した軌道を表示");
    homo->setObjectName("resultsHomo");
    lumo->setObjectName("resultsLumo");
    showOrbital->setObjectName("resultsShowOrbital");
    for (auto *b : {homo, lumo, showOrbital})
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
    ol->addWidget(orbitals, 1);
    tabs->addTab(orbitalPage, "軌道");
    connect(showOrbital, &QPushButton::clicked, this, [this] { renderOrbital(orbitals->currentRow()); });
    connect(orbitals, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { renderOrbital(row); });
    connect(orbitals, &QTableWidget::currentCellChanged, this, [this] { updateActions(); });
    const auto frontier = [this](bool virtualOrbital) {
        const int row = orbitals->currentRow();
        const auto spin = row >= 0 && row < data.orbitals.size() ? data.orbitals[row].spin : QString("Alpha");
        renderOrbital(frontierOrbital(data.orbitals, spin, virtualOrbital));
    };
    connect(homo, &QPushButton::clicked, this, [frontier] { frontier(false); });
    connect(lumo, &QPushButton::clicked, this, [frontier] { frontier(true); });
    auto *vibrations = new QWidget;
    auto *vl = new QVBoxLayout(vibrations);
    modes = table({"振動数 / cm⁻¹", "IR強度 / km mol⁻¹"}, "resultsModes");
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
    auto *modeHint = new QLabel("振幅は表示用に規格化した最大変位です。負の振動数は虚振動を表します。アニメー"
                                "ションの速さは実際の振動数を表しません。");
    modeHint->setWordWrap(true);
    vl->addWidget(modeHint);
    tabs->addTab(vibrations, "振動");
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
    connect(selected, &QPushButton::clicked, this, [this] {
        if (files->currentIndex() >= 0)
            openFile(QDir(folder).filePath(files->currentText()));
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
            showMode();
        }
    });
    connect(modes, &QTableWidget::currentCellChanged, this, [this] {
        stopAnimation();
        updateActions();
        if (modes->currentRow() >= 0 && modes->currentRow() < data.modes.size() &&
            !data.modes[modes->currentRow()].displacement.isEmpty()) {
            modeDisplayed = true;
            view->setDocument(data.vibrationGeometry);
            view->setSurface({});
        }
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
    status->setText("読込・表示エラー: " + message + "\n前の結果は保持しています。");
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
    status->setText("読込を中止しました。前の結果を保持しています。");
    updateActions();
}
void ResultsPanel::openFile(const QString &path) {
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
    files->clear();
    files->addItems(resultFiles(folder));
    if (files->count() == 0) {
        error("対応する結果ファイルがありません。");
        return;
    }
    int index = files->findText("output.out");
    files->setCurrentIndex(index >= 0 ? index : 0);
    openFile(dir.filePath(files->currentText()));
}
void ResultsPanel::renderOrbital(int orbital) {
    if (data.format != "MOLDEN" || orbital < 0 || orbital >= data.orbitals.size())
        return;
    bool ok;
    const double iso = orbitalIso->text().toDouble(&ok);
    if (!ok || !std::isfinite(iso) || iso <= 0 || iso > 1e6) {
        error("軌道の等値は0より大きい有限の数で指定してください。");
        return;
    }
    startLoad(data.source, iso, 0, orbital, orbitalNorm->currentData().toString(), orbitalGrid->value());
}
void ResultsPanel::startLoad(const QString &path, double iso, int channel, int orbital,
                             const QString &normalization, int divisions) {
    cancelLoad();
    stopAnimation();
    if (path.isEmpty())
        return;
    work = std::make_unique<QTemporaryDir>();
    if (!work->isValid()) {
        error("一時フォルダーを作成できません。");
        return;
    }
    const QFileInfo before(path);
    const auto originalSize = before.size();
    const auto originalTime = before.lastModified();
    const auto originalTarget = before.canonicalFilePath();
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
    connect(p, &QProcess::finished, this,
            [this, p, before, originalSize, originalTime, originalTarget,
             response](int code, QProcess::ExitStatus state) {
                if (process != p)
                    return;
                process = nullptr;
                p->deleteLater();
                try {
                    if (code != 0 || state != QProcess::NormalExit) {
                        const QString e = QFileInfo::exists(response + ".error")
                                              ? QString::fromUtf8(readFile(response + ".error"))
                                              : "結果の読込プロセスが失敗しました。";
                        fail(e);
                    }
                    const QFileInfo after(before.absoluteFilePath());
                    if (!after.exists() || after.size() != originalSize ||
                        after.lastModified() != originalTime || after.canonicalFilePath() != originalTarget)
                        fail("読込中にファイルが更新されました。再読込してください。");
                    auto next = loadResultData(response);
                    const bool newSource = data.source != next.source;
                    data = std::move(next);
                    display();
                    if (newSource)
                        view->fit();
                    if (newSource && data.format == "MOLDEN" && !data.orbitals.isEmpty())
                        details->setCurrentIndex(2);
                    else if (newSource && data.format == "NWChem" && !data.energies.isEmpty())
                        details->setCurrentIndex(1);
                    emit loaded();
                } catch (const std::exception &e) {
                    error(QString::fromUtf8(e.what()));
                }
                pendingPath.clear();
                work.reset();
                updateActions();
            });
    status->setText("読込中: " + pendingPath + "\n完了まで前の結果を表示します。");
    updateActions();
    p->start();
}
void ResultsPanel::updateActions() {
    const bool ready = !loading() && !data.source.isEmpty();
    cancel->setEnabled(loading());
    reload->setEnabled(ready);
    saveXyz->setEnabled(ready && !data.frames.isEmpty());
    savePng->setEnabled(ready && !data.frames.isEmpty());
    frame->setEnabled(ready && !data.frames.isEmpty());
    playFrames->setEnabled(ready && data.frames.size() > 1);
    const int mode = modes->currentRow();
    playMode->setEnabled(ready && mode >= 0 && mode < data.modes.size() &&
                         !data.modes[mode].displacement.isEmpty());
    applyIso->setEnabled(ready && data.format == "CUBE");
    dataset->setEnabled(applyIso->isEnabled());
    const int selected = orbitals->currentRow();
    const bool orbitalReady = ready && data.format == "MOLDEN" && data.orbitalBasis;
    showOrbital->setEnabled(orbitalReady && selected >= 0 && selected < data.orbitals.size());
    orbitalIso->setEnabled(orbitalReady);
    orbitalGrid->setEnabled(orbitalReady);
    orbitalNorm->setEnabled(orbitalReady);
    const auto spin =
        selected >= 0 && selected < data.orbitals.size() ? data.orbitals[selected].spin : QString("Alpha");
    homo->setEnabled(orbitalReady && frontierOrbital(data.orbitals, spin) >= 0);
    lumo->setEnabled(orbitalReady && frontierOrbital(data.orbitals, spin, true) >= 0);
}
void ResultsPanel::display() {
    source->setText(data.source);
    status->setText(data.status);
    QString selectedFile;
    for (int i = 0; i < files->count(); ++i)
        if (QFileInfo(QDir(folder).filePath(files->itemText(i))).absoluteFilePath() ==
            QFileInfo(data.source).absoluteFilePath()) {
            selectedFile = files->itemText(i);
            break;
        }
    if (selectedFile.isEmpty()) {
        folder = QFileInfo(data.source).absolutePath();
        selectedFile = QFileInfo(data.source).fileName();
    }
    files->clear();
    files->addItems(resultFiles(folder));
    files->setCurrentText(selectedFile);
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
    info += "\n" + data.warnings.join("\n\n");
    summary->setText(info);
    log->setPlainText(data.log);
    energy->setRowCount(data.energies.size());
    for (int i = 0; i < data.energies.size(); ++i) {
        const auto &e = data.energies[i];
        row(energy, i, {e.method, real(e.hartree), e.line ? QString::number(e.line) : "—"});
    }
    {
        const QSignalBlocker guard(orbitals);
        orbitals->setRowCount(data.orbitals.size());
        QMap<QString, int> indices;
        QMap<QString, int> homos, lumos;
        for (const auto &spin : {QString("Alpha"), QString("Beta")}) {
            homos[spin] = frontierOrbital(data.orbitals, spin);
            lumos[spin] = frontierOrbital(data.orbitals, spin, true);
        }
        for (int i = 0; i < data.orbitals.size(); ++i) {
            const auto &o = data.orbitals[i];
            const QString frontier = i == homos[o.spin] ? "HOMO" : i == lumos[o.spin] ? "LUMO" : QString();
            row(orbitals, i,
                {QString::number(++indices[o.spin]), frontier, o.symmetry, o.spin, real(o.energy),
                 real(o.occupation)});
        }
        orbitals->clearSelection();
        orbitals->setCurrentCell(data.selectedOrbital, 0);
        if (data.selectedOrbital >= 0)
            orbitals->scrollToItem(orbitals->item(data.selectedOrbital, 0));
    }
    orbitalIso->setText(real(data.isovalue));
    orbitalGrid->setValue(data.orbitalDivisions);
    orbitalNorm->setCurrentIndex(orbitalNorm->findData(data.normalizationRequest));
    orbitalStatus->setText(
        data.format != "MOLDEN"        ? "MOLDENファイルを開くと分子軌道を表示します。"
        : !data.orbitalError.isEmpty() ? data.orbitalError
        : data.selectedOrbital >= 0
            ? QString("表示中: %1 %2番 / 等値 ±%3 / 正規化: "
                      "%4\n軌道を選び「選択した軌道を表示」、またはダブルクリック。赤: 正 / 青: 負。")
                  .arg(data.orbitals[data.selectedOrbital].spin,
                       orbitals->item(data.selectedOrbital, 0)->text(), real(data.isovalue),
                       data.orbitalNormalization == "nwchem" ? "NWChem" : "標準MOLDEN")
            : "このファイルには表示可能な分子軌道がありません。");
    {
        const QSignalBlocker guard(modes);
        modes->setRowCount(data.modes.size());
        for (int i = 0; i < data.modes.size(); ++i) {
            const auto &m = data.modes[i];
            row(modes, i, {real(m.frequency), real(m.intensity)});
        }
        modes->clearSelection();
        modes->setCurrentCell(-1, -1);
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
void ResultsPanel::showFrame() {
    modeDisplayed = false;
    const int i = frame->value() - 1;
    view->setDocument(i >= 0 && i < data.frames.size() ? data.frames[i].molecule : Document());
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
    view->setDocument(resultModeGeometry(data, n, phase, amplitude->value()));
}
void ResultsPanel::saveGeometry(const QString &path) const {
    if (loading())
        fail("読込完了後に構造を保存してください。");
    const int i = frame->value() - 1;
    if (i < 0 || i >= data.frames.size())
        fail("表示する構造がありません。");
    const int mode = modes->currentRow();
    const auto &d = modeDisplayed && mode >= 0 ? data.vibrationGeometry : data.frames[i].molecule;
    writeFile(path, d.xyz().toUtf8());
}
} // namespace mf
