// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/coordination.h"
#include "editing/peptide.h"
#include "foundation/files.h"
#include "ui/figure_export.h"
#include "ui/main_window.h"
#include "ui/molecule_style.h"
#include "ui/molecule_view.h"
#include "ui/result_charts.h"
#include "ui/theme.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImageReader>
#include <QMimeData>
#include <QWheelEvent>
#include <QtTest>
#ifdef MOLFORMA_HAS_VTK
#include "ui/vtk_molecule_renderer.h"
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
    QTemporaryDir settingsHome;
  private slots:
    void paletteColors() {
        // Fixed colors are adjusted to the palette, so dark themes stay readable.
        QPalette light, dark;
        light.setColor(QPalette::Window, QColor("#f2f2f2"));
        dark.setColor(QPalette::Window, QColor("#202020"));
        QVERIFY(!darkPalette(light) && darkPalette(dark));
        QVERIFY(errorColor(dark).lightness() > errorColor(light).lightness());
        QVERIFY(accentColor(dark, "#487fc4", "#7fb0ff") != accentColor(light, "#487fc4", "#7fb0ff"));
    }
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsHome.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settingsHome.path()); // MolForma uses NativeFormat.
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
    // The backbone style draws each chain through its Cα atoms, broken where residues lie apart; chain colors
    // paint the polymer residues by chain and leave the other atoms in element colors.
    void backboneAndChainColors() {
        Document d, second;
        buildPeptide(d, {"GLY", "ALA", "GLY"}, "charged", "charged");
        buildPeptide(second, {"GLY", "GLY"}, "charged", "charged");
        for (auto &a : second.atoms)
            a.position += Vec(0, 20, 0);
        d.atoms += second.atoms;
        d.bonds += second.bonds;
        d.residues += second.residues;
        for (auto it = second.chains.begin(); it != second.chains.end(); ++it)
            d.chains[it.key()] = it.value();
        auto water = Document::fromXyz("3\n\nO 0 -20 0\nH .76 -20 .59\nH -.76 -20 .59\n");
        d.atoms += water.atoms;
        d.bonds += water.bonds;
        MoleculeView view;
        view.resize(400, 300);
        view.setDocument(d);
        view.setStyle("backbone");
        QCOMPARE(view.style(), QString("backbone"));
        QCOMPARE(view.traceSegments().size(), 3); // Two in the first chain, one in the second.
        QVERIFY(!view.grab().isNull());
        auto apart = d;
        for (const auto &id : apart.residues[2].atoms)
            apart.atom(id).position += Vec(10, 0, 0);
        view.setDocument(apart);
        QCOMPARE(view.traceSegments().size(), 2);
        view.setStyle("ball_and_stick");
        QVERIFY(view.traceSegments().isEmpty());
        view.setDocument(d);
        view.setColorMode("chain");
        const auto first = d.residues[0].atoms["CA"], other = second.residues[0].atoms["CA"];
        QVERIFY(view.atomColor(first) != view.atomColor(other));
        QCOMPARE(view.atomColor(d.residues[1].atoms["CB"]), view.atomColor(first));
        QCOMPARE(view.atomColor(water.atoms[0].id), toColor(element("O").color));
        view.setUniformColor(QColor("#c8323c"));
        view.setColorMode("uniform"); // Every atom, the water too.
        QCOMPARE(view.atomColor(first), QColor("#c8323c"));
        QCOMPARE(view.atomColor(water.atoms[0].id), QColor("#c8323c"));
        view.setUniformColor(QColor("#3f6fb5"));
        QCOMPARE(view.atomColor(water.atoms[1].id), QColor("#3f6fb5"));
        view.setColorMode("element");
        QCOMPARE(view.atomColor(first), toColor(element("C").color));
    }
    // What is drawn, and how large, is one rule for both renderers (molecule_style.h).
    void styleOfBothRenderers() {
        QCOMPARE(atomRadius("ball_and_stick", AtomSize::Heavy, 65, 1.7), 13.);
        QCOMPARE(atomRadius("ball_and_stick", AtomSize::Hydrogen, 65, 1.1), 7.8);
        QCOMPARE(atomRadius("ball_and_stick", AtomSize::Heavy, 600, 1.7), 20.); // Between two limits.
        QCOMPARE(atomRadius("stick", AtomSize::Hydrogen, 65, 1.1), 6.5);
        QCOMPARE(atomRadius("backbone", AtomSize::TraceNode, 65, 1.7), 6.5);
        QCOMPARE(atomRadius("spacefill", AtomSize::Heavy, 10, 1.7), 17.);
        // A wireframe shows its atoms by their bonds; an atom without one (an ion) is a small sphere.
        QCOMPARE(atomRadius("wireframe", AtomSize::Heavy, 65, 1.7), 0.);
        QCOMPARE(atomRadius("wireframe", AtomSize::Unbonded, 65, 1.7), 7.8);
        QCOMPARE(bondWidth("stick", 65), 12.);
        QCOMPARE(bondWidth("wireframe", 65), 2.);
        QCOMPARE(bondWidth("ball_and_stick", 65), 3.5);
        QCOMPARE(traceWidth(20), 7.);
        auto d = Document::fromXyz("4\nwater and an ion\nO 0 0 0\nH 0.76 0.59 0\nH -0.76 0.59 0\nNa 5 0 0\n");
        d.atoms[1].ghost = true;
        QVERIFY(atomSize("wireframe", d.atoms[3], false, false) == AtomSize::Unbonded);
        QVERIFY(atomSize("wireframe", d.atoms[0], true, false) == AtomSize::Heavy);
        QVERIFY(atomSize("ball_and_stick", d.atoms[3], false, false) == AtomSize::Heavy);
        QVERIFY(atomSize("backbone", d.atoms[0], true, true) == AtomSize::TraceNode);
        QVERIFY(atomSize("stick", d.atoms[2], true, false) == AtomSize::Hydrogen);
        QVERIFY(atomSphere(d.atoms[0], false) && atomSphere(d.atoms[2], true));
        QVERIFY(!atomSphere(d.atoms[1], true) && !atomSphere(d.atoms[2], false));
        QVERIFY(bondShown(d.atoms[0], d.atoms[2], true) && !bondShown(d.atoms[0], d.atoms[2], false));
        QVERIFY(bondShown(d.atoms[0], d.atoms[3], false));
        // The QPainter view draws the ion of a wireframe as the scene of VTK does, larger than the two
        // pixels around which the marks of an atom without a sphere go.
        d.atoms[1].ghost = false;
        MoleculeView view(RendererChoice::Painter);
        view.resize(400, 300);
        view.setLabelMode("none");
        view.setDocument(d);
        view.setStyle("wireframe");
        view.fit();
        const auto image = view.grab().toImage();
        const auto ion = view.atomScreenPosition(d.atoms[3].id).toPoint(),
                   oxygen = view.atomScreenPosition(d.atoms[0].id).toPoint();
        const auto background = [&](const QPoint &p) { return image.pixelColor(2, p.y()); };
        QVERIFY(image.pixelColor(ion + QPoint(3, 0)) != background(ion));
        QCOMPARE(image.pixelColor(ion + QPoint(14, 0)), background(ion));
        QCOMPARE(image.pixelColor(oxygen + QPoint(0, -6)), background(oxygen + QPoint(0, -6)));
    }
    // The handles beside a molecule are where the view shows them now: they need no painting first.
    void handlesWithoutPainting() {
        Document d;
        const auto metal = addCenter(d, "Pt", "square_planar", Vec::Zero(), 2.0, 2);
        MoleculeView view(RendererChoice::Painter);
        view.resize(400, 300);
        view.setDocument(d);
        view.fit();
        auto handles = view.markerList();
        QCOMPARE(handles.size(), 4);
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(handles[i].owner, metal);
            QCOMPARE(handles[i].site, i);
            QVERIFY(view.rect().contains(handles[i].point.toPoint()));
        }
        QSignalSpy clicked(&view, &MoleculeView::siteClicked);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, handles[2].point.toPoint());
        QCOMPARE(clicked.size(), 1);
        QCOMPARE(clicked[0][1].toInt(), 2);
        // They follow the camera, and a figure neither shows nor changes them.
        view.setAngles(40, 25, 0);
        const auto turned = view.markerList();
        QVERIFY(QLineF(turned[0].point, handles[0].point).length() > 1);
        QVERIFY(!view.renderImage(1, ImageBackground::White).isNull());
        QCOMPARE(view.markerList().size(), 4);
        QCOMPARE(view.markerList()[0].point, turned[0].point);
        // The termini of residues are handles only while an amino acid is placed.
        Document peptide;
        buildPeptide(peptide, {"GLY", "ALA"}, "neutral", "neutral");
        view.setDocument(peptide);
        QVERIFY(view.markerList().isEmpty());
        view.setAminoHandles(true);
        handles = view.markerList();
        QCOMPARE(handles.size(), 2);
        QVERIFY(handles[0].site < 0 && handles[0].n && !handles[1].n);
    }
    void fitsLargeStructures() {
        // As large as a spike protein of a virus (160 Å) and larger: fitting shows every atom.
        for (const double size : {160., 400., 1000.}) {
            auto d = Document::fromXyz(QString("3\n\nC 0 0 0\nC 0 0 %1\nC 0 3 %1\n").arg(size));
            MoleculeView view;
            view.resize(400, 300);
            view.setDocument(d);
            view.fit();
            for (const auto &atom : d.atoms)
                QVERIFY2(QRectF(0, 0, 400, 300).contains(view.atomScreenPosition(atom.id)),
                         qPrintable(QString::number(size)));
        }
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
    void mappedSurfaceColors() {
        MoleculeView view;
        view.resize(640, 480);
        const auto surface = sphereMesh(16, 24);
        view.setSurface(surface, QVector<QColor>(surface.size(), QColor("#23bc63")));
        view.show();
        view.fit();
        settle();
        const auto image = view.grab().toImage();
        int green = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const auto c = image.pixelColor(x, y);
                green += c.green() > c.red() + 25 && c.green() > c.blue() + 20;
            }
        QVERIFY(green > 1000);
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
    void figureImages() {
        // Pictures of the view at a larger size and on the chosen background, without the marks of
        // editing (the selection ring). With a display this is the VTK path, rendered in tiles.
        MoleculeView view;
        view.resize(320, 240);
        const auto d = molecule();
        view.setDocument(d);
        view.setSelection({d.atoms[0].id});
        view.show();
        view.fit();
        QTest::qWait(40);
        // The view's pixels on the screen, times the factor.
        const QSize device = (QSizeF(view.size()) * view.devicePixelRatioF()).toSize();
        const auto screen = view.renderImage(2, ImageBackground::Screen);
        QCOMPARE(screen.size(), device * 2);
        QCOMPARE(screen.pixelColor(3, 3).alpha(), 255);
        QVERIFY(screen.pixelColor(3, 3) != QColor(Qt::white));
        const auto white = view.renderImage(1, ImageBackground::White);
        QCOMPARE(white.size(), device);
        const auto corner = white.pixelColor(3, 3);
        QVERIFY(corner.red() > 250 && corner.green() > 250 && corner.blue() > 250);
        const auto clear = view.renderImage(3, ImageBackground::Transparent);
        QCOMPARE(clear.size(), device * 3);
        QCOMPARE(clear.pixelColor(3, 3).alpha(), 0);
        const auto oxygen = view.atomScreenPosition(d.atoms[0].id) * 3 * view.devicePixelRatioF();
        QVERIFY(clear.pixelColor(oxygen.toPoint()).alpha() > 200);
        // The selected atom looks the same as an unselected one.
        view.setSelection({});
        const auto unselected = view.renderImage(1, ImageBackground::White);
        int differing = 0;
        for (int y = 0; y < white.height(); ++y)
            for (int x = 0; x < white.width(); ++x)
                differing += white.pixel(x, y) != unselected.pixel(x, y);
        QVERIFY2(differing < white.width() * white.height() / 1000, qPrintable(QString::number(differing)));
    }
    void animationsAndFigures() {
        QTemporaryDir dir;
        // An animated GIF of three frames of more than 255 colors, the last with a transparent corner;
        // Qt's reader gets the colors back within the quantization.
        const auto picture = [](int k) {
            QImage image(64, 48, QImage::Format_ARGB32);
            for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 64; ++x)
                    image.setPixel(x, y, qRgba(x * 4, y * 5, k * 80, k == 2 && x < 8 && y < 8 ? 0 : 255));
            return image;
        };
        const auto gifPath = dir.filePath("frames.gif");
        GifWriter gif;
        QVERIFY(gif.open(gifPath, 10));
        for (int k = 0; k < 3; ++k)
            QVERIFY(gif.add(picture(k)));
        QVERIFY(gif.close());
        const auto bytes = readFile(gifPath);
        QVERIFY(bytes.startsWith("GIF89a"));
        QVERIFY(bytes.contains("NETSCAPE2.0"));
        QVERIFY(bytes.endsWith(';'));
        QImageReader reader(gifPath);
        if (QImageReader::supportedImageFormats().contains("gif")) {
            QCOMPARE(reader.imageCount(), 3);
            for (int k = 0; k < 3; ++k) {
                const auto frame = reader.read().convertToFormat(QImage::Format_ARGB32);
                QCOMPARE(frame.size(), QSize(64, 48));
                for (const auto &[x, y] : {std::pair{10, 10}, {40, 20}, {63, 47}, {20, 40}}) {
                    const QColor got = frame.pixelColor(x, y), want = picture(k).pixelColor(x, y);
                    QVERIFY2(std::abs(got.red() - want.red()) <= 16 &&
                                 std::abs(got.green() - want.green()) <= 16 &&
                                 std::abs(got.blue() - want.blue()) <= 16,
                             qPrintable(QString("%1 %2 %3").arg(k).arg(x).arg(y)));
                }
                if (k == 2)
                    QCOMPARE(frame.pixelColor(2, 2).alpha(), 0);
            }
        }
        // Numbered PNG files, forward and back.
        AnimationOptions options;
        options.format = "png";
        options.bounce = true;
        QVERIFY(writeAnimation(nullptr, dir.filePath("frames"), "path", options, 4, picture));
        QCOMPARE(QDir(dir.filePath("frames")).entryList({"*.png"}).size(), 6);
        QCOMPARE(QImage(dir.filePath("frames/path_0005.png")).pixel(30, 30), picture(2).pixel(30, 30));
        // MP4 through ffmpeg, where it is installed.
        if (!ffmpegExecutable().isEmpty()) {
            options.format = "mp4";
            options.bounce = false;
            QVERIFY(writeAnimation(nullptr, dir.filePath("frames.mp4"), "path", options, 3, picture));
            QCOMPARE(readFile(dir.filePath("frames.mp4")).mid(4, 4), QByteArray("ftyp"));
        }
        // A chart as a figure: PNG at twice its size, PDF and SVG as vectors.
        ChartView chart;
        chart.resize(400, 240);
        chart.setChart("test", "x", "y", {{"curve", {{0, 0}, {1, 2}, {2, 1}}, ChartTone::Accent}});
        saveFigure(&chart, dir.filePath("chart.png"));
        QCOMPARE(QImage(dir.filePath("chart.png")).size(), QSize(800, 480));
        saveFigure(&chart, dir.filePath("chart.pdf"));
        QVERIFY(readFile(dir.filePath("chart.pdf")).startsWith("%PDF"));
#ifdef MOLFORMA_HAS_QTSVG
        saveFigure(&chart, dir.filePath("chart.svg"));
        QVERIFY(readFile(dir.filePath("chart.svg")).contains("<svg"));
        QVERIFY(figureFilter().contains("*.svg"));
#endif
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, saveFigure(&chart, dir.filePath("chart.bmp")));
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
