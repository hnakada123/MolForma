#include "main_window.h"
#include "molecule_view.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QMimeData>
#include <QWheelEvent>
#include <QtTest>
#ifdef MOLFORMA_HAS_VTK
#include "vtk_molecule_renderer.h"
#include <vtkGenericOpenGLRenderWindow.h>
#endif
#include <algorithm>
#include <cmath>
using namespace mf;
namespace {
Document molecule() {
    auto d = Document::fromXyz("3\nrender test\nO 0 0 0\nH 1.1 0.3 0.2\nC -1.4 0.7 -0.4\n");
    d.setBond(d.atoms[0].id, d.atoms[1].id, "single");
    d.setBond(d.atoms[0].id, d.atoms[2].id, "double");
    return d;
}
void settle() {
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}
void wheel(MoleculeView &view, int amount) {
    const QPointF position = view.rect().center();
    QWheelEvent event(position, view.mapToGlobal(position.toPoint()), {}, {0, amount}, Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&view, &event);
    settle();
}
QVector<SurfaceTriangle> sphereMesh(int rings, int sectors) {
    QVector<SurfaceTriangle> result;
    const double pi = std::acos(-1.);
    auto point = [=](int r, int s) -> Vec {
        const double latitude = pi * r / rings, longitude = 2 * pi * s / sectors;
        return Vec(2.5 * std::sin(latitude) * std::cos(longitude),
                   2.5 * std::sin(latitude) * std::sin(longitude), 2.5 * std::cos(latitude));
    };
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < sectors; ++s) {
            const auto a = point(r, s), b = point(r + 1, s), c = point(r + 1, s + 1), d = point(r, s + 1);
            if (r != rings - 1)
                result << SurfaceTriangle{a, b, c, s < sectors / 2};
            if (r != 0)
                result << SurfaceTriangle{a, c, d, s < sectors / 2};
        }
    return result;
}
} // namespace
class RenderingTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, "/tmp/molforma-test-settings");
    }
    void backendSelection() {
        MoleculeView view;
        view.show();
        QTest::qWait(30);
        if (qEnvironmentVariableIsSet("MOLFORMA_REQUIRE_VTK"))
            QCOMPARE(view.rendererName(), QString("vtk"));
        if (QApplication::platformName() == "offscreen" ||
            qEnvironmentVariable("MOLFORMA_RENDERER") == "painter")
            QCOMPARE(view.rendererName(), QString("painter"));
    }
    void editingAndCamera() {
        MainWindow window;
        auto d = molecule();
        d.atoms[0].fixed = true;
        window.loadDocument(d);
        window.show();
        QTest::qWait(30);
        auto *view = window.canvas();
        view->fit();
        settle();
        const auto original = window.document().toJson();
        const auto start = view->rect().center();
        QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(view, start + QPoint(55, 30));
        QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, start + QPoint(55, 30));
        wheel(*view, 120);
        QCOMPARE(window.document().toJson(), original);
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                          view->atomScreenPosition(d.atoms[2].id).toPoint());
        QVERIFY(view->selection().contains(d.atoms[2].id));
        window.edit("Delete", [id = d.atoms[2].id](Document &next) { next.remove({id}); }, true);
        QVERIFY(window.hasCandidate());
        settle();
        window.confirmCandidate();
        QCOMPARE(window.document().atoms.size(), 2);
        window.history()->undo();
        QCOMPARE(window.document().toJson(), original);
    }
    void surfaceWithoutAtoms() {
        MoleculeView view;
        view.resize(640, 480);
        view.setSurface(sphereMesh(16, 24));
        view.show();
        view.fit();
        QTest::qWait(40);
        auto image = view.grab().toImage();
        int colored = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                colored += std::abs(color.red() - color.blue()) > 40;
            }
        QVERIFY(colored > 1000);
    }
    void vtkProjectionAndRetainedSurface() {
#ifdef MOLFORMA_HAS_VTK
        MoleculeView view;
        view.resize(640, 480);
        const auto d = molecule();
        auto surface = sphereMesh(40, 60);
        view.setDocument(d);
        view.setSurface(surface);
        view.show();
        view.fit();
        QTest::qWait(40);
        if (view.rendererName() != "vtk") {
            QVERIFY(!qEnvironmentVariableIsSet("MOLFORMA_REQUIRE_VTK"));
            QSKIP("Native OpenGL rendering requires a display; offscreen uses QPainter.");
        }
        auto *renderer = static_cast<VtkMoleculeRenderer *>(
            view.findChild<QVTKOpenGLNativeWidget *>("vtkMoleculeRenderer"));
        QVERIFY(renderer);
        const auto builds = renderer->surfaceBuildCount();
        QVERIFY(builds > 0);
        auto checkProjection = [&] {
            settle();
            for (const auto &atom : d.atoms)
                QVERIFY(QLineF(renderer->screenPosition(atom.position), view.atomScreenPosition(atom.id))
                            .length() < .1);
        };
        checkProjection();
        for (int i = 0; i < 12; ++i) {
            view.setAngles(i * 21, i * 13, i * 9);
            checkProjection();
        }
        wheel(view, 240);
        checkProjection();
        QTest::mousePress(&view, Qt::RightButton, Qt::NoModifier, QPoint(200, 200));
        QTest::mouseMove(&view, QPoint(240, 220));
        QTest::mouseRelease(&view, Qt::RightButton, Qt::NoModifier, QPoint(240, 220));
        checkProjection();
        view.resize(803, 527);
        QTest::qWait(20);
        checkProjection();
        view.setSurfaceStyle(true, false, 100);
        view.setSelection({d.atoms[1].id});
        view.setDocument(d);
        view.setSurface(surface);
        settle();
        QCOMPARE(renderer->surfaceBuildCount(), builds);
        surface[0].a.x() += .1;
        view.setSurface(surface);
        settle();
        QCOMPARE(renderer->surfaceBuildCount(), builds + 1);
        view.setSurfaceStyle(true, true, 150);
        settle();
        QVERIFY(renderer->depthPeelingUsed());
        // Child rendering and annotation layers must not consume editor mouse events.
        const auto position = view.atomScreenPosition(d.atoms[0].id).toPoint();
        QCOMPARE(QApplication::widgetAt(view.mapToGlobal(position)), static_cast<QWidget *>(&view));
        QSignalSpy click(&view, &MoleculeView::atomClicked);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, position);
        QCOMPARE(click.size(), 1);
        const auto artifacts = qEnvironmentVariable("MOLFORMA_RENDER_ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            QVERIFY(view.grab().save(artifacts + "/vtk-surface.png"));
        }
        view.setSurface({});
        settle();
        QCOMPARE(renderer->surfaceBuildCount(), builds + 2);
