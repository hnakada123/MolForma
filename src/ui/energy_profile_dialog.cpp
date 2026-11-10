// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/energy_profile_dialog.h"
#include "model/i18n.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "results/energy_profile.h"
#include "ui/main_window.h"
#include "ui/result_charts.h"
#include <QtWidgets>

namespace mf {
EnergyProfileDialog::EnergyProfileDialog(MainWindow *owner) : QDialog(owner), window(owner), loader(this) {
    setObjectName("energyProfileDialog");
    setWindowTitle(tr("熱補正・反応エネルギー図"));
    resize(1050, 700);
    auto *layout = new QVBoxLayout(this);
    auto *settings = new QHBoxLayout;
    layout->addLayout(settings);
    metric = new QComboBox;
    metric->setObjectName("profileMetric");
    for (const auto &pair :
         {qMakePair(tr("電子エネルギー"), QString("E")), qMakePair(QString("E + ZPE"), QString("ZPE")),
          qMakePair(tr("エンタルピー"), QString("H")), qMakePair(tr("Gibbs自由エネルギー"), QString("G"))})
        metric->addItem(pair.first, pair.second);
    unit = new QComboBox;
    unit->setObjectName("profileUnit");
    unit->addItem("kcal/mol", "kcal");
    unit->addItem("kJ/mol", "kj");
    unit->addItem("Hartree", "hartree");
    temperature = new QDoubleSpinBox;
    temperature->setObjectName("profileTemperature");
    temperature->setDecimals(2);
    temperature->setRange(.01, 10000);
    temperature->setValue(298.15);
    temperature->setSuffix(" K");
    reference = new QComboBox;
    reference->setObjectName("profileReference");
    settings->addWidget(metric);
    settings->addWidget(unit);
    settings->addWidget(temperature);
    settings->addWidget(new QLabel(tr("基準")));
    settings->addWidget(reference, 1);
    // Corrections to the thermochemistry NWChem printed (results/energy_profile.h).
    auto *corrections = new QHBoxLayout;
    layout->addLayout(corrections);
    frequencyScale = new QDoubleSpinBox;
    frequencyScale->setObjectName("profileFrequencyScale");
    frequencyScale->setDecimals(4);
    frequencyScale->setRange(.5, 1.5);
    frequencyScale->setSingleStep(.01);
    frequencyScale->setValue(1);
    frequencyScale->setToolTip(tr("ZPE・熱振動エネルギー・振動エントロピーに使う振動数すべてに掛けます。1は"
                                  "NWChemの出力のままです。"));
    entropyModel = new QComboBox;
    entropyModel->setObjectName("profileEntropyModel");
    entropyModel->addItem(tr("調和振動子（NWChemの値）"), "rrho");
    entropyModel->addItem(tr("準RRHO（Grimme）"), "grimme");
    entropyModel->addItem(tr("準調和（Truhlar）"), "truhlar");
    entropyModel->setToolTip(tr("Gの振動エントロピーだけを変えます。Grimmeはしきい値付近から下のモードを自由"
                                "回転子のエントロピーへ滑らかに置き換え、Truhlarはしきい値より低い振動数をし"
                                "きい値に引き上げます。"));
    entropyCutoff = new QDoubleSpinBox;
    entropyCutoff->setObjectName("profileEntropyCutoff");
    entropyCutoff->setDecimals(1);
    entropyCutoff->setRange(10, 500);
    entropyCutoff->setValue(100);
    entropyCutoff->setPrefix(tr("しきい値 "));
    entropyCutoff->setSuffix(" cm⁻¹");
    standardCorrection = new QComboBox;
    standardCorrection->setObjectName("profileStandardCorrection");
    standardCorrection->addItem(tr("1 atm（NWChemの値）"), "none");
    standardCorrection->addItem(tr("1 mol/L（溶液）"), "1M");
    standardCorrection->setToolTip(tr("1 mol/Lでは各成分のGにRT·ln(R′T)（298.15 Kで+1.894 kcal/mol）を加えま"
                                      "す。E・E + ZPE・Hは変わりません。"));
    standard = new QLineEdit("1 atm");
    standard->setObjectName("profileStandardState");
    standard->setToolTip(tr("標準状態の記録用ラベルです。換算は左の「標準状態」の選択で行い、ラベルの文字"
                            "では行いません。"));
    corrections->addWidget(new QLabel(tr("振動数スケール")));
    corrections->addWidget(frequencyScale);
    corrections->addWidget(new QLabel(tr("エントロピー")));
    corrections->addWidget(entropyModel);
    corrections->addWidget(entropyCutoff);
    corrections->addWidget(new QLabel(tr("標準状態")));
    corrections->addWidget(standardCorrection);
    corrections->addWidget(new QLabel(tr("記録用ラベル")));
    corrections->addWidget(standard, 1);
    auto *split = new QSplitter;
    layout->addWidget(split, 1);
    auto *left = new QWidget;
    auto *ll = new QVBoxLayout(left);
    sources = new QTableWidget(0, 3);
    sources->setObjectName("profileSources");
    sources->setHorizontalHeaderLabels({tr("計算名"), "E / Hartree", tr("出典")});
    sources->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sources->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    sources->horizontalHeader()->setStretchLastSection(true);
    ll->addWidget(sources);
    terms = new QTableWidget(0, 4);
    terms->setObjectName("profileTerms");
    terms->setHorizontalHeaderLabels(
        {tr("反応段階"), tr("電子エネルギーのジョブ"), tr("熱補正のジョブ"), tr("係数")});
    terms->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    terms->setSelectionBehavior(QAbstractItemView::SelectRows);
    ll->addWidget(terms, 1);
    auto *actions = new QHBoxLayout;
    ll->addLayout(actions);
    const auto button = [this](QHBoxLayout *row, const QString &title, const QString &name,
                               const std::function<void()> &fn) {
        auto *b = new QPushButton(title);
        b->setObjectName(name);
        row->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, fn] { window->safe(fn); });
    };
    button(actions, tr("段階を追加"), "profileAddStage", [this] {
        if (!records.isEmpty())
            addTerm(tr("段階 %1").arg(terms->rowCount() + 1),
                    {{"record", records.last().toObject()["id"]}, {"coefficient", 1}});
        updateProfile();
    });
    button(actions, tr("成分を追加"), "profileAddTerm", [this] {
        const int row = terms->currentRow();
        if (row >= 0 && !records.isEmpty())
            addTerm(terms->item(row, 0)->text(),
                    {{"record", records.last().toObject()["id"]}, {"coefficient", 1}});
        updateProfile();
    });
    button(actions, tr("行を削除"), "profileRemoveTerm", [this] {
        terms->removeRow(terms->currentRow());
        updateProfile();
    });
    for (const auto &pair : {qMakePair(tr("上へ"), -1), qMakePair(tr("下へ"), 1)})
        button(actions, pair.first, pair.second < 0 ? "profileMoveUp" : "profileMoveDown",
               [this, step = pair.second] {
                   const int row = terms->currentRow();
                   if (row < 0)
                       return;
                   auto p = specification();
                   auto stages = p["stages"].toArray();
                   for (int i = 0; i < stages.size(); ++i)
                       if (stages[i].toObject()["name"] == terms->item(row, 0)->text()) {
                           if (i + step < 0 || i + step >= stages.size())
                               return;
                           const auto stage = stages.takeAt(i);
                           stages.insert(i + step, stage);
                           p["stages"] = stages;
                           setSpecification(p);
                           break;
                       }
               });
    split->addWidget(left);
    auto *right = new QWidget;
    auto *rl = new QVBoxLayout(right);
    chart = new ChartView;
    chart->setObjectName("profileChart");
    rl->addWidget(chart, 2);
    values = new QTableWidget(0, 5);
    values->setObjectName("profileValues");
    // The last two columns are shown for G: against the reference stage.
    values->setHorizontalHeaderLabels(
        {tr("反応段階"), "E / Hartree", tr("相対値"), "exp(−ΔG/RT)", tr("k（Eyring）/ s⁻¹")});
    values->setEditTriggers(QAbstractItemView::NoEditTriggers);
    values->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    rl->addWidget(values, 1);
    split->addWidget(right);
    message = new QLabel;
    message->setObjectName("profileMessage");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto *files = new QHBoxLayout;
    layout->addLayout(files);
    button(files, tr("ジョブを追加…"), "profileAddJob", [this] {
        const auto dir = QFileDialog::getExistingDirectory(this, tr("保存済みジョブを選ぶ"));
        if (!dir.isEmpty())
            addJob(dir);
    });
    button(files, tr("読込を中止"), "profileCancelLoad", [this] {
        loader.cancel();
        message->setText(tr("読込を中止しました。"));
    });
    button(files, tr("保存…"), "profileSave", [this] {
        const auto path = QFileDialog::getSaveFileName(this, tr("比較条件を保存"), {}, "JSON (*.json)");
        if (!path.isEmpty())
            save(path);
    });
    button(files, tr("読込…"), "profileLoad", [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("比較条件を読込"), {}, "JSON (*.json)");
        if (!path.isEmpty())
            load(path);
    });
    button(files, "CSV…", "profileCsv", [this] {
        const auto result = evaluateEnergyProfile(specification());
        const auto path = QFileDialog::getSaveFileName(this, tr("比較表を書き出す"), {}, "CSV (*.csv)");
        if (path.isEmpty())
            return;
        window->checkMcpPath(path);
        // The rate constant (s⁻¹, Eyring) and exp(−ΔG/RT) are empty unless G is compared.
        QByteArray csv = "stage,hartree,relative,unit,metric,temperature,standard_state,frequency_scale,"
                         "entropy_model,entropy_cutoff,standard_state_correction,boltzmann_factor,"
                         "rate_constant\n";
        const auto quote = [](QString s) { return ('"' + s.replace('"', "\"\"") + '"').toUtf8(); };
        const auto optional = [](const QJsonObject &row, const char *key) {
            return row.contains(key) ? QByteArray::number(row[key].toDouble(), 'g', 16) : QByteArray();
        };
        for (const auto &value : result["stages"].toArray()) {
            const auto row = value.toObject();
            csv += quote(row["name"].toString()) + ',' +
                   QByteArray::number(row["hartree"].toDouble(), 'g', 16) + ',' +
                   QByteArray::number(row["relative"].toDouble(), 'g', 16) + ',' +
                   unit->currentData().toString().toUtf8() + ',' + metric->currentData().toString().toUtf8() +
                   ',' + QByteArray::number(temperature->value()) + ',' + quote(standard->text()) + ',' +
                   QByteArray::number(result["frequency_scale"].toDouble()) + ',' +
                   result["entropy_model"].toString().toUtf8() + ',' +
                   QByteArray::number(result["entropy_cutoff"].toDouble()) + ',' +
                   result["standard_state_correction"].toString().toUtf8() + ',' +
                   optional(row, "boltzmann_factor") + ',' + optional(row, "rate_constant") + '\n';
        }
        writeFile(path, csv);
    });
    button(files, "PNG…", "profilePng", [this] {
        evaluateEnergyProfile(specification());
        const auto path = QFileDialog::getSaveFileName(this, tr("反応図を書き出す"), {}, "PNG (*.png)");
        if (!path.isEmpty()) {
            window->checkMcpPath(path);
            if (!chart->grab().save(path))
                fail(tr("画像を保存できません。"));
        }
    });
    connect(&loader, &ResultLoader::failed, this, [this](const QString &error) { message->setText(error); });
    connect(this, &QDialog::finished, &loader, &ResultLoader::cancel);
    connect(&loader, &ResultLoader::loaded, this, [this](const ResultData &r, const QString &stamp) {
        window->safe([&] {
            auto record = energyRecord(r, pending, pendingSettings);
            record["source_stamp"] = stamp;
            records.append(record);
            rebuildSources();
            addTerm(tr("段階 %1").arg(terms->rowCount() + 1), {{"record", record["id"]}, {"coefficient", 1}});
            updateProfile();
        });
    });
    for (auto *combo : {metric, unit, reference, entropyModel})
        connect(combo, &QComboBox::currentIndexChanged, this, &EnergyProfileDialog::updateProfile);
    for (auto *spin : {temperature, frequencyScale, entropyCutoff})
        connect(spin, &QDoubleSpinBox::valueChanged, this, &EnergyProfileDialog::updateProfile);
    connect(standardCorrection, &QComboBox::currentIndexChanged, this, [this] {
        // The label follows the choice while it is one of the two standard texts.
        if (standard->text() == "1 atm" || standard->text() == "1 mol/L")
            standard->setText(standardCorrection->currentData() == "1M" ? "1 mol/L" : "1 atm");
        updateProfile();
    });
    connect(standard, &QLineEdit::editingFinished, this, &EnergyProfileDialog::updateProfile);
    connect(terms, &QTableWidget::cellChanged, this, &EnergyProfileDialog::updateProfile);
    updateProfile();
}
void EnergyProfileDialog::addJob(const QString &directory) {
    if (loader.active() || records.size() >= 200)
        fail(tr("読込の終了を待ってください。比較の上限は200件です。"));
    window->checkMcpPath(directory);
    for (const auto *name : {"run.json", "project.molforma.json", "input.nw", "output.out"})
        window->checkMcpPath(QDir(directory).filePath(name));
    pending = historyDocument(directory);
    pendingSettings = nwchemFinalSettings(pending);
    loader.start({.path = QDir(directory).filePath("output.out")});
    message->setText(tr("計算結果を読み込んでいます…"));
}
void EnergyProfileDialog::rebuildSources() {
    sources->setRowCount(records.size());
    for (int i = 0; i < records.size(); ++i) {
        const auto r = records[i].toObject();
        const QStringList values{r["name"].toString(),
                                 QString::number(r["energy_hartree"].toDouble(), 'g', 14),
                                 r["source"].toString()};
        for (int j = 0; j < values.size(); ++j) {
            auto *item = new QTableWidgetItem(values[j]);
            item->setToolTip(values[j]);
            sources->setItem(i, j, item);
        }
    }
    // Existing components can choose a newly added correction job too.
    for (int i = 0; i < terms->rowCount(); ++i)
        for (int j : {1, 2}) {
            auto *c = qobject_cast<QComboBox *>(terms->cellWidget(i, j));
            const QSignalBlocker block(c);
            const auto selected = c->currentData();
            c->clear();
            if (j == 2)
                c->addItem(tr("同じジョブ"), "");
            for (int k = 0; k < records.size(); ++k)
                c->addItem(QString("%1: %2").arg(k + 1).arg(records[k].toObject()["name"].toString()),
                           records[k].toObject()["id"].toString());
            c->setCurrentIndex(c->findData(selected));
        }
}
void EnergyProfileDialog::addTerm(const QString &stage, const QJsonObject &term) {
    if (terms->rowCount() >= 1000)
        fail(tr("反応図の成分は1000行までです。"));
    const QSignalBlocker block(terms);
    const int row = terms->rowCount();
    terms->insertRow(row);
    terms->setItem(row, 0, new QTableWidgetItem(stage));
    for (int column : {1, 2}) {
        auto *c = new QComboBox;
        if (column == 2)
            c->addItem(tr("同じジョブ"), "");
        for (int i = 0; i < records.size(); ++i)
            c->addItem(QString("%1: %2").arg(i + 1).arg(records[i].toObject()["name"].toString()),
                       records[i].toObject()["id"].toString());
        c->setCurrentIndex(c->findData(term[column == 1 ? "record" : "thermal"].toString()));
        terms->setCellWidget(row, column, c);
        connect(c, &QComboBox::currentIndexChanged, this, &EnergyProfileDialog::updateProfile);
    }
    auto *coefficient = new QDoubleSpinBox;
    coefficient->setRange(.001, 1000);
    coefficient->setDecimals(3);
    coefficient->setValue(term["coefficient"].toDouble(1));
    terms->setCellWidget(row, 3, coefficient);
    connect(coefficient, &QDoubleSpinBox::valueChanged, this, &EnergyProfileDialog::updateProfile);
    terms->setCurrentCell(row, 0);
}
QJsonObject EnergyProfileDialog::specification() const {
    QJsonArray stages;
    QMap<QString, int> named;
    for (int row = 0; row < terms->rowCount(); ++row) {
        const auto name = terms->item(row, 0)->text();
        if (!named.contains(name)) {
            named[name] = stages.size();
            stages.append(QJsonObject{{"name", name}, {"terms", QJsonArray()}});
        }
        auto stage = stages[named[name]].toObject();
        auto components = stage["terms"].toArray();
        QJsonObject term{
            {"record", qobject_cast<QComboBox *>(terms->cellWidget(row, 1))->currentData().toString()},
            {"coefficient", qobject_cast<QDoubleSpinBox *>(terms->cellWidget(row, 3))->value()}};
        const auto thermal = qobject_cast<QComboBox *>(terms->cellWidget(row, 2))->currentData().toString();
        if (!thermal.isEmpty())
            term["thermal"] = thermal;
        components.append(term);
        stage["terms"] = components;
        stages[named[name]] = stage;
    }
    return {{"format", "molforma-energy-profile"},
            {"version", 1},
            {"records", records},
            {"stages", stages},
            {"metric", metric->currentData().toString()},
            {"unit", unit->currentData().toString()},
            {"temperature", temperature->value()},
            {"standard_state", standard->text()},
            {"frequency_scale", frequencyScale->value()},
            {"entropy_model", entropyModel->currentData().toString()},
            {"entropy_cutoff", entropyCutoff->value()},
            {"standard_state_correction", standardCorrection->currentData().toString()},
            {"reference", std::max(0, reference->currentIndex())}};
}
void EnergyProfileDialog::setSpecification(const QJsonObject &p) {
    evaluateEnergyProfile(p);
    loader.cancel();
    const QSignalBlocker bm(metric), bu(unit), bt(temperature), br(reference), bterms(terms),
        bs(frequencyScale), be(entropyModel), bc(entropyCutoff), bstate(standardCorrection);
    terms->setRowCount(0);
    records = p["records"].toArray();
    rebuildSources();
    metric->setCurrentIndex(metric->findData(p["metric"].toString()));
    unit->setCurrentIndex(unit->findData(p["unit"].toString()));
    temperature->setValue(p["temperature"].toDouble());
    standard->setText(p["standard_state"].toString());
    // Comparisons saved before these corrections have none of the fields.
    frequencyScale->setValue(p["frequency_scale"].toDouble(1));
    entropyModel->setCurrentIndex(entropyModel->findData(p["entropy_model"].toString("rrho")));
    entropyCutoff->setValue(p["entropy_cutoff"].toDouble(100));
    standardCorrection->setCurrentIndex(
        standardCorrection->findData(p["standard_state_correction"].toString("none")));
    for (const auto &value : p["stages"].toArray())
        for (const auto &term : value.toObject()["terms"].toArray())
            addTerm(value.toObject()["name"].toString(), term.toObject());
    updateProfile();
    reference->setCurrentIndex(p["reference"].toInt());
    updateProfile();
}
void EnergyProfileDialog::updateProfile() {
    if (!terms || !values)
        return;
    auto p = specification();
    {
        const QSignalBlocker block(reference);
        const auto selected = reference->currentText();
        reference->clear();
        for (const auto &stage : p["stages"].toArray())
            reference->addItem(stage.toObject()["name"].toString());
        reference->setCurrentIndex(std::max(0, reference->findText(selected)));
    }
    p["reference"] = std::max(0, reference->currentIndex());
    const bool gibbs = metric->currentData() == "G";
    entropyCutoff->setEnabled(entropyModel->currentData() != "rrho");
    values->setColumnHidden(3, !gibbs);
    values->setColumnHidden(4, !gibbs);
    try {
        const auto result = evaluateEnergyProfile(p);
        const auto rows = result["stages"].toArray();
        values->setRowCount(rows.size());
        QVector<QPointF> points;
        for (int i = 0; i < rows.size(); ++i) {
            const auto r = rows[i].toObject();
            const auto optional = [&](const char *key) {
                return r.contains(key) ? QString::number(r[key].toDouble(), 'g', 4) : QString("—");
            };
            const QStringList cells{r["name"].toString(), QString::number(r["hartree"].toDouble(), 'g', 14),
                                    QString::number(r["relative"].toDouble(), 'f', 4),
                                    optional("boltzmann_factor"), optional("rate_constant")};
            for (int j = 0; j < cells.size(); ++j)
                values->setItem(i, j, new QTableWidgetItem(cells[j]));
            points << QPointF(i + .75, r["relative"].toDouble())
                   << QPointF(i + 1.25, r["relative"].toDouble());
        }
        chart->setChart(tr("反応エネルギー図"), tr("反応段階"), unit->currentText(),
                        {{metric->currentText(), points}});
        QStringList messages{tr(
            "熱補正はZPEを二重加算せず、HまたはH−TSとして加えます。同じ段階名の行は係数付きで合計します。")};
        // Which corrections change the values NWChem printed.
        QStringList applied;
        if (metric->currentData() != "E" && frequencyScale->value() != 1)
            applied << tr("振動数スケール %1（ZPE・熱振動エネルギー・振動エントロピー）")
                           .arg(frequencyScale->value(), 0, 'g', 6);
        if (gibbs && entropyModel->currentData() != "rrho")
            applied << tr("振動エントロピーは%1（しきい値 %2 cm⁻¹）")
                           .arg(entropyModel->currentText())
                           .arg(entropyCutoff->value(), 0, 'g', 6);
        if (gibbs && standardCorrection->currentData() == "1M")
            applied << tr("標準状態 1 mol/L（各成分のGに +%1 kcal/mol）")
                           .arg(result["standard_state_shift_kcal"].toDouble(), 0, 'f', 3);
        messages << (applied.isEmpty()
                         ? tr("熱補正はNWChemの出力値（調和振動子・理想気体・1 atm）のままです。")
                         : tr("NWChemの出力値に適用した補正: %1。").arg(applied.join(tr("、"))));
        if (gibbs)
            messages << tr("exp(−ΔG/RT)とk＝(k_BT/h)·exp(−ΔG/RT)（透過係数1、ΔG＞0の段階）は基準段階に対する"
                           "値です。");
        for (const auto &warning : result["warnings"].toArray())
            messages << warning.toString();
        message->setText(messages.join('\n'));
    } catch (const std::exception &e) {
        chart->clear();
        values->setRowCount(0);
        message->setText(QString::fromUtf8(e.what()));
    }
}
void EnergyProfileDialog::save(const QString &path) const {
    window->checkMcpPath(path);
    const auto p = specification();
    evaluateEnergyProfile(p);
    writeFile(path, QJsonDocument(p).toJson());
}
void EnergyProfileDialog::load(const QString &path) {
    window->checkMcpPath(path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
        fail(tr("エネルギー比較ファイルを読めません。"));
    setSpecification(parseJson(file.read(16 * 1024 * 1024 + 1)));
}
} // namespace mf
