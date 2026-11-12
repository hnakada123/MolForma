// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/conformer_set_dialog.h"
#include "editing/coordinates.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "ui/editor_context.h"
#include "ui/figure_export.h"
#include "ui/result_charts.h"
#include <QtWidgets>

namespace mf {
ConformerSetDialog::ConformerSetDialog(EditorContext &editorServices, UiServices &uiServices,
                                       WorkspaceActions &workspaceActions, Enqueue callback, QWidget *parent)
    : QDialog(parent), editorContext(editorServices), uiContext(uiServices), workspace(workspaceActions),
      enqueue(std::move(callback)), model(this) {
    setObjectName("conformerSetDialog");
    setWindowTitle(tr("配座セット"));
    resize(1050, 600);
    auto *layout = new QVBoxLayout(this);
    auto *settings = new QHBoxLayout;
    layout->addLayout(settings);
    metric = new QComboBox;
    metric->setObjectName("conformerEnergyMetric");
    metric->addItem(tr("電子エネルギー"), "E");
    metric->addItem(tr("Gibbs自由エネルギー"), "G");
    temperature = new QDoubleSpinBox;
    temperature->setObjectName("conformerTemperature");
    temperature->setRange(.01, 10000);
    temperature->setDecimals(2);
    temperature->setValue(298.15);
    temperature->setSuffix(" K");
    tolerance = new QDoubleSpinBox;
    tolerance->setObjectName("conformerRmsd");
    tolerance->setDecimals(3);
    tolerance->setRange(.001, 2);
    tolerance->setValue(.1);
    tolerance->setSuffix(" Å");
    frequencies = new QCheckBox(tr("最適化後に振動数も計算"));
    frequencies->setObjectName("conformerFrequencies");
    settings->addWidget(metric);
    settings->addWidget(temperature);
    settings->addWidget(new QLabel(tr("同一配座のRMSD")));
    settings->addWidget(tolerance);
    settings->addWidget(frequencies);
    // The corrections of the energy comparison for G (results/energy_profile.h). The standard state
    // shifts every conformer alike and is left out.
    frequencyScale = new QDoubleSpinBox;
    frequencyScale->setObjectName("conformerFrequencyScale");
    frequencyScale->setDecimals(4);
    frequencyScale->setRange(.5, 1.5);
    frequencyScale->setSingleStep(.01);
    frequencyScale->setValue(1);
    entropyModel = new QComboBox;
    entropyModel->setObjectName("conformerEntropyModel");
    entropyModel->addItem(tr("調和振動子（NWChemの値）"), "rrho");
    entropyModel->addItem(tr("準RRHO（Grimme）"), "grimme");
    entropyModel->addItem(tr("準調和（Truhlar）"), "truhlar");
    entropyCutoff = new QDoubleSpinBox;
    entropyCutoff->setObjectName("conformerEntropyCutoff");
    entropyCutoff->setDecimals(1);
    entropyCutoff->setRange(10, 500);
    entropyCutoff->setValue(100);
    entropyCutoff->setPrefix(tr("しきい値 "));
    entropyCutoff->setSuffix(" cm⁻¹");
    auto *corrections = new QHBoxLayout;
    layout->addLayout(corrections);
    corrections->addWidget(new QLabel(tr("Gの補正: 振動数スケール")));
    corrections->addWidget(frequencyScale);
    corrections->addWidget(new QLabel(tr("エントロピー")));
    corrections->addWidget(entropyModel);
    corrections->addWidget(entropyCutoff);
    corrections->addStretch(1);
    table = new QTableWidget(0, 9);
    table->setObjectName("conformerSetTable");
    table->setHorizontalHeaderLabels({tr("選択"), tr("配座名"), tr("力場 / kcal mol⁻¹"), tr("計算状態"),
                                      "E / Hartree", tr("虚振動"), "Δ / kcal mol⁻¹", tr("群"),
                                      tr("分布 / %")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table, 1);
    message = new QLabel;
    message->setObjectName("conformerSetMessage");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto *actions = new QHBoxLayout;
    layout->addLayout(actions);
    auto *files = new QHBoxLayout;
    layout->addLayout(files);
    const auto button = [this](QHBoxLayout *row, const QString &text, const QString &name,
                               const std::function<void()> &fn) {
        auto *b = new QPushButton(text);
        b->setObjectName(name);
        row->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, fn] { uiContext.safe(fn); });
        return b;
    };
    button(actions, tr("候補にする"), "conformerApply", [this] { applySelected(); });
    button(actions, tr("元の分子を復元"), "conformerRestore", [this] {
        if (model.original().atoms.isEmpty())
            return;
        editorContext.replaceUnsavedDocument(model.original());
    });
    button(actions, tr("選択配座を最適化キューへ"), "conformerQueue", [this] { queueSelected(); });
    button(actions, tr("結果を更新"), "conformerRefresh", [this] { refreshResults(); });
    button(actions, tr("読込を中止"), "conformerCancel", [this] { model.cancel(); });
    spectrumButton =
        button(actions, tr("配座平均スペクトル…"), "conformerSpectrum", [this] { showSpectrum(); });
    spectrumButton->setToolTip(tr("分布を重みにしたIR・通常ラマン・UV–Vis・ECD・NMRの平均スペクトルです。"
                                  "分布を計算できると使えます。"));
    button(files, tr("保存…"), "conformerSave", [this] {
        const auto path = QFileDialog::getSaveFileName(this, tr("配座セットを保存"), {}, "JSON (*.json)");
        if (!path.isEmpty()) {
            uiContext.checkMcpPath(path);
            writeFile(path, QJsonDocument(specification()).toJson());
        }
    });
    button(files, tr("読込…"), "conformerLoad", [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("配座セットを読込"), {}, "JSON (*.json)");
        if (!path.isEmpty()) {
            uiContext.checkMcpPath(path);
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
                fail(tr("配座セットを読めません。"));
            setSpecification(parseJson(file.read(16 * 1024 * 1024 + 1)));
        }
    });
    button(files, tr("XYZを書き出す…"), "conformerXyz", [this] {
        const auto path = QFileDialog::getSaveFileName(this, tr("配座を書き出す"), {}, "XYZ (*.xyz)");
        if (!path.isEmpty()) {
            uiContext.checkMcpPath(path);
            writeFile(path, model.selectedXyz());
        }
    });
    connect(table, &QTableWidget::cellChanged, this, [this](int row, int column) {
        if (row < 0 || row >= model.entries().size())
            return;
        if (column == 0)
            model.setSelected(row, table->item(row, 0)->checkState() == Qt::Checked);
        if (column == 1)
            model.setName(row, table->item(row, 1)->text());
    });
    for (auto *combo : {metric, entropyModel})
        connect(combo, &QComboBox::currentIndexChanged, this, &ConformerSetDialog::refresh);
    for (auto *spin : {temperature, tolerance, frequencyScale, entropyCutoff})
        connect(spin, &QDoubleSpinBox::valueChanged, this, &ConformerSetDialog::refresh);
    connect(frequencies, &QCheckBox::toggled, this, &ConformerSetDialog::refresh);
    connect(&model, &ConformerSet::changed, this, &ConformerSetDialog::refresh);
    connect(this, &QDialog::finished, this, [this] { model.cancel(); });
    refresh();
}
ConformerOptions ConformerSetDialog::options() const {
    return {metric->currentData().toString(),
            temperature->value(),
            tolerance->value(),
            frequencies->isChecked(),
            {frequencyScale->value(), entropyModel->currentData().toString(), entropyCutoff->value()}};
}
void ConformerSetDialog::setConformers(const Document &base, const QVector<Conformer> &conformers) {
    const QSignalBlocker block(model);
    model.setConformers(base, conformers);
    setSpecification(model.specification());
}
QJsonObject ConformerSetDialog::specification() const {
    return model.specification();
}
void ConformerSetDialog::setSpecification(const QJsonObject &p) {
    const QSignalBlocker block(model);
    model.setSpecification(p);
    const auto &o = model.options();
    const QSignalBlocker bm(metric), bt(temperature), br(tolerance), bs(frequencyScale), be(entropyModel),
        bc(entropyCutoff), bf(frequencies);
    metric->setCurrentIndex(metric->findData(o.metric));
    temperature->setValue(o.temperature);
    tolerance->setValue(o.rmsd);
    frequencies->setChecked(o.frequencies);
    frequencyScale->setValue(o.thermal.scale);
    entropyModel->setCurrentIndex(entropyModel->findData(o.thermal.model));
    entropyCutoff->setValue(o.thermal.cutoff);
    refresh();
}
void ConformerSetDialog::checkMolecule() const {
    if (editorContext.hasCandidate() || editorContext.chemistryRunning())
        fail(tr("候補を確定・取消し、整形の終了後に実行できます。"));
    if (editorContext.document().ids() != model.original().ids() ||
        editorContext.document().bonds != model.original().bonds)
        fail(tr("配座セットと編集中の分子が一致しません。元の分子を復元してください。"));
}
void ConformerSetDialog::applySelected() {
    checkMolecule();
    const auto d = model.geometry(table->currentRow());
    editorContext.edit(tr("配座を適用"), [&](Document &next) { applyResultGeometry(next, d); }, true);
    workspace.showSetting(SettingTarget::Molecule);
}
void ConformerSetDialog::queueSelected() {
    checkMolecule();
    model.setOptions(options());
    model.queueSelected(enqueue);
}
void ConformerSetDialog::refreshResults() {
    model.refreshResults([this](const QString &path) { uiContext.checkMcpPath(path); });
}
void ConformerSetDialog::refresh() {
    const QSignalBlocker block(table);
    const int selected = table->currentRow();
    const auto &entries = model.entries();
    table->setRowCount(entries.size());
    for (int i = 0; i < entries.size(); ++i) {
        const auto row = entries[i].toObject();
        const auto result = row["result"].toObject();
        const QStringList cells{
            "",
            row["name"].toString(),
            row.contains("force_field_kcal") ? QString::number(row["force_field_kcal"].toDouble(), 'g', 8)
                                             : "—",
            row["error"].toString(
                row["state"].toString(row["job"].toString().isEmpty() ? tr("未登録") : tr("登録済み"))),
            result.isEmpty() ? "—" : QString::number(result["energy_hartree"].toDouble(), 'g', 14),
            result["imaginary"].isDouble() ? QString::number(result["imaginary"].toInt()) : "—",
            "—",
            "—",
            "—"};
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells[column]);
            item->setToolTip(column == 2   ? row["force_field"].toString()
                             : column == 3 ? row["job"].toString() + '\n' + cells[column]
                                           : cells[column]);
            if (column != 1)
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            if (column == 0) {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(row["selected"].toBool() ? Qt::Checked : Qt::Unchecked);
            }
            table->setItem(i, column, item);
        }
    }
    message->setText(tr("配座を保存し、現在の計算条件で一括最適化できます。力場と量子化学のエネルギーは別の列"
                        "です。分布は原子順を保った全原子RMSDで同一極小をまとめ、各極小の縮重を1として計算し"
                        "ます。未探索の配座は含みません。"));
    const bool gibbs = metric->currentData() == "G";
    for (auto *widget : {static_cast<QWidget *>(frequencyScale), static_cast<QWidget *>(entropyModel)})
        widget->setEnabled(gibbs);
    entropyCutoff->setEnabled(gibbs && entropyModel->currentData() != "rrho");
    members = {};
    try {
        model.setOptions(options());
        const auto analysis = model.analyze();
        members = analysis.spectra;
        for (const auto &value : analysis.rows) {
            const auto r = value.toObject();
            const int i = r["entry"].toInt();
            table->item(i, 6)->setText(QString::number(r["relative_kcal"].toDouble(), 'f', 4));
            table->item(i, 7)->setText(QString::number(r["group"].toInt()));
            table->item(i, 8)->setText(QString::number(100 * r["population"].toDouble(), 'f', 3));
        }
    } catch (const std::exception &e) {
        message->setText(message->text() + '\n' + QString::fromUtf8(e.what()));
    }
    spectrumButton->setEnabled(!members.isEmpty());
    if (spectrumDialog)
        spectrumDialog->setMembers(members);
    if (selected >= 0 && selected < table->rowCount())
        table->selectRow(selected);
    else if (table->rowCount())
        table->selectRow(0);
}
QJsonArray ConformerSetDialog::spectrumMembers() const {
    return members;
}
ConformerSpectrumDialog *ConformerSetDialog::showSpectrum() {
    const auto members = spectrumMembers();
    if (members.isEmpty())
        fail(tr("分布を計算できる配座がありません。選択した配座の結果を読み込んでから開いてください。"));
    if (!spectrumDialog)
        spectrumDialog = new ConformerSpectrumDialog(uiContext, this);
    spectrumDialog->setMembers(members);
    spectrumDialog->show();
    spectrumDialog->raise();
    return spectrumDialog;
}
ConformerSpectrumDialog::ConformerSpectrumDialog(UiServices &uiServices, QWidget *parent)
    : QDialog(parent), uiContext(uiServices) {
    setObjectName("conformerSpectrumDialog");
    setWindowTitle(tr("配座平均スペクトル"));
    resize(780, 560);
    auto *layout = new QVBoxLayout(this);
    auto *settings = new QHBoxLayout;
    layout->addLayout(settings);
    kind = new QComboBox;
    kind->setObjectName("conformerSpectrumKind");
    kind->addItem("IR", "ir");
    kind->addItem(tr("通常ラマン"), "raman");
    kind->addItem("UV–Vis", "uv");
    kind->addItem("ECD", "ecd");
    kind->addItem(tr("NMR（分裂なし）"), "nmr");
    nucleus = new QComboBox;
    nucleus->setObjectName("conformerSpectrumNucleus");
    nucleus->addItem("¹H", "1H");
    nucleus->addItem("¹³C", "13C");
    reference = new QLineEdit;
    reference->setObjectName("conformerSpectrumReference");
    reference->setPlaceholderText(tr("同じ条件のTMS等の遮蔽定数"));
    width = new QDoubleSpinBox;
    width->setObjectName("conformerSpectrumWidth");
    width->setDecimals(4);
    width->setRange(.0001, 100000);
    width->setPrefix(tr("半値全幅 "));
    nucleusLabel = new QLabel(tr("核種"));
    referenceLabel = new QLabel(tr("基準遮蔽定数 / ppm"));
    settings->addWidget(kind);
    settings->addWidget(width);
    settings->addWidget(nucleusLabel);
    settings->addWidget(nucleus);
    settings->addWidget(referenceLabel);
    settings->addWidget(reference, 1);
    settings->addStretch();
    message = new QLabel;
    message->setObjectName("conformerSpectrumMessage");
    message->setWordWrap(true);
    message->setTextFormat(Qt::PlainText);
    layout->addWidget(message);
    chart = new ChartView;
    chart->setObjectName("conformerSpectrumChart");
    layout->addWidget(chart, 1);
    auto *buttons = new QHBoxLayout;
    layout->addLayout(buttons);
    buttons->addStretch();
    const auto button = [&](const QString &text, const QString &name, const std::function<void()> &action) {
        auto *b = new QPushButton(text);
        b->setObjectName(name);
        buttons->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, action] { uiContext.safe(action); });
    };
    button(tr("CSV…"), "conformerSpectrumCsv", [this] {
        const auto path = QFileDialog::getSaveFileName(this, tr("スペクトルを書き出す"), {}, "CSV (*.csv)");
        if (!path.isEmpty())
            exportCsv(path);
    });
    button(tr("図…"), "conformerSpectrumPng", [this] {
        if (spectrum.curve.isEmpty())
            fail(message->text());
        const auto path = askFigurePath(this, tr("スペクトルを書き出す"), "spectrum");
        if (!path.isEmpty()) {
            uiContext.checkMcpPath(path);
            saveFigure(chart, path);
        }
    });
    // The widths the charts of the result viewer start with, in the unit of the axis.
    const auto reset = [this] {
        const QSignalBlocker block(width);
        const auto k = kind->currentData().toString();
        width->setValue(k == "nmr" ? .05 : k == "ecd" ? .2 : k == "uv" ? .3 : 20);
        width->setSuffix(k == "nmr" ? " ppm" : k == "ecd" || k == "uv" ? " eV" : " cm⁻¹");
    };
    reset();
    connect(kind, &QComboBox::currentIndexChanged, this, [this, reset] {
        reset();
        refresh();
    });
    connect(nucleus, &QComboBox::currentIndexChanged, this, [this] {
        reference->clear();
        refresh();
    });
    connect(reference, &QLineEdit::editingFinished, this, &ConformerSpectrumDialog::refresh);
    connect(width, &QDoubleSpinBox::valueChanged, this, &ConformerSpectrumDialog::refresh);
    refresh();
}
void ConformerSpectrumDialog::setMembers(const QJsonArray &next) {
    members = next;
    // Start with a kind the set has data of.
    const auto available = [&](const QString &k) {
        return std::any_of(members.begin(), members.end(), [&](const auto &member) {
            return member.toObject()["weight"].toDouble() > 0 &&
                   !member.toObject()["spectra"].toObject()[k == "nmr" ? "shielding" : k].toArray().isEmpty();
        });
    };
    if (!available(kind->currentData().toString()))
        for (int i = 0; i < kind->count(); ++i)
            if (available(kind->itemData(i).toString())) {
                kind->setCurrentIndex(i); // Refreshes.
                return;
            }
    refresh();
}
QJsonObject ConformerSpectrumDialog::parameters() const {
    QJsonObject p{{"kind", kind->currentData().toString()}, {"width", width->value()}};
    if (p["kind"] == "nmr") {
        p["nucleus"] = nucleus->currentData().toString();
        bool ok = false;
        const double shielding = reference->text().toDouble(&ok);
        if (!reference->text().trimmed().isEmpty())
            p["reference_ppm"] = ok ? QJsonValue(shielding) : QJsonValue();
    }
    return p;
}
void ConformerSpectrumDialog::setParameters(const QJsonObject &p) {
    averagedSpectrum(members, p); // Validate everything before changing the visible settings.
    {
        const QSignalBlocker blockKind(kind), blockNucleus(nucleus), blockWidth(width);
        kind->setCurrentIndex(kind->findData(p["kind"].toString()));
        const auto k = p["kind"].toString();
        width->setSuffix(k == "nmr" ? " ppm" : k == "ecd" || k == "uv" ? " eV" : " cm⁻¹");
        width->setValue(p["width"].toDouble());
        nucleus->setCurrentIndex(nucleus->findData(p["nucleus"].toString("1H")));
        reference->setText(p.contains("reference_ppm")
                               ? QString::number(p["reference_ppm"].toDouble(), 'g', 16)
                               : QString());
    }
    refresh();
}
void ConformerSpectrumDialog::refresh() {
    const bool nmr = kind->currentData() == "nmr";
    for (auto *widget : {static_cast<QWidget *>(nucleusLabel), static_cast<QWidget *>(nucleus),
                         static_cast<QWidget *>(referenceLabel), static_cast<QWidget *>(reference)})
        widget->setVisible(nmr);
    try {
        spectrum = averagedSpectrum(members, parameters());
        chart->setReversedX(spectrum.reversed);
        chart->setXRange(spectrum.parameters["minimum"].toDouble(),
                         spectrum.parameters["maximum"].toDouble());
        chart->setChart(tr("配座平均 %1").arg(kind->currentText()), spectrum.xUnit, spectrum.yUnit,
                        {{tr("広がり"), spectrum.curve, ChartTone::Muted},
                         {tr("ピーク"), spectrum.sticks, ChartTone::Text, true}});
        QStringList weights, lines;
        for (const auto &value : spectrum.parameters["conformers"].toArray())
            weights << QString("%1 %2%")
                           .arg(value.toObject()["name"].toString())
                           .arg(100 * value.toObject()["weight"].toDouble(), 0, 'f', 1);
        lines << tr("含めた配座と重み: %1。").arg(weights.join(tr("、")));
        if (const auto omitted = spectrum.parameters["omitted"].toArray(); !omitted.isEmpty()) {
            QStringList names;
            for (const auto &name : omitted)
                names << name.toString();
            lines << tr("このスペクトルのデータがないため除いた配座: "
                        "%1。残りの配座の重みを合計1に直しています。")
                         .arg(names.join(tr("、")));
        }
        lines << (nmr ? tr("配座間の速い交換を仮定し、原子ごとの遮蔽定数を分布で平均します（δ ≈ σ基準 − "
                           "平均σ）。J分裂・等価核の平均化は含みません。")
                      : tr("各配座の棒に分布の重みを掛けて合わせ、1回だけ広げた表示用の曲線です。重複した"
                           "配座（重み0）は含みません。"));
        lines << tr("分布は選んだセット内のモデル値で、未探索の配座は含みません。");
        message->setText(lines.join('\n'));
    } catch (const std::exception &e) {
        spectrum = {};
        chart->clear();
        message->setText(QString::fromUtf8(e.what()));
    }
}
void ConformerSpectrumDialog::exportCsv(const QString &path) const {
    uiContext.checkMcpPath(path);
    writeFile(path, spectrumCsv(averagedSpectrum(members, parameters())));
}
} // namespace mf
