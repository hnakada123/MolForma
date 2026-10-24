#include "ui/vtk_molecule_renderer.h"
#include <Eigen/Geometry>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <algorithm>
#include <array>
#include <cmath>
#include <openbabel/elements.h>
#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkFloatArray.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkGlyph3DMapper.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkSphereSource.h>
#include <vtkUnsignedCharArray.h>

namespace mf {
namespace {
struct Mesh {
    vtkNew<vtkPolyData> data;
    vtkNew<vtkPolyDataMapper> mapper;
    vtkNew<vtkActor> actor;
    Mesh() {
        mapper->SetInputData(data);
        actor->SetMapper(mapper);
    }
};
void appendColor(vtkUnsignedCharArray *colors, const QColor &color) {
    const unsigned char rgb[] = {static_cast<unsigned char>(color.red()),
                                 static_cast<unsigned char>(color.green()),
                                 static_cast<unsigned char>(color.blue())};
    colors->InsertNextTypedTuple(rgb);
}
} // namespace
struct VtkMoleculeRenderer::Scene {
    vtkNew<vtkGenericOpenGLRenderWindow> window;
    vtkNew<vtkRenderer> renderer;
    vtkNew<vtkSphereSource> sphere;
    vtkNew<vtkPolyData> atoms;
    vtkNew<vtkGlyph3DMapper> atomMapper;
    vtkNew<vtkActor> atomActor;
    vtkNew<vtkFloatArray> radii;
    std::array<Mesh, 2> surfaces;
    Mesh bonds, arrows;
    Document document;
    QHash<QString, int> atomIndices;
    QVector<int> visibleAtoms;
    QVector<QColor> colors;
    QSet<QString> bonded; // Atoms with a bond: drawn as lines only in the wireframe style.
    QString style = "ball_and_stick";
    QSet<QString> hidden;
    bool hydrogens = true, bondsDirty = true;
    double atomScale = -1;
    Mat bondRotation = Mat::Zero();
    double bondScale = -1;
    Vec low = Vec::Zero(), high = Vec::Zero();
    Vec surfaceLow = Vec::Zero(), surfaceHigh = Vec::Zero();
    bool hasSurface = false;
    quint64 surfaceBuilds = 0;

