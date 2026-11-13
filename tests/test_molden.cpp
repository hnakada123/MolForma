// SPDX-License-Identifier: GPL-2.0-or-later
#include "foundation/files.h"
#include "result_support.h"
#include "results/molden_orbitals.h"
#include "ui/main_window.h"
#include "ui/results_panel.h"
#include <QtTest>
#include <QtWidgets>
using namespace mf;

class MoldenTest : public QObject, ResultFiles {
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
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settings.path()); // MolForma uses NativeFormat.
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
        // The tab's contents: the level diagram is drawn to its height, so that a change moves the levels.
        auto orbitalTab = [&] {
            geometry();
            QVector<QRect> rectangles;
            for (const auto *name : {"resultsDetails", "resultsOrbitalSource", "resultsOrbitalLevels",
                                     "resultsOrbitals", "resultsOrbitalStatus"}) {
                auto *control = widget<QWidget>(w, name);
                rectangles << QRect(control->mapTo(panel, QPoint()), control->size());
            }
            return rectangles;
        };
        for (const auto &size : {QSize(1380, 900), QSize(1000, 760)}) {
            w.resize(size);
            const int count = done.count();
            panel->openFile(path);
            QTRY_COMPARE_WITH_TIMEOUT(done.count(), count + 1, 10000);
            const auto before = geometry();
            const auto tabBefore = orbitalTab();
            auto *orbitalStatus = widget<QLabel>(w, "resultsOrbitalStatus");
            QVERIFY(status->isReadOnly());
            QVERIFY(status->isHidden()); // How the file ended is in "概要".
            // Stepping through orbitals says it under the settings: nothing in the tab changes size.
            next->click();
            QVERIFY(panel->loading());
            QVERIFY(status->isHidden());
            QVERIFY(orbitalStatus->text().startsWith("読込中: Alpha 2番"));
            QCOMPARE(geometry(), before);
            QCOMPARE(orbitalTab(), tabBefore);
            panel->cancelLoad();
            QVERIFY(status->toPlainText().contains("中止"));
            QVERIFY(orbitalStatus->text().startsWith("表示中: Alpha 1番"));
            QCOMPARE(geometry(), before);
            next->click(); // Closes the message of the cancelled load at once.
            QVERIFY(status->isHidden());
            QCOMPARE(geometry(), before);
            QCOMPARE(orbitalTab(), tabBefore);
            QTRY_COMPARE_WITH_TIMEOUT(done.count(), count + 2, 10000);
            QCOMPARE(panel->result().selectedOrbital, 1);
            QVERIFY(orbitalStatus->text().startsWith("表示中: Alpha 2番"));
            QCOMPARE(geometry(), before);
            QCOMPARE(orbitalTab(), tabBefore);
            previous->click(); // Cached orbital: no loading message or worker.
            QVERIFY(!panel->loading());
            QCOMPARE(panel->result().selectedOrbital, 0);
            QCOMPARE(geometry(), before);
            QCOMPARE(orbitalTab(), tabBefore);
            // Reading a file does use the message, for its path.
            panel->openFile(path);
            QVERIFY(panel->loading());
            QVERIFY(status->toPlainText().contains("\n完了まで前の結果を表示します。"));
            QCOMPARE(geometry(), before);
            // Under the details, beside the 3D view.
            QVERIFY(status->mapTo(panel, QPoint()).y() > before.last().top());
            QVERIFY(status->mapTo(panel, QPoint()).x() > before[0].right());
            QVERIFY(status->verticalScrollBar()->maximum() > 0); // Wrapped long path is accessible.
            QTRY_COMPARE_WITH_TIMEOUT(done.count(), count + 4, 10000);
            QCOMPARE(orbitalTab(), tabBefore);
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
