// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/coordinates.h"
#include "foundation/files.h"
#include "model/geometry.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_execution.h"
#include "result_support.h"
#include "results/energy_profile.h"
#include "results/field_analysis.h"
#include "results/result_files.h"
#include "results/result_path.h"
#include "results/result_summary.h"
#include "results/spectrum.h"
#include "ui/calculation_panel.h"
#include "ui/conformer_set_dialog.h"
#include "ui/energy_profile_dialog.h"
#include "ui/execution_panel.h"
#include "ui/main_window.h"
#include "ui/molecule_view.h"
#include "ui/property_geometry_view.h"
#include "ui/result_charts.h"
#include "ui/result_energies_view.h"
#include "ui/result_excitations_view.h"
#include "ui/result_measurement.h"
#include "ui/result_optimization_view.h"
#include "ui/result_path_view.h"
#include "ui/result_properties_view.h"
#include "ui/results_panel.h"
#include "ui/run_progress.h"
#include "ui/spectroscopy_view.h"
#include <QtTest>
#include <QtWidgets>
#include <algorithm>
#include <cmath>
using namespace mf;

namespace {
class TestUiServices : public UiServices {
  public:
    QMap<QString, QVariant> preferences;
    mutable QStringList checkedPaths;
    void safe(const std::function<void()> &operation) override {
        operation();
    }
    QVariant preference(const QString &key, const QVariant &fallback = {}) const override {
        return preferences.value(key, fallback);
    }
    void setPreference(const QString &key, const QVariant &value) override {
        preferences[key] = value;
    }
    void checkMcpPath(const QString &path) const override {
        checkedPaths << path;
    }
    bool outsideMcpFolders(const QString &) const override {
        return false;
    }
    void showMessage(const QString &, int = 0) override {}
    void addStatusWidget(QWidget *) override {}
};
} // namespace