    void updateBounds() {
        low = hasSurface ? surfaceLow : Vec::Zero();
        high = hasSurface ? surfaceHigh : Vec::Zero();
        for (const auto &atom : document.atoms) {
            low = low.cwiseMin(atom.position);
            high = high.cwiseMax(atom.position);
        }
    }
};
VtkMoleculeRenderer::VtkMoleculeRenderer(QWidget *parent)
    : QVTKOpenGLNativeWidget(parent), scene(std::make_unique<Scene>()) {
    setObjectName("vtkMoleculeRenderer");
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    auto format = defaultFormat();
    format.setSamples(0);
    format.setAlphaBufferSize(8);
    setFormat(format);
    auto &s = *scene;
    s.window->SetMultiSamples(0);
    s.window->SetAlphaBitPlanes(1);
    s.window->AddRenderer(s.renderer);
    setRenderWindow(s.window);
    s.renderer->SetBackground(233. / 255, 239. / 255, 244. / 255);
    s.renderer->SetBackground2(248. / 255, 250. / 255, 252. / 255);
    s.renderer->GradientBackgroundOn();
    s.renderer->SetUseDepthPeeling(true);
    s.renderer->SetMaximumNumberOfPeels(16);
    s.renderer->SetOcclusionRatio(0.);
    s.renderer->SetUseFXAA(true);
    s.renderer->GetActiveCamera()->ParallelProjectionOn();
    s.sphere->SetRadius(1);
    s.sphere->SetThetaResolution(24);
    s.sphere->SetPhiResolution(16);
    s.radii->SetName("radius");
    s.atoms->GetPointData()->AddArray(s.radii);
    s.atomMapper->SetInputData(s.atoms);
    s.atomMapper->SetSourceConnection(s.sphere->GetOutputPort());
    s.atomMapper->SetScaleArray("radius");
    s.atomMapper->SetScaleModeToScaleByMagnitude();
    s.atomMapper->OrientOff();
    s.atomMapper->SetColorModeToDirectScalars();
    s.atomActor->SetMapper(s.atomMapper);
    s.atomActor->GetProperty()->SetAmbient(.3);
    s.atomActor->GetProperty()->SetDiffuse(.7);
    s.atomActor->GetProperty()->SetSpecular(.25);
    s.atomActor->GetProperty()->SetSpecularPower(24);
    s.renderer->AddActor(s.atomActor);
    for (int i = 0; i < 2; ++i) {
        auto *property = s.surfaces[i].actor->GetProperty();
        const QColor color(i == 0 ? "#ce5268" : "#397cce");
        property->SetColor(color.redF(), color.greenF(), color.blueF());
        property->SetAmbient(.4);
        property->SetDiffuse(.6);
        property->SetOpacity(150. / 255);
        s.renderer->AddActor(s.surfaces[i].actor);
    }
    for (auto *mesh : {&s.bonds, &s.arrows}) {
        mesh->mapper->SetScalarModeToUseCellData();
        mesh->mapper->SetColorModeToDirectScalars();
        mesh->actor->GetProperty()->LightingOff();
        s.renderer->AddActor(mesh->actor);
    }
    s.bonds.actor->GetProperty()->RenderLinesAsTubesOn();
}
VtkMoleculeRenderer::~VtkMoleculeRenderer() {
    // Release VTK resources while the Qt context and render-window adapter still exist.
    setRenderWindow(static_cast<vtkGenericOpenGLRenderWindow *>(nullptr));
}
void VtkMoleculeRenderer::setMolecule(const Document &document, const QVector<QColor> &colors, bool hydrogens,
                                      const QString &style, const QSet<QString> &hidden) {
    auto &s = *scene;
    s.document = document;
    s.hydrogens = hydrogens;
    s.colors = colors;
    s.style = style;
    s.hidden = hidden;
    s.bonded.clear();
    for (const auto &bond : document.bonds)
        s.bonded << bond.a << bond.b;
    s.bonds.actor->SetVisibility(style != "spacefill");
    s.arrows.actor->SetVisibility(style == "ball_and_stick");
    s.visibleAtoms.clear();
    s.atomIndices.clear();
    vtkNew<vtkPoints> points;
    points->SetDataTypeToDouble();
    vtkNew<vtkUnsignedCharArray> rgb;
    rgb->SetNumberOfComponents(3);
    for (int i = 0; i < document.atoms.size(); ++i) {
        const auto &atom = document.atoms[i];
        s.atomIndices.insert(atom.id, i);
        // Ghosts and hidden-hydrogen handles retain their established 2D notation.
        if (atom.ghost || (!hydrogens && atom.element == "H") || hidden.contains(atom.id))
            continue;
        s.visibleAtoms << i;
        points->InsertNextPoint(atom.position.data());
        appendColor(rgb, colors[i]);
    }
    s.atoms->SetPoints(points);
    s.atoms->GetPointData()->SetScalars(rgb);
    s.atomScale = -1;
    s.bondsDirty = true;
    s.updateBounds();
}
void VtkMoleculeRenderer::setSurface(const QVector<SurfaceTriangle> &triangles) {
    auto &s = *scene;
    std::array<vtkNew<vtkPoints>, 2> points;
    std::array<vtkNew<vtkCellArray>, 2> cells;
    std::array<vtkNew<vtkFloatArray>, 2> normals;
    for (int i = 0; i < 2; ++i) {
        points[i]->SetDataTypeToDouble();
        normals[i]->SetNumberOfComponents(3);
    }
    s.hasSurface = false;
    for (const auto &triangle : triangles) {
        Vec normal = (triangle.b - triangle.a).cross(triangle.c - triangle.a);
        const double length = normal.norm();
        if (!triangle.a.allFinite() || !triangle.b.allFinite() || !triangle.c.allFinite() ||
            !std::isfinite(length) || length <= 1e-20)
            continue;
        normal /= length;
        const int phase = triangle.positive ? 0 : 1;
        const vtkIdType first = points[phase]->GetNumberOfPoints();
        for (const auto &point : {triangle.a, triangle.b, triangle.c}) {
            points[phase]->InsertNextPoint(point.data());
            if (!s.hasSurface) {
                s.surfaceLow = s.surfaceHigh = point;
                s.hasSurface = true;
            }
            s.surfaceLow = s.surfaceLow.cwiseMin(point);
            s.surfaceHigh = s.surfaceHigh.cwiseMax(point);
        }
        const vtkIdType indices[] = {first, first + 1, first + 2};
        cells[phase]->InsertNextCell(3, indices);
        normals[phase]->InsertNextTuple3(normal.x(), normal.y(), normal.z());
    }
    for (int i = 0; i < 2; ++i) {
        s.surfaces[i].data->SetPoints(points[i]);
        s.surfaces[i].data->SetPolys(cells[i]);
        s.surfaces[i].data->GetCellData()->SetNormals(normals[i]);
    }
    ++s.surfaceBuilds;
    s.updateBounds();
}
void VtkMoleculeRenderer::setSurfaceStyle(bool positive, bool negative, int opacity) {
    for (int i = 0; i < 2; ++i) {
        scene->surfaces[i].actor->SetVisibility((i == 0 ? positive : negative) && opacity > 0);
        scene->surfaces[i].actor->GetProperty()->SetOpacity(std::clamp(opacity, 0, 255) / 255.);
    }
}
void VtkMoleculeRenderer::setCamera(const Mat &rotation, const Vec &center, double scale,
                                    const QPointF &pan) {
    auto &s = *scene;
    const Vec target = center + rotation.transpose() * Vec(-pan.x() / scale, pan.y() / scale, 0);
    const double radius = (s.low - target).cwiseAbs().cwiseMax((s.high - target).cwiseAbs()).norm() + 50;
    const Vec eye = target + rotation.transpose() * Vec(0, 0, 2 * radius);
    const Vec up = rotation.transpose() * Vec::UnitY();
    auto *camera = s.renderer->GetActiveCamera();
    camera->SetFocalPoint(target.data());
    camera->SetPosition(eye.data());
    camera->SetViewUp(up.data());
    camera->SetParallelScale(std::max(1, height()) / (2 * scale));
    camera->SetClippingRange(.01, 4 * radius);
    if (scale != s.atomScale) {
        s.radii->SetNumberOfValues(s.visibleAtoms.size());
        for (int i = 0; i < s.visibleAtoms.size(); ++i) {
            const auto &atom = s.document.atoms[s.visibleAtoms[i]];
            const bool h = atom.element == "H";
            double radius = std::clamp(scale * (h ? .12 : .20), h ? 5. : 8., h ? 11. : 20.) / scale;
            if (s.style == "stick")
                radius = std::clamp(scale * .1, 3., 7.) / scale;
            else if (s.style == "wireframe") // Isolated atoms (ions) stay visible as small dots.
                radius = s.bonded.contains(atom.id) ? 0 : std::clamp(scale * .12, 4., 8.) / scale;
            else if (s.style == "spacefill")
                try {
                    radius = OpenBabel::OBElements::GetVdwRad(element(atom.element).number);
                } catch (const std::exception &) {
                    radius = 1.7;
                }
            s.radii->SetValue(i, radius);
        }
        s.radii->Modified();
        s.atomScale = scale;
    }
    const bool halfBonds = s.style == "stick" || s.style == "wireframe";
    s.bonds.actor->GetProperty()->SetLineWidth((s.style == "stick"       ? std::clamp(scale * .2, 5., 12.)
                                                : s.style == "wireframe" ? 2.
                                                                         : 3.5) *
                                               devicePixelRatioF());
    if (!s.bondsDirty && scale == s.bondScale && rotation == s.bondRotation)
        return;
    // Only the small camera-facing bond strokes change on rotation, never the isosurface.
    vtkNew<vtkPoints> points, arrowPoints;
    points->SetDataTypeToDouble();
    arrowPoints->SetDataTypeToDouble();
    vtkNew<vtkCellArray> lines, arrowCells;
    vtkNew<vtkUnsignedCharArray> colors, arrowColors;
    colors->SetNumberOfComponents(3);
    arrowColors->SetNumberOfComponents(3);
    auto line = [&](const Vec &a, const Vec &b, const QColor &color) {
        const vtkIdType indices[] = {points->InsertNextPoint(a.data()), points->InsertNextPoint(b.data())};
        lines->InsertNextCell(2, indices);
        appendColor(colors, color);
    };
    for (const auto &bond : s.document.bonds) {
        const int ai = s.atomIndices.value(bond.a, -1), bi = s.atomIndices.value(bond.b, -1);
        if (ai < 0 || bi < 0)
            continue;
        const auto &a = s.document.atoms[ai], &b = s.document.atoms[bi];
        if ((!s.hydrogens && (a.element == "H" || b.element == "H")) || s.hidden.contains(a.id) ||
            s.hidden.contains(b.id))
            continue;
        if (halfBonds) {
            // Half bonds in the atom colors, one line per bond.
            const Vec middle = (a.position + b.position) / 2;
            line(a.position, middle, s.colors.value(ai));
            line(middle, b.position, s.colors.value(bi));
            continue;
        }
        const Vec delta = rotation * (b.position - a.position);
        const double length = std::hypot(delta.x(), delta.y());
        if (length * scale < 1)
            continue;
        const Vec normal = rotation.transpose() * Vec(-delta.y(), delta.x(), 0) / (length * scale);
        const QColor color(bond.kind == "dative"     ? "#2879bd"
                           : bond.kind == "aromatic" ? "#a36a2b"
                                                     : "#748595");
        const int count = bond.kind == "double" ? 2 : bond.kind == "triple" ? 3 : 1;
        for (int j = 0; j < count; ++j) {
            const Vec offset = normal * ((j - (count - 1) * .5) * 5);
            line(a.position + offset, b.position + offset, color);
        }
        if (bond.kind == "aromatic") {
            const int segments = std::clamp(int(length * scale / 7), 1, 256);
            for (int j = 0; j < segments; j += 2)
                line(a.position + (b.position - a.position) * (double(j) / segments) + normal * 5,
                     a.position + (b.position - a.position) * (double(j + 1) / segments) + normal * 5, color);
        }
        if (bond.kind == "dative") {
            const Vec head = a.position + (b.position - a.position) * .72;
            const Vec back =
                head - rotation.transpose() * Vec(delta.x(), delta.y(), 0) * (9 / (length * scale));
            const Vec left = back + normal * 4, right = back - normal * 4;
            const vtkIdType indices[] = {arrowPoints->InsertNextPoint(head.data()),
                                         arrowPoints->InsertNextPoint(left.data()),
                                         arrowPoints->InsertNextPoint(right.data())};
            arrowCells->InsertNextCell(3, indices);
            appendColor(arrowColors, color);
        }
    }
    s.bonds.data->SetPoints(points);
    s.bonds.data->SetLines(lines);
    s.bonds.data->GetCellData()->SetScalars(colors);
    s.arrows.data->SetPoints(arrowPoints);
    s.arrows.data->SetPolys(arrowCells);
    s.arrows.data->GetCellData()->SetScalars(arrowColors);
    s.bondsDirty = false;
    s.bondRotation = rotation;
    s.bondScale = scale;
}
bool VtkMoleculeRenderer::draw() {
    if (!isValid())
        return false;
    scene->window->Render();
    return true;
}
quint64 VtkMoleculeRenderer::surfaceBuildCount() const {
    return scene->surfaceBuilds;
}
bool VtkMoleculeRenderer::depthPeelingUsed() const {
    return scene->renderer->GetLastRenderingUsedDepthPeeling();
}
QPointF VtkMoleculeRenderer::screenPosition(const Vec &position) const {
    auto *r = scene->renderer.Get();
    r->SetWorldPoint(position.x(), position.y(), position.z(), 1);
    r->WorldToDisplay();
    const auto *p = r->GetDisplayPoint();
    const auto *size = scene->window->GetSize();
    return {p[0] * width() / std::max(1, size[0]), height() - p[1] * height() / std::max(1, size[1])};
}
} // namespace mf
