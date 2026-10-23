#include "coordinates.h"
#include "main_window.h"
#include "molden_orbitals.h"
#include "results_panel.h"
#include <QtTest>
#include <QtWidgets>
#include <numbers>
using namespace mf;

class MoldenTest : public QObject {
    Q_OBJECT
    QTemporaryDir files, settings;
    static Document hydrogen() {
        return Document::fromXyz("1\nH\nH 0 0 0\n");
    }
    QString save(const QString &name, const QString &text) {
        const auto path = files.filePath(name);
        writeFile(path, text.toUtf8());
        return path;
    }
    static QString sample() {
        return "[Molden Format]\n[Atoms] AU\nH 1 1 0 0 0\n[GTO]\n1 0\nsp 1 0\n1 1 1\n"
               "[MO]\nSym= a\nEne= -0.5\nSpin= Alpha\nOccup= 1\n1 1\n"
               "Sym= b\nEne= 0.2\nSpin= Alpha\nOccup= 0\n2 1\n"
               "Sym= c\nEne= -0.4\nSpin= Beta\nOccup= 1\n3 1\n";
    }
    template <typename T> static T *widget(MainWindow &w, const char *name) {
        auto *p = w.findChild<T *>(name);
        Q_ASSERT(p);
        return p;
    }
  private slots:
    void initTestCase() {
        QVERIFY(files.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    }
    void analyticOrbitalsAndNormalization() {
        const Vec point = Vec(.3, .5, .7);
        const double exponential = std::exp(-point.squaredNorm());
        for (int l = 0; l <= 4; ++l) {
            QMap<QString, QStringList> blocks{{"GTO", {"1 0", QString("%1 1 0").arg("spdfg"[l]), "1 1"}}};
            double df = 1;
            for (int n = 2 * l - 1; n > 1; n -= 2)
                df *= n;
            const double radial = std::pow(2 / std::numbers::pi, .75) * std::sqrt(std::pow(4., l) / df);
            for (bool spherical : {false, true}) {
                if (spherical) {
                    blocks["5D"] = {};
                    blocks["9G"] = {};
                }
                const int n = spherical && l >= 2 ? 2 * l + 1 : (l + 1) * (l + 2) / 2;
                QVector<QVector<double>> coefficients;
                for (int i = 0; i < n; ++i) {
                    QVector<double> c(n);
                    c[i] = 1;
                    coefficients << c;
                }
                const auto w = moldenWavefunction(blocks, hydrogen(), coefficients);
                QCOMPARE(w.normalization, "standard");
                double sum = 0;
                for (int i = 0; i < n; ++i) {
                    const double v = w.value(i, point * BohrToAngstrom);
                    sum += v * v;
                }
                if (spherical && l >= 2)
                    QVERIFY(std::abs(sum - radial * radial * exponential * exponential *
                                               std::pow(point.squaredNorm(), l)) < 1e-12);
                double polynomial = std::pow(point.x(), l);
                if (spherical && l == 2)
                    polynomial = (3 * point.z() * point.z() - point.squaredNorm()) / 2;
                if (spherical && l == 3)
                    polynomial = (5 * std::pow(point.z(), 3) - 3 * point.z() * point.squaredNorm()) / 2;
                if (spherical && l == 4)
                    polynomial =
                        (35 * std::pow(point.z(), 4) - 30 * point.z() * point.z() * point.squaredNorm() +
                         3 * std::pow(point.squaredNorm(), 2)) /
                        8;
                QVERIFY(std::abs(w.value(0, point * BohrToAngstrom) - radial * exponential * polynomial) <
                        1e-12);
                auto embedded = blocks;
                embedded["GTO"][2] = "1 " + QString::number(radial, 'g', 17);
                const auto nw = moldenWavefunction(embedded, hydrogen(), coefficients);
                QCOMPARE(nw.normalization, "nwchem");
                for (int i = 0; i < n; ++i)
                    QVERIFY(std::abs(nw.value(i, point * BohrToAngstrom) -
                                     w.value(i, point * BohrToAngstrom)) < 1e-12);
            }
        }
        // Cartesian xy has a different primitive normalization from xx.
        QMap<QString, QStringList> blocks{{"GTO", {"1", "d 1", "1 1"}}};
        auto w = moldenWavefunction(blocks, hydrogen(), {{0, 0, 0, 1, 0, 0}});
        const double norm = std::pow(2 / std::numbers::pi, .75) * 4;
        QVERIFY(std::abs(w.value(0, point * BohrToAngstrom) - norm * point.x() * point.y() * exponential) <
                1e-12);
    }
    void contractedBasisAndBounds() {
        QMap<QString, QStringList> b{{"GTO", {"1 0", "s 2 1", "2 .5", ".5 .6"}}};
        const auto standard = moldenWavefunction(b, hydrogen(), {{1}}, "standard");
        auto embedded = b;
        for (int i = 0; i < 2; ++i) {
            const auto p = standard.shells[0].primitives[i];
            embedded["GTO"][i + 2] =
                QString::number(p.exponent, 'g', 17) + ' ' + QString::number(p.coefficient, 'g', 17);
        }
        const auto nw = moldenWavefunction(embedded, hydrogen(), {{1}});
        QCOMPARE(nw.normalization, "nwchem");
        for (double x : {0., .2, 1., 3.})
            QVERIFY(std::abs(nw.value(0, Vec(x, 0, 0)) - standard.value(0, Vec(x, 0, 0))) < 1e-12);
        for (const auto &bad : {QStringList{"1 0", "h 1 0", "1 1"}, QStringList{"1 0", "s 2 0", "1 1"},
                                QStringList{"1 0", "s 1 0", "-1 1"}, QStringList{"1 0", "s 1 0", "nan 1"},
                                QStringList{"2 0", "s 1 0", "1 1"}, QStringList{"1 0", "s 1 0", "1 0"}}) {
            b["GTO"] = bad;
            QVERIFY_EXCEPTION_THROWN(moldenWavefunction(b, hydrogen(), {{1}}), std::runtime_error);
        }
        b["GTO"] = {"1 0", "p 1 0", "1 1"};
        QVERIFY_EXCEPTION_THROWN(moldenWavefunction(b, hydrogen(), {{0, 0, 0, 1}}), std::runtime_error);
        b["GTO"] = {"1 0", "d 1 0", "1 1"};
        b["5D7F9G"] = {};
        QCOMPARE(moldenWavefunction(b, hydrogen(), {{1, 0, 0, 0, 0}}).shells[0].functions.size(), 5);
        b["6D"] = {};
        QVERIFY_EXCEPTION_THROWN(moldenWavefunction(b, hydrogen(), {{1}}), std::runtime_error);
        b.remove("5D7F9G");
        QVector<QVector<double>> sparse(20000, QVector<double>{1});
        b["GTO"] = {"1 0"};
        for (int i = 0; i < 100; ++i)
            b["GTO"] << "g 1 0" << "1 1";
        QVERIFY_EXCEPTION_THROWN(moldenWavefunction(b, hydrogen(), sparse), std::runtime_error);
    }
    void metadataSelectionAndTransfer() {
        const auto path = save("orbitals.molden", sample());
        const auto r = readResult(path);
        QCOMPARE(r.selectedOrbital, 0);
        QCOMPARE(r.orbitals.size(), 3);
        QVERIFY(r.orbitalError.isEmpty());
        QVERIFY(!r.surface.isEmpty());
        QCOMPARE(r.orbitalNormalization, "standard");
        QCOMPARE(frontierOrbital(r.orbitals, "Alpha", true), 1);
        QCOMPARE(frontierOrbital(r.orbitals, "Beta"), 2);
        QCOMPARE(frontierOrbital(r.orbitals, "Beta", true), -1);
        const auto p = readResult(path, .03, 0, 1);
        bool positive = false, negative = false;
        for (const auto &t : p.surface) {
            positive |= t.positive;
            negative |= !t.positive;
            QVERIFY(t.positive ? t.a.x() > 0 : t.a.x() < 0);
        }
        QVERIFY(positive && negative);
        const auto bin = files.filePath("result.bin");
        saveResultData(bin, p);
        const auto loaded = loadResultData(bin);
        QCOMPARE(loaded.selectedOrbital, 1);
        QCOMPARE(loaded.orbitalFrame, 0);
        QCOMPARE(loaded.orbitalNormalization, p.orbitalNormalization);
        QCOMPARE(loaded.surface.size(), p.surface.size());
        QVERIFY_EXCEPTION_THROWN(readResult(path, .03, 0, 100), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(readResult(path, .03, 0, 0, "auto", 1000), std::runtime_error);
        auto incomplete = sample();
        incomplete.replace("sp 1 0", "s 1 0").replace("1 1 1", "1 1");
        const auto bad = save("incomplete.molden", incomplete);
        QVERIFY(!readResult(bad).orbitalError.isEmpty());
        QVERIFY_EXCEPTION_THROWN(readResult(bad, .03, 0, 0), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(readResult(save("duplicate.molden", sample() + "3 1\n")),
                                 std::runtime_error);
    }
    void nativeSelectionAndCancellation() {
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        const auto before = w.document().project();
        const int history = w.history()->count();
        QSignalSpy done(panel, &ResultsPanel::loaded), failed(panel, &ResultsPanel::loadFailed);
        panel->openFile(save("native.molden", sample()));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(widget<QTabWidget>(w, "resultsDetails")->currentIndex(), 2);
        QCOMPARE(widget<QTableWidget>(w, "resultsOrbitals")->currentRow(), 0);
        widget<QPushButton>(w, "resultsLumo")->click();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
        QCOMPARE(panel->result().selectedOrbital, 1);
        widget<QLineEdit>(w, "resultsOrbitalIso")->setText(".05");
        widget<QSpinBox>(w, "resultsOrbitalGrid")->setValue(24);
        widget<QTableWidget>(w, "resultsOrbitals")->setCurrentCell(2, 0);
        widget<QPushButton>(w, "resultsShowOrbital")->click();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 3, 10000);
        QCOMPARE(panel->result().selectedOrbital, 2);
        QCOMPARE(panel->result().isovalue, .05);
        QCOMPARE(panel->result().grid[0], 25);
        QVERIFY(!widget<QPushButton>(w, "resultsLumo")->isEnabled());
        widget<QLineEdit>(w, "resultsOrbitalIso")->setText("nan");
        widget<QPushButton>(w, "resultsShowOrbital")->click();
        QCOMPARE(failed.count(), 1);
        QCOMPARE(panel->result().selectedOrbital, 2);
        widget<QLineEdit>(w, "resultsOrbitalIso")->setText(".03");
        widget<QPushButton>(w, "resultsShowOrbital")->click();
        panel->cancelLoad();
        QVERIFY(!panel->loading());
        QCOMPARE(panel->result().selectedOrbital, 2);
        QCOMPARE(w.document().project(), before);
        QCOMPARE(w.history()->count(), history);
    }
    void realBenzeneArtifacts() {
        const auto path = qEnvironmentVariable("MOLFORMA_MOLDEN_REAL");
        if (path.isEmpty())
            return;
        const auto r = readResult(path);
        QCOMPARE(r.orbitals.size(), 66);
        QCOMPARE(r.selectedOrbital, 20);
        QCOMPARE(frontierOrbital(r.orbitals, "Alpha", true), 21);
        QCOMPARE(r.orbitalNormalization, "nwchem");
        QVERIFY(!r.surface.isEmpty());
        const auto reference = qEnvironmentVariable("MOLFORMA_MOLDEN_CUBE");
        if (!reference.isEmpty()) {
            const auto cube = readResult(reference);
            QCOMPARE(r.grid, cube.grid);
            QVERIFY(std::abs(r.minimum - cube.minimum) < 2e-5);
            QVERIFY(std::abs(r.maximum - cube.maximum) < 2e-5);
            QCOMPARE(r.surface.size(), cube.surface.size());
            for (int i = 0; i < r.surface.size(); ++i) {
                QCOMPARE(r.surface[i].positive, cube.surface[i].positive);
                QVERIFY((r.surface[i].a - cube.surface[i].a).norm() < .001);
                QVERIFY((r.surface[i].b - cube.surface[i].b).norm() < .001);
                QVERIFY((r.surface[i].c - cube.surface[i].c).norm() < .001);
            }
        }
        MainWindow w;
        w.resize(1440, 1000);
        w.show();
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentIndex(6);
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        QSignalSpy done(panel, &ResultsPanel::loaded);
        panel->openFile(path);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 15000);
        auto *view = widget<MoleculeView>(w, "resultsView");
        view->viewFrom(2);
        view->fit();
        const auto root = qEnvironmentVariable("MOLFORMA_MOLDEN_ARTIFACTS");
        if (!root.isEmpty()) {
            QVERIFY(QDir().mkpath(root));
            QVERIFY(w.grab().save(root + "/molden-homo.png"));
        }
        widget<QPushButton>(w, "resultsLumo")->click();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 15000);
        QCOMPARE(panel->result().selectedOrbital, 21);
        QVERIFY(!panel->result().surface.isEmpty());
        if (!root.isEmpty())
            QVERIFY(w.grab().save(root + "/molden-lumo.png"));
    }
    void realAngularReferences() {
        const auto root = qEnvironmentVariable("MOLFORMA_MOLDEN_REFERENCES");
        if (root.isEmpty())
            return;
        for (const auto &name : {"spherical", "cartesian", "g_spherical"}) {
            for (int orbital : {5, 20, 40}) {
                const auto prefix = root + '/' + name;
                const auto r = readResult(prefix + ".molden", .03, 0, orbital - 1);
                const auto cube = readResult(prefix + '_' + QString::number(orbital) + ".cube");
                QCOMPARE(r.orbitalNormalization, "nwchem");
                QCOMPARE(r.selectedOrbital, orbital - 1);
                QVERIFY2(std::abs(r.minimum - cube.minimum) < 2e-5, qPrintable(prefix));
                QVERIFY2(std::abs(r.maximum - cube.maximum) < 2e-5, qPrintable(prefix));
                QCOMPARE(r.surface.size(), cube.surface.size());
                for (int i = 0; i < r.surface.size(); ++i) {
                    QCOMPARE(r.surface[i].positive, cube.surface[i].positive);
                    QVERIFY((r.surface[i].a - cube.surface[i].a).norm() < .001);
                    QVERIFY((r.surface[i].b - cube.surface[i].b).norm() < .001);
                    QVERIFY((r.surface[i].c - cube.surface[i].c).norm() < .001);
                }
            }
        }
    }
};
int main(int argc, char **argv) {
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--result-worker") {
        QCoreApplication app(argc, argv);
        return resultWorker(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    }
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    MoldenTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_molden.moc"