#else
        QSKIP("Built without VTK.");
#endif
    }
    void rotationBenchmark() {
        if (!qEnvironmentVariableIsSet("MOLFORMA_RENDER_BENCHMARK"))
            QSKIP("Opt-in benchmark; not a timing assertion in CI.");
        MoleculeView view;
        view.resize(800, 600);
        view.setDocument(molecule());
        view.show();
        QTest::qWait(40);
        for (int rings : {51, 251, 501}) {
            const auto surface = sphereMesh(rings, 100);
            view.setSurface(surface);
            view.fit();
            QTest::qWait(40);
            QVector<double> times;
            for (int i = 0; i < 35; ++i) {
                QElapsedTimer timer;
                timer.start();
                view.setAngles(i * 7, i * 3, 0);
                settle();
#ifdef MOLFORMA_HAS_VTK
                if (auto *renderer = view.findChild<QVTKOpenGLNativeWidget *>("vtkMoleculeRenderer"))
                    renderer->renderWindow()->WaitForCompletion();
#endif
                if (i >= 5)
                    times << timer.nsecsElapsed() / 1e6;
            }
            std::sort(times.begin(), times.end());
            qInfo().noquote() << QString("renderer=%1 triangles=%2 median_ms=%3 p95_ms=%4")
                                     .arg(view.rendererName())
                                     .arg(surface.size())
                                     .arg(times[times.size() / 2], 0, 'f', 2)
                                     .arg(times[int(times.size() * .95)], 0, 'f', 2);
        }
    }
    void vtkSurfaceStyleAndCapture() {
#ifdef MOLFORMA_HAS_VTK
        MoleculeView view;
        view.resize(640, 480);
        const QVector<SurfaceTriangle> triangles = {{Vec(-2, -1, 0), Vec(-.2, -1, 0), Vec(-1.1, 1, 0), true},
                                                    { Vec(.2, -1, 0),
                                                      Vec(2, -1, 0),
                                                      Vec(1.1, 1, 0),
                                                      false }};
        view.setSurface(triangles);
        view.show();
        view.fit();
        QTest::qWait(30);
        if (view.rendererName() != "vtk") {
            QVERIFY(!qEnvironmentVariableIsSet("MOLFORMA_REQUIRE_VTK"));
            QSKIP("Native OpenGL rendering requires a display.");
        }
        auto *renderer = static_cast<VtkMoleculeRenderer *>(
            view.findChild<QVTKOpenGLNativeWidget *>("vtkMoleculeRenderer"));
        auto sample = [&](int phase) {
            // Deliberately do not process events: capture must flush pending scene changes.
            const QImage image = view.grab().toImage();
            const auto &t = triangles[phase];
            const QPointF position = renderer->screenPosition((t.a + t.b + t.c) / 3);
            return image.pixelColor((position * image.devicePixelRatio()).toPoint());
        };
        view.setSurfaceStyle(true, true, 255);
        auto red = sample(0), blue = sample(1);
        QVERIFY(red.red() > red.blue() + 50);
        QVERIFY(blue.blue() > blue.red() + 50);
        view.setSurfaceStyle(false, true, 255);
        auto hidden = sample(0);
        QVERIFY(std::abs(hidden.red() - hidden.blue()) < 30);
        QVERIFY(sample(1).blue() > sample(1).red() + 50);
        view.setSurfaceStyle(true, false, 255);
        hidden = sample(1);
        QVERIFY(std::abs(hidden.red() - hidden.blue()) < 30);
        view.setSurfaceStyle(true, true, 0);
        hidden = sample(0);
        QVERIFY(std::abs(hidden.red() - hidden.blue()) < 30);
#else
        QSKIP("Built without VTK.");
#endif
    }
};
int main(int argc, char **argv) {
    MoleculeView::configureDefaultSurfaceFormat();
    QApplication app(argc, argv);
    RenderingTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_rendering.moc"
