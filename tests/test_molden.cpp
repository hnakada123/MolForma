// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/coordinates.h"
#include "results/molden_orbitals.h"
#include "ui/main_window.h"
#include "ui/results_panel.h"
#include <Eigen/Geometry>
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
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settings.path()); // MolForma uses NativeFormat.
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
        // Valid standard coefficients need not describe a normalized contraction. Do not
        // silently guess when the file cannot distinguish standard from embedded factors.
        const auto standard = moldenWavefunction(b, hydrogen(), {{1}}, "standard");
        const QMap<QString, QStringList> unnormalized{{"GTO", {"1 0", "s 2 1", "2 1", ".5 1.2"}}};
        QVERIFY_EXCEPTION_THROWN(moldenWavefunction(unnormalized, hydrogen(), {{1}}), std::runtime_error);
        const auto explicitStandard = moldenWavefunction(unnormalized, hydrogen(), {{1}}, "standard");
        QVERIFY(std::abs(explicitStandard.value(0, Vec(.2, .3, .4)) - standard.value(0, Vec(.2, .3, .4))) <
                1e-12);
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
    void separableGridMatchesPointEvaluation() {
        auto atoms = Document::fromXyz("2\noff-center\nH .23 -.41 .37\nH -.71 .53 -.29\n");
        for (bool spherical : {false, true}) {
            QMap<QString, QStringList> blocks{{"GTO",
                                               {"1 0", "sp 2 0", ".7 .3 -.4", "2.1 .8 .9", "2 0", "d 2 0",
                                                ".9 .6", "3.2 -.2", "f 1 0", "1.3 1", "g 1 0", "1.7 1"}}};
            if (spherical)
                blocks["5D7F9G"] = {};
            const int basis = spherical ? 25 : 35;
            QVector<double> coefficients(basis);
            for (int i = 0; i < basis; ++i)
                coefficients[i] = std::sin(i + .4);
            const auto w =
                moldenWavefunction(blocks, atoms, {coefficients, QVector<double>(basis)}, "standard");
            const Vec origin(-1.21, -.83, -.47), step(.31, .27, .19);
            const std::array<int, 3> grid{7, 6, 5};
            const auto values = moldenOrbitalGrid(w, 0, origin, step, grid);
            QCOMPARE(values.size(), 7 * 6 * 5);
            int index = 0;
            for (int x = 0; x < grid[0]; ++x)
                for (int y = 0; y < grid[1]; ++y)
                    for (int z = 0; z < grid[2]; ++z) {
                        const auto expected = w.value(0, origin + step.cwiseProduct(Vec(x, y, z)));
                        QVERIFY(std::abs(values[index++] - expected) < 1e-12);
                    }
            for (double v : moldenOrbitalGrid(w, 1, origin, step, grid))
                QCOMPARE(v, 0.);
            const auto edge = moldenOrbitalGrid(w, 0, origin, step, {101, 2, 3});
            QVERIFY(std::abs(edge.last() - w.value(0, origin + step.cwiseProduct(Vec(100, 1, 2)))) < 1e-12);
            QVERIFY_EXCEPTION_THROWN(moldenOrbitalGrid(w, 2, origin, step, grid), std::runtime_error);
            QVERIFY_EXCEPTION_THROWN(moldenOrbitalGrid(w, 0, origin, step, {102, 2, 3}), std::runtime_error);
        }
    }
    void isosurfaceNormals() {
        // exp(-r²) around the origin (Bohr): a sphere whose normals point straight outward.
        QString text = "gaussian\nlobe\n    1 -2.0 -2.0 -2.0\n";
        for (int axis = 0; axis < 3; ++axis)
            text += QString("   21 %1 %2 %3\n")
                        .arg(axis == 0 ? .2 : 0.)
                        .arg(axis == 1 ? .2 : 0.)
                        .arg(axis == 2 ? .2 : 0.);
        text += "    1 0.0 0.0 0.0 0.0\n";
        for (int x = 0; x < 21; ++x)
            for (int y = 0; y < 21; ++y)
                for (int z = 0; z < 21; ++z) {
                    const Vec p = Vec(x, y, z) * .2 - Vec::Constant(2);
                    text += QString::number(std::exp(-p.squaredNorm()), 'e', 8) + (z % 6 == 5 ? "\n" : " ");
                }
        const auto r = readResult(save("lobe.cube", text + "\n"));
        QVERIFY(!r.surface.isEmpty());
        // A closed surface: triangles share exact vertices, every edge belongs to two of them, and
        // the winding agrees with the outward normals.
        QMap<std::pair<std::array<double, 3>, std::array<double, 3>>, int> edges;
        const auto key = [](const Vec &v) { return std::array<double, 3>{v.x(), v.y(), v.z()}; };
        for (const auto &t : r.surface) {
            QVERIFY(t.positive);
            for (const auto &[point, normal] : {std::pair{t.a, t.na}, {t.b, t.nb}, {t.c, t.nc}}) {
                QVERIFY(std::abs(normal.norm() - 1) < 1e-9);
                QVERIFY(normal.dot(point.normalized()) > .98);
            }
            QVERIFY((t.b - t.a).cross(t.c - t.a).dot(t.na + t.nb + t.nc) > 0);
            for (const auto &[p, q] : {std::pair{t.a, t.b}, {t.b, t.c}, {t.c, t.a}})
                ++edges[std::minmax(key(p), key(q))];
        }
        QVERIFY(std::all_of(edges.cbegin(), edges.cend(), [](int n) { return n == 2; }));
        // The worker hands the normals over with the triangles.
        const auto bin = files.filePath("lobe.bin");
        saveResultData(bin, r);
        const auto loaded = loadResultData(bin);
        QCOMPARE(loaded.surface.size(), r.surface.size());
        QVERIFY((loaded.surface.first().nb - r.surface.first().nb).norm() < 1e-6);
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
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentWidget(panel);
        w.show();
        const auto before = w.document().project();
        const int history = w.history()->count();
        QSignalSpy done(panel, &ResultsPanel::loaded), failed(panel, &ResultsPanel::loadFailed);
        panel->openFile(save("native.molden", sample()));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(widget<QTabWidget>(w, "resultsDetails")->currentIndex(), 2);
        auto *table = widget<QTableWidget>(w, "resultsOrbitals");
        QCOMPARE(table->currentRow(), 0);
        QVERIFY(table->verticalHeader()->isHidden());
        QVERIFY(!w.findChild<QPushButton *>("resultsShowOrbital"));
        QCOMPARE(table->item(0, 0)->text(), QString("1"));
        QCOMPARE(table->item(1, 0)->text(), QString("2"));
        QCOMPARE(table->item(2, 0)->text(), QString("1")); // Beta numbering is preserved.
        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                          table->visualItemRect(table->item(1, 4)).center());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 10000);
        QCOMPARE(panel->result().selectedOrbital, 1);
        QVERIFY(!panel->loading());
        QTest::keyClick(table, Qt::Key_Up);
        QCOMPARE(done.count(), 3); // Return to the cached orbital with one key press.
        QCOMPARE(panel->result().selectedOrbital, 0);
        QTest::keyClick(table, Qt::Key_Down);
        QCOMPARE(done.count(), 4);
        QCOMPARE(panel->result().selectedOrbital, 1);
        table->setCurrentCell(1, 5); // Moving within the same row must not reload it.
        QCOMPARE(done.count(), 4);
        widget<QLineEdit>(w, "resultsOrbitalIso")->setText(".05");
        widget<QSpinBox>(w, "resultsOrbitalGrid")->setValue(24);
        table->setCurrentCell(2, 0);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 5, 10000);
        QCOMPARE(panel->result().selectedOrbital, 2);
        QCOMPARE(panel->result().isovalue, .05);
        QCOMPARE(panel->result().grid[0], 25);
        QVERIFY(!widget<QPushButton>(w, "resultsLumo")->isEnabled());
        widget<QLineEdit>(w, "resultsOrbitalIso")->setText("nan");
        emit widget<QLineEdit>(w, "resultsOrbitalIso")->editingFinished();
        QCOMPARE(failed.count(), 1);
        QCOMPARE(panel->result().selectedOrbital, 2);
        widget<QLineEdit>(w, "resultsOrbitalIso")->setText(".03");
        emit widget<QLineEdit>(w, "resultsOrbitalIso")->editingFinished();
        panel->cancelLoad();
        QVERIFY(!panel->loading());
        QCOMPARE(panel->result().selectedOrbital, 2);
        QCOMPARE(w.document().project(), before);
        QCOMPARE(w.history()->count(), history);
    }
    void navigationAndCachedSurfaces() {
        QString text = sample().section("[MO]", 0, 0) + "[MO]\n";
        for (int i = 0; i < 26; ++i)
            text += QString("Sym= %1\nEne= %2\nSpin= %3\nOccup= %4\n%5 1\n")
                        .arg(i)
                        .arg(i * .1)
                        .arg(i < 13 ? "Alpha" : "Beta")
                        .arg(i == 0 ? 1 : 0)
                        .arg(i % 4 + 1);
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        auto button = [&](const char *name) { return widget<QPushButton>(w, name); };
        QVERIFY(!button("resultsOrbitalFirst")->isEnabled());
        const auto before = w.document().project();
        const auto history = w.history()->count();
        QSignalSpy done(panel, &ResultsPanel::loaded), failed(panel, &ResultsPanel::loadFailed);
        panel->openFile(save("navigation.molden", text));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        const auto firstSurface = panel->result().surface;
        const auto firstItem = widget<QTableWidget>(w, "resultsOrbitals")->item(0, 0);
        auto navigate = [&](const char *name, int expected) {
            const int count = done.count();
            QVERIFY(button(name)->isEnabled());
            button(name)->click();
            QTRY_COMPARE_WITH_TIMEOUT(done.count(), count + 1, 10000);
            QCOMPARE(panel->result().selectedOrbital, expected);
            QCOMPARE(widget<QTableWidget>(w, "resultsOrbitals")->currentRow(), expected);
        };
        for (const auto *name : {"resultsOrbitalFirst", "resultsOrbitalBack10", "resultsOrbitalPrevious"})
            QVERIFY(!button(name)->isEnabled());
        navigate("resultsOrbitalForward10", 10);
        navigate("resultsOrbitalNext", 11);
        navigate("resultsOrbitalPrevious", 10);
        navigate("resultsOrbitalForward10", 20);
        QCOMPARE(widget<QLabel>(w, "resultsOrbitalPosition")->text(), QString("Beta 8番"));
        navigate("resultsOrbitalForward10", 25);
        for (const auto *name : {"resultsOrbitalLast", "resultsOrbitalForward10", "resultsOrbitalNext"})
            QVERIFY(!button(name)->isEnabled());
        navigate("resultsOrbitalBack10", 15);
        navigate("resultsOrbitalBack10", 5);
        navigate("resultsOrbitalBack10", 0);
        QCOMPARE(panel->result().surface.constData(), firstSurface.constData());
        QCOMPARE(widget<QTableWidget>(w, "resultsOrbitals")->item(0, 0), firstItem);
        navigate("resultsOrbitalLast", 25);
        // Already rendered orbitals are restored synchronously, with no worker or mesh copy.
        const int count = done.count();
        button("resultsOrbitalFirst")->click();
        QVERIFY(!panel->loading());
        QCOMPARE(done.count(), count + 1);
        QCOMPARE(panel->result().surface.constData(), firstSurface.constData());
        auto *table = widget<QTableWidget>(w, "resultsOrbitals");
        const int selectedCount = done.count();
        table->setCurrentCell(24, 0);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), selectedCount + 1, 10000);
        QCOMPARE(panel->result().selectedOrbital, 24);
        navigate("resultsOrbitalNext", 25);
        const int cachedCount = done.count();
        table->setCurrentCell(0, 0);
        QCOMPARE(done.count(), cachedCount + 1);
        QCOMPARE(panel->result().selectedOrbital, 0);
        // Cancel an in-flight request by selecting a cached orbital.
        table->setCurrentCell(2, 0);
        QVERIFY(panel->loading());
        table->setCurrentCell(0, 0);
        QVERIFY(!panel->loading());
        QCOMPARE(panel->result().surface.constData(), firstSurface.constData());
        QTest::qWait(100);
        QCOMPARE(panel->result().selectedOrbital, 0);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(w.document().project(), before);
        QCOMPARE(w.history()->count(), history);
        panel->openFile(save("navigation.xyz", "1\nH\nH 0 0 0\n"));
        QTRY_VERIFY_WITH_TIMEOUT(!panel->loading(), 10000);
        for (const auto *name : {"resultsOrbitalFirst", "resultsOrbitalBack10", "resultsOrbitalPrevious",
                                 "resultsOrbitalNext", "resultsOrbitalForward10", "resultsOrbitalLast"})
            QVERIFY(!button(name)->isEnabled());
    }
    void statusMessagesKeepViewerGeometry() {
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentWidget(panel);
        w.show();
        const auto path = save(QString(180, 'a') + ".molden", sample());
        auto *status = widget<QPlainTextEdit>(w, "resultsStatus");
        auto *next = widget<QPushButton>(w, "resultsOrbitalNext");
        auto *previous = widget<QPushButton>(w, "resultsOrbitalPrevious");
        QSignalSpy done(panel, &ResultsPanel::loaded), failed(panel, &ResultsPanel::loadFailed);
        auto geometry = [&] {
            // Deliver layout changes while leaving worker completion queued, so the loading
            // message is checked deterministically even when the worker finishes quickly. Again for
            // the changes they cause: the bar that turns compact when "中止" would wrap it.
            for (int pass = 0; pass < 3; ++pass)
                QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            QVector<QRect> rectangles;
            for (const auto *name : {"resultsView", "resultsHint", "resultsOrbitalPosition", "resultsFiles",
                                     "resultsOrbitalFirst", "resultsOrbitalNext"}) {
                auto *control = widget<QWidget>(w, name);
                rectangles << QRect(control->mapTo(panel, QPoint()), control->size());
            }
            // The messages come and go under the details: only the details' bottom follows them.
            auto *details = widget<QWidget>(w, "resultsDetails");
            rectangles << QRect(details->mapTo(panel, QPoint()), QSize(details->width(), 1));
            return rectangles;
        };
        for (const auto &size : {QSize(1380, 900), QSize(1000, 760)}) {
            w.resize(size);
            const int count = done.count();
            panel->openFile(path);
            QTRY_COMPARE_WITH_TIMEOUT(done.count(), count + 1, 10000);
            const auto before = geometry();
            QVERIFY(status->isReadOnly());
            QVERIFY(status->isHidden()); // How the file ended is in "概要".
            next->click();
            QVERIFY(panel->loading());
            QVERIFY(status->toPlainText().contains("\n完了まで前の結果を表示します。"));
            QCOMPARE(geometry(), before);
            // Under the details, beside the 3D view.
            QVERIFY(status->mapTo(panel, QPoint()).y() > before.last().top());
            QVERIFY(status->mapTo(panel, QPoint()).x() > before[0].right());
            QVERIFY(status->verticalScrollBar()->maximum() > 0); // Wrapped long path is accessible.
            panel->cancelLoad();
            QVERIFY(status->toPlainText().contains("中止"));
            QCOMPARE(geometry(), before);
            next->click();
            QCOMPARE(geometry(), before);
            QTRY_COMPARE_WITH_TIMEOUT(done.count(), count + 2, 10000);
            QCOMPARE(panel->result().selectedOrbital, 1);
            QCOMPARE(geometry(), before);
            previous->click(); // Cached orbital: no loading message or worker.
            QVERIFY(!panel->loading());
            QCOMPARE(panel->result().selectedOrbital, 0);
            QCOMPARE(geometry(), before);
            widget<QLineEdit>(w, "resultsOrbitalIso")->setText("nan");
            next->click();
            QVERIFY(status->toPlainText().startsWith("読込・表示エラー:"));
            QCOMPARE(geometry(), before);
        }
        QCOMPARE(failed.count(), 2);
    }
    void cacheSettingsAndSourceInvalidation() {
        MainWindow w;
        auto *panel = widget<ResultsPanel>(w, "resultsTab");
        const auto path = save("cache.molden", sample());
        QSignalSpy done(panel, &ResultsPanel::loaded), failed(panel, &ResultsPanel::loadFailed);
        panel->openFile(path);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        const auto firstSurface = panel->result().surface;
        auto *iso = widget<QLineEdit>(w, "resultsOrbitalIso");
        auto *grid = widget<QSpinBox>(w, "resultsOrbitalGrid");
        auto *norm = widget<QComboBox>(w, "resultsOrbitalNorm");
        emit iso->editingFinished();
        emit grid->editingFinished();
        QCOMPARE(done.count(), 1);
        QVERIFY(!panel->loading());
        auto recompute = [&] {
            const int count = done.count();
            emit iso->editingFinished();
            QVERIFY(panel->loading());
            QTRY_COMPARE_WITH_TIMEOUT(done.count(), count + 1, 10000);
        };
        iso->setText(".05");
        recompute();
        QCOMPARE(panel->result().isovalue, .05);
        grid->setValue(24);
        const int gridCount = done.count();
        emit grid->editingFinished();
        QVERIFY(panel->loading());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), gridCount + 1, 10000);
        QCOMPARE(panel->result().grid[0], 25);
        const int normCount = done.count();
        norm->setCurrentIndex(norm->findData("standard"));
        QVERIFY(panel->loading());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), normCount + 1, 10000);
        QCOMPARE(panel->result().normalizationRequest, QString("standard"));
        iso->setText(".03");
        grid->setValue(40);
        norm->setCurrentIndex(norm->findData("auto"));
        emit iso->editingFinished();
        QVERIFY(!panel->loading());
        QCOMPARE(panel->result().surface.constData(), firstSurface.constData());
        // Explicit reload bypasses the cache even when the file metadata is unchanged.
        widget<QPushButton>(w, "resultsReload")->click();
        QVERIFY(panel->loading());
        QTRY_VERIFY_WITH_TIMEOUT(!panel->loading(), 10000);
        QVERIFY(panel->result().surface.constData() != firstSurface.constData());