class ResultsTest : public QObject, ResultFiles {
    Q_OBJECT
    QTemporaryDir settings;
    template <typename T> static T *widget(MainWindow &w, const char *name) {
        auto *p = w.findChild<T *>(name);
        Q_ASSERT(p);
        return p;
    }
  private slots:
    void initTestCase() {
        QVERIFY(files.isValid());
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settings.path()); // MolForma uses NativeFormat.
        qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
        qputenv("XDG_DATA_HOME", settings.filePath("data").toUtf8());
    }
    void independentViewerAndCancellation() {
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        auto *tabs = widget<QTabWidget>(w, "workspaceTabs");
        QCOMPARE(tabs->count(), 6);
        QCOMPARE(tabs->tabText(4), "結果ビューア");
        tabs->setCurrentIndex(4);
        const auto before = w.document().project();
        const int history = w.history()->count();
        QSignalSpy done(panel, &ResultsPanel::loaded), failed(panel, &ResultsPanel::loadFailed);
        const auto a = save("viewer.xyz", "2\nframe 1\nH 0 0 0\nH 0 0 1\n2\nframe 2\nH 0 0 0\nH 0 0 2\n");
        panel->openFile(a);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(panel->result().frames.size(), 2);
        panel->playTrajectory();
        QCOMPARE(panel->viewerState().animation, ResultAnimation::Frames);
        panel->selectFrame(0);
        QCOMPARE(panel->viewerState().animation, ResultAnimation::None);
        QCOMPARE(panel->viewerState().frame, 0);
        panel->selectFrame(1);
        const auto output = files.filePath("saved.xyz");
        panel->saveGeometry(output);
        QCOMPARE(Document::fromXyz(QString::fromUtf8(readFile(output))).atoms[1].position.z(), 2.0);
        widget<QSpinBox>(w, "resultsFrame")->setValue(1);
        panel->saveGeometry(output);
        QCOMPARE(Document::fromXyz(QString::fromUtf8(readFile(output))).atoms[1].position.z(), 1.0);
        panel->openFile(a);
        panel->cancelLoad();
        QVERIFY(!panel->loading());
        QCOMPARE(panel->result().source, a);
        panel->openFile(save("first.cube", cubeText()));
        panel->openFile(a);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
        QCOMPARE(panel->result().format, "XYZ");
        panel->openFile(save("broken.cube", "bad"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QCOMPARE(panel->result().source, a);
        panel->openFile(save("modes.molden", moldenText()));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 3, 10000);
        widget<QTableWidget>(w, "resultsModes")->setCurrentCell(1, 0);
        widget<QPushButton>(w, "resultsPlayMode")->click();
        QCOMPARE(panel->viewerState().animation, ResultAnimation::Mode);
        QCOMPARE(panel->viewerState().mode, 1);
        QTest::qWait(100);
        panel->saveGeometry(output);
        QVERIFY(std::abs(Document::fromXyz(QString::fromUtf8(readFile(output))).atoms[1].position.z() -
                         1.4 * BohrToAngstrom) < 1e-10);
        QCOMPARE(w.document().project(), before);
        QCOMPARE(w.history()->count(), history);
        // Destruction cancels an in-flight reader and stops animation.
        panel->openFile(a);
    }
    void pathViewAsksForTheFrameOfAPoint() {
        // The tab of the paths without its viewer: fed a result with a scan it lists the points, and
        // asks whoever composes it for the structure of the one chosen. "最高点を選択" chooses the
        // highest point between the ends.
        const auto data = readResult(QString(FIXTURE_DIR) + "/nwchem/butane_scan.out");
        const auto path = resultPath(data);
        QCOMPARE(path.kind, ResultPathKind::Scan);
        TestUiServices services;
        ResultPathView view(services);
        QSignalSpy frames(&view, &ResultPathView::frameRequested),
            saddle(&view, &ResultPathView::saddleSearchRequested);
        view.setResult(data);
        QCOMPARE(frames.count(), 0); // Showing a result chooses no point.
        auto *table = view.findChild<QTableWidget *>("resultsPath");
        QCOMPARE(table->rowCount(), int(path.points.size()));
        QCOMPARE(view.shown().points.size(), path.points.size());
        const int highest = path.highest(PathEnds::Excluded);
        QVERIFY(highest > 0 && highest < path.points.size() - 1);
        auto *choose = view.findChild<QPushButton *>("resultsPathHighest");
        QVERIFY(choose->isEnabled());
        choose->click();
        QCOMPARE(table->currentRow(), highest);
        QCOMPARE(frames.count(), 1);
        QCOMPARE(frames[0][0].toInt(), path.points[highest].frame);
        // A row is a point too; one whose structure the result does not hold asks for nothing.
        table->setCurrentCell(0, 0);
        QCOMPARE(frames.count(), 2);
        QCOMPARE(frames[1][0].toInt(), path.points[0].frame);
        auto fewer = data;
        fewer.frames.resize(path.points[highest].frame);
        view.setResult(fewer);
        view.findChild<QPushButton *>("resultsPathHighest")->click();
        QCOMPARE(table->currentRow(), highest);
        QCOMPARE(frames.count(), 2);
        // The saddle search is the viewer's: the view only asks for it, while it may.
        auto *search = view.findChild<QPushButton *>("resultsPathSaddle");
        view.setSaddleEnabled(false);
        QVERIFY(!search->isEnabled());
        view.setSaddleEnabled(true);
        search->click();
        QCOMPARE(saddle.count(), 1);
        // Without a path there is nothing to choose.
        view.setResult(ResultData{});
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(view.shown().points.isEmpty());
        QVERIFY(!choose->isEnabled());
    }
    void resultViewsOnTheirOwn() {
        // The other tabs that only show a result, each without the viewer.
        const QString root = QString(FIXTURE_DIR) + "/nwchem/";
        TestUiServices services;
        {
            // The differences refer to the last energy until a row is made the reference.
            const auto data = readResult(root + "water_optfreq.out");
            QVERIFY(data.energies.size() > 2);
            ResultEnergiesView view;
            view.setResult(data);
            auto *table = view.findChild<QTableWidget *>("resultsEnergy");
            const int last = data.energies.size() - 1;
            QCOMPARE(table->rowCount(), last + 1);
            QCOMPARE(table->item(last, 2)->text(), QString("0.000"));
            QVERIFY(table->item(last, 0)->font().bold() && !table->item(0, 0)->font().bold());
            auto *reference = view.findChild<QPushButton *>("resultsEnergyReference");
            view.setReferenceEnabled(false);
            QVERIFY(!reference->isEnabled());
            view.setReferenceEnabled(true);
            table->setCurrentCell(0, 0);
            reference->click();
            QCOMPARE(table->item(0, 2)->text(), QString("0.000"));
            QVERIFY(table->item(0, 0)->font().bold() && !table->item(last, 0)->font().bold());
            auto *unit = view.findChild<QComboBox *>("resultsEnergyUnit");
            unit->setCurrentIndex(unit->findData("hartree"));
            QCOMPARE(table->horizontalHeaderItem(2)->text(), QString("ΔE / Hartree"));
            QCOMPARE(table->item(last, 2)->text(),
                     QString::number(data.energies[last].hartree - data.energies[0].hartree, 'f', 8));
            view.setResult(data); // Another result: its last energy again.
            QCOMPARE(table->item(last, 2)->text(), QString::number(0., 'f', 8));
        }
        {
            // The steps of the optimization, with the criteria of the last one as cards.
            const auto data = readResult(root + "water_optfreq.out");
            QVERIFY(!data.steps.isEmpty());
            ResultOptimizationView view(services);
            view.setResult(data, view.palette());
            QCOMPARE(view.findChild<QTableWidget *>("resultsSteps")->rowCount(), int(data.steps.size()));
            QVERIFY(!view.findChild<QWidget *>("resultsCriteria")->isHidden());
            QVERIFY(view.findChild<QWidget *>("resultsCriterionGmax"));
            QCOMPARE(view.findChild<ChartView *>("resultsEnergyChart")->seriesData().size(), 1);
            view.setResult(ResultData{}, view.palette());
            QVERIFY(view.findChild<QWidget *>("resultsCriteria")->isHidden());
            QVERIFY(view.findChild<ChartView *>("resultsEnergyChart")->seriesData().isEmpty());
        }
        {
            // A bond index names its two atoms, for the viewer to show them in the last structure.
            const auto data = readResult(root + "ethanol_mulliken.out");
            ResultPropertiesView view;
            QSignalSpy pairs(&view, &ResultPropertiesView::atomPairSelected);
            view.setResult(data);
            QCOMPARE(pairs.count(), 0);
            auto *bonds = view.findChild<QTableWidget *>("resultsBondIndices");
            QVERIFY(bonds->rowCount() > 0 && !bonds->parentWidget()->isHidden());
            QVERIFY(view.findChild<QWidget *>("resultsCouplings")->parentWidget()->isHidden());
            const auto pair = bonds->item(0, 0)->data(Qt::UserRole).toPoint();
            bonds->setCurrentCell(0, 0);
            QCOMPARE(pairs.count(), 1);
            QCOMPARE(pairs[0][0].toInt(), pair.x());
            QCOMPARE(pairs[0][1].toInt(), pair.y());
            QVERIFY(pair.x() >= 0 && pair.y() > pair.x());
            const int notable = bonds->rowCount();
            view.findChild<QCheckBox *>("resultsBondIndicesAll")->setChecked(true);
            QVERIFY(bonds->rowCount() > notable);
            QVERIFY(view.findChild<QLabel *>("resultsPropertySummary")->text().contains("双極子モーメント"));
            // The lines "概要" shares with this tab: none of this log, the correction of another.
            QVERIFY(resultPropertyLines(data).isEmpty());
            QVERIFY(resultPropertyLines(readResult(root + "water_dimer_bsse.out")).contains("BSSE"));
        }
        {
            // The orbital pairs of the chosen state; one of them names its orbital for the viewer, and
            // the note names the MOLDEN file the viewer knows of.
            const auto data = readResult(root + "formaldehyde_tda.out");
            QVERIFY(data.excitations.size() > 1);
            QString molden = "job.molden";
            ResultExcitationsView view(services, [&] { return molden; });
            QSignalSpy orbitals(&view, &ResultExcitationsView::orbitalRequested);
            view.setResult(data);
            auto *states = view.findChild<QTableWidget *>("resultsExcitations");
            auto *listed = view.findChild<QTableWidget *>("resultsContributions");
            auto *title = view.findChild<QLabel *>("resultsContributionsTitle");
            QCOMPARE(states->rowCount(), int(data.excitations.size()));
            QCOMPARE(states->currentRow(), 0);
            const auto first = resultTransitions(data.excitations[0]);
            QVERIFY(!first.isEmpty());
            QCOMPARE(listed->rowCount(), int(first.size()));
            QVERIFY(title->text().contains("job.molden"));
            emit listed->cellDoubleClicked(0, 1);
            QCOMPARE(orbitals.count(), 1);
            QCOMPARE(orbitals[0][0].toString(), QString(first[0].spin == "beta" ? "Beta" : "Alpha"));
            QCOMPARE(orbitals[0][1].toInt(), first[0].to);
            emit listed->cellDoubleClicked(0, 0);
            QCOMPARE(orbitals[1][1].toInt(), first[0].from);
            molden.clear();
            view.selectState(1);
            QCOMPARE(states->currentRow(), 1);
            QCOMPARE(listed->rowCount(), int(resultTransitions(data.excitations[1]).size()));
            QVERIFY(!title->text().contains("job.molden"));
            view.selectState(int(data.excitations.size())); // Out of range: the state stays.
            QCOMPARE(states->currentRow(), 1);
            QCOMPARE(view.findChild<ChartView *>("resultsSpectrum")->seriesData().size(), 2);
        }
        {
            // The measurement keeps atoms by number: a kept one follows the next structure.
            auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
            MoleculeView molecule;
            ResultMeasurement card(&molecule, services);
            QSignalSpy clicked(&card, &ResultMeasurement::atomClicked);
            card.setDocument(d);
            QVERIFY(card.isHidden());
            auto *label = card.findChild<QLabel *>("resultsMeasurement");
            emit molecule.atomClicked(d.atoms[0].id, Qt::NoModifier);
            emit molecule.atomClicked(d.atoms[2].id, Qt::NoModifier);
            QCOMPARE(clicked.count(), 2);
            QCOMPARE(clicked[1][0].toInt(), 2);
            QVERIFY(!card.isHidden());
            QVERIFY(label->text().contains("O1 → H3") && label->text().contains("0.9103"));
            QCOMPARE(molecule.selection(), (Ids{d.atoms[0].id, d.atoms[2].id}));
            card.findChild<QPushButton *>("resultsPinMeasurement")->click();
            QVERIFY(molecule.selection().isEmpty());
            QVERIFY(label->text().contains("1件"));
            // Another structure with other atom IDs: the picked numbers select its atoms.
            auto next = Document::fromXyz("3\nwater\nO 0 0 0\nH 0.9 0 0.5\nH -0.9 0 0.5\n");
            card.setPicked({0, 1});
            card.setDocument(next);
            QCOMPARE(molecule.selection(), (Ids{next.atoms[0].id, next.atoms[1].id}));
            card.clear();
            card.setDocument(next);
            QVERIFY(card.isHidden() && molecule.selection().isEmpty());
            QCOMPARE(card.controls().size(), 2);
        }
    }
    void hiddenViewerFitsWhenShown() {
        // A result that loads while another tab is shown (a finished run) fills the view once opened.
        MainWindow w;
        w.show();
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openFile(save("hidden-fit.xyz", "2\n\nH 0 0 0\nH 0 0 1\n"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentWidget(panel);
        auto *view = widget<MoleculeView>(w, "resultsView");
        const auto ids = view->documentRef().ids();
        QTRY_VERIFY(QLineF(view->atomScreenPosition(ids[0]), view->atomScreenPosition(ids[1])).length() > 25);
    }
    void outcomeTabsAndJobMolden() {
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        auto *details = widget<QTabWidget>(w, "resultsDetails");
        // Before anything is loaded only the summary, which says what to open.
        QVERIFY(details->isTabVisible(0));
        for (int i = 1; i < details->count(); ++i)
            QVERIFY(!details->isTabVisible(i));
        auto *pin = widget<QPushButton>(w, "resultsPinMeasurement");
        QVERIFY(!pin->isEnabled() && !widget<QSlider>(w, "resultsYaw")->isEnabled()); // Nothing to view.
        QTemporaryDir job;
        const QString log = "NWChem\n" + geometry() +
                            "@ Step       Energy      Delta E   Gmax     Grms     Xrms     Xmax   Walltime\n"
                            "@ ---- ---------------- -------- -------- -------- -------- -------- --------\n"
                            "@    0     -76.00723755  0.0D+00  0.06730  0.02421  0.00000  0.00000      0.3\n"
                            "      Optimization converged\n"
                            "         Total SCF energy =    -76.00723755\n"
                            "                       ROHF Final Molecular Orbital Analysis\n"
                            " Vector    5  Occ=2.000000D+00  E=-3.907179D-01\n"
                            " Vector    6  Occ=0.000000D+00  E= 6.054023D-01\n";
        writeFile(job.filePath("output.out"), log.toUtf8());
        // H2 with all 3N modes: five translations/rotations near 0 cm⁻¹ and the stretch.
        QString moldenFile =
            "[Molden Format]\n[Atoms] AU\nH 1 1 0 0 0\nH 2 1 0 0 1.4\n[GTO]\n1 0\ns 1 1.0\n1.0 1.0\n"
            "[MO]\nSym= A1\nEne= -0.5D+00\nSpin= Alpha\nOccup= 2.0\n1 0.5\n2 0.5\n"
            "[FREQ]\n0.1\n0.2\n0.3\n12\n15\n4400\n[FR-COORD]\nH 0 0 0\nH 0 0 1.4\n[FR-NORM-COORD]\n";
        for (int mode = 1; mode <= 6; ++mode)
            moldenFile += QString("vibration %1\n0 0 .5\n0 0 -.5\n").arg(mode);
        moldenFile += "[INT]\n9\n9\n9\n9\n9\n20\n";
        writeFile(job.filePath("job.molden"), moldenFile.toUtf8());
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openDirectory(job.path());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QVERIFY(pin->isEnabled());
        // Only the tabs with data, and the outcome in one line.
        QVERIFY(details->isTabVisible(1) && details->isTabVisible(2) && details->isTabVisible(4));
        QVERIFY(!details->isTabVisible(3) && !details->isTabVisible(5) && !details->isTabVisible(8));
        QVERIFY(panel->briefSummary().contains("最適化: 収束"));
        // Values to read have the minus sign of "履歴"; table cells keep "-".
        QVERIFY(panel->briefSummary().contains("E = −"));
        // Said once, in "概要": the message box under the details is for loading, errors and
        // comparisons, while the MCP results still read the outcome as the message.
        QVERIFY(widget<QPlainTextEdit>(w, "resultsStatus")->toPlainText().isEmpty());
        QVERIFY(widget<QPlainTextEdit>(w, "resultsStatus")->isHidden());
        QVERIFY(panel->message().contains("最適化: 収束"));
        QCOMPARE(widget<QTableWidget>(w, "resultsOrbitals")->item(0, 0)->text(), QString("5"));
        // The log has orbital energies; the shapes are one click away in the job's MOLDEN file.
        auto *molden = widget<QPushButton>(w, "resultsOpenMolden");
        QVERIFY(!molden->isHidden());
        molden->click();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
        QVERIFY(panel->result().source.endsWith("job.molden"));
        QVERIFY(molden->isHidden());
        QVERIFY(details->isTabVisible(3));
        // Translations and rotations are listed only on request, without an IR intensity.
        auto *modes = widget<QTableWidget>(w, "resultsModes");
        for (int i = 0; i < 5; ++i)
            QVERIFY(modes->isRowHidden(i));
        QVERIFY(!modes->isRowHidden(5));
        QCOMPARE(modes->item(0, 1)->text(), QString("—"));
        widget<QCheckBox>(w, "resultsShowRigid")->setChecked(true);
        QVERIFY(!modes->isRowHidden(0));
    }
    void outcomeOverviewAndDock() {
        // A log cut short (no "Total times"): two structures, an optimization, orbitals 1–2 and modes.
        QTemporaryDir job;
        const QString log =
            "NWChem\n" + geometry() + geometry() +
            " maximum gradient threshold         (gmax) =   0.000450\n"
            " rms gradient threshold             (grms) =   0.000300\n"
            "@ Step       Energy      Delta E   Gmax     Grms     Xrms     Xmax   Walltime\n"
            "@ ---- ---------------- -------- -------- -------- -------- -------- --------\n"
            "@    0     -76.00723755  0.0D+00  0.06730  0.02421  0.00000  0.00000      0.3\n"
            "@    1     -76.01098000 -3.7D-03  0.00035  0.00348  0.02135  0.04987      0.7\n"
            "      Optimization converged\n"
            "                       DFT Final Molecular Orbital Analysis\n"
            " Vector    1  Occ=2.000000D+00  E=-5.000000D-01\n"
            " Vector    2  Occ=0.000000D+00  E= 2.500000D-01\n"
            " (Projected Frequencies expressed in cm-1)\n\n P.Frequency -100 1600\n\n" +
            [] {
                QString rows;
                for (int i = 0; i < 9; ++i)
                    rows += QString(" %1 %2 %3\n").arg(i + 1).arg(i == 0 ? 1 : 0).arg(i == 4 ? .5 : 0);
                return rows;
            }() +
            "\n Normal Eigenvalue || Projected Infra Red Intensities\n Mode [cm**-1] || units units "
            "units units\n ---- ---- || ---- ---- ---- ----\n"
            " 1 -100 || 0.1 0.2 5.5 1.0\n 2 1600 || 0.2 0.3 8.5 1.2\n ----\n";
        writeFile(job.filePath("output.out"), log.toUtf8());
        writeFile(job.filePath("job.molden"),
                  "[Molden Format]\n[Atoms] AU\nH 1 1 0 0 0\nH 2 1 0 0 1.4\n[GTO]\n1 0\ns 1 1.0\n1.0 1.0\n"
                  "2 0\ns 1 1.0\n1.0 1.0\n"
                  "[MO]\nSym= A1\nEne= -0.5\nSpin= Alpha\nOccup= 2.0\n1 0.5\n2 0.5\n"
                  "Sym= B1\nEne= 0.25\nSpin= Alpha\nOccup= 0\n1 0.5\n2 -0.5\n");
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentWidget(panel);
        w.show();
        auto *details = widget<QTabWidget>(w, "resultsDetails");
        auto *dock = widget<QStackedWidget>(w, "resultsDockPages");
        QVERIFY(widget<QLabel>(w, "resultsOutcome")->isHidden() &&
                widget<QFrame>(w, "resultsBanner")->isHidden());
        // Nothing open: no camera controls or timeline to wait for a structure.
        auto *controls = widget<QWidget>(w, "resultsViewControls");
        QVERIFY(controls->isHidden() && widget<QWidget>(w, "resultsDock")->isHidden());
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openDirectory(job.path());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QVERIFY(controls->isVisible() && widget<QWidget>(w, "resultsDock")->isVisible());
        // Narrow, the actions keep their icons and the bar its one row; wide, their names come back.
        auto *bar = widget<QWidget>(w, "resultBar");
        auto *compare = widget<QPushButton>(w, "resultsCompare");
        w.resize(2400, 900);
        QTRY_COMPARE(compare->text(), QString("分子と比較"));
        w.resize(900, 800); // The narrowest window, with a short job name here.
        QTRY_VERIFY(compare->text().isEmpty());
        QCOMPARE(compare->accessibleName(), QString("分子と比較"));
        QVERIFY(compare->toolTip().startsWith("分子と比較："));
        QTRY_COMPARE(bar->layout()->heightForWidth(bar->width()), bar->layout()->sizeHint().height());
        w.resize(2400, 900);
        QTRY_COMPARE(compare->text(), QString("分子と比較"));
        // How the run ended: a banner over the details, not again in the bar or "概要".
        QCOMPARE(resultOutcome(panel->result()), QString("incomplete"));
        QVERIFY(widget<QLabel>(w, "resultsOutcome")->isHidden());
        QVERIFY(widget<QFrame>(w, "resultsBanner")->isVisible());
        for (auto *label : widget<QScrollArea>(w, "resultsOverview")->widget()->findChildren<QLabel *>())
            QVERIFY(label->text() != panel->result().status);
        QVERIFY(widget<QLabel>(w, "resultsBannerText")->text().contains("最適化は収束しています"));
        // Tab names count what they list; the summary cards open their tabs.
        QCOMPARE(details->tabText(3), QString("振動 2"));
        QCOMPARE(details->tabText(4), QString("最適化 2"));
        widget<QPushButton>(w, "resultsTileImaginary")->click();
        QCOMPARE(details->currentIndex(), 3);
        details->setCurrentIndex(0);
        widget<QPushButton>(w, "resultsTileOptimization")->click();
        QCOMPARE(details->currentIndex(), 4);
        QCOMPARE(widget<QFrame>(w, "resultsCriterionGmax")->property("tone").toString(), QString("good"));
        QCOMPARE(widget<QFrame>(w, "resultsCriterionGrms")->property("tone").toString(), QString("warn"));
        // Under the view: the structures first, with a timeline to pick one.
        QCOMPARE(dock->currentIndex(), 0);
        QCOMPARE(widget<QLabel>(w, "resultsContextTitle")->text(), QString("構造 2 / 2（最後）"));
        emit widget<FrameTimeline>(w, "resultsTimeline")->frameSelected(0);
        QCOMPARE(widget<QSpinBox>(w, "resultsFrame")->value(), 1);
        // A stick of the IR spectrum picks its mode (the imaginary one has no stick) and plays it below.
        emit widget<ChartView>(w, "resultsIrSpectrum")->stickClicked(1, 0);
        QCOMPARE(widget<QTableWidget>(w, "resultsModes")->currentRow(), 1);
        QCOMPARE(dock->currentIndex(), 2);
        QVERIFY(widget<QLabel>(w, "resultsContextTitle")->text().startsWith("モード 2 · 1600.0"));
        widget<QPushButton>(w, "resultsCloseMode")->click();
        QCOMPARE(dock->currentIndex(), 0);
        QCOMPARE(widget<QTableWidget>(w, "resultsModes")->currentRow(), -1);
        // The log's LUMO opens in the job's MOLDEN file as the same orbital (spin and number).
        auto *table = widget<QTableWidget>(w, "resultsOrbitals");
        QVERIFY(table->isColumnHidden(3)); // One spin: no column saying "Alpha" on every row.
        emit table->cellDoubleClicked(1, 0);
        QTRY_VERIFY_WITH_TIMEOUT(panel->result().source.endsWith("job.molden"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(panel->result().selectedOrbital, 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!panel->loading(), 10000);
        QCOMPARE(dock->currentIndex(), 1);
        QVERIFY(widget<QLabel>(w, "resultsContextTitle")->text().startsWith("LUMO · Alpha 2番"));
        QVERIFY(widget<QFrame>(w, "resultsBanner")->isHidden()); // A MOLDEN file has no outcome to judge.
        QCOMPARE(frontierName(panel->result().orbitals, 0), QString("HOMO"));
    }
    void chosenRowStaysInSightAsTheTableGetsLower() {
        auto log = "NWChem\n" + geometry() + "                       DFT Final Molecular Orbital Analysis\n";
        for (int i = 1; i <= 60; ++i)
            log += QString(" Vector %1  Occ=%2  E=%3D+00\n")
                       .arg(i, 4)
                       .arg(i <= 30 ? "2.000000D+00" : "0.000000D+00")
                       .arg(-3 + i * .1, 9, 'f', 6);
        const auto path = save("many_orbitals.out", log);
        MainWindow w;
        w.resize(1380, 900);
        w.show();
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentWidget(panel);
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openFile(path);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        auto *details = widget<QTabWidget>(w, "resultsDetails");
        auto *table = widget<QTableWidget>(w, "resultsOrbitals");
        for (int i = 0; i < details->count(); ++i)
            if (details->tabText(i).startsWith("軌道"))
                details->setCurrentIndex(i);
        QTRY_VERIFY(table->isVisible() && table->rowCount() == 60);
        auto *rows = table->viewport();
        auto *bar = table->verticalScrollBar();
        const auto inSight = [&] { return rows->rect().contains(table->visualRect(table->currentIndex())); };
        const auto panelOpen = [&](bool open) {
            const int height = rows->height();
            w.executionPanel()->showRunPanel(open);
            QTRY_VERIFY(open ? rows->height() < height - 100 : rows->height() > height + 100);
            QTest::qWait(50);
        };
        // A row low in the table moves up with it, with its neighbors in sight.
        table->setCurrentCell(table->rowAt(rows->height() - 30), 0);
        QVERIFY(inSight());
        panelOpen(true);
        QVERIFY(inSight());
        const auto chosen = table->visualRect(table->currentIndex());
        QVERIFY(chosen.top() >= chosen.height() && chosen.bottom() + chosen.height() < rows->height());
        panelOpen(false);
        QVERIFY(inSight());
        // A row in sight already stays where it is.
        table->scrollToTop();
        table->setCurrentCell(1, 0);
        panelOpen(true);
        QCOMPARE(bar->value(), 0);
        panelOpen(false);
        // A row scrolled away stays away, on either side.
        table->setCurrentCell(55, 0);
        table->scrollToTop();
        QVERIFY(!inSight());
        panelOpen(true);
        QCOMPARE(bar->value(), 0);
        panelOpen(false);
        table->setCurrentCell(1, 0);
        table->scrollToBottom();
        const int end = bar->value();
        QVERIFY(end > 0);
        panelOpen(true);
        QCOMPARE(bar->value(), end);
    }
    void fileSelectionLoadsAndCancels() {
        QTemporaryDir job;
        QVERIFY(job.isValid());
        const auto output = job.filePath("output.out"), xyz = job.filePath("a.xyz"),
                   cube = job.filePath("z.cube");
        writeFile(output, ("NWChem\n" + geometry()).toUtf8());
        writeFile(xyz, "1\nhydrogen\nH 0 0 0\n");
        writeFile(cube, cubeText().toUtf8());
        writeFile(job.filePath("broken.xyz"), "invalid XYZ\n");
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentWidget(panel);
        w.show();
        auto *chooser = widget<QComboBox>(w, "resultsFiles");
        auto *view = widget<MoleculeView>(w, "resultsView");
        const auto before = w.document().project();
        const int history = w.history()->count();
        QVERIFY(!w.findChild<QPushButton *>("resultsOpenSelected"));
        QSignalSpy done(panel, &ResultsPanel::loaded), failed(panel, &ResultsPanel::loadFailed);
        panel->openDirectory(job.path());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QCOMPARE(chooser->currentText(), "output.out"); // Prefer the log over the first list item.
        QCOMPARE(panel->result().source, output);
        QVERIFY(!panel->loading());
        chooser->setCurrentText("a.xyz");
        QVERIFY(panel->loading());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
        QCOMPARE(panel->result().source, xyz);
        QCOMPARE(panel->result().frames[0].molecule.atoms.size(), 1);
        const auto atomId = panel->result().frames[0].molecule.atoms[0].id;
        QVERIFY(view->rect().contains(view->atomScreenPosition(atomId).toPoint()));
        // Keyboard selection also opens the newly chosen file without an extra action.
        chooser->setFocus();
        QTest::keyClick(chooser, Qt::Key_End);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 3, 10000);
        QCOMPARE(chooser->currentText(), "z.cube");
        QCOMPARE(panel->result().source, cube);
        QVERIFY(!panel->result().surface.isEmpty());
        chooser->setCurrentText("a.xyz");
        QVERIFY(panel->loading());
        chooser->setCurrentText("output.out");
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 4, 10000);
        QCOMPARE(panel->result().source, output);
        QVERIFY(panel->result().surface.isEmpty());
        QVERIFY(!panel->loading()); // Refreshing the list must not start another reader.
        QCOMPARE(failed.count(), 0);
        chooser->setCurrentText("broken.xyz");
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QCOMPARE(panel->result().source, output);
        QCOMPARE(done.count(), 4);
        chooser->setCurrentText("a.xyz");
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 5, 10000);
        QCOMPARE(panel->result().source, xyz);
        chooser->setCurrentText("output.out");
        QVERIFY(panel->loading());
        widget<QLineEdit>(w, "resultsIsovalue")->setText("nan");
        chooser->setCurrentText("z.cube");
        QCOMPARE(failed.count(), 2);
        QVERIFY(!panel->loading()); // Even a rejected selection cancels the older request.
        QTest::qWait(100);
        QCOMPARE(done.count(), 5);
        QCOMPARE(panel->result().source, xyz);
        chooser->setCurrentText("output.out");
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 6, 10000);
        QCOMPARE(panel->result().source, output);
        const auto other = job.filePath("result.dat");
        writeFile(other, ("NWChem\n" + geometry()).toUtf8());
        panel->openFile(other);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 7, 10000);
        QCOMPARE(chooser->currentText(), "result.dat");
        QCOMPARE(panel->result().source, other);
        QVERIFY(!panel->loading());
        QCOMPARE(w.document().project(), before);
        QCOMPARE(w.history()->count(), history);
    }
    void pathsAndProperties() {
        // MolForma's scan titles mark the points; the last energy and structure of each point count.
        QString scan = "NWChem\n";
        for (int k = 1; k <= 2; ++k)
            scan += QString("                                 MolForma scan %1/2 bond 1 2 = %2\n")
                        .arg(k)
                        .arg(0.9 + k * .1) +
                    geometry() + QString("         Total SCF energy =    -76.0%1\n").arg(k) +
                    QString("         Total SCF energy =    -76.1%1\n").arg(k) +
                    // The stored title of an earlier point may be printed again later.
                    "MolForma scan 1/2 bond 1 2 = 1\n";
        auto r = readResult(save("scan.out", scan));
        const auto points = r.properties["scan"].toArray();
        QCOMPARE(points.size(), 2);
        QCOMPARE(points[1].toObject()["energy"].toDouble(), -76.12);
        QCOMPARE(points[1].toObject()["value"].toDouble(), 1.1);
        QCOMPARE(points[1].toObject()["coordinate"].toString(), QString("bond 1 2"));
        QCOMPARE(points[1].toObject()["frame"].toInt(), 1);
        QVERIFY(r.frames[1].label.startsWith("スキャン 2"));
        // IRC: the transition state before the path, then forward and backward points.
        const QString irc =
            "NWChem\n" + geometry() + "         Total SCF energy =    -92.79195\n" +
            "          Gonzalez & Schlegel IRC Optimization\n          Forward IRC optimization\n" +
            geometry() + "&    0     -92.79330941  0.14572  0.14572  0.00652  0.01232   -1.26D-03\n" +
            "      IRC Optimization converged\n          Backward IRC optimization\n" + geometry() +
            "&    0     -92.79334658  0.14062  0.14062  0.00693  0.01296   -1.29D-03\n" +
            "      IRC Optimization converged\n";
        r = readResult(save("irc.out", irc));
        const auto path = r.properties["irc"].toObject();
        QCOMPARE(path["ts_energy"].toDouble(), -92.79195);
        QCOMPARE(path["ts_frame"].toInt(), 0);
        QVERIFY(path["complete"].toBool());
        QCOMPARE(path["points"].toArray().size(), 2);
        QCOMPARE(path["points"][1].toObject()["direction"].toString(), QString("backward"));
        QCOMPARE(path["points"][1].toObject()["s"].toDouble(), 0.14062);
        QCOMPARE(path["points"][1].toObject()["frame"].toInt(), 2);
        // One direction ("backward" in MEPGS) is complete with its own convergence.
        const auto backward =
            readResult(save("irc-backward.out", "NWChem\n" + geometry() +
                                                    "         Total SCF energy =    -92.79195\n"
                                                    "          Backward IRC optimization\n" +
                                                    geometry() +
                                                    "&    0     -92.79334658  0.14062  0.14062  "
                                                    "0.00693  0.01296   -1.29D-03\n"
                                                    "      IRC Optimization converged\n"))
                .properties["irc"]
                .toObject();
        QVERIFY(backward["complete"].toBool());
        QCOMPARE(backward["directions"].toArray(), QJsonArray{"backward"});
        // A side that stops before converging leaves the path incomplete.
        QVERIFY(!readResult(
                     save("irc-stopped.out", QString(irc).section("      IRC Optimization converged", 0, 0)))
                     .properties["irc"]
                     .toObject()["complete"]
                     .toBool());
        // Properties, BSSE and solvation in the layout of NWChem 7.3.1.
        const QString properties =
            "NWChem\n" + geometry() +
            "          ----- Total      gross population on atoms ----\n\n"
            "              1  O    8.0     8.77813\n              2  H    1.0     0.61093\n"
            "              3  H    1.0     0.61093\n\n"
            "    Atom              Coordinates                           Charge\n\n"
            "                                                  ESP         RESP        RESP2 \n"
            "                                                                         constr \n \n"
            "    1 O     0.000000    0.000000    0.011730   -0.760660   -0.759168   -0.759168\n"
            "    2 H     0.000000    0.075720   -0.046920    0.380037    0.379287    0.379287\n"
            "    3 H     0.000000   -0.075720   -0.046920    0.380623    0.379881    0.379881\n"
            "                                            ------------------------------------\n"
            "                                                0.000000    0.000000    0.000000\n\n"
            "      Atom:    1  O \n           isotropic =     323.1942\n          anisotropy =      40.5748\n"
            "      Atom:    2  H \n           isotropic =      32.3344\n          anisotropy =      18.0490\n"
            " DFT Linear Response polarizability / au \n Frequency  =       0.0000000 / au\n"
            "              X              Y              Z\n "
            "-----------------------------------------------\n"
            " X       2.8404754     -0.0000000      0.0000000\n Y      -0.0000000      7.1938924     "
            "-0.0000000\n"
            " Z       0.0000000     -0.0000000      5.2699243\n "
            "-----------------------------------------------\n"
            " Isotropic   =       5.1014307\n Anisotropic =       3.7786317\n"
            "             BSSE error =       0.000430051349\n  Supermolecular energy =    -152.829244137478\n"
            "       Corrected energy =    -152.828814086129\n"
            "    1 M fixed-concentration free energy of solvation =        -0.013350123100 (   -8.38 "
            "kcal/mol)\n"
            "    total free energy in solvent including G(SMD-CDS) =       -76.422081467149\n";
        r = readResult(save("properties.out", properties));
        // The monomer geometries a counterpoise correction prints are not frames, and not incomplete.
        r = readResult(save("monomers.out", "NWChem\n" + geometry() +
                                                " Output coordinates in angstroms\n\n No. Tag\n ---- ----\n"
                                                " 1 O 8.0 0.0 0.0 3.4\n\n" +
                                                properties.mid(QString("NWChem\n").size())));
        QCOMPARE(r.frames.size(), 2);
        QVERIFY2(std::none_of(r.warnings.begin(), r.warnings.end(),
                              [](const QString &w) { return w.contains("座標表"); }),
                 qPrintable(r.warnings.join('\n')));
        // NWChem 7.3.1 sums the errors of only the first half of the monomers; the monomer energies
        // (own basis, then the full one) give the error over all of them.
        const auto counterpoise =
            readResult(save("bsse.out", "NWChem\n" + geometry() +
                                            "             number of monomers =    2\n"
                                            "         Total DFT energy =     -152.829244137478\n"
                                            "         Total DFT energy =      -76.400000000000\n"
                                            "         Total DFT energy =      -76.400300000000\n"
                                            "         Total DFT energy =      -76.410000000000\n"
                                            "         Total DFT energy =      -76.410200000000\n"
                                            "             BSSE error =       0.000300000000\n"
                                            "  Supermolecular energy =    -152.829244137478\n"
                                            "       Corrected energy =    -152.828944137478\n"
                                            " Total times  cpu:        1.0s     wall:        1.0s\n"));
        const auto bsse = counterpoise.properties["bsse"].toObject();
        QVERIFY(std::abs(bsse["error"].toDouble() - 0.0005) < 1e-12);
        QCOMPARE(bsse["nwchem_error"].toDouble(), 0.0003);
        QVERIFY(std::abs(bsse["corrected"].toDouble() + 152.828744137478) < 1e-9);
        QVERIFY(std::abs(counterpoise.properties["final_energy"].toObject()["hartree"].toDouble() +
                         152.828744137478) < 1e-9);
        QVERIFY(counterpoise.warnings.join('\n').contains("一部の単量体"));
        // The interaction energy against the monomers in their own bases, with and without the correction.
        QVERIFY(std::abs(bsse["interaction"].toDouble() - (-152.828744137478 + 152.81)) < 1e-9);
        QVERIFY(std::abs(bsse["interaction_uncorrected"].toDouble() - (-152.829244137478 + 152.81)) < 1e-9);
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openFile(files.filePath("bsse.out"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QVERIFY(widget<QLabel>(w, "resultsPropertySummary")->text().contains("相互作用エネルギー"));
        const auto mulliken = r.properties["mulliken"].toArray();
        QCOMPARE(mulliken.size(), 3);
        QVERIFY(std::abs(mulliken[0].toDouble() + 0.77813) < 1e-9);
        QCOMPARE(r.properties["esp"].toArray()[2].toDouble(), 0.380623);
        QCOMPARE(r.properties["resp"].toArray()[0].toDouble(), -0.759168);
        QCOMPARE(r.properties["shielding"].toArray().size(), 2);
        QCOMPARE(r.properties["shielding"][1].toObject()["anisotropy"].toDouble(), 18.049);
        QCOMPARE(r.properties["polarizability"].toObject()["isotropic"].toDouble(), 5.1014307);
        QCOMPARE(r.properties["polarizability"].toObject()["tensor"].toArray()[4].toDouble(), 7.1938924);
        QCOMPARE(r.properties["bsse"].toObject()["error"].toDouble(), 0.000430051349);
        QCOMPARE(r.properties["bsse"].toObject()["corrected"].toDouble(), -152.828814086129);
        QCOMPARE(r.properties["solvation"].toObject()["smd_dg"].toDouble(), -0.0133501231);
        QCOMPARE(loadResultData([&] {
                     const auto file = files.filePath("properties.bin");
                     saveResultData(file, r);
                     return file;
                 }())
                     .properties,
                 r.properties);
    }
    void viewerSpectraUseSharedCalculation() {
        MainWindow window;
        auto *panel = widget<ResultsPanel>(window, "resultsTab");
        const QString root = QString(FIXTURE_DIR) + "/nwchem/";
        panel->openFile(root + "conformers/nitrous_acid_cis.out");
        QTRY_VERIFY_WITH_TIMEOUT(!panel->loading(), 10000);
        QVERIFY2(!panel->result().excitations.isEmpty(), qPrintable(panel->message()));
        auto *axis = widget<QComboBox>(window, "resultsSpectrumAxis");
        auto *width = widget<QDoubleSpinBox>(window, "resultsBroadening");
        auto *chart = widget<ChartView>(window, "resultsSpectrum");
        axis->setCurrentIndex(axis->findData("ev"));
        width->setValue(.45);
        QCOMPARE(chart->seriesData().size(), 2);
        const auto &curve = chart->seriesData()[0].points;
        const auto uv = calculateSpectrum(
            panel->result(),
            {{"kind", "uv"}, {"width", .45}, {"minimum", curve.first().x()}, {"maximum", curve.last().x()}});
        QCOMPARE(chart->seriesData()[0].points, uv.curve);
        QCOMPARE(chart->seriesData()[1].points, uv.sticks);
        axis->setCurrentIndex(axis->findData("nm"));
        for (int i = 0; i < uv.curve.size(); ++i) {
            QCOMPARE(chart->seriesData()[0].points[i].x(), EvNanometer / uv.curve[i].x());
            QCOMPARE(chart->seriesData()[0].points[i].y(), uv.curve[i].y());
        }
        auto *irChart = widget<ChartView>(window, "resultsIrSpectrum");
        widget<QDoubleSpinBox>(window, "resultsIrWidth")->setValue(30);
        QCOMPARE(irChart->seriesData().size(), 2);
        const auto ir =
            calculateSpectrum(panel->result(), {{"kind", "ir"},
                                                {"width", 30.},
                                                {"minimum", 0.},
                                                {"maximum", irChart->seriesData()[0].points.last().x()}});
        QCOMPARE(irChart->seriesData()[0].points, ir.curve);
        QCOMPARE(irChart->seriesData()[1].points, ir.sticks);
        emit irChart->stickClicked(1, 0);
        QCOMPARE(panel->viewerState().mode, ir.peaks[0].toObject()["mode"].toInt());
    }
    void nmrSpectrum() {
        ResultData r;
        r.properties["shielding"] =
            QJsonArray{QJsonObject{{"atom", 2}, {"element", "H"}, {"isotropic", 29.0}},
                       QJsonObject{{"atom", 5}, {"element", "C"}, {"isotropic", 180.0}},
                       QJsonObject{{"atom", 7}, {"element", "H"}, {"isotropic", 30.0}}};
        QJsonObject p{{"kind", "nmr"}, {"nucleus", "1H"}, {"reference_ppm", 31.0}, {"width", .1}};
        const auto s = calculateSpectrum(r, p);
        QCOMPARE(s.sticks.size(), 2);
        QCOMPARE(s.sticks[0], QPointF(2, 1));
        QCOMPARE(s.peaks[1].toObject()["atom"].toInt(), 7);
        QVERIFY(s.reversed);
        QVERIFY(spectrumCsv(s).contains("peak,2,,2,1"));
        p["weights"] = QJsonObject{{"2", 3}};
        QCOMPARE(calculateSpectrum(r, p).sticks[0].y(), 3);
        p["nucleus"] = "13C";
        p["reference_ppm"] = 190.;
        QCOMPARE(calculateSpectrum(r, p).sticks[0], QPointF(10, 1));
        p.remove("reference_ppm");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, calculateSpectrum(r, p));
        p["reference_ppm"] = 190.;
        p["width"] = 0;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, calculateSpectrum(r, p));
        // Subset shielding tables retain source atom numbers, including repeated tasks.
        const auto path = save(
            "nmr-spectrum.out",
            "NWChem\nChemical Shielding Tensors (GIAO, in ppm)\nAtom: 2 H\nisotropic = 28.0\nanisotropy = "
            "1\nChemical Shielding Tensors (GIAO, in ppm)\nAtom: 3 H\nisotropic = 29.0\nanisotropy = 1\n");
        auto parsed = readResult(path);
        QCOMPARE(parsed.properties["shielding"].toArray().size(), 1);
        QCOMPARE(parsed.properties["shielding"][0].toObject()["atom"].toInt(), 3);
        TestUiServices services;
        SpectroscopyView view(services);
        view.setResult(parsed);
        p = {{"kind", "nmr"}, {"nucleus", "1H"}, {"reference_ppm", 31.0}, {"width", .05}};
        view.setParameters(p);
        QCOMPARE(view.findChild<QTableWidget *>("spectrumPeaks")->item(0, 0)->text(), "3");
        const auto settingsFile = files.filePath("nmr-settings.json");
        view.saveSettings(settingsFile);
        p["reference_ppm"] = 32.;
        view.setParameters(p);
        view.loadSettings(settingsFile);
        QCOMPARE(view.parameters()["reference_ppm"].toDouble(), 31.0);
        view.exportCsv(files.filePath("nmr-spectrum.csv"));
        QVERIFY(readFile(files.filePath("nmr-spectrum.csv")).contains("peak,3,,2,1"));
        QVERIFY(services.checkedPaths.contains(settingsFile));
        QVERIFY(services.checkedPaths.contains(files.filePath("nmr-spectrum.csv")));
        parsed.source = save("another-result.out", "NWChem\n");
        view.setResult(parsed);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.loadSettings(settingsFile));
    }
    void spinSpinCouplingsAndMultiplets() {
        // CH3F as NWChem prints its shieldings and indirect spin–spin couplings.
        QString log = "NWChem\n Output coordinates in angstroms (scale by 1.8897 to convert to a.u.)\n\n"
                      " No. Tag Charge X Y Z\n ---- ---- ------ ----- ----- -----\n"
                      " 1 C 6.0 0.0 0.0 0.0\n 2 F 9.0 0.0 0.0 1.383\n 3 H 1.0 1.0267 0.0 -0.363\n"
                      " 4 H 1.0 -0.5134 -0.8892 -0.363\n 5 H 1.0 -0.5134 0.8892 -0.363\n\n"
                      "Chemical Shielding Tensors (GIAO, in ppm)\n";
        for (const auto &[atom, element, sigma] :
             {std::tuple{1, "C", 120.0}, {2, "F", 459.0}, {3, "H", 27.80}, {4, "H", 27.81}, {5, "H", 27.82}})
            log += QString("Atom: %1 %2\nisotropic = %3\nanisotropy = 1\n").arg(atom).arg(element).arg(sigma);
        log += "      Indirect Spin-Spin Tensors (in Hz)\n";
        const QVector<std::tuple<int, QString, int, QString, double>> pairs{
            {1, "13-C", 2, "19-F", -184.14}, {1, "13-C", 3, "1-H", 152.35}, {1, "13-C", 4, "1-H", 152.35},
            {1, "13-C", 5, "1-H", 152.35},   {2, "19-F", 3, "1-H", 48.81},  {2, "19-F", 4, "1-H", 48.81},
            {2, "19-F", 5, "1-H", 48.81},    {3, "1-H", 4, "1-H", -11.06},  {3, "1-H", 5, "1-H", -11.06},
            {4, "1-H", 5, "1-H", -11.06}};
        for (const auto &[a, ia, b, ib, hz] : pairs)
            log += QString("      Atom    %1:  %2  and Atom    %3:  %4 \n        Isotropic =    -231.3481\n"
                           "      Isotropic Spin-Spin Coupling =    %5 Hz\n")
                       .arg(a)
                       .arg(ia)
                       .arg(b)
                       .arg(ib)
                       .arg(hz);
        const auto r = readResult(save("ch3f-spinspin.out", log));
        const auto couplings = r.properties["spinspin"].toArray();
        QCOMPARE(couplings.size(), 10);
        QCOMPARE(couplings[0].toObject()["isotope_b"].toString(), QString("19-F"));
        QCOMPARE(couplings[4].toObject()["hz"].toDouble(), 48.81);
        QCOMPARE(couplings[7].toObject()["a"].toInt(), 3);
        // ¹H: the three equivalent protons as one doublet (²J(H,F)); their own coupling does not show.
        QJsonObject p{{"kind", "nmr"}, {"nucleus", "1H"},  {"reference_ppm", 31.8},
                      {"width", .002}, {"coupling", true}, {"spectrometer_mhz", 400.}};
        auto s = calculateSpectrum(r, p);
        QCOMPARE(s.peaks.size(), 1);
        const auto peak = s.peaks[0].toObject();
        QCOMPARE(peak["multiplet"].toString(), QString("d"));
        QCOMPARE(peak["j_hz"].toString(), QString("48.8"));
        QCOMPARE(peak["atoms"].toArray().size(), 3);
        QVERIFY(std::abs(peak["x"].toDouble() - (31.8 - 27.81)) < 1e-9);
        QCOMPARE(s.sticks.size(), 2);
        QCOMPARE(s.stickPeaks, (QVector<int>{0, 0}));
        QVERIFY(std::abs(std::abs(s.sticks[1].x() - s.sticks[0].x()) - 48.81 / 400) < 1e-6);
        QCOMPARE(s.sticks[0].y() + s.sticks[1].y(), 3.0);
        // ¹³C: proton-decoupled, split by ¹⁹F only.
        p["nucleus"] = "13C";
        p["reference_ppm"] = 190.;
        s = calculateSpectrum(r, p);
        QCOMPARE(s.peaks[0].toObject()["multiplet"].toString(), QString("d"));
        QVERIFY(std::abs(std::abs(s.sticks[1].x() - s.sticks[0].x()) - 184.14 / (400 * .25145020)) < 1e-6);
        // Without couplings in the log the option is refused; off, the spectrum is as before.
        auto plain = r;
        plain.properties.remove("spinspin");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, calculateSpectrum(plain, p));
        p["coupling"] = false;
        QCOMPARE(calculateSpectrum(r, p).peaks.size(), 1);
        p["nucleus"] = "1H";
        p["reference_ppm"] = 31.8;
        QCOMPARE(calculateSpectrum(r, p).peaks.size(), 3);
        // The view lists the multiplet.
        MainWindow w;
        SpectroscopyView view(w, &w);
        view.setResult(r);
        p["coupling"] = true;
        view.setParameters(p);
        const auto *table = view.findChild<QTableWidget *>("spectrumPeaks");
        QCOMPARE(table->item(0, 0)->text(), QString("3, 4, 5"));
        QCOMPARE(table->item(0, 4)->text(), QString("d (48.8)"));
        // A two-dimensional scan names both coordinates of each point.
        QString scan = "NWChem\n";
        for (int k = 1; k <= 4; ++k)
            scan += QString("MolForma scan %1/4 bond 1 2 = %2 and angle 2 1 3 = %3\n")
                        .arg(k)
                        .arg(k <= 2 ? 1.0 : 1.1)
                        .arg(k % 2 ? 100 : 110) +
                    geometry() + QString("         Total SCF energy =    -76.0%1\n").arg(k);
        const auto plane = readResult(save("scan2.out", scan)).properties["scan"].toArray();
        QCOMPARE(plane.size(), 4);
        QCOMPARE(plane[3].toObject()["value"].toDouble(), 1.1);
        QCOMPARE(plane[3].toObject()["value2"].toDouble(), 110.);
        QCOMPARE(plane[3].toObject()["coordinate2"].toString(), QString("angle 2 1 3"));
        QVERIFY(!readResult(save("scan1.out", "NWChem\nMolForma scan 1/1 bond 1 2 = 1\n" + geometry() +
                                                  "         Total SCF energy =    -76.0\n"))
                     .properties["scan"][0]
                     .toObject()
                     .contains("value2"));
    }
    void energyProfiles() {
        const auto record = [](const QString &id, double energy, double enthalpy) {
            return QJsonObject{{"id", id},
                               {"name", id},
                               {"normal", true},
                               {"energy_hartree", energy},
                               {"composition", QJsonObject{{"H", 2}}},
                               {"charge", 0},
                               {"settings", QJsonObject{{"method", "hf"}, {"basis", "sto-3g"}}},
                               {"thermochemistry", QJsonArray{QJsonObject{{"temperature", 300.},
                                                                          {"zpe_kcal", 2.},
                                                                          {"enthalpy_kcal", enthalpy},
                                                                          {"entropy_cal", 5.}}}}};
        };
        QJsonObject p{
            {"format", "molforma-energy-profile"},
            {"version", 1},
            {"metric", "H"},
            {"unit", "kcal"},
            {"temperature", 300.},
            {"standard_state", "1 atm"},
            {"reference", 0},
            {"records", QJsonArray{record("a", -1, 3), record("b", -1.01, 4)}},
            {"stages",
             QJsonArray{
                 QJsonObject{{"name", "a"},
                             {"terms", QJsonArray{QJsonObject{{"record", "a"}, {"coefficient", 1}}}}},
                 QJsonObject{{"name", "b"},
                             {"terms", QJsonArray{QJsonObject{{"record", "b"}, {"coefficient", 1}}}}}}}};
        auto result = evaluateEnergyProfile(p);
        QCOMPARE(result["comparable"].toBool(), true);
        QVERIFY(std::abs(result["stages"][0].toObject()["hartree"].toDouble() - (-1 + 3 / 627.509474)) <
                1e-12); // H includes ZPE already.
        QVERIFY(std::abs(result["stages"][1].toObject()["relative"].toDouble() - (-.01 * 627.509474 + 1)) <
                1e-10);
        p["metric"] = "G";
        QVERIFY(std::abs(evaluateEnergyProfile(p)["stages"][0].toObject()["hartree"].toDouble() -
                         (-1 + 1.5 / 627.509474)) < 1e-12);
        p["temperature"] = 298.15;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, evaluateEnergyProfile(p));
        p["temperature"] = 300.;
        MainWindow w;
        EnergyProfileDialog dialog(w, &w);
        dialog.setSpecification(p);
        QCOMPARE(dialog.specification()["stages"], p["stages"]);
        QCOMPARE(dialog.findChild<QTableWidget *>("profileValues")->rowCount(), 2);
        auto stages = p["stages"].toArray();
        auto stage = stages[1].toObject();
        stage["terms"] = QJsonArray{QJsonObject{{"record", "b"}, {"coefficient", 2}}};
        stages[1] = stage;
        p["stages"] = stages;
        QVERIFY(!evaluateEnergyProfile(p)["comparable"].toBool());
    }
    void conformerSets() {
        const auto base = Document::fromXyz("3\nwater\nO 0 0 0\nH .8 0 .6\nH -.8 0 .6\n");
        auto changed = base;
        changed.atoms[1].position.x() += .3;
        MainWindow w;
        w.loadDocument(base);
        int queued = 0;
        ConformerSetDialog dialog(w, w, w, [&](const QVector<Document> &documents, bool frequencies) {
            queued = documents.size();
            if (frequencies)
                return QStringList{};
            return QStringList{"job-a", "job-b"};
        });
        dialog.setConformers(base, {{base, 0., "MMFF94"}, {changed, 1., "MMFF94"}});
        auto spec = dialog.specification();
        QCOMPARE(spec["entries"].toArray().size(), 2);
        dialog.setSpecification(spec);
        QCOMPARE(dialog.specification(), spec);
        dialog.queueSelected();
        QCOMPARE(queued, 2);
        dialog.findChild<QTableWidget *>("conformerSetTable")->selectRow(1);
        dialog.applySelected();
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.document().toJson(), base.toJson());
        QVERIFY((w.displayDocument().atoms[1].position - changed.atoms[1].position).norm() < 1e-9);
        w.confirmCandidate();
        w.history()->undo();
        QCOMPARE(w.document().toJson(), base.toJson());
        spec["entries"] = QJsonArray{QJsonObject{{"geometry", "1\nwrong\nHe 0 0 0\n"}}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dialog.setSpecification(spec));
        const auto record = [&](QString id, const Document &d, double e) {
            return QJsonObject{{"id", id},
                               {"name", id},
                               {"normal", true},
                               {"energy_hartree", e},
                               {"composition", QJsonObject{{"O", 1}, {"H", 2}}},
                               {"charge", 0},
                               {"multiplicity", 1},
                               {"settings", QJsonObject{{"method", "hf"}, {"basis", "sto-3g"}}},
                               {"geometry", d.xyz()}};
        };
        auto translated = base;
        for (auto &a : translated.atoms)
            a.position += Vec(2, 3, 4);
        QJsonArray records{record("a", base, -76.), record("b", translated, -75.999),
                           record("c", changed, -75.998)};
        const auto populations = conformerPopulations(records, "E", 300., .01);
        QCOMPARE(populations[0].toObject()["group"], populations[1].toObject()["group"]);
        QCOMPARE(populations[1].toObject()["population"].toDouble(), 0.);
        QVERIFY(std::abs(populations[0].toObject()["population"].toDouble() +
                         populations[2].toObject()["population"].toDouble() - 1) < 1e-12);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, conformerPopulations(records, "G", 300., .01));
    }
    // A picture of a dialog for a look at it, into the folder MOLFORMA_RESULTS_ARTIFACTS names.
    static void picture(QWidget *widget, const QString &name) {
        const auto artifacts = qEnvironmentVariable("MOLFORMA_RESULTS_ARTIFACTS");
        if (artifacts.isEmpty())
            return;
        QVERIFY(QDir().mkpath(artifacts));
        widget->show();
        QTest::qWait(100);
        QVERIFY(widget->grab().save(QDir(artifacts).filePath(name)));
    }
    void energyProfileRates() {
        // A stage 20 kcal/mol above the reference in G at 298.15 K: exp(−ΔG/RT) = 2.187249e-15 and the
        // Eyring rate constant (k_B·T/h)·exp(−ΔG/RT) = 6.212438e12 × 2.187249e-15 = 1.358815e-2 s⁻¹.
        const auto record = [](const QString &id, double energy) {
            return QJsonObject{{"id", id},
                               {"name", id},
                               {"normal", true},
                               {"energy_hartree", energy},
                               {"composition", QJsonObject{{"H", 2}}},
                               {"charge", 0},
                               {"settings", QJsonObject{{"method", "hf"}, {"basis", "sto-3g"}}},
                               {"thermochemistry", QJsonArray{QJsonObject{{"temperature", 298.15},
                                                                          {"zpe_kcal", 2.},
                                                                          {"enthalpy_kcal", 3.},
                                                                          {"entropy_cal", 5.}}}}};
        };
        auto p = profileOf({record("reactant", -1), record("barrier", -1 + 20 / HartreeToKcal)}, "G", 298.15);
        auto stages = evaluateEnergyProfile(p)["stages"].toArray();
        QVERIFY(std::abs(stages[1].toObject()["relative"].toDouble() - 20) < 1e-9);
        QVERIFY(std::abs(stages[1].toObject()["boltzmann_factor"].toDouble() / 2.1872492033246132e-15 - 1) <
                1e-8);
        QVERIFY(std::abs(stages[1].toObject()["rate_constant"].toDouble() / 1.358815004787466e-2 - 1) < 1e-8);
        QCOMPARE(stages[0].toObject()["boltzmann_factor"].toDouble(), 1.);
        QVERIFY(!stages[0].toObject().contains("rate_constant")); // No barrier.
        // Downhill from the other stage: a factor above 1 and no rate constant.
        p["reference"] = 1;
        stages = evaluateEnergyProfile(p)["stages"].toArray();
        QVERIFY(std::abs(stages[0].toObject()["boltzmann_factor"].toDouble() * 2.1872492033246132e-15 - 1) <
                1e-8);
        QVERIFY(!stages[0].toObject().contains("rate_constant"));
        // Only free energies give rates.
        p["reference"] = 0;
        for (const auto *metric : {"E", "ZPE", "H"}) {
            p["metric"] = metric;
            const auto other = evaluateEnergyProfile(p)["stages"][1].toObject();
            QVERIFY(!other.contains("rate_constant") && !other.contains("boltzmann_factor"));
        }
        // The dialog shows both for G, keeps the choices of the corrections and labels the standard state.
        p["metric"] = "G";
        MainWindow w;
        EnergyProfileDialog dialog(w, &w);
        dialog.setSpecification(p);
        auto *values = dialog.findChild<QTableWidget *>("profileValues");
        QVERIFY(!values->isColumnHidden(3) && !values->isColumnHidden(4));
        QCOMPARE(values->item(1, 4)->text(), "0.01359");
        QCOMPARE(values->item(0, 4)->text(), "—");
        QCOMPARE(dialog.specification()["frequency_scale"].toDouble(), 1.);
        QCOMPARE(dialog.specification()["entropy_model"].toString(), "rrho");
        QCOMPARE(dialog.specification()["entropy_cutoff"].toDouble(), 100.);
        QCOMPARE(dialog.specification()["standard_state_correction"].toString(), "none");
        QVERIFY(dialog.findChild<QLabel *>("profileMessage")->text().contains("NWChemの出力値"));
        dialog.findChild<QComboBox *>("profileStandardCorrection")->setCurrentIndex(1);
        QCOMPARE(dialog.specification()["standard_state_correction"].toString(), "1M");
        QCOMPARE(dialog.specification()["standard_state"].toString(), "1 mol/L");
        QVERIFY(dialog.findChild<QLabel *>("profileMessage")->text().contains("1.894"));
        picture(&dialog, "energy-profile.png");
        dialog.findChild<QComboBox *>("profileMetric")->setCurrentIndex(2); // H
        QVERIFY(values->isColumnHidden(3) && values->isColumnHidden(4));
        // Records without wavenumbers: the reason replaces the table.
        dialog.findChild<QDoubleSpinBox *>("profileFrequencyScale")->setValue(.97);
        QCOMPARE(values->rowCount(), 0);
        QVERIFY(dialog.findChild<QLabel *>("profileMessage")->text().contains("振動数の記録がない"));
        auto saved = p;
        saved["frequency_scale"] = 1.;
        saved["entropy_model"] = "truhlar";
        saved["entropy_cutoff"] = 250.;
        saved["standard_state_correction"] = "1M";
        saved["standard_state"] = "1 M in water";
        saved["metric"] = "H";
        dialog.setSpecification(saved);
        for (const auto *key : {"frequency_scale", "entropy_model", "entropy_cutoff",
                                "standard_state_correction", "standard_state"})
            QCOMPARE(dialog.specification()[key], saved[key]);
    }
    void conformerAveragedSpectra() {
        const QString root = QString(FIXTURE_DIR) + "/nwchem/";
        const auto cis = readResult(root + "conformers/nitrous_acid_cis.out"),
                   trans = readResult(root + "conformers/nitrous_acid_trans.out");
        // What the logs hold: six vibrations, four excited states (with rotatory strengths, all zero
        // in a planar molecule) and the shieldings of the four atoms.
        const auto cisSpectra = spectrumContributions(cis), transSpectra = spectrumContributions(trans);
        QCOMPARE(cisSpectra.keys(), (QStringList{"ecd", "ir", "shielding", "uv"}));
        QCOMPARE(cisSpectra["ir"].toArray().size(), 6);
        QCOMPARE(cisSpectra["uv"].toArray().size(), 4);
        QCOMPARE(cisSpectra["ecd"].toArray().size(), 4);
        QCOMPARE(cisSpectra["shielding"].toArray().size(), 4);
        QVERIFY(spectrumContributions(readResult(root + "water_tddft.out")).keys() == QStringList{"uv"});
        // The kinds of the viewer's own charts are those of calculateSpectrum too.
        const auto ir = calculateSpectrum(cis, {{"kind", "ir"}, {"width", 20.}});
        QCOMPARE(ir.sticks.size(), 6);
        QCOMPARE(ir.sticks[0], QPointF(567.87, 6.5));
        QCOMPARE(ir.peaks[0].toObject()["mode"].toInt(), 6);
        QVERIFY(ir.reversed);
        const auto uv = calculateSpectrum(cis, {{"kind", "uv"}, {"width", .3}});
        QCOMPARE(uv.sticks[1], QPointF(5.7917, .0262739527));
        QCOMPARE(uv.peaks[1].toObject()["root"].toInt(), 2);
        // Weights of 80 % and 20 %; a duplicate (weight 0) adds nothing.
        QJsonArray members{QJsonObject{{"name", "cis"}, {"weight", .8}, {"spectra", cisSpectra}},
                           QJsonObject{{"name", "trans"}, {"weight", .2}, {"spectra", transSpectra}},
                           QJsonObject{{"name", "copy"}, {"weight", 0.}, {"spectra", cisSpectra}}};
        auto s = averagedSpectrum(members, {{"kind", "ir"}, {"width", 20.}});
        QCOMPARE(s.sticks.size(), 12);
        QVERIFY(std::abs(s.sticks[0].y() - .8 * 6.5) < 1e-12); // cis, 567.87 cm⁻¹: 6.500 km/mol
        QCOMPARE(s.sticks[6].x(), 558.04);                     // trans: 57.571 km/mol
        QVERIFY(std::abs(s.sticks[6].y() - .2 * 57.571) < 1e-12);
        QCOMPARE(s.peaks[0].toObject()["conformer"].toString(), "cis");
        QCOMPARE(s.peaks[6].toObject()["conformer"].toString(), "trans");
        QCOMPARE(s.peaks[6].toObject()["mode"].toInt(), 6);
        QCOMPARE(s.parameters["conformers"].toArray().size(), 2);
        QVERIFY(s.parameters["omitted"].toArray().isEmpty());
        QCOMPARE(s.xUnit, "cm⁻¹");
        QVERIFY(s.reversed);
        // Broadened once: at every point the sum of the weighted Lorentzians (FWHM 20 cm⁻¹) of the
        // vibrations of both logs.
        const auto vibrationsOf = [](const ResultData &r) {
            QVector<QPointF> sticks;
            for (int i = 6; i < r.modes.size(); ++i)
                sticks << QPointF(r.modes[i].frequency, r.modes[i].intensity);
            return sticks;
        };
        const auto excitationsOf = [](const ResultData &r) {
            QVector<QPointF> sticks;
            for (const auto &x : r.excitations)
                sticks << QPointF(x.ev, x.oscillator);
            return sticks;
        };
        const auto same = [&](const SpectrumData &spectrum, const QVector<QPointF> &first,
                              const QVector<QPointF> &second, const std::function<double(double)> &shape) {
            double top = 0;
            for (const auto &point : spectrum.curve)
                top = std::max(top, point.y());
            for (const auto &point : spectrum.curve) {
                double y = 0;
                for (const auto &stick : first)
                    y += .8 * stick.y() *
                         shape((point.x() - stick.x()) / spectrum.parameters["width"].toDouble());
                for (const auto &stick : second)
                    y += .2 * stick.y() *
                         shape((point.x() - stick.x()) / spectrum.parameters["width"].toDouble());
                if (std::abs(point.y() - y) > 1e-12 * top)
                    return false;
            }
            return top > 0 && spectrum.curve.size() > 4000;
        };
        QVERIFY(
            same(s, vibrationsOf(cis), vibrationsOf(trans), [](double d) { return 1 / (1 + 4 * d * d); }));
        // UV–Vis over eV: Gaussians of FWHM 0.3 eV, as the absorption chart of the viewer.
        s = averagedSpectrum(members, {{"kind", "uv"}, {"width", .3}});
        QCOMPARE(s.sticks.size(), 8);
        QVERIFY(std::abs(s.sticks[1].y() - .8 * .0262739527) < 1e-15);
        QCOMPARE(s.xUnit, "eV");
        QVERIFY(same(s, excitationsOf(cis), excitationsOf(trans),
                     [](double d) { return std::exp(-4 * std::log(2.) * d * d); }));
        // One conformer alone is its own spectrum.
        s = averagedSpectrum({members[1]}, {{"kind", "ir"}, {"width", 20.}});
        const auto alone = calculateSpectrum(trans, {{"kind", "ir"}, {"width", 20.}});
        QCOMPARE(s.sticks, alone.sticks);
        QCOMPARE(s.curve, alone.curve);
        // NMR in fast exchange: the weighted mean shielding of each atom. H (atom 4) has 17.0295 ppm
        // in cis and 13.5408 ppm in trans: 16.33176, and δ = 31 − 16.33176.
        QJsonObject nmr{{"kind", "nmr"}, {"nucleus", "1H"}, {"reference_ppm", 31.}, {"width", .05}};
        s = averagedSpectrum(members, nmr);
        QCOMPARE(s.sticks.size(), 1);
        QCOMPARE(s.peaks[0].toObject()["atom"].toInt(), 4);
        QVERIFY(std::abs(s.peaks[0].toObject()["shielding_ppm"].toDouble() - 16.33176) < 1e-9);
        QVERIFY(std::abs(s.sticks[0].x() - (31 - 16.33176)) < 1e-9);
        // A conformer without the data is left out and named, and the others weigh 1 together.
        members.append(QJsonObject{{"name", "bare"}, {"weight", .25}, {"spectra", QJsonObject()}});
        s = averagedSpectrum(members, nmr);
        QCOMPARE(s.parameters["omitted"].toArray(), QJsonArray{"bare"});
        QVERIFY(std::abs(s.sticks[0].x() - (31 - 16.33176)) < 1e-9);
        QVERIFY(std::abs(s.parameters["conformers"][0].toObject()["weight"].toDouble() - .8) < 1e-12);
        s = averagedSpectrum(members, {{"kind", "ir"}, {"width", 20.}});
        QVERIFY(std::abs(s.sticks[0].y() - .8 * 6.5) < 1e-12);
        const auto csv = spectrumCsv(s);
        QVERIFY(csv.startsWith("type,atom,mode,x,weight,conformer\n"));
        QVERIFY(csv.contains("peak,,6,558.04,11.514")); // 0.2 × 57.571 km/mol
        QVERIFY(csv.contains(",\"trans\"\n"));
        QVERIFY(csv.contains("\ncurve,,,"));
        // No conformer has Raman activities, and shieldings of other atoms cannot be averaged.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 averagedSpectrum(members, {{"kind", "raman"}, {"width", 20.}}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 averagedSpectrum(members, {{"kind", "epr"}, {"width", 1.}}));
        auto other = members;
        other[1] = QJsonObject{
            {"name", "other"},
            {"weight", .2},
            {"spectra", QJsonObject{{"shielding", QJsonArray{QJsonObject{
                                                      {"atom", 9}, {"element", "H"}, {"isotropic", 30.}}}}}}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, averagedSpectrum(other, nmr));
        // ECD keeps its signs: H2O2 and its mirror image in equal amounts cancel, and 3:1 leave half.
        const auto ecd = readResult(root + "peroxide_ecd.out");
        const auto handed = spectrumContributions(ecd);
        auto mirrored = handed["ecd"].toArray();
        for (int i = 0; i < mirrored.size(); ++i) {
            auto row = mirrored[i].toObject();
            row["weight"] = -row["weight"].toDouble();
            mirrored[i] = row;
        }
        const auto pair = [&](double first, double second) {
            return QJsonArray{
                QJsonObject{{"name", "P"}, {"weight", first}, {"spectra", handed}},
                QJsonObject{{"name", "M"}, {"weight", second}, {"spectra", QJsonObject{{"ecd", mirrored}}}}};
        };
        s = averagedSpectrum(pair(.5, .5), {{"kind", "ecd"}, {"width", .2}});
        QCOMPARE(s.sticks.size(), 6);
        QVERIFY(std::all_of(s.curve.begin(), s.curve.end(), [](auto p) { return std::abs(p.y()) < 1e-12; }));
        s = averagedSpectrum(pair(3, 1), {{"kind", "ecd"}, {"width", .2}});
        const auto single = calculateSpectrum(ecd, {{"kind", "ecd"}, {"width", .2}});
        QCOMPARE(s.curve.size(), single.curve.size());
        for (int i = 0; i < s.curve.size(); ++i)
            QVERIFY(std::abs(s.curve[i].y() - single.curve[i].y() / 2) < 1e-10);
        // The conformer set weighs by its populations: G at 298.15 K gives cis 79.9 % (ΔG 0.81809).
        const auto entry = [&](const QString &name, const QString &log, const QJsonObject &spectra) {
            const auto record = logRecord(log);
            return QJsonObject{{"name", name},
                               {"selected", true},
                               {"geometry", record["geometry"]},
                               {"result", record},
                               {"spectra", spectra}};
        };
        MainWindow w;
        ConformerSetDialog dialog(w, w, w, [](const QVector<Document> &, bool) { return QStringList{}; });
        auto *button = dialog.findChild<QPushButton *>("conformerSpectrum");
        QVERIFY(!button->isEnabled());
        QJsonObject spec{
            {"format", "molforma-conformer-set"},
            {"version", 1},
            {"original", cis.frames.last().molecule.project()},
            {"entries", QJsonArray{entry("cis", "conformers/nitrous_acid_cis", cisSpectra),
                                   entry("trans", "conformers/nitrous_acid_trans", QJsonObject())}},
            {"metric", "G"},
            {"temperature", 298.15},
            {"rmsd", .1}};
        dialog.setSpecification(spec);
        QVERIFY(button->isEnabled());
        const double population = 1 / (1 + std::exp(-.81809 / (1.9872042586408316e-3 * 298.15)));
        auto shown = dialog.spectrumMembers();
        QCOMPARE(shown.size(), 2);
        QVERIFY(std::abs(shown[0].toObject()["weight"].toDouble() - population) < 1e-5);
        QCOMPARE(dialog.findChild<QTableWidget *>("conformerSetTable")->item(0, 8)->text(),
                 QString::number(100 * shown[0].toObject()["weight"].toDouble(), 'f', 3));
        // The corrections of G are kept with the set and change the weights.
        dialog.findChild<QDoubleSpinBox *>("conformerFrequencyScale")->setValue(.95);
        dialog.findChild<QComboBox *>("conformerEntropyModel")->setCurrentIndex(1);
        dialog.findChild<QDoubleSpinBox *>("conformerEntropyCutoff")->setValue(500);
        QCOMPARE(dialog.specification()["frequency_scale"].toDouble(), .95);
        QCOMPARE(dialog.specification()["entropy_model"].toString(), "grimme");
        QCOMPARE(dialog.specification()["entropy_cutoff"].toDouble(), 500.);
        QVERIFY(std::abs(dialog.spectrumMembers()[0].toObject()["weight"].toDouble() - population) > 1e-4);
        dialog.setSpecification(spec);
        QCOMPARE(dialog.specification()["frequency_scale"].toDouble(), 1.);
        QCOMPARE(dialog.specification()["entropy_model"].toString(), "rrho");
        // The trans log was read before the spectra were kept: IR of cis alone, and trans is named.
        auto *spectrum = dialog.showSpectrum();
        QCOMPARE(spectrum->parameters()["kind"].toString(), "ir");
        auto *message = spectrum->findChild<QLabel *>("conformerSpectrumMessage");
        QVERIFY(message->text().contains("cis 100.0%"));
        QVERIFY(message->text().contains("除いた配座: trans"));
        // With both, the dialog follows the set.
        auto entries = spec["entries"].toArray();
        entries[1] = entry("trans", "conformers/nitrous_acid_trans", transSpectra);
        spec["entries"] = entries;
        dialog.setSpecification(spec);
        QVERIFY(message->text().contains("cis 79.9%、trans 20.1%"));
        QVERIFY(!message->text().contains("除いた配座"));
        picture(&dialog, "conformer-set.png");
        picture(spectrum, "conformer-spectrum-ir.png");
        spectrum->exportCsv(files.filePath("conformers-ir.csv"));
        QVERIFY(readFile(files.filePath("conformers-ir.csv")).contains(",\"trans\"\n"));
        spectrum->setParameters({{"kind", "uv"}, {"width", .3}});
        picture(spectrum, "conformer-spectrum-uv.png");
        nmr["reference_ppm"] = 31.5;
        spectrum->setParameters(nmr);
        QCOMPARE(spectrum->parameters(), nmr);
        QVERIFY(message->text().contains("速い交換"));
        picture(spectrum, "conformer-spectrum-nmr.png");
        spectrum->exportCsv(files.filePath("conformers-nmr.csv"));
        QVERIFY(
            readFile(files.filePath("conformers-nmr.csv")).startsWith("type,atom,mode,x,weight\npeak,4,,"));
        // Without the populations of every chosen conformer there is nothing to weigh by.
        entries[1] = QJsonObject{
            {"name", "trans"}, {"selected", true}, {"geometry", entries[1].toObject()["geometry"]}};
        spec["entries"] = entries;
        dialog.setSpecification(spec);
        QVERIFY(!button->isEnabled());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dialog.showSpectrum());
    }
    void opticalActivity() {
        const auto ecd = readResult(QString(FIXTURE_DIR) + "/nwchem/peroxide_ecd.out");
        const auto rows = ecd.properties["ecd"].toArray();
        QCOMPARE(rows.size(), 3);
        QCOMPARE(ecd.excitations.size(), 6);
        QCOMPARE(rows[0].toObject()["strength"].toDouble(), -8.1592998);
        QCOMPARE(rows[0].toObject()["excitation"].toInt(), 0);
        auto spectrum = calculateSpectrum(ecd, {{"kind", "ecd"}, {"width", .2}});
        QVERIFY(std::any_of(spectrum.curve.begin(), spectrum.curve.end(), [](auto p) { return p.y() < 0; }));
        const auto ord = readResult(QString(FIXTURE_DIR) + "/nwchem/peroxide_ord.out");
        QCOMPARE(ord.properties["ord"].toArray().size(), 2);
        spectrum = calculateSpectrum(ord, {{"kind", "ord"}, {"width", 1.}});
        QCOMPARE(spectrum.curve.size(), 2);
        QVERIFY(std::abs(spectrum.curve[0].x() - 532.) < .001);
        QCOMPARE(spectrum.curve[0].y(), .03106);
        auto repeated = ord;
        auto values = ord.properties["ord"].toArray();
        values.append(values[0]);
        repeated.properties["ord"] = values;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 calculateSpectrum(repeated, {{"kind", "ord"}, {"width", 1.}}));
        MainWindow w;
        SpectroscopyView view(w, &w);
        view.setResult(ecd);
        QCOMPARE(view.parameters()["kind"].toString(), "ecd");
        view.setResult(ord);
        QCOMPARE(view.parameters()["kind"].toString(), "ord");
        ResultData nmr;
        nmr.properties["shielding"] =
            QJsonArray{QJsonObject{{"atom", 1}, {"element", "H"}, {"isotropic", 28.}}};
        view.setResult(nmr);
        QCOMPARE(view.parameters()["kind"].toString(), "nmr");
    }
    void fieldsAndTensors() {
        const auto r = readResult(save("field.cube", cubeText()), .1);
        QCOMPARE(r.values.size(), 8);
        const Vec point = r.gridOrigin + r.gridAxes * Vec(.25, .5, .75);
        const auto v = sampleField(r, point);
        QVERIFY(v.has_value());
        QVERIFY(std::abs(*v + .25) < 1e-12);
        QVERIFY(!sampleField(r, r.gridOrigin - r.gridAxes * Vec::Ones()));
        validateFieldOverlay(r, r);
        auto wrong = r;
        wrong.frames[0].molecule.atoms[0].position.x() += .01;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateFieldOverlay(r, wrong));
        saveResultData(files.filePath("field.transfer"), r);
        const auto restored = loadResultData(files.filePath("field.transfer"));
        QCOMPARE(restored.values, r.values);
        QVERIFY((restored.gridAxes - r.gridAxes).norm() < 1e-12);
        const auto principal = polarizabilityAxes({2., 1., 0., 1., 2., 0., 0., 0., 4.});
        QVERIFY((principal.values - Vec(1, 3, 4)).norm() < 1e-12);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 polarizabilityAxes({1., 2., 0., 0., 1., 0., 0., 0., 1.}));
        MainWindow w;
        PropertyGeometryView view(w, &w);
        view.setResult(r);
        QVERIFY(view.findChild<MoleculeView *>("propertyGeometryMolecule"));
    }
    void solutionEpr() {
        const auto r = readResult(QString(FIXTURE_DIR) + "/nwchem/methyl_epr.out");
        QCOMPARE(r.properties["spin_multiplicity"].toInt(), 2);
        QCOMPARE(r.properties["epr_gshift_ppt"].toArray().size(), 9);
        QCOMPARE(r.properties["epr_hyperfine"].toArray().size(), 4);
        QVERIFY(!r.properties.contains("shielding"));
        const auto nuclei = eprNuclei(r);
        QCOMPARE(nuclei[0].toObject()["isotope"].toString(), "13C");
        QVERIFY(std::abs(nuclei[1].toObject()["isotropic_mhz"].toDouble() + 77.3) < .1);
        QJsonArray selected;
        for (int atom = 2; atom <= 4; ++atom)
            selected.append(QJsonObject{{"atom", atom}, {"isotope", "1H"}});
        QJsonObject p{{"kind", "epr"}, {"width", .1}, {"microwave_ghz", 9.5}, {"nuclei", selected}};
        const auto s = calculateSpectrum(r, p);
        QVERIFY(std::abs(s.parameters["g_isotropic"].toDouble() - 2.00274900436) < 1e-9);
        double weight = 0;
        for (const auto &peak : s.sticks) {
            weight += peak.y();
            QVERIFY(peak.x() > 330 && peak.x() < 350);
        }
        QVERIFY(std::abs(weight - 1) < 1e-12);
        p["derivative"] = true;
        const auto derivative = calculateSpectrum(r, p);
        QVERIFY(
            std::any_of(derivative.curve.begin(), derivative.curve.end(), [](auto p) { return p.y() < 0; }));
        QVERIFY(
            std::any_of(derivative.curve.begin(), derivative.curve.end(), [](auto p) { return p.y() > 0; }));
        p["nuclei"] = QJsonArray{QJsonObject{{"atom", 2}, {"isotope", "13C"}}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, calculateSpectrum(r, p));
        MainWindow w;
        SpectroscopyView view(w, &w);
        view.setResult(r);
        QCOMPARE(view.findChild<QTableWidget *>("spectrumEprNuclei")->rowCount(), 4);
        QCOMPARE(view.parameters()["nuclei"].toArray().size(), 3);
    }
    void applyResultStructure() {
        // Core rule: same atoms in the same order; fixed atoms must not have moved.
        auto editor = Document::fromXyz("3\nwater\nO 0 0 0\nH .9 0 .6\nH -.9 0 .6\n");
        const auto optimized = Document::fromXyz("3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
        auto copy = editor;
        const auto [rmsd, largest] = applyResultGeometry(copy, optimized);
        QVERIFY((copy.atoms[1].position - Vec(.758, 0, .504)).norm() < 1e-12);
        QCOMPARE(copy.ids(), editor.ids());
        QCOMPARE(copy.bonds.size(), editor.bonds.size());
        QVERIFY(std::abs(largest - (Vec(.9, 0, .6) - Vec(.758, 0, .504)).norm()) < 1e-12);
        QVERIFY(rmsd > 0 && rmsd < largest);
        auto pinned = editor;
        pinned.atoms[1].fixed = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, applyResultGeometry(pinned, optimized));
        pinned.atoms[1].fixed = false;
        pinned.atoms[0].fixed = true; // O did not move in the result.
        applyResultGeometry(pinned, optimized);
        auto other = Document::fromXyz("3\nwrong order\nH .758 0 .504\nO 0 0 0\nH -.758 0 .504\n");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, applyResultGeometry(copy, other));
        // GUI: the displayed frame becomes a candidate of the editor document.
        MainWindow w;
        w.loadDocument(editor);
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentWidget(panel);
        w.show();
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openFile(save("optimized.xyz", optimized.xyz()));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        widget<QPushButton>(w, "resultsApplyStructure")->click();
        QVERIFY(w.hasCandidate());
        QCOMPARE(widget<QTabWidget>(w, "workspaceTabs")->currentIndex(), 0);
        QVERIFY((w.displayDocument().atoms[2].position - Vec(-.758, 0, .504)).norm() < 1e-9);
        QVERIFY((w.document().atoms[2].position - Vec(-.9, 0, .6)).norm() < 1e-12);
        w.confirmCandidate();
        QVERIFY((w.document().atoms[2].position - Vec(-.758, 0, .504)).norm() < 1e-9);
        w.history()->undo();
        QVERIFY((w.document().atoms[2].position - Vec(-.9, 0, .6)).norm() < 1e-12);
    }
    void prepareRecalculation() {
        auto original = Document::fromXyz("3\nwater\nO 0 0 0\nH .9 0 .6\nH -.9 0 .6\n");
        setNwchemSetting(original, "title", "recalculation snapshot");
        setExecutionSetting(original, "job_root", files.filePath("recalculate"));
        const auto bundle = saveExecutionBundle(original, files.path());
        writeFile(bundle.directory + "/final.xyz", "3\nfinal\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
        MainWindow w;
        w.loadDocument(original);
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openFile(bundle.directory + "/final.xyz");
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        panel->prepareRecalculation();
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.document().atoms[1].id, original.atoms[1].id);
        QCOMPARE(nwchemSettings(w.document())["title"].toString(), "recalculation snapshot");
        QVERIFY((w.displayDocument().atoms[1].position - Vec(.758, 0, .504)).norm() < 1e-12);
        QVERIFY(!w.executionPanel()->running());
        w.confirmCandidate();
        w.history()->undo();
        QVERIFY((w.document().atoms[1].position - original.atoms[1].position).norm() < 1e-12);
    }
    void nebPathViewAndSaddle() {
        // An NEB job in the viewer: the path from the start, its highest point, and a transition-state
        // search prepared from there; the end of another NEB taken from the structure shown.
        auto original = Document::fromXyz("3\nHCN\nC 0 0 0\nN 0 0 1.156\nH -0.9 0 -0.7\n");
        setExecutionSetting(original, "job_root", files.filePath("neb"));
        const auto bundle = saveExecutionBundle(original, files.path());
        QVERIFY(QFile::copy(FIXTURE_DIR "/nwchem/hcn_neb.out", bundle.directory + "/output.out"));
        MainWindow w;
        w.loadDocument(original);
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        // While the viewer shows no structure, the end from it is refused: as the failure of the
        // request, which the button shows in a message box and in the status bar.
        auto *calculation = w.findChild<CalculationPanel *>();
        QVERIFY(calculation);
        const QString missing = "「結果ビューア」で終点にする構造を開いてください。";
        try {
            calculation->setNebEndFromResult();
            QFAIL("The end of an NEB was taken from an empty result viewer");
        } catch (const std::exception &e) {
            QCOMPARE(QString::fromUtf8(e.what()), missing);
        }
        QString boxed;
        QTimer::singleShot(0, &w, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                boxed = box->text();
                box->accept();
            }
        });
        widget<QPushButton>(w, "calculationNebEndResult")->click();
        QCOMPARE(boxed, missing);
        QCOMPARE(w.statusBar()->currentMessage(), missing);
        QVERIFY(!nwchemNebEnd(w.document()).document);
        QCOMPARE(w.history()->count(), 0);
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openFile(bundle.directory + "/output.out");
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        auto *path = widget<QTableWidget>(w, "resultsPath");
        QCOMPARE(path->rowCount(), 5);
        QCOMPARE(path->item(0, 1)->text(), QString("始点"));
        QCOMPARE(path->item(4, 1)->text(), QString("終点"));
        QCOMPARE(path->item(0, 4)->text(), QString("0.000"));
        QVERIFY(widget<QLabel>(w, "resultsPathNote")->text().contains("未収束"));
        // The end of an NEB from the shown structure: the last image, fitted onto the molecule.
        path->setCurrentCell(4, 0);
        // The panel asks for the structure, and whoever composes the panels answers.
        QSignalSpy asked(calculation, &CalculationPanel::resultStructureRequested);
        calculation->setNebEndFromResult();
        QCOMPARE(asked.count(), 1);
        QCOMPARE(nwchemSettings(w.document())["task"].toString(), QString("neb"));
        QVERIFY(nwchemNebEnd(w.document()).document);
        QCOMPARE(w.history()->undoText(), QString("NEBの終点を結果の構造から設定"));
        // The end is the structure shown (the last image), fitted onto the molecule.
        QVERIFY(superpose(*nwchemNebEnd(w.document()).document, panel->displayedGeometry()).rmsd < 1e-6);
        QVERIFY(!w.findChild<QLabel *>("calculationNebEnd")->text().contains("未設定"));
        w.history()->undo();
        widget<QPushButton>(w, "resultsPathHighest")->click();
        QCOMPARE(path->currentRow(), 2);
        const auto shown = panel->displayedGeometry();
        QVERIFY((shown.atoms[2].position - Vec(-0.050003, 0, 0.575004)).norm() < 1e-9);
        auto *saddle = widget<QPushButton>(w, "resultsPathSaddle");
        QVERIFY(saddle->isEnabled());
        saddle->click();
        QVERIFY(w.hasCandidate());
        QCOMPARE(nwchemSettings(w.document())["task"].toString(), QString("saddle_frequencies"));
        QCOMPARE(nwchemSettings(w.document())["start"].toString(), QString("molforma_ts"));
        QVERIFY((w.displayDocument().atoms[2].position - shown.atoms[2].position).norm() < 1e-12);
    }
    void spectrumSelectionUsesDisplayedGeometry_data() {
        QTest::addColumn<bool>("playing");
        QTest::newRow("mode-reference") << false;
        QTest::newRow("mode-playing") << true;
    }
    void spectrumSelectionUsesDisplayedGeometry() {
        QFETCH(bool, playing);
        // The vibrational reference and the last structure differ, as in a multi-task log.
        QString text =
            "NWChem\n" + geometry() + " (Projected Frequencies expressed in cm-1)\n\n P.Frequency 1600\n\n";
        for (int i = 0; i < 9; ++i)
            text += QString(" %1 %2\n").arg(i + 1).arg(i == 4 ? .5 : 0);
        text += "\nProjected Infra Red Intensities\n 1 1600 || 1 1 10 1\n\n";
        auto last = geometry();
        last.replace("0.758", "1.25");
        text += '\n' + last +
                "Chemical Shielding Tensors (GIAO, in ppm)\n"
                "Atom: 2 H\nisotropic = 29.0\nanisotropy = 1\n";
        MainWindow w;
        const auto original = Document::fromXyz("3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
        w.loadDocument(original);
        auto *panel = w.resultsPanel();
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openFile(save("mode-and-nmr.out", text));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        auto *spectra = widget<SpectroscopyView>(w, "resultsSpectroscopy");
        spectra->setParameters({{"kind", "nmr"}, {"nucleus", "1H"}, {"reference_ppm", 31.0}, {"width", .05}});
        auto *peaks = widget<QTableWidget>(w, "spectrumPeaks");
        QCOMPARE(peaks->rowCount(), 1);
        auto *view = widget<MoleculeView>(w, "resultsView");
        panel->selectMode(0, .4, playing);
        QCOMPARE(panel->displayedGeometry().atoms[1].position.x(), .758);
        if (playing)
            QTRY_VERIFY(view->documentRef().atoms[1].position != original.atoms[1].position);
        peaks->setCurrentCell(0, 0);
        QCOMPARE(panel->viewerState().animation, ResultAnimation::None);
        QCOMPARE(view->documentRef().atoms[1].position.x(), 1.25);
        QCOMPARE(panel->displayedGeometry().atoms, view->documentRef().atoms);
        const auto xyz = files.filePath("selected-result.xyz");
        panel->saveGeometry(xyz);
        QCOMPARE(Document::fromXyz(QString::fromUtf8(readFile(xyz))).atoms[1].position.x(), 1.25);
        panel->applyStructure();
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.candidate()->atoms[1].position.x(), 1.25);
        w.cancelCandidate();
        // A direct frame control also interrupts playback, even when it returns to the same frame.
        panel->selectMode(0, {}, true);
        panel->selectFrame(1);
        QCOMPARE(panel->viewerState().animation, ResultAnimation::None);
        QCOMPARE(panel->displayedGeometry().atoms, view->documentRef().atoms);
        // Stopping a mode brings the picture to the same reference that XYZ export uses.
        panel->selectMode(0, {}, true);
        QTRY_VERIFY(view->documentRef().atoms[1].position != original.atoms[1].position);
        panel->stopAnimation();
        QCOMPARE(panel->displayedGeometry().atoms, view->documentRef().atoms);
        QCOMPARE(panel->displayedGeometry().atoms[1].position.x(), .758);
        // The pause button and the public stop operation return to the same reference structure.
        auto *play = widget<QPushButton>(w, "resultsPlayMode");
        play->click();
        QCOMPARE(panel->viewerState().animation, ResultAnimation::Mode);
        play->click();
        QCOMPARE(panel->viewerState().animation, ResultAnimation::None);
        QCOMPARE(panel->displayedGeometry().atoms[1].position.x(), .758);
        QCOMPARE(panel->displayedGeometry().atoms, view->documentRef().atoms);
        // Selecting a frame keeps the remembered mode; clicking that same IR stick must show it again.
        panel->selectFrame(1);
        emit widget<ChartView>(w, "resultsIrSpectrum")->stickClicked(1, 0);
        QCOMPARE(panel->displayedGeometry().atoms[1].position.x(), .758);
    }
    void viewerPathsPropertiesAndMeasurement() {
        MainWindow w;
        w.loadDocument(Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n"));
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentWidget(panel);
        w.show();
        QSignalSpy done(panel, &ResultsPanel::loaded);
        QString scan = "NWChem\n";
        for (int k = 1; k <= 3; ++k)
            scan += QString("MolForma scan %1/3 bond 1 2 = %2\n").arg(k).arg(0.8 + k * .1) + geometry() +
                    QString("         Total SCF energy =    -76.0%1\n").arg(k == 2 ? 3 : k);
        panel->openFile(save("scan-view.out", scan));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        auto *path = widget<QTableWidget>(w, "resultsPath");
        QCOMPARE(path->rowCount(), 3);
        QCOMPARE(path->item(1, 4)->text(), QString("0.000")); // The lowest point is the reference.
        QCOMPARE(path->item(0, 1)->text(), QString("距離 1–2"));
        path->selectRow(0);
        QCOMPARE(widget<QSpinBox>(w, "resultsFrame")->value(), 1);
        // ΔE in the energy table against a chosen row.
        auto *energy = widget<QTableWidget>(w, "resultsEnergy");
        energy->selectRow(0);
        widget<QPushButton>(w, "resultsEnergyReference")->click();
        QCOMPARE(energy->item(0, 2)->text(), QString("0.000"));
        auto *unit = widget<QComboBox>(w, "resultsEnergyUnit");
        unit->setCurrentIndex(unit->findData("hartree"));
        QCOMPARE(energy->item(1, 2)->text(), QString("-0.02000000"));
        // Measuring atoms in the viewer, and comparing with the editor molecule.
        auto *view = widget<MoleculeView>(w, "resultsView");
        const auto ids = view->documentRef().ids();
        emit view->atomClicked(ids[0], Qt::NoModifier);
        emit view->atomClicked(ids[1], Qt::NoModifier);
        QVERIFY(widget<QLabel>(w, "resultsMeasurement")->text().contains("距離 0.9103 Å"));
        widget<QPushButton>(w, "resultsPinMeasurement")->click();
        QCOMPARE(view->monitorList().size(), 1);
        widget<QPushButton>(w, "resultsCompare")->click();
        QVERIFY(widget<QPlainTextEdit>(w, "resultsStatus")->toPlainText().contains("RMSD"));
        // Per-atom properties.
        panel->openFile(
            save("properties-view.out",
                 "NWChem\n" + geometry() +
                     "          ----- Total      gross population on atoms ----\n\n"
                     "              1  O    8.0     8.77813\n              2  H    1.0     0.61093\n"
                     "              3  H    1.0     0.61093\n\n"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
        auto *atoms = widget<QTableWidget>(w, "resultsAtomProperties");
        QCOMPARE(atoms->rowCount(), 3);
        QCOMPARE(atoms->item(0, 2)->text(), QString("-0.7781"));
        QCOMPARE(atoms->item(1, 1)->text(), QString("H"));
        QVERIFY(view->monitorList().isEmpty()); // Measurements belong to the previous result.
        // Valencies and bond indices only with a log that has them.
        QVERIFY(atoms->isColumnHidden(3));
        QVERIFY(widget<QTableWidget>(w, "resultsBondIndices")->parentWidget()->isHidden());
    }
    void orbitalContributionsOfStates() {
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentWidget(panel);
        w.show();
        QSignalSpy done(panel, &ResultsPanel::loaded);
        const QDir dir(FIXTURE_DIR "/nwchem");
        auto *states = widget<QTableWidget>(w, "resultsExcitations");
        auto *pairs = widget<QTableWidget>(w, "resultsContributions");
        auto *title = widget<QLabel>(w, "resultsContributionsTitle");
        const auto cells = [&](int row) {
            QStringList texts;
            for (int column = 0; column < pairs->columnCount(); ++column)
                if (!pairs->isColumnHidden(column))
                    texts << pairs->item(row, column)->text();
            return texts.join(" | ");
        };
        // Unrestricted states: the heaviest pair of each in the table, with its spin, and the pairs of
        // the chosen state (the first one at once) under it.
        panel->openFile(dir.filePath("water_cation_tddft.out"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QCOMPARE(states->rowCount(), 3);
        QVERIFY(!states->isColumnHidden(7) && !pairs->parentWidget()->isHidden());
        QCOMPARE(states->item(0, 7)->text(), QString("β HOMO → LUMO（100%）"));
        QCOMPARE(states->item(2, 7)->text(), QString("β HOMO → LUMO+1（70%）"));
        QCOMPARE(states->item(2, 7)->toolTip(), states->item(2, 7)->text()); // Where the column is narrow.
        // The transitions take the room of what says the same on every row: one symmetry label here.
        QVERIFY(states->isColumnHidden(2) && !states->isColumnHidden(6));
        QCOMPARE(states->currentRow(), 0);
        QCOMPARE(pairs->rowCount(), 1);
        QVERIFY(title->text().contains("状態 1 "));
        states->selectRow(2);
        QCOMPARE(pairs->rowCount(), 4);
        QCOMPARE(cells(0), QString("4 | 6 | β HOMO → LUMO+1 | -0.83332 | 69.8")); // No Y amplitudes.
        QCOMPARE(cells(1), QString("5 | 6 | α HOMO → LUMO | 0.54022 | 29.3"));
        QVERIFY(title->text().contains("状態 3 "));
        // X and Y amplitudes of restricted states.
        panel->openFile(dir.filePath("peroxide_ecd.out"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
        QCOMPARE(states->item(0, 7)->text(), QString("HOMO−1 → LUMO（99%）"));
        QVERIFY(states->isColumnHidden(6)); // No <S²> of restricted states.
        QCOMPARE(pairs->rowCount(), 2);
        QCOMPARE(cells(0), QString("8 | 10 | HOMO−1 → LUMO | 0.99487 | -0.05366 | 98.8"));
        QCOMPARE(cells(1), QString("8 | 12 | HOMO−1 → LUMO+2 | 0.10767 | — | 1.2"));
        // Without a MOLDEN file, a double click chooses the orbital among the energies of the log.
        auto *details = widget<QTabWidget>(w, "resultsDetails");
        auto *orbitals = widget<QTableWidget>(w, "resultsOrbitals");
        emit pairs->cellDoubleClicked(0, 1);
        QCOMPARE(details->currentIndex(), 2);
        QVERIFY(orbitals->currentRow() >= 0);
        QCOMPARE(orbitals->item(orbitals->currentRow(), 0)->text(), QString("10"));
        // A log without the pairs keeps the table as it was; one whose frontier orbitals are not
        // known gives the numbers.
        panel->openFile(
            save("states-only.out",
                 "NWChem\n  Root   1 singlet a              0.313760064 a.u.                "
                 "8.5378 eV \n     Dipole Oscillator Strength                    0.0198731899\n"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 3, 10000);
        QVERIFY(states->isColumnHidden(7) && pairs->parentWidget()->isHidden());
        QVERIFY(!states->isColumnHidden(2) && !states->isColumnHidden(6));
        const QString log =
            "NWChem\n  Root   1 singlet a              0.313760064 a.u.                "
            "8.5378 eV \n     Dipole Oscillator Strength                    0.0198731899\n\n"
            "     Occ.    1  a   ---  Virt.    2  a    0.99000 X\n"
            "  ----------------------------------------------------------------------------\n";
        panel->openFile(save("states-numbers.out", log));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 4, 10000);
        QCOMPARE(states->item(0, 7)->text(), QString("1 → 2（100%）"));
        QCOMPARE(cells(0), QString("1 | 2 | 0.99000 | 100.0"));
        // With the MOLDEN file of the job, a double click on the virtual orbital shows its shape.
        QTemporaryDir job;
        writeFile(job.filePath("output.out"), log.toUtf8());
        writeFile(job.filePath("job.molden"), "[Molden Format]\n[Atoms] AU\nH 1 1 0 0 0\nH 2 1 0 0 1.4\n"
                                              "[GTO]\n1 0\ns 1 1.0\n1.0 1.0\n\n2 0\ns 1 1.0\n1.0 1.0\n\n"
                                              "[MO]\nSym= a\nEne= -0.5\nSpin= Alpha\nOccup= 2\n1 0.5\n2 0.5\n"
                                              "Sym= b\nEne= 0.3\nSpin= Alpha\nOccup= 0\n1 0.7\n2 -0.7\n");
        panel->openDirectory(job.path());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 5, 10000);
        QVERIFY(panel->result().source.endsWith("output.out"));
        QVERIFY(title->text().contains("job.molden"));
        emit pairs->cellDoubleClicked(0, 1);
        QTRY_VERIFY_WITH_TIMEOUT(panel->result().source.endsWith("job.molden") &&
                                     panel->result().selectedOrbital == 1 && !panel->loading(),
                                 10000);
        QCOMPARE(details->currentIndex(), 2);
    }
    void bondIndicesInViewer() {
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentWidget(panel);
        w.show();
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openFile(FIXTURE_DIR "/nwchem/ethanol_mulliken.out");
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        // The valency beside the Mulliken charge of each atom.
        auto *atoms = widget<QTableWidget>(w, "resultsAtomProperties");
        QCOMPARE(atoms->rowCount(), 9);
        QVERIFY(!atoms->isColumnHidden(3));
        QCOMPARE(atoms->item(0, 3)->text(), QString("3.9591"));
        QCOMPARE(atoms->item(2, 3)->text(), QString("2.0215"));
        // The bonds (0.05 and more), the largest index first, with the distance in the structure.
        auto *bonds = widget<QTableWidget>(w, "resultsBondIndices");
        auto *all = widget<QCheckBox>(w, "resultsBondIndicesAll");
        QVERIFY(!bonds->parentWidget()->isHidden() && !all->isHidden() && !all->isChecked());
        QCOMPARE(bonds->rowCount(), 8);
        const auto cells = [&](int row) {
            return bonds->item(row, 0)->text() + " " + bonds->item(row, 1)->text() + " " +
                   bonds->item(row, 2)->text();
        };
        QCOMPARE(cells(0), QString("C2–O3 1.00439 1.4207"));
        QCOMPARE(cells(1), QString("C1–C2 1.00216 1.5191"));
        QCOMPARE(cells(7), QString("O3–H9 0.93779 0.9658"));
        QVERIFY(widget<QLabel>(w, "resultsBondIndicesTitle")->text().contains("8"));
        // Choosing a row picks its two atoms in the view, as for a measurement.
        auto *view = widget<MoleculeView>(w, "resultsView");
        QVERIFY(view->selection().isEmpty());
        bonds->selectRow(0);
        const auto &molecule = view->documentRef();
        QCOMPARE(view->selection(), (Ids{molecule.atoms[1].id, molecule.atoms[2].id}));
        QVERIFY(widget<QLabel>(w, "resultsMeasurement")->text().contains("距離 1.4207 Å"));
        // Every pair on request.
        all->setChecked(true);
        QCOMPARE(bonds->rowCount(), 36);
        QCOMPARE(cells(8), QString("O3–H7 0.02891 2.0779"));
        QCOMPARE(bonds->item(35, 1)->text(), QString("0.00011"));
        all->setChecked(false);
        QCOMPARE(bonds->rowCount(), 8);
    }
    void animationFilesGrouped() {
        // NWChem "freq animate": one XYZ per mode and step, shown as one entry per mode.
        QTemporaryDir job;
        writeFile(job.filePath("output.out"), ("NWChem\n" + geometry()).toUtf8());
        for (int mode = 1; mode <= 2; ++mode)
            for (int step = 1; step <= 3; ++step)
                writeFile(job.filePath(QString("w_vib.freq.m-%1.s-%2.xyz")
                                           .arg(mode, 3, 10, QChar('0'))
                                           .arg(step, 3, 10, QChar('0'))),
                          QString("1\nstep %1\nH 0 0 %2\n").arg(step).arg(step).toUtf8());
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openDirectory(job.path());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        auto *chooser = widget<QComboBox>(w, "resultsFiles");
        QStringList entries;
        for (int i = 0; i < chooser->count(); ++i)
            if (!chooser->itemText(i).isEmpty())
                entries << chooser->itemText(i);
        QCOMPARE(entries.size(), 3); // output.out and two modes, not six files.
        const int mode = chooser->findText("振動アニメーション w_vib.freq: モード 2（3枚）");
        QVERIFY2(mode >= 0, qPrintable(entries.join(" | ")));
        chooser->setCurrentIndex(mode);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
        QCOMPARE(panel->result().frames.size(), 3);
        QCOMPARE(panel->result().frames[2].molecule.atoms[0].position.z(), 3.0);
        QCOMPARE(chooser->currentText(), QString("振動アニメーション w_vib.freq: モード 2（3枚）"));
        const auto first = job.filePath("w_vib.freq.m-002.s-001.xyz");
        QCOMPARE(panel->result().source, first); // Original source, never a temporary joined file.
        writeFile(first, "1\nupdated\nH 0 0 9\n");
        writeFile(job.filePath("w_vib.freq.m-002.s-010.xyz"), "1\nnew step\nH 0 0 10\n");
        panel->reloadResult();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 3, 10000);
        QCOMPARE(panel->result().frames.size(), 4);
        QCOMPARE(panel->result().frames[0].molecule.atoms[0].position.z(), 9.0);
        QCOMPARE(panel->result().frames[3].molecule.atoms[0].position.z(), 10.0);
        QCOMPARE(chooser->currentText(), QString("振動アニメーション w_vib.freq: モード 2（4枚）"));
        QVERIFY(QFile::remove(first));
        panel->reloadResult();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 4, 10000);
        QCOMPARE(panel->result().frames.size(), 3);
        QCOMPARE(panel->result().source, job.filePath("w_vib.freq.m-002.s-002.xyz"));
        QCOMPARE(chooser->currentText(), QString("振動アニメーション w_vib.freq: モード 2（3枚）"));
    }
    void animationLoadingBoundsReadsAndKeepsPreviousResult() {
        QTemporaryDir job;
        const auto first = job.filePath("mode.m-001.s-001.xyz");
        writeFile(first, "1\nfirst\nH 0 0 1\n");
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        QSignalSpy loaded(panel, &ResultsPanel::loaded), failed(panel, &ResultsPanel::loadFailed);
        panel->openDirectory(job.path());
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        QFile oversized(job.filePath("mode.m-001.s-002.xyz"));
        QVERIFY(oversized.open(QIODevice::WriteOnly));
        QVERIFY(oversized.resize(64 * 1024 * 1024 + 1));
        oversized.close();
        panel->reloadResult();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QVERIFY(failed[0][0].toString().contains("大きすぎます"));
        QCOMPARE(panel->result().frames.size(), 1);
        QCOMPARE(panel->result().frames[0].molecule.atoms[0].position.z(), 1.0);
        QVERIFY(QFile::remove(oversized.fileName()));
        QVERIFY(QFile::remove(first));
        panel->reloadResult();
        QCOMPARE(failed.count(), 2);
        QCOMPARE(panel->result().frames.size(), 1);
        QCOMPARE(loaded.count(), 1);
    }
    void cameraSlidersPreserveGeometry() {
        MainWindow w;
        auto editor = Document::fromXyz("2\neditor\nH 0 0 0\nH 0 0 .74\n");
        editor.atoms[0].fixed = true;
        w.loadDocument(editor);
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentWidget(panel);
        w.show();
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openFile(save("camera.xyz", "3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        auto *view = widget<MoleculeView>(w, "resultsView");
        const auto molecule = panel->result().frames[0].molecule;
        const auto before = w.document().project();
        const int history = w.history()->count();
        const auto editorPosition = w.canvas()->atomScreenPosition(w.document().atoms[0].id);
        const auto output = files.filePath("camera-saved.xyz");
        panel->saveGeometry(output);
        const auto saved = readFile(output);
        MoleculeView expected;
        expected.resize(view->size());
        expected.setDocument(molecule);
        expected.fit();
        int angles[] = {-25, 25, 0};
        const int rotated[] = {60, -45, 90};
        const char *names[] = {"resultsYaw", "resultsPitch", "resultsRoll"};
        expected.setAngles(angles[0], angles[1], angles[2]);
        for (const auto &atom : molecule.atoms)
            QCOMPARE(view->atomScreenPosition(atom.id), expected.atomScreenPosition(atom.id));
        // The sliders are in the popup of "回転" over the view.
        widget<QToolButton>(w, "resultsRotation")->click();
        for (int i = 0; i < 3; ++i) {
            auto *slider = widget<QSlider>(w, names[i]);
            QVERIFY(slider->isVisible());
            QCOMPARE(slider->orientation(), Qt::Horizontal);
            QCOMPARE(slider->minimum(), -180);
            QCOMPARE(slider->maximum(), 180);
            QCOMPARE(slider->value(), angles[i]);
            const auto position = view->atomScreenPosition(molecule.atoms[1].id);
            slider->setValue(rotated[i]);
            angles[i] = rotated[i];
            expected.setAngles(angles[0], angles[1], angles[2]);
            QVERIFY(QLineF(position, view->atomScreenPosition(molecule.atoms[1].id)).length() > 1);
            for (const auto &atom : molecule.atoms)
                QCOMPARE(view->atomScreenPosition(atom.id), expected.atomScreenPosition(atom.id));
            panel->saveGeometry(output);
            QCOMPARE(readFile(output), saved);
        }
        QCOMPARE(panel->result().frames[0].molecule.toJson(), molecule.toJson());
        QCOMPARE(w.document().project(), before);
        QCOMPARE(w.history()->count(), history);
        QCOMPARE(w.canvas()->atomScreenPosition(w.document().atoms[0].id), editorPosition);
    }
    void changedSourceRejected() {
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        QSignalSpy done(panel, &ResultsPanel::loaded), failed(panel, &ResultsPanel::loadFailed);
        const auto path = save("changing.xyz", "1\ninitial\nH 0 0 0\n");
        panel->openFile(path);
        writeFile(path, "1\nmodified before worker completes\nH 1 0 0\n");
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QCOMPARE(done.count(), 0);
        QVERIFY(panel->result().source.isEmpty());
    }
    void executionNavigationAndCubeControls() {
        MainWindow w;
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 .504\nH -.758 0 .504\n");
        setExecutionSetting(d, "job_root", files.filePath("viewer-jobs"));
        w.loadDocument(d);
        auto *execution = widget<ExecutionPanel>(w, "executionTab");
        execution->saveBundle();
        // The label writes the home folder as "~"; its tooltip holds the whole path.
        const auto directory = widget<QLabel>(w, "executionLocation")->toolTip();
        QVERIFY(!directory.isEmpty());
        QCOMPARE(widget<QLabel>(w, "executionLocation")->text(), displayPath(directory));
        QCOMPARE(displayPath(QDir::homePath() + "/MolForma/jobs/a"), QString("~/MolForma/jobs/a"));
        QCOMPARE(displayPath(QDir::homePath() + "x"), QDir::homePath() + "x");
        const auto output = QDir(directory).filePath("output.out");
        writeFile(output, ("NWChem\n" + geometry() + "Total SCF energy = -74.9\n").toUtf8());
        execution->refresh();
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        QSignalSpy done(panel, &ResultsPanel::loaded), failed(panel, &ResultsPanel::loadFailed);
        QVERIFY(widget<QPushButton>(w, "executionResults")->isEnabled());
        widget<QPushButton>(w, "executionResults")->click();
        QCOMPARE(widget<QTabWidget>(w, "workspaceTabs")->currentIndex(), 4);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QCOMPARE(panel->result().source, output);
        panel->openFile(save("control.cube", cubeText(true)));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
        widget<QLineEdit>(w, "resultsIsovalue")->setText("0.25");
        widget<QComboBox>(w, "resultsDataset")->setCurrentIndex(1);
        widget<QPushButton>(w, "resultsApplyIso")->click();
        QVERIFY(!w.findChild<QAction *>("resultsSaveXyz")->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 3, 10000);
        QCOMPARE(panel->result().isovalue, .25);
        QCOMPARE(panel->result().dataset, 1);
        widget<QLineEdit>(w, "resultsIsovalue")->setText("nan");
        widget<QPushButton>(w, "resultsApplyIso")->click();
        QCOMPARE(failed.count(), 1);
        QCOMPARE(panel->result().isovalue, .25);
        panel->openFile(output); // An invalid CUBE setting must not block logs.
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 4, 10000);
        QCOMPARE(w.document().project(), d.project());
        QVERIFY(w.history()->isClean());
    }
    void realEngineArtifacts() {
        const auto root = qEnvironmentVariable("MOLFORMA_RESULTS_REAL");
        if (root.isEmpty())
            return;
        auto logData = readResult(QDir(root).filePath("output.out"));
        QVERIFY(logData.frames.size() > 2);
        QCOMPARE(logData.modes.size(), 9);
        QVERIFY(logData.status.contains("エラー"));
        QVERIFY(std::abs(logData.modes[6].frequency - 2169.67) < .01);
        QVERIFY(std::abs(logData.modes[6].intensity - 7.247) < .001);
        auto electronic = readResult(QDir(root).filePath("export.out"));
        QVERIFY(electronic.status.contains("正常終了マーカーあり"));
        const auto methods = readResult(QDir(root).filePath("methods2.out"));
        QVERIFY(methods.status.contains("正常終了マーカーあり"));
        QMap<QString, double> energies;
        for (const auto &e : methods.energies)
            energies[e.method] = e.hartree;
        QVERIFY(std::abs(energies.value("MP2") + 76.106091880695) < 1e-9);
        QVERIFY(std::abs(energies.value("CCSD") + 76.112438787689) < 1e-9);
        QVERIFY(std::abs(energies.value("CCSD(T)") + 76.113292997246) < 1e-9);
        QVERIFY(std::abs(energies.value("DFT") + 76.377870344163) < 1e-9);
        auto mo = readResult(QDir(root).filePath("export.molden"));
        QCOMPARE(mo.orbitals.size(), 7);
        QCOMPARE(mo.frames[0].molecule.atoms.size(), 3);
        auto cube = readResult(QDir(root).filePath("homo.cube"));
        QVERIFY(!cube.surface.isEmpty());
        auto trajectory = readResult(QDir(root).filePath("water_vib.freq.m-007.xyz"));
        QVERIFY(trajectory.frames.size() > 1);
        MainWindow w;
        w.resize(1380, 900);
        w.show();
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentIndex(4);
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openFile(QDir(root).filePath("homo.cube"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QCoreApplication::processEvents();
        QVERIFY(w.grab().save(QDir(root).filePath("viewer-cube.png")));
        panel->openFile(QDir(root).filePath("output.out"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
        widget<QTabWidget>(w, "resultsDetails")->setCurrentIndex(3);
        widget<QTableWidget>(w, "resultsModes")->setCurrentCell(6, 0);
        QCoreApplication::processEvents();
        QVERIFY(w.grab().save(QDir(root).filePath("viewer-vibration.png")));
    }
};
int main(int argc, char **argv) {
    if (const int code = resultWorkerMain(argc, argv); code >= 0)
        return code;
    QApplication app(argc, argv);
    ResultsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_results.moc"
