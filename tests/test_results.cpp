#include "editing/coordinates.h"
#include "ui/execution_panel.h"
#include "ui/main_window.h"
#include "ui/molecule_view.h"
#include "ui/result_charts.h"
#include "ui/results_panel.h"
#include <Eigen/LU>
#include <QtTest>
#include <QtWidgets>
#include <numbers>
using namespace mf;

class ResultsTest : public QObject {
    Q_OBJECT
    QTemporaryDir files, settings;
    QString save(const QString &name, const QString &contents) {
        auto path = files.filePath(name);
        writeFile(path, contents.toUtf8());
        return path;
    }
    static QString geometry() {
        return " Output coordinates in angstroms (scale by 1.8897 to convert to a.u.)\n\n"
               " No. Tag Charge X Y Z\n ---- ---- ------ ----- ----- -----\n"
               " 1 O 8.0 0.0 0.0 0.0\n 2 H 1.0 0.758 0.0 0.504\n 3 H 1.0 -0.758 0.0 0.504\n\n";
    }
    static QString cubeText(bool multiple = false) {
        QString s = "CUBE regression\nlinear field\n";
        s += multiple ? "-1 1 2 3\n" : "1 1 2 3\n";
        s += "2 1 0 0\n2 0.2 1 0\n2 0 0.3 1\n6 4 1 2 3\n";
        if (multiple)
            s += "2 5 6\n";
        for (int x = 0; x < 2; ++x)
            for (int y = 0; y < 2; ++y)
                for (int z = 0; z < 2; ++z) {
                    s += QString::number(x - .5) + " ";
                    if (multiple)
                        s += QString::number(y - .5) + " ";
                }
        return s + "\n";
    }
    static QString moldenText() {
        return "[Molden Format]\n[Atoms] AU\nH 1 1 0 0 0\nH 2 1 0 0 1.4\n"
               "[GTO]\n1 0\ns 1 1.0\n1.0 1.0\n"
               "[MO]\nSym= A1\nEne= -0.5D+00\nSpin= Alpha\nOccup= 2.0\n1 0.5\n2 0.5\n"
               "Sym= B1\nEne= 0.25\nSpin= Beta\nOccup= 0\n1 0.5\n2 -0.5\n"
               "[FREQ]\n-100\n1500\n[FR-COORD]\nH 0 0 0\nH 0 0 1.4\n"
               "[FR-NORM-COORD]\nvibration 1\n0 0 .5\n0 0 -.5\nvibration 2\n0 0 .1\n0 0 -.1\n"
               "[INT]\n10\n20\n";
    }
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
    }
    void logGeometryEnergyAndFailure() {
        auto source = "NWChem\n" + geometry() +
                      " Total SCF energy = -7.4945628203255D+01\n"
                      " Total MP2 energy  -75.0\n Total CCSD(T) energy: -75.1\n"
                      " Optimization converged\n Total times  cpu: 1.0s wall: 1.0s\n";
        auto r = readResult(save("normal.out", source));
        QCOMPARE(r.frames.size(), 1);
        QCOMPARE(r.frames[0].molecule.atoms.size(), 3);
        QCOMPARE(r.frames[0].molecule.bonds.size(), 2);
        QCOMPARE(r.energies.size(), 3);
        QVERIFY(std::abs(r.energies[0].hartree + 74.945628203255) < 1e-12);
        QCOMPARE(r.energies[2].method, "CCSD(T)");
        QVERIFY(r.status.contains("正常終了マーカーあり"));
        QCOMPARE(resultOutcome(r), QString("normal"));
        r = readResult(save("failed.out", source + "An error occurred\nMPI_ABORT\n"));
        QVERIFY(r.status.contains("エラー"));
        QCOMPARE(resultOutcome(r), QString("error"));
        // NWChem 7.3 errquit block (here: an unknown basis name), without any generic error words.
        const QString dashes(" " + QString(72, '-') + "\n");
        r = readResult(save("errquit.out",
                            "NWChem\n" + geometry() + dashes +
                                " bas_tag_lib: failed opening basis file                   0\n" + dashes +
                                dashes + "  current input line : \n" + "    11: task scf energy\n" + dashes +
                                dashes + " There is an error in the specified basis set\n" + dashes));
        QCOMPARE(r.status, QString("NWChemエラー: bas_tag_lib: failed opening basis file"));
        QCOMPARE(resultOutcome(r), QString("error"));
        r = readResult(save("optfail.out", "NWChem\n" + geometry() + dashes +
                                               " scf optimize failed                                    0\n" +
                                               dashes + dashes + "  current input line : \n" +
                                               "    17: task scf optimize\n" + dashes));
        QCOMPARE(r.status, QString("NWChemエラー: scf optimize failed"));
        r = readResult(save("partial.out", "NWChem\n" + geometry() + geometry().chopped(35)));
        QCOMPARE(r.frames.size(), 1);
        QVERIFY(r.status.contains("マーカーなし"));
        QCOMPARE(resultOutcome(r), QString("incomplete"));
        QVERIFY(!r.warnings.isEmpty());
        auto au = geometry();
        au.replace("in angstroms", "in a.u.");
        r = readResult(save("bohr.log", "NWChem\n" + au));
        QVERIFY(std::abs(r.frames[0].molecule.atoms[1].position.x() - .758 * BohrToAngstrom) < 1e-12);
        QVERIFY_EXCEPTION_THROWN(readResult(save("not.log", "hello")), std::runtime_error);
    }
    void projectedModes() {
        QString s = "NWChem\n" + geometry() +
                    " (Projected Frequencies expressed in cm-1)\n\n P.Frequency -100 1600\n\n";
        for (int i = 0; i < 9; ++i)
            s += QString(" %1 %2 %3\n").arg(i + 1).arg(i == 0 ? 1 : 0).arg(i == 4 ? .5 : 0);
        s += "\n Normal Eigenvalue || Projected Infra Red Intensities\n Mode [cm**-1] || units units units "
             "units\n ---- ---- || ---- ---- ---- ----\n"
             " 1 -100 || 0.1 0.2 5.5 1.0\n 2 1600 || 0.2 0.3 8.5 1.2\n ----\n";
        const auto r = readResult(save("modes.out", s));
        QCOMPARE(r.modes.size(), 2);
        QCOMPARE(r.modes[1].intensity, 8.5);
        const auto moved = resultModeGeometry(r, 0, std::numbers::pi / 2, .3);
        QVERIFY(std::abs(moved.atoms[0].position.x() - .3) < 1e-12);
        QCOMPARE(r.vibrationGeometry.atoms[0].position.x(), 0.0);
    }
    void xyzAndMolden() {
        auto r = readResult(save("path.xyz", "2\n\nH 0 0 0\nH 0 0 1\n2\nsecond\nH 0 0 0\nH 0 0 2\n"));
        QCOMPARE(r.frames.size(), 2);
        QCOMPARE(r.frames[1].label, "second");
        QVERIFY(resultOutcome(r).isEmpty()); // Only logs have an outcome.
        QVERIFY_EXCEPTION_THROWN(readResult(save("truncated.xyz", "2\ntitle\nH 0 0 0\n")),
                                 std::runtime_error);
        r = readResult(save("orbitals.molden", moldenText()));
        QCOMPARE(r.orbitals.size(), 2);
        QCOMPARE(r.orbitals[0].energy, -.5);
        QCOMPARE(r.orbitals[1].spin, "Beta");
        QCOMPARE(r.orbitals[0].number, 1);
        QCOMPARE(r.orbitals[1].number, 1); // Counted within each spin.
        QCOMPARE(r.modes.size(), 2);
        QCOMPARE(r.modes[1].intensity, 20.0);
        QVERIFY(std::abs(r.frames[0].molecule.atoms[1].position.z() - 1.4 * BohrToAngstrom) < 1e-12);
        auto bad = moldenText();
        bad.replace("[Atoms] AU", "[Atoms] unknown");
        QVERIFY_EXCEPTION_THROWN(readResult(save("bad.molden", bad)), std::runtime_error);
        bad = moldenText();
        bad.replace("Occup= 2.0", "Occup= nan");
        QVERIFY_EXCEPTION_THROWN(readResult(save("nan.molden", bad)), std::runtime_error);
        bad = moldenText();
        bad.replace("vibration 2\n0 0 .1\n0 0 -.1", "vibration 2\n0 0 .1");
        QVERIFY_EXCEPTION_THROWN(readResult(save("partial.molden", bad)), std::runtime_error);
    }
    void affineCubeAndDatasets() {
        const auto path = save("affine.cube", cubeText());
        auto r = readResult(path, .25);
        QCOMPARE(r.frames[0].molecule.atoms[0].element, "C");
        QVERIFY(!r.surface.isEmpty());
        Mat axes;
        axes << 1, .2, 0, 0, 1, .3, 0, 0, 1;
        axes *= BohrToAngstrom;
        const Vec origin = Vec(1, 2, 3) * BohrToAngstrom;
        for (const auto &t : r.surface)
            for (const Vec &p : {t.a, t.b, t.c}) {
                Vec grid = axes.inverse() * (p - origin);
                QVERIFY(std::abs(grid.x() - (t.positive ? .75 : .25)) < 1e-10);
            }
        r = readResult(save("orbitals.cube", cubeText(true)), .25, 1);
        QCOMPARE(r.datasets, QVector<int>({5, 6}));
        for (const auto &t : r.surface)
            for (const Vec &p : {t.a, t.b, t.c}) {
                Vec grid = axes.inverse() * (p - origin);
                QVERIFY(std::abs(grid.y() - (t.positive ? .75 : .25)) < 1e-10);
            }
        auto nval = cubeText(true);
        nval.replace("-1 1 2 3", "1 1 2 3 2");
        nval.replace("2 5 6\n", "");
        QCOMPARE(readResult(save("channels.cube", nval), .25, 1).datasets, QVector<int>({1, 2}));
        QVERIFY(readResult(path, 10).surface.isEmpty());
        for (const auto &bad :
             {cubeText() + "1", cubeText().chopped(8), QString(cubeText()).replace("2 1 0 0", "-2 1 0 0"),
              QString(cubeText()).replace("2 1 0 0", "2000000 1 0 0"),
              QString(cubeText()).replace("0.5", "nan")})
            QVERIFY_EXCEPTION_THROWN(readResult(save("invalid.cube", bad)), std::runtime_error);
    }
    void heavyElementResults() {
        for (const auto &symbol : {"Fr", "U", "Nh", "Og"}) {
            const int z = element(symbol).number;
            auto cube = cubeText();
            cube.replace("6 4 1 2 3", QString("%1 %1 1 2 3").arg(z));
            const auto c = readResult(save("heavy.cube", cube), .25);
            QCOMPARE(c.frames[0].molecule.atoms[0].element, QString(symbol));
            const auto molden = QString("[Molden Format]\n[Atoms] Angs\n%1 1 %2 0 0 0\n").arg(symbol).arg(z);
            const auto m = readResult(save("heavy.molden", molden));
            QCOMPARE(m.frames[0].molecule.atoms[0].element, QString(symbol));
            const auto log = QString("NWChem\n Output coordinates in angstroms\n\n"
                                     " No. Tag Charge X Y Z\n ---- ---- ------ ----- ----- -----\n"
                                     " 1 %1 %2.0 0.0 0.0 0.0\n\n")
                                 .arg(symbol)
                                 .arg(z);
            const auto l = readResult(save("heavy.out", log));
            QCOMPARE(l.frames[0].molecule.atoms[0].element, QString(symbol));
        }
        auto invalid = cubeText();
        invalid.replace("6 4 1 2 3", "119 119 1 2 3");
        QVERIFY_EXCEPTION_THROWN(readResult(save("unknown.cube", invalid)), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(
            readResult(save("unknown.molden", "[Molden Format]\n[Atoms] Angs\nUue 1 119 0 0 0\n")),
            std::runtime_error);
    }
    void logSummaries() {
        // Excerpts in the layout NWChem 7.3.1 prints (TDDFT, DRIVER, vib, multipole and MO analysis).
        const QString log =
            "NWChem\n" + geometry() +
            " maximum gradient threshold         (gmax) =   0.000450\n"
            " rms gradient threshold             (grms) =   0.000300\n"
            "@ Step       Energy      Delta E   Gmax     Grms     Xrms     Xmax   Walltime\n"
            "@ ---- ---------------- -------- -------- -------- -------- -------- --------\n"
            "@    0     -76.00723755  0.0D+00  0.06730  0.02421  0.00000  0.00000      0.3\n"
            "@    1     -76.01098000 -3.7D-03  0.00835  0.00348  0.02135  0.04987      0.7\n"
            "      Optimization converged\n"
            "@    1     -76.01098000 -3.7D-03  0.00835  0.00348  0.02135  0.04987      0.7\n"
            "                       DFT Final Molecular Orbital Analysis\n"
            " Vector    5  Occ=2.000000D+00  E=-3.907179D-01\n"
            " Vector    6  Occ=0.000000D+00  E= 6.054023D-01\n"
            "       Multipole analysis of the density wrt the origin\n"
            "     L   x y z        total         open         nuclear\n"
            "     0   0 0 0     -0.000000      0.000000     10.000000\n\n"
            "     1   1 0 0      0.000000      0.000000      0.000000\n"
            "     1   0 1 0     -0.000000      0.000000      0.000000\n"
            "     1   0 0 1     -0.677559      0.000000     -0.113384\n"
            "  Root   1 singlet a              0.313760064 a.u.                8.5378 eV \n"
            "     Dipole Oscillator Strength                    0.0198731899\n"
            "  Root   1 triplet a              0.286149906 a.u.                7.7865 eV \n"
            "  Root   2         a              0.076143009 a.u.                2.0720 eV \n"
            "  <S2> =   0.7528\n"
            "     Dipole Oscillator Strength                    0.0010067966\n"
            " Temperature                      =   298.15K\n"
            " Zero-Point correction to Energy  =   15.291 kcal/mol  (  0.024367 au)\n"
            " Thermal correction to Energy     =   17.068 kcal/mol  (  0.027199 au)\n"
            " Thermal correction to Enthalpy   =   17.660 kcal/mol  (  0.028143 au)\n"
            " Total Entropy                    =   46.638 cal/mol-K\n"
            "   - Rotational                   =   12.045 cal/mol-K (symmetry #  =        1)\n"
            " Cv (constant volume heat capacity) =    5.965 cal/mol-K\n";
        auto r = readResult(save("summaries.out", log));
        QCOMPARE(r.steps.size(), 2);
        QCOMPARE(r.steps[1].gmax, 0.00835);
        QCOMPARE(r.properties["optimization"].toString(), QString("converged"));
        QCOMPARE(r.properties["thresholds"].toObject()["gmax"].toDouble(), 0.00045);
        QCOMPARE(r.orbitals.size(), 2);
        QCOMPARE(r.orbitals[0].occupation, 2.0);
        // A log lists only some orbitals: keep NWChem's numbers (as in the MOLDEN file).
        QCOMPARE(r.orbitals[0].number, 5);
        QCOMPARE(r.orbitals[1].number, 6);
        QVERIFY(std::abs(r.properties["dipole"].toObject()["debye"].toDouble() - 0.677559 * 2.541746473) <
                1e-9);
        QCOMPARE(r.excitations.size(), 3);
        QCOMPARE(r.excitations[0].spin, QString("singlet"));
        QCOMPARE(r.excitations[0].oscillator, 0.0198731899);
        QCOMPARE(r.excitations[1].oscillator, 0.0);
        QVERIFY(r.excitations[2].spin.isEmpty());
        QCOMPARE(r.excitations[2].s2, 0.7528);
        const auto thermo = r.properties["thermochemistry"].toArray()[0].toObject();
        QCOMPARE(thermo["temperature"].toDouble(), 298.15);
        QCOMPARE(thermo["entropy_cal"].toDouble(), 46.638);
        QCOMPARE(thermo["symmetry_number"].toDouble(), 1.0);
        // The worker transfer keeps the new sections.
        const auto path = files.filePath("summaries.bin");
        saveResultData(path, r);
        const auto loaded = loadResultData(path);
        QCOMPARE(loaded.excitations.size(), 3);
        QCOMPARE(loaded.excitations[2].s2, 0.7528);
        QCOMPARE(loaded.steps.size(), 2);
        QCOMPARE(loaded.properties, r.properties);
    }
    void calculationEnergyByTask() {
        // Optimization, then numerical frequencies: the displaced structures' energies come last.
        const QString log = "NWChem\n" + geometry() +
                            "                                NWChem Input Module\n"
                            "         Total DFT energy =    -76.42\n"
                            "         Total DFT energy =    -76.43\n"
                            "                                NWChem Input Module\n"
                            "                   NWChem Nuclear Hessian and Frequency Analysis\n"
                            "         Total DFT energy =    -76.43\n"
                            "         Total DFT energy =    -76.4299\n"
                            "                                NWChem Input Module\n";
        auto r = readResult(save("frequencies.out", log));
        QCOMPARE(r.energies.last().hartree, -76.4299);
        auto energy = r.properties["final_energy"].toObject();
        QCOMPARE(energy["hartree"].toDouble(), -76.43);
        QCOMPARE(energy["method"].toString(), "DFT");
        QCOMPARE(energy["source"].toString(), "frequency_reference");
        // An energy task after it (e.g. a second level) is the calculation's energy again.
        r = readResult(save("followup.out", log + "         Total CCSD(T) energy:    -76.30\n"));
        energy = r.properties["final_energy"].toObject();
        QCOMPARE(energy["hartree"].toDouble(), -76.30);
        QCOMPARE(energy["source"].toString(), "final");
    }
    void transferAndBounds() {
        auto r = readResult(save("transfer.cube", cubeText()), .25);
        auto path = files.filePath("result.bin");
        saveResultData(path, r);
        auto loaded = loadResultData(path);
        QCOMPARE(loaded.surface.size(), r.surface.size());
        QCOMPARE(loaded.frames[0].molecule.toJson(), r.frames[0].molecule.toJson());
        QVERIFY((loaded.surface[0].a - r.surface[0].a).norm() < 1e-15);
        writeFile(path, readFile(path).chopped(4));
        QVERIFY_EXCEPTION_THROWN(loadResultData(path), std::runtime_error);
        QFile huge(files.filePath("huge.xyz"));
        QVERIFY(huge.open(QIODevice::WriteOnly));
        QVERIFY(huge.resize(128 * 1024 * 1024 + 1));
        huge.close();
        QVERIFY_EXCEPTION_THROWN(readResult(huge.fileName()), std::runtime_error);
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
        QTest::qWait(100);
        panel->saveGeometry(output);
        QVERIFY(std::abs(Document::fromXyz(QString::fromUtf8(readFile(output))).atoms[1].position.z() -
                         1.4 * BohrToAngstrom) < 1e-10);
        QCOMPARE(w.document().project(), before);
        QCOMPARE(w.history()->count(), history);
        // Destruction cancels an in-flight reader and stops animation.
        panel->openFile(a);
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
        QVERIFY(widget<QPlainTextEdit>(w, "resultsStatus")->toPlainText().contains("最適化: 収束"));
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
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openDirectory(job.path());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        // How the run ended: in the bar and as a banner over the details.
        QCOMPARE(resultOutcome(panel->result()), QString("incomplete"));
        QCOMPARE(widget<QLabel>(w, "resultsOutcome")->text(), QString("正常終了の記録なし"));
        QVERIFY(widget<QFrame>(w, "resultsBanner")->isVisible());
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
    void rigidModes() {
        ResultData r;
        r.vibrationGeometry = Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
        for (double f : {-0.02, 1600.0, 0.01, -1.5, 0.0, 3700.0, 2.0, 0.5, 3800.0})
            r.modes.append({f, 1, {}});
        const auto rigid = resultRigidModes(r);
        QCOMPARE(rigid, (QVector<bool>{true, false, true, true, true, false, true, true, false}));
        // Linear molecules have five rigid motions; incomplete mode sets are left alone.
        r.vibrationGeometry = Document::fromXyz("3\nCO2\nO -1.16 0 0\nC 0 0 0\nO 1.16 0 0\n");
        r.modes[6].frequency = 667;
        QCOMPARE(resultRigidModes(r).count(true), 5);
        r.modes.removeLast();
        QCOMPARE(resultRigidModes(r).count(true), 0);
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
        QVERIFY_EXCEPTION_THROWN(applyResultGeometry(pinned, optimized), std::runtime_error);
        pinned.atoms[1].fixed = false;
        pinned.atoms[0].fixed = true; // O did not move in the result.
        applyResultGeometry(pinned, optimized);
        auto other = Document::fromXyz("3\nwrong order\nH .758 0 .504\nO 0 0 0\nH -.758 0 .504\n");
        QVERIFY_EXCEPTION_THROWN(applyResultGeometry(copy, other), std::runtime_error);
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
        const auto directory = widget<QLabel>(w, "executionLocation")->text();
        QVERIFY(!directory.isEmpty());
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
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--result-worker") {
        QCoreApplication app(argc, argv);
        return resultWorker(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    }
    QApplication app(argc, argv);
    ResultsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_results.moc"
