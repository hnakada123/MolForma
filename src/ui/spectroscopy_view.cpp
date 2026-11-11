// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/spectroscopy_view.h"
#include "model/i18n.h"
#include "results/result_loader.h"
#include "ui/figure_export.h"
#include "ui/main_window.h"
#include "ui/result_charts.h"
#include <QtWidgets>

namespace mf {
SpectroscopyView::SpectroscopyView(MainWindow *owner) : QWidget(owner), window(owner) {
    setObjectName("resultsSpectroscopy");
    auto *layout = new QVBoxLayout(this);
    form = new QFormLayout;
    layout->addLayout(form);
    kind = new QComboBox;
    kind->setObjectName("spectrumKind");
    kind->addItem("NMR", "nmr");
    kind->addItem(tr("通常ラマン"), "raman");
    kind->addItem("ECD", "ecd");
    kind->addItem("ORD (β)", "ord");
    kind->addItem(tr("ESR（溶液・S＝1/2）"), "epr");
    form->addRow(tr("スペクトル"), kind);
    nucleus = new QComboBox;
    nucleus->setObjectName("spectrumNucleus");
    nucleus->addItem("¹H", "1H");
    nucleus->addItem("¹³C", "13C");
    form->addRow(tr("核種"), nucleus);
    reference = new QLineEdit;
    reference->setObjectName("spectrumReference");
    reference->setPlaceholderText(tr("同じ条件のTMS等の遮蔽定数"));
    form->addRow(tr("基準遮蔽定数 / ppm"), reference);
    coupling = new QCheckBox(tr("J結合で分裂させる（一次の多重線）"));
    coupling->setObjectName("spectrumCoupling");
    coupling->setToolTip(tr("計算したJ結合で、等価な核をまとめた多重線にします（¹H・¹⁹F・³¹Pとの結合。¹³Cは"
                            "¹H デカップリング）。"));
    form->addRow(coupling);
    spectrometer = new QDoubleSpinBox;
    spectrometer->setObjectName("spectrumSpectrometer");
    spectrometer->setRange(10, 2000);
    spectrometer->setDecimals(1);
    spectrometer->setValue(400);
    spectrometer->setSuffix(" MHz");
    form->addRow(tr("測定周波数（¹H）"), spectrometer);
    width = new QDoubleSpinBox;
    width->setObjectName("spectrumWidth");
    width->setDecimals(4);
    width->setRange(.0001, 100000);
    width->setValue(.05);
    form->addRow(tr("半値全幅（横軸の単位）"), width);
    microwave = new QDoubleSpinBox;
    microwave->setObjectName("spectrumMicrowave");
    microwave->setRange(.1, 1000);
    microwave->setDecimals(4);
    microwave->setValue(9.5);
    form->addRow(tr("ESRマイクロ波 / GHz"), microwave);
    derivative = new QCheckBox(tr("ESRを一次微分で表示"));
    derivative->setObjectName("spectrumDerivative");
    derivative->setChecked(true);
    form->addRow(derivative);
    minimum = new QLineEdit;
    minimum->setObjectName("spectrumMinimum");
    maximum = new QLineEdit;
    maximum->setObjectName("spectrumMaximum");
    minimum->setPlaceholderText(tr("自動"));
    maximum->setPlaceholderText(tr("自動"));
    auto *range = new QHBoxLayout;
    range->addWidget(minimum);
    range->addWidget(new QLabel("…"));
    range->addWidget(maximum);
    form->addRow(tr("表示範囲（小 → 大）"), range);
    message = new QLabel;
    message->setObjectName("spectrumMessage");
    message->setWordWrap(true);
    message->setTextFormat(Qt::PlainText);
    layout->addWidget(message);
    chart = new ChartView;
    chart->setObjectName("spectrumChart");
    layout->addWidget(chart, 2);
    peaks = new QTableWidget(0, 5);
    peaks->setObjectName("spectrumPeaks");
    peaks->setHorizontalHeaderLabels(
        {tr("原子 / モード"), tr("ピーク位置"), tr("遮蔽定数 / ppm"), tr("重み"), tr("分裂（J / Hz）")});
    peaks->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    peaks->setSelectionBehavior(QAbstractItemView::SelectRows);
    peaks->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(peaks, 1);
    nuclei = new QTableWidget(0, 4);
    nuclei->setObjectName("spectrumEprNuclei");
    nuclei->setHorizontalHeaderLabels({tr("含める"), tr("原子番号"), tr("核種"), "Aiso / MHz"});
    nuclei->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    nuclei->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(nuclei, 1);
    connect(nuclei, &QTableWidget::cellChanged, this, [this] { refresh(); });
    connect(nuclei, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (row >= 0 && row < nuclei->rowCount())
            emit atomSelected(nuclei->item(row, 1)->text().toInt() - 1);
    });
    auto *buttons = new QHBoxLayout;
    layout->addLayout(buttons);
    const auto button = [&](const QString &text, const QString &name, const std::function<void()> &action) {
        auto *b = new QPushButton(text);
        b->setObjectName(name);
        buttons->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, action] { window->safe(action); });
    };
    button(tr("表示条件を保存…"), "spectrumSaveSettings", [this] {
        const auto path = QFileDialog::getSaveFileName(this, tr("表示条件を保存"), {}, "JSON (*.json)");
        if (!path.isEmpty())
            saveSettings(path);
    });
    button(tr("表示条件を読込…"), "spectrumLoadSettings", [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("表示条件を読込"), {}, "JSON (*.json)");
        if (!path.isEmpty())
            loadSettings(path);
    });
    button(tr("CSV…"), "spectrumExportCsv", [this] {
        const auto path = QFileDialog::getSaveFileName(this, tr("スペクトルを書き出す"), {}, "CSV (*.csv)");
        if (!path.isEmpty())
            exportCsv(path);
    });
    button(tr("図…"), "spectrumExportPng", [this] {
        if (spectrum.curve.isEmpty())
            fail(message->text());
        const auto path = askFigurePath(this, tr("スペクトルを書き出す"), "spectrum");
        if (!path.isEmpty()) {
            window->checkMcpPath(path);
            saveFigure(chart, path);
        }
    });
    for (auto *line : {reference, minimum, maximum})
        connect(line, &QLineEdit::editingFinished, this, &SpectroscopyView::refresh);
    connect(width, &QDoubleSpinBox::valueChanged, this, &SpectroscopyView::refresh);
    connect(microwave, &QDoubleSpinBox::valueChanged, this, &SpectroscopyView::refresh);
    connect(derivative, &QCheckBox::toggled, this, &SpectroscopyView::refresh);
    connect(spectrometer, &QDoubleSpinBox::valueChanged, this, &SpectroscopyView::refresh);
    connect(coupling, &QCheckBox::toggled, this, [this](bool on) {
        // Multiplets need lines narrower than their couplings: about 1 Hz.
        const QSignalBlocker block(width);
        width->setValue(on ? std::max(.0005, 1.2 / spectrometer->value()) : .05);
        refresh();
    });
    connect(kind, &QComboBox::currentIndexChanged, this, [this] {
        minimum->clear();
        maximum->clear();
        width->setValue(kind->currentData() == "nmr"   ? .05
                        : kind->currentData() == "epr" ? .1
                        : kind->currentData() == "ecd" ? .2
                                                       : 20);
        refresh();
    });
    connect(nucleus, &QComboBox::currentIndexChanged, this, [this] {
        reference->clear();
        weights = {};
        refresh();
    });
    connect(chart, &ChartView::stickClicked, this, [this](int series, int index) {
        if (series == 1)
            peaks->selectRow(peakOfStick(index));
    });
    connect(peaks, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (row < 0 || row >= spectrum.peaks.size())
            return;
        chart->setSelectedStick(1, firstStick(row));
        const auto peak = spectrum.peaks[row].toObject();
        if (peak.contains("atom"))
            emit atomSelected(peak["atom"].toInt() - 1);
        if (peak.contains("mode"))
            emit modeSelected(peak["mode"].toInt());
        if (peak.contains("excitation"))
            emit excitationSelected(peak["excitation"].toInt());
    });
    connect(peaks, &QTableWidget::cellChanged, this, [this](int row, int column) {
        if (column != 3 || row < 0 || row >= spectrum.peaks.size())
            return;
        const auto atom = spectrum.peaks[row].toObject()["atom"].toInt();
        if (atom <= 0)
            return;
        bool ok;
        const auto weight = peaks->item(row, column)->text().toDouble(&ok);
        if (!ok || !std::isfinite(weight) || weight < 0 || weight > 1e6) {
            refresh();
            message->setText(tr("ピークの重みは0以上1000000以下にしてください。"));
            return;
        }
        weights[QString::number(atom)] = weight;
        refresh();
    });
    refresh();
}
int SpectroscopyView::peakOfStick(int stick) const {
    return spectrum.stickPeaks.isEmpty() ? stick : spectrum.stickPeaks.value(stick, -1);
}
int SpectroscopyView::firstStick(int peak) const {
    return spectrum.stickPeaks.isEmpty() ? peak : int(spectrum.stickPeaks.indexOf(peak));
}
QJsonObject SpectroscopyView::parameters() const {
    QJsonObject p{{"kind", kind->currentData().toString()},
                  {"nucleus", nucleus->currentData().toString()},
                  {"width", width->value()},
                  {"weights", weights}};
    if (p["kind"] == "nmr" && coupling->isChecked()) {
        p["coupling"] = true;
        p["spectrometer_mhz"] = spectrometer->value();
    }
    for (const auto &pair :
         {qMakePair(QString("reference_ppm"), reference), qMakePair(QString("minimum"), minimum),
          qMakePair(QString("maximum"), maximum)}) {
        if (pair.second->text().trimmed().isEmpty())
            continue;
        bool ok;
        const auto n = pair.second->text().toDouble(&ok);
        p[pair.first] = ok ? QJsonValue(n) : QJsonValue();
    }
    if (p["kind"] == "epr") {
        p["microwave_ghz"] = microwave->value();
        p["derivative"] = derivative->isChecked();
        QJsonArray chosen;
        for (int i = 0; i < nuclei->rowCount(); ++i)
            if (nuclei->item(i, 0)->checkState() == Qt::Checked)
                chosen.append(QJsonObject{{"atom", nuclei->item(i, 1)->text().toInt()},
                                          {"isotope", nuclei->item(i, 2)->text()}});
        p["nuclei"] = chosen;
    }
    return p;
}
void SpectroscopyView::setParameters(const QJsonObject &p) {
    calculateSpectrum(result, p); // Validate everything before changing the visible settings.
    const QSignalBlocker blockKind(kind), blockNucleus(nucleus), blockWidth(width), blockNuclei(nuclei),
        blockMicrowave(microwave), blockDerivative(derivative), blockCoupling(coupling),
        blockSpectrometer(spectrometer);
    kind->setCurrentIndex(kind->findData(p["kind"].toString()));
    nucleus->setCurrentIndex(nucleus->findData(p["nucleus"].toString("1H")));
    width->setValue(p["width"].toDouble());
    coupling->setChecked(p["coupling"].toBool());
    if (p.contains("spectrometer_mhz"))
        spectrometer->setValue(p["spectrometer_mhz"].toDouble());
    weights = p["weights"].toObject();
    if (p["kind"] == "epr") {
        microwave->setValue(p["microwave_ghz"].toDouble());
        derivative->setChecked(p["derivative"].toBool());
        QSet<int> atoms;
        for (const auto &value : p["nuclei"].toArray())
            atoms.insert(value.toObject()["atom"].toInt());
        for (int i = 0; i < nuclei->rowCount(); ++i)
            nuclei->item(i, 0)->setCheckState(
                atoms.contains(nuclei->item(i, 1)->text().toInt()) ? Qt::Checked : Qt::Unchecked);
    }
    for (const auto &pair : {qMakePair(QString("reference_ppm"), reference),
                             qMakePair(QString("minimum"), minimum), qMakePair(QString("maximum"), maximum)})
        pair.second->setText(p.contains(pair.first) ? QString::number(p[pair.first].toDouble(), 'g', 16)
                                                    : QString());
    refresh();
}
void SpectroscopyView::setResult(const ResultData &r) {
    if (result.source != r.source || stamp != resultSourceStamp(r.source)) {
        reference->clear();
        minimum->clear();
        maximum->clear();
        weights = {};
    }
    result = r;
    {
        const QSignalBlocker block(nuclei);
        nuclei->setRowCount(0);
        if (r.properties.contains("epr_gshift_ppt")) {
            try {
                const auto rows = eprNuclei(r);
                nuclei->setRowCount(rows.size());
                for (int i = 0; i < rows.size(); ++i) {
                    const auto row = rows[i].toObject();
                    const auto isotope = row["isotope"].toString();
                    const QStringList values{"", QString::number(row["atom"].toInt()), isotope,
                                             QString::number(row["isotropic_mhz"].toDouble(), 'g', 10)};
                    for (int j = 0; j < 4; ++j) {
                        auto *item = new QTableWidgetItem(values[j]);
                        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                        if (j == 0 && !isotope.isEmpty()) {
                            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                            item->setCheckState(QStringList{"1H", "14N", "19F", "31P"}.contains(isotope)
                                                    ? Qt::Checked
                                                    : Qt::Unchecked);
                        }
                        nuclei->setItem(i, j, item);
                    }
                }
            } catch (const std::exception &) {
                nuclei->setRowCount(0);
            }
        }
    }
    if (r.properties.contains("shielding"))
        kind->setCurrentIndex(kind->findData("nmr"));
    else if (r.properties.contains("raman"))
        kind->setCurrentIndex(kind->findData("raman"));
    else if (!r.properties.contains("shielding") && r.properties.contains("epr_gshift_ppt"))
        kind->setCurrentIndex(kind->findData("epr"));
    else if (!r.properties.contains("shielding") && r.properties.contains("ecd"))
        kind->setCurrentIndex(kind->findData("ecd"));
    else if (!r.properties.contains("shielding") && r.properties.contains("ord"))
        kind->setCurrentIndex(kind->findData("ord"));
    stamp = resultSourceStamp(r.source);
    refresh();
}
void SpectroscopyView::refresh() {
    const QSignalBlocker block(peaks);
    const bool nmr = kind->currentData() == "nmr";
    const bool epr = kind->currentData() == "epr";
    microwave->setEnabled(epr);
    derivative->setEnabled(epr);
    nuclei->setVisible(epr);
    form->setRowVisible(width, kind->currentData() != "ord");
    form->setRowVisible(nucleus, nmr);
    form->setRowVisible(reference, nmr);
    const bool couplings = nmr && result.properties.contains("spinspin");
    form->setRowVisible(coupling, couplings);
    form->setRowVisible(spectrometer, couplings);
    spectrometer->setEnabled(coupling->isChecked());
    if (!couplings && coupling->isChecked()) {
        const QSignalBlocker block(coupling);
        coupling->setChecked(false);
    }
    peaks->setColumnHidden(4, !couplings || !coupling->isChecked());
    form->setRowVisible(microwave, epr);
    form->setRowVisible(derivative, epr);
    nucleus->setEnabled(nmr);
    reference->setEnabled(nmr);
    peaks->setColumnHidden(2, !nmr);
    try {
        spectrum = calculateSpectrum(result, parameters());
        chart->setReversedX(spectrum.reversed);
        chart->setXRange(spectrum.parameters["minimum"].toDouble(),
                         spectrum.parameters["maximum"].toDouble());
        chart->setChart(nmr && coupling->isChecked() ? tr("NMR（J結合による一次の多重線）")
                                                     : kind->currentText(),
                        spectrum.xUnit, spectrum.yUnit,
                        {{tr("広がり"), spectrum.curve, ChartTone::Muted},
                         {tr("ピーク"), epr && derivative->isChecked() ? QVector<QPointF>{} : spectrum.sticks,
                          ChartTone::Text, true}});
        message->setText(
            coupling->isChecked() && nmr
                ? tr("δ ≈ σ基準 − σ対象。結合の並びが同じ（トポロジー的に等価な）核を1本にまとめ、遮蔽定数と"
                     "J結合を平均した一次の多重線です。強く結合した系（Δν ≈ J）の2次効果・配座の平均化は含み"
                     "ません。")
                : tr("δ ≈ σ基準 − "
                     "σ対象。反磁性分子の表示用モデルです。J分裂・等価核や配座の平均化は含みません。重"
                     "みは原子数を基準とし、実験の積分値ではありません。"));
        if (!nmr)
            message->setText(tr("通常ラマンの活性を表示用に広げた曲線です。散乱断面積や実験強度への換算は行い"
                                "ません。対応する振動を含むログではピークからモードを選べます。"));
        if (kind->currentData() == "ecd")
            message->setText(
                tr("ECDは長さゲージの旋光強度の符号を保った表示用の曲線です。Δεへの換算は行いません。"));
        if (kind->currentData() == "ord")
            message->setText(tr("ORDはGIAOのβ等方成分（原子単位）を波長順に結びます。比旋光度への換算・共鳴域"
                                "の補間は行いません。"));
        if (epr)
            message->setText(tr("giso＝%1。高速回転・高磁場近似の溶液ESRです。選んだ同位体を100%"
                                "含むモデルで、天然存在比の混合・粉末・高スピンには対応しません。AはNWChemの"
                                "規約に従いA×g/geで補正します。")
                                 .arg(spectrum.parameters["g_isotropic"].toDouble(), 0, 'g', 9));
        peaks->setRowCount(spectrum.peaks.size());
        for (int i = 0; i < spectrum.peaks.size(); ++i) {
            const auto p = spectrum.peaks[i].toObject();
            QStringList atoms;
            for (const auto &atom : p["atoms"].toArray())
                atoms << QString::number(atom.toInt());
            const QStringList values{
                !atoms.isEmpty()     ? atoms.join(", ")
                : p.contains("atom") ? QString::number(p["atom"].toInt())
                : p.contains("mode") ? QString::number(p["mode"].toInt() + 1)
                : p.contains("root") ? QString::number(p["root"].toInt())
                                     : QString::number(p["raman_index"].toInt()),
                QString::number(p["x"].toDouble(), 'g', 10),
                p.contains("shielding_ppm") ? QString::number(p["shielding_ppm"].toDouble(), 'g', 10) : "—",
                QString::number(p["weight"].toDouble(), 'g', 10),
                p.contains("multiplet")
                    ? (p["j_hz"].toString().isEmpty()
                           ? p["multiplet"].toString()
                           : QString("%1 (%2)").arg(p["multiplet"].toString(), p["j_hz"].toString()))
                    : QString()};
            for (int j = 0; j < values.size(); ++j) {
                auto *item = new QTableWidgetItem(values[j]);
                // Weights are per atom: a peak of several equivalent atoms takes them from the atom list.
                if (j != 3 || !p.contains("atom") || p.contains("atoms"))
                    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                peaks->setItem(i, j, item);
            }
        }
    } catch (const std::exception &e) {
        spectrum = {};
        chart->clear();
        peaks->setRowCount(0);
        message->setText(QString::fromUtf8(e.what()));
    }
}
void SpectroscopyView::selectAtom(int index) {
    for (int i = 0; i < spectrum.peaks.size(); ++i)
        if (spectrum.peaks[i].toObject()["atom"].toInt(-1) == index + 1 ||
            spectrum.peaks[i].toObject()["atoms"].toArray().contains(index + 1)) {
            const QSignalBlocker block(peaks);
            peaks->selectRow(i);
            chart->setSelectedStick(1, firstStick(i));
            return;
        }
}
void SpectroscopyView::saveSettings(const QString &path) const {
    window->checkMcpPath(path);
    calculateSpectrum(result, parameters());
    writeFile(path, QJsonDocument(QJsonObject{{"format", "molforma-spectrum"},
                                              {"version", 1},
                                              {"source", result.source},
                                              {"source_stamp", stamp},
                                              {"parameters", parameters()}})
                        .toJson());
}
void SpectroscopyView::loadSettings(const QString &path) {
    window->checkMcpPath(path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        fail(tr("スペクトルの表示条件を読めません。"));
    const auto p = parseJson(file.read(1024 * 1024 + 1));
    if (p["format"] != "molforma-spectrum" || p["version"] != 1 || p["source"] != result.source ||
        p["source_stamp"] != stamp)
        fail(tr("表示条件の形式または結果ファイルが一致しません。保存時の結果を読み込んでください。"));
    setParameters(p["parameters"].toObject());
}
void SpectroscopyView::exportCsv(const QString &path) const {
    window->checkMcpPath(path);
    writeFile(path, spectrumCsv(calculateSpectrum(result, parameters())));
}
} // namespace mf