#ifndef Q_OS_WIN // Qt reports no metadata change time there: a rewrite that keeps size and mtime goes unseen.
        const auto oldTime = QFileInfo(path).lastModified();
        QTest::qWait(5);
        auto changed = sample();
        changed.replace("Ene= -0.5", "Ene= -0.6");
        save("cache.molden", changed);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(oldTime, QFileDevice::FileModificationTime));
        file.close();
        emit iso->editingFinished();
        QVERIFY(panel->loading());
        panel->cancelLoad(); // A canceled refresh must not mark old metadata as current.
        QCOMPARE(panel->result().orbitals[0].energy, -.5);
        recompute(); // Same size and mtime: metadata change time must still invalidate it.
        QCOMPARE(panel->result().orbitals[0].energy, -.6);
        QCOMPARE(widget<QTableWidget>(w, "resultsOrbitals")->item(0, 4)->text(), QString("-0.600000"));
#else
        QFile file(path);
#endif
        const double kept = panel->result().orbitals[0].energy;
        QVERIFY(file.remove());
        const int count = failed.count();
        emit iso->editingFinished();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), count + 1, 10000);
        QCOMPARE(panel->result().orbitals[0].energy, kept); // A missing file keeps the result shown.
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
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentIndex(4);
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
        const auto baseline = qEnvironmentVariable("MOLFORMA_MOLDEN_BASELINE");
        for (int pass = 0; pass < 2; ++pass)
            for (int orbital : {0, 20, 21, 40, 65}) {
                QElapsedTimer timer;
                qint64 elapsed = -1;
                const auto connection =
                    connect(panel, &ResultsPanel::loaded, this, [&] { elapsed = timer.nsecsElapsed(); });
                const int count = done.count();
                timer.start();
                widget<QTableWidget>(w, "resultsOrbitals")->setCurrentCell(orbital, 0);
                QTRY_COMPARE_WITH_TIMEOUT(done.count(), count + 1, 15000);
                disconnect(connection);
                QCOMPARE(panel->result().selectedOrbital, orbital);
                qInfo("Orbital %d, pass %d: %.3f ms to apply surface", orbital + 1, pass + 1, elapsed / 1e6);
                // A missing baseline is recorded from this run; later runs must reproduce it.
                const auto saved = baseline + QString("/before-%1.bin").arg(orbital);
                if (!baseline.isEmpty() && !QFileInfo::exists(saved)) {
                    QVERIFY(QDir().mkpath(baseline));
                    saveResultData(saved, panel->result());
                    qInfo("Recorded baseline %s", qPrintable(saved));
                } else if (!baseline.isEmpty()) {
                    const auto old = loadResultData(saved);
                    const auto &now = panel->result();
                    QCOMPARE(now.surface.size(), old.surface.size());
                    QVERIFY(std::abs(now.minimum - old.minimum) < 1e-12);
                    QVERIFY(std::abs(now.maximum - old.maximum) < 1e-12);
                    for (int i = 0; i < now.surface.size(); ++i) {
                        QCOMPARE(now.surface[i].positive, old.surface[i].positive);
                        QVERIFY((now.surface[i].a - old.surface[i].a).norm() < 1e-10);
                        QVERIFY((now.surface[i].b - old.surface[i].b).norm() < 1e-10);
                        QVERIFY((now.surface[i].c - old.surface[i].c).norm() < 1e-10);
                    }
                }
            }
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
    if (const int code = resultWorkerMain(argc, argv); code >= 0)
        return code;
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    MoldenTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_molden.moc"
