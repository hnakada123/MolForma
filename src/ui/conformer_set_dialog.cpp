// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/conformer_set_dialog.h"
#include "editing/coordinates.h"
#include "model/i18n.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "results/energy_profile.h"
#include "ui/main_window.h"
#include "ui/result_charts.h"
#include <QtWidgets>

namespace mf {
ConformerSetDialog::ConformerSetDialog(MainWindow *owner, Enqueue callback)
    : QDialog(owner), window(owner), enqueue(std::move(callback)), loader(this) {
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
        connect(b, &QPushButton::clicked, this, [this, fn] { window->safe(fn); });
        return b;
    };
    button(actions, tr("候補にする"), "conformerApply", [this] { applySelected(); });
    button(actions, tr("元の分子を復元"), "conformerRestore", [this] {
        if (original.atoms.isEmpty())
            return;
        window->replaceUnsavedDocument(original);
    });
    button(actions, tr("選択配座を最適化キューへ"), "conformerQueue", [this] { queueSelected(); });
    button(actions, tr("結果を更新"), "conformerRefresh", [this] { refreshResults(); });
    button(actions, tr("読込を中止"), "conformerCancel", [this] {
        loader.cancel();
        pending.clear();
    });
    spectrumButton =
        button(actions, tr("配座平均スペクトル…"), "conformerSpectrum", [this] { showSpectrum(); });
    spectrumButton->setToolTip(tr("分布を重みにしたIR・通常ラマン・UV–Vis・ECD・NMRの平均スペクトルです。"
                                  "分布を計算できると使えます。"));
    button(files, tr("保存…"), "conformerSave", [this] {
        const auto path = QFileDialog::getSaveFileName(this, tr("配座セットを保存"), {}, "JSON (*.json)");
        if (!path.isEmpty()) {
            window->checkMcpPath(path);
            writeFile(path, QJsonDocument(specification()).toJson());
        }
    });
    button(files, tr("読込…"), "conformerLoad", [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("配座セットを読込"), {}, "JSON (*.json)");
        if (!path.isEmpty()) {
            window->checkMcpPath(path);
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
                fail(tr("配座セットを読めません。"));
            setSpecification(parseJson(file.read(16 * 1024 * 1024 + 1)));
        }
    });
    button(files, tr("XYZを書き出す…"), "conformerXyz", [this] {
        const auto path = QFileDialog::getSaveFileName(this, tr("配座を書き出す"), {}, "XYZ (*.xyz)");
        if (!path.isEmpty()) {
            window->checkMcpPath(path);
            QByteArray xyz;
            for (int i = 0; i < entries.size(); ++i)
                if (entries[i].toObject()["selected"].toBool())
                    xyz += geometry(i).xyz().toUtf8();
            writeFile(path, xyz);
        }
    });
    connect(table, &QTableWidget::cellChanged, this, [this](int row, int column) {
        if (row < 0 || row >= entries.size())
            return;
        auto item = entries[row].toObject();
        if (column == 0)
            item["selected"] = table->item(row, 0)->checkState() == Qt::Checked;
        if (column == 1)
            item["name"] = table->item(row, 1)->text();
        entries[row] = item;
        if (column == 0)
            refresh();
        else if (spectrumDialog)
            spectrumDialog->setMembers(spectrumMembers());
    });
    for (auto *combo : {metric, entropyModel})
        connect(combo, &QComboBox::currentIndexChanged, this, &ConformerSetDialog::refresh);
    for (auto *spin : {temperature, tolerance, frequencyScale, entropyCutoff})
        connect(spin, &QDoubleSpinBox::valueChanged, this, &ConformerSetDialog::refresh);
    connect(&loader, &ResultLoader::loaded, this, [this](const ResultData &r, const QString &stamp) {
        if (reading < 0 || reading >= entries.size())
            return;
        auto item = entries[reading].toObject();
        try {
            if (r.properties["optimization"] != "converged")
                fail(tr("最適化の収束を確認できません。"));
            auto result = energyRecord(r, pendingOriginal, nwchemFinalSettings(pendingOriginal));
            result["source_stamp"] = stamp;
            item["result"] = result;
            // What the log holds for the averaged spectra of the set.
            item["spectra"] = spectrumContributions(r);
            item.remove("error");
        } catch (const std::exception &e) {
            item["error"] = QString::fromUtf8(e.what());
        }
        entries[reading] = item;
        refresh();
        QTimer::singleShot(0, this, &ConformerSetDialog::readNext);
    });
    connect(&loader, &ResultLoader::failed, this, [this](const QString &error) {
        if (reading >= 0 && reading < entries.size()) {
            auto item = entries[reading].toObject();
            item["error"] = error;
            entries[reading] = item;
        }
        refresh();
        QTimer::singleShot(0, this, &ConformerSetDialog::readNext);
    });
    connect(this, &QDialog::finished, this, [this] {
        loader.cancel();
        pending.clear();
    });
    refresh();
}
void ConformerSetDialog::setConformers(const Document &base, const QVector<Conformer> &conformers) {
    QJsonArray rows;
    for (int i = 0; i < conformers.size(); ++i)
        rows.append(QJsonObject{{"name", tr("配座 %1").arg(i + 1)},
                                {"selected", true},
                                {"geometry", conformers[i].document.xyz()},
                                {"force_field", conformers[i].forceField},
                                {"force_field_kcal", conformers[i].energy}});
    setSpecification({{"format", "molforma-conformer-set"},
                      {"version", 1},
                      {"original", base.project()},
                      {"entries", rows},
                      {"metric", "E"},
                      {"temperature", 298.15},
                      {"rmsd", .1}});
}
QJsonObject ConformerSetDialog::specification() const {
    return {{"format", "molforma-conformer-set"},
            {"version", 1},
            {"original", original.project()},
            {"entries", entries},
            {"metric", metric->currentData().toString()},
            {"temperature", temperature->value()},
            {"rmsd", tolerance->value()},
            {"frequencies", frequencies->isChecked()},
            {"frequency_scale", frequencyScale->value()},
            {"entropy_model", entropyModel->currentData().toString()},
            {"entropy_cutoff", entropyCutoff->value()}};
}
void ConformerSetDialog::setSpecification(const QJsonObject &p) {
    if (p["format"] != "molforma-conformer-set" || p["version"] != 1 || p["entries"].toArray().isEmpty() ||
        p["entries"].toArray().size() > 100)
        fail(tr("配座セットの形式が不正です（1〜100配座）。"));
    const auto base = Document::fromProject(p["original"].toObject());
    for (const auto &value : p["entries"].toArray()) {
        auto candidate = base;
        applyResultGeometry(candidate, Document::fromXyz(value.toObject()["geometry"].toString()));
    }
    loader.cancel();
    pending.clear();
    reading = -1;
    original = base;
    entries = p["entries"].toArray();
    const QSignalBlocker bm(metric), bt(temperature), br(tolerance), bs(frequencyScale), be(entropyModel),
        bc(entropyCutoff);
    metric->setCurrentIndex(metric->findData(p["metric"].toString("E")));
    temperature->setValue(p["temperature"].toDouble(298.15));
    tolerance->setValue(p["rmsd"].toDouble(.1));
    frequencies->setChecked(p["frequencies"].toBool());
    // Sets saved before these corrections have none of the fields.
    frequencyScale->setValue(p["frequency_scale"].toDouble(1));
    entropyModel->setCurrentIndex(std::max(0, entropyModel->findData(p["entropy_model"].toString("rrho"))));
    entropyCutoff->setValue(p["entropy_cutoff"].toDouble(100));
    refresh();
}
Document ConformerSetDialog::geometry(int index) const {
    if (index < 0 || index >= entries.size())
        fail(tr("配座を選んでください。"));
    auto d = original;
    applyResultGeometry(d, Document::fromXyz(entries[index].toObject()["geometry"].toString()));
    return d;
}
void ConformerSetDialog::checkMolecule() const {
    if (window->hasCandidate() || window->chemistryRunning())
        fail(tr("候補を確定・取消し、整形の終了後に実行できます。"));
    if (window->document().ids() != original.ids() || window->document().bonds != original.bonds)
        fail(tr("配座セットと編集中の分子が一致しません。元の分子を復元してください。"));
}
void ConformerSetDialog::applySelected() {
    checkMolecule();
    const auto d = geometry(table->currentRow());
    window->edit(tr("配座を適用"), [&](Document &next) { applyResultGeometry(next, d); }, true);
    window->showPanel(0);
}
void ConformerSetDialog::queueSelected() {
    checkMolecule();
    QVector<Document> selected;
    QVector<int> indices;
    for (int i = 0; i < entries.size(); ++i)
        if (entries[i].toObject()["selected"].toBool()) {
            selected.append(geometry(i));
            indices.append(i);
        }
    if (selected.isEmpty())
        fail(tr("配座を選んでください。"));
    const auto jobs = enqueue(selected, frequencies->isChecked());
    for (int i = 0; i < jobs.size(); ++i) {
        auto entry = entries[indices[i]].toObject();
        entry["job"] = jobs[i];
        entry.remove("result");
        entry.remove("spectra");
        entry.remove("error");
        entries[indices[i]] = entry;
    }
    refresh();
}
void ConformerSetDialog::refreshResults() {
    if (loader.active())
        return;
    pending.clear();
    for (int i = 0; i < entries.size(); ++i)
        if (!entries[i].toObject()["job"].toString().isEmpty())
            pending.append(i);
    readNext();
}
void ConformerSetDialog::readNext() {
    if (pending.isEmpty()) {
        reading = -1;
        refresh();
        return;
    }
    reading = pending.takeFirst();
    auto item = entries[reading].toObject();
    const auto dir = item["job"].toString();
    try {
        window->checkMcpPath(dir);
        for (const auto *file : {"run.json", "project.molforma.json", "input.nw", "output.out"})
            window->checkMcpPath(QDir(dir).filePath(file));
        const auto job = readHistoryEntry(dir);
        item["state"] = historyStatus(job.status);
        item.remove("result");
        item.remove("spectra");
        entries[reading] = item;
        if (job.status != RunState::Finished) {
            QTimer::singleShot(0, this, &ConformerSetDialog::readNext);
            return;
        }
        pendingOriginal = historyDocument(dir);
        loader.start({.path = QDir(dir).filePath("output.out")});
    } catch (const std::exception &e) {
        item["error"] = QString::fromUtf8(e.what());
        entries[reading] = item;
        QTimer::singleShot(0, this, &ConformerSetDialog::readNext);
    }
}
void ConformerSetDialog::refresh() {
    const QSignalBlocker block(table);
    const int selected = table->currentRow();
    table->setRowCount(entries.size());
    QJsonArray results;
    QVector<int> indices;
    bool complete = true;
    for (int i = 0; i < entries.size(); ++i) {
        const auto row = entries[i].toObject();
        const auto result = row["result"].toObject();
        if (row["selected"].toBool()) {
            complete &= !result.isEmpty();
            if (!result.isEmpty()) {
                results.append(result);
                indices.append(i);
            }
        }
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
    populations.fill(-1, entries.size());
    if (complete && !results.isEmpty()) {
        try {
            const auto rows = conformerPopulations(results, metric->currentData().toString(),
                                                   temperature->value(), tolerance->value(),
                                                   {{"frequency_scale", frequencyScale->value()},
                                                    {"entropy_model", entropyModel->currentData().toString()},
                                                    {"entropy_cutoff", entropyCutoff->value()}});
            for (int k = 0; k < rows.size(); ++k) {
                const auto r = rows[k].toObject();
                const int i = indices[k];
                populations[i] = r["population"].toDouble();
                table->item(i, 6)->setText(QString::number(r["relative_kcal"].toDouble(), 'f', 4));
                table->item(i, 7)->setText(QString::number(r["group"].toInt()));
                table->item(i, 8)->setText(QString::number(100 * r["population"].toDouble(), 'f', 3));
            }
        } catch (const std::exception &e) {
            populations.fill(-1);
            message->setText(message->text() + '\n' + QString::fromUtf8(e.what()));
        }
    }
    const auto members = spectrumMembers();
    spectrumButton->setEnabled(!members.isEmpty());
    if (spectrumDialog)
        spectrumDialog->setMembers(members);
    if (selected >= 0 && selected < table->rowCount())
        table->selectRow(selected);
    else if (table->rowCount())
        table->selectRow(0);
}
QJsonArray ConformerSetDialog::spectrumMembers() const {
    QJsonArray members;
    for (int i = 0; i < entries.size() && i < populations.size(); ++i)
        if (populations[i] >= 0) {
            const auto entry = entries[i].toObject();
            members.append(QJsonObject{
                {"name", entry["name"]}, {"weight", populations[i]}, {"spectra", entry["spectra"]}});
        }
    return members;
}
ConformerSpectrumDialog *ConformerSetDialog::showSpectrum() {
    const auto members = spectrumMembers();
    if (members.isEmpty())
        fail(tr("分布を計算できる配座がありません。選択した配座の結果を読み込んでから開いてください。"));
    if (!spectrumDialog)
        spectrumDialog = new ConformerSpectrumDialog(window, this);
    spectrumDialog->setMembers(members);
    spectrumDialog->show();
    spectrumDialog->raise();
    return spectrumDialog;
}
ConformerSpectrumDialog::ConformerSpectrumDialog(MainWindow *owner, QWidget *parent)
    : QDialog(parent), window(owner) {
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
        connect(b, &QPushButton::clicked, this, [this, action] { window->safe(action); });
    };
    button(tr("CSV…"), "conformerSpectrumCsv", [this] {
        const auto path = QFileDialog::getSaveFileName(this, tr("スペクトルを書き出す"), {}, "CSV (*.csv)");
        if (!path.isEmpty())
            exportCsv(path);
    });
    button("PNG…", "conformerSpectrumPng", [this] {
        if (spectrum.curve.isEmpty())
            fail(message->text());
        const auto path = QFileDialog::getSaveFileName(this, tr("スペクトルを書き出す"), {}, "PNG (*.png)");
        if (!path.isEmpty()) {
            window->checkMcpPath(path);
            if (!chart->grab().save(path))
                fail(tr("画像を保存できません。"));
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
    window->checkMcpPath(path);
    writeFile(path, spectrumCsv(averagedSpectrum(members, parameters())));
}
} // namespace mf
