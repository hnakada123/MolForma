// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/vtk_molecule_renderer.h"
#include "ui/bond_style.h"
#include <Eigen/Geometry>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <algorithm>
#include <array>
#include <cmath>
#include <openbabel/elements.h>
#include <unordered_map>
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
    Mesh plain, bonds, arrows; // Bonds that look the same from every side, and camera-facing ones.
    Document document;
    QHash<QString, int> atomIndices;
    struct DrawnBond {
        int a, b;
        const BondStyle *style;
    };
    QVector<DrawnBond> drawnBonds; // The bonds shown, between shown atoms.
    // Per shown atom: 0 heavy, 1 hydrogen, 2 without a bond (wireframe); and the van der Waals radius.
    QVector<char> radiusKind;
    QVector<double> vdw;
    QVector<int> visibleAtoms;
    QVector<QColor> colors;
    QSet<QString> bonded; // Atoms with a bond: drawn as lines only in the wireframe style.
    QString style = "ball_and_stick";
    QSet<QString> hidden;
    bool hydrogens = true, bondsDirty = true, plainDirty = true;
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
        const QColor color = surfaceColor(i == 0);
        property->SetColor(color.redF(), color.greenF(), color.blueF());
        property->SetAmbient(.4);
        property->SetDiffuse(.6);
        property->SetOpacity(150. / 255);
        s.renderer->AddActor(s.surfaces[i].actor);
    }
    for (auto *mesh : {&s.plain, &s.bonds, &s.arrows}) {
        mesh->mapper->SetScalarModeToUseCellData();
        mesh->mapper->SetColorModeToDirectScalars();
        mesh->actor->GetProperty()->LightingOff();
        s.renderer->AddActor(mesh->actor);
    }
    s.plain.actor->GetProperty()->RenderLinesAsTubesOn();
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
    s.plain.actor->SetVisibility(style != "spacefill");
    s.bonds.actor->SetVisibility(style != "spacefill");
    s.arrows.actor->SetVisibility(style == "ball_and_stick");
    s.visibleAtoms.clear();
    s.atomIndices.clear();
    s.radiusKind.clear();
    s.vdw.clear();
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
        s.radiusKind << (style == "wireframe" && !s.bonded.contains(atom.id) ? 2
                         : atom.element == "H"                               ? 1
                                                                             : 0);
        double radius = 1.7;
        if (style == "spacefill")
            try {
                radius = OpenBabel::OBElements::GetVdwRad(element(atom.element).number);
            } catch (const std::exception &) {
            }
        s.vdw << radius;
    }
    s.drawnBonds.clear();
    for (const auto &bond : document.bonds) {
        const int ai = s.atomIndices.value(bond.a, -1), bi = s.atomIndices.value(bond.b, -1);
        if (ai < 0 || bi < 0)
            continue;
        const auto &a = document.atoms[ai], &b = document.atoms[bi];
        if ((!hydrogens && (a.element == "H" || b.element == "H")) || hidden.contains(a.id) ||
            hidden.contains(b.id))
            continue;
        s.drawnBonds.append({ai, bi, &bondStyle(bond.kind)});
    }
    s.atoms->SetPoints(points);
    s.atoms->GetPointData()->SetScalars(rgb);
    s.atomScale = -1;
    s.bondsDirty = s.plainDirty = true;
    s.updateBounds();
}
void VtkMoleculeRenderer::setPositions(const Document &document) {
    auto &s = *scene;
    s.document = document;
    auto *points = s.atoms->GetPoints();
    for (int k = 0; k < s.visibleAtoms.size(); ++k)
        points->SetPoint(k, document.atoms[s.visibleAtoms[k]].position.data());
    points->Modified();
    s.bondsDirty = s.plainDirty = true;
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
    // Triangles from the mesher share exact corner positions: give each position one point.
    struct Hash {
        size_t operator()(const std::array<double, 3> &p) const {
            const std::hash<double> h;
            return h(p[0]) ^ (h(p[1]) * 0x9e3779b97f4a7c15ULL) ^ (h(p[2]) * 0xc2b2ae3d27d4eb4fULL);
        }
    };
    std::array<std::unordered_map<std::array<double, 3>, vtkIdType, Hash>, 2> shared;
    for (const auto &triangle : triangles) {
        Vec normal = (triangle.b - triangle.a).cross(triangle.c - triangle.a);
        const double length = normal.norm();
        if (!triangle.a.allFinite() || !triangle.b.allFinite() || !triangle.c.allFinite() ||
            !std::isfinite(length) || length <= 1e-20)
            continue;
        normal /= length;
        const int phase = triangle.positive ? 0 : 1;
        // Normals of the field at the corners shade the surface smoothly across the triangles; a
        // corner without one takes this triangle's normal and a point of its own.
        const std::array<std::pair<Vec, Vec>, 3> corners{
            std::pair{triangle.a, triangle.na}, {triangle.b, triangle.nb}, {triangle.c, triangle.nc}};
        vtkIdType indices[3];
        for (int i = 0; i < 3; ++i) {
            const auto &[point, smooth] = corners[i];
            const std::array<double, 3> key{point.x(), point.y(), point.z()};
            if (!smooth.isZero())
                if (const auto it = shared[phase].find(key); it != shared[phase].end()) {
                    indices[i] = it->second;
                    continue;
                }
            indices[i] = points[phase]->InsertNextPoint(point.data());
            const Vec n = smooth.isZero() ? normal : smooth;
            normals[phase]->InsertNextTuple3(n.x(), n.y(), n.z());
            if (!smooth.isZero())
                shared[phase].emplace(key, indices[i]);
            if (!s.hasSurface) {
                s.surfaceLow = s.surfaceHigh = point;
                s.hasSurface = true;
            }
            s.surfaceLow = s.surfaceLow.cwiseMin(point);
            s.surfaceHigh = s.surfaceHigh.cwiseMax(point);
        }
        cells[phase]->InsertNextCell(3, indices);
    }
    for (int i = 0; i < 2; ++i) {
        s.surfaces[i].data->SetPoints(points[i]);
        s.surfaces[i].data->SetPolys(cells[i]);
        s.surfaces[i].data->GetPointData()->SetNormals(normals[i]);
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
    // Large structures draw coarser spheres while they are small on the screen (edges of about 6 px):
    // most of a frame is their triangles. Up to 2000 atoms, and zoomed in, they keep 24 × 16.
    int segments = 24;
    if (s.visibleAtoms.size() > 2000) {
        const double pixels = s.style == "spacefill"   ? 2. * scale
                              : s.style == "stick"     ? std::clamp(scale * .1, 3., 7.)
                              : s.style == "wireframe" ? std::clamp(scale * .12, 4., 8.)
                                                       : std::clamp(scale * .20, 8., 20.);
        segments = std::clamp(int(std::ceil(2 * M_PI * pixels / 6)), 12, 24);
    }
    if (s.sphere->GetThetaResolution() != segments) {
        s.sphere->SetThetaResolution(segments);
        s.sphere->SetPhiResolution(segments * 2 / 3);
    }
    // Spacefill radii are fixed in Å; the others are sizes on the screen, by kind of atom.
    if (scale != s.atomScale && !(s.style == "spacefill" && s.atomScale > 0)) {
        double radius[3];
        for (int kind = 0; kind < 3; ++kind) {
            const bool h = kind == 1;
            radius[kind] = std::clamp(scale * (h ? .12 : .20), h ? 5. : 8., h ? 11. : 20.) / scale;
            if (s.style == "stick")
                radius[kind] = std::clamp(scale * .1, 3., 7.) / scale;
            else if (s.style == "wireframe") // Isolated atoms (ions) stay visible as small dots.
                radius[kind] = kind == 2 ? std::clamp(scale * .12, 4., 8.) / scale : 0;
        }
        s.radii->SetNumberOfValues(s.visibleAtoms.size());
        for (int i = 0; i < s.visibleAtoms.size(); ++i)
            s.radii->SetValue(i, s.style == "spacefill" ? s.vdw[i] : radius[int(s.radiusKind[i])]);
        s.radii->Modified();
        s.atomScale = scale;
    }
    const bool halves = halfBonds(s.style);
    s.bonds.actor->GetProperty()->SetLineWidth((s.style == "stick"       ? std::clamp(scale * .2, 5., 12.)
                                                : s.style == "wireframe" ? 2.
                                                                         : 3.5) *
                                               devicePixelRatioF());
    s.plain.actor->GetProperty()->SetLineWidth(s.bonds.actor->GetProperty()->GetLineWidth());
    struct Lines {
        vtkNew<vtkPoints> points;
        vtkNew<vtkCellArray> cells;
        vtkNew<vtkUnsignedCharArray> colors;
        Lines() {
            points->SetDataTypeToDouble();
            colors->SetNumberOfComponents(3);
        }
        void add(const Vec &a, const Vec &b, const QColor &color) {
            const vtkIdType indices[] = {points->InsertNextPoint(a.data()),
                                         points->InsertNextPoint(b.data())};
            cells->InsertNextCell(2, indices);
            appendColor(colors, color);
        }
        void show(Mesh &mesh) {
            mesh.data->SetPoints(points);
            mesh.data->SetLines(cells);
            mesh.data->GetCellData()->SetScalars(colors);
        }
    };
    // Half bonds and plain single bonds look the same from every side: they are built again only
    // when the molecule changes, not on every turn or zoom.
    const auto plain = [&](const Scene::DrawnBond &bond) { return halves || plainBond(*bond.style); };
    if (s.plainDirty) {
        Lines fixed;
        for (const auto &bond : std::as_const(s.drawnBonds)) {
            if (!plain(bond))
                continue;
            const auto &a = s.document.atoms[bond.a].position, &b = s.document.atoms[bond.b].position;
            if (halves) {
                // Half bonds in the atom colors, one line per bond.
                const Vec middle = (a + b) / 2;
                fixed.add(a, middle, s.colors.value(bond.a));
                fixed.add(middle, b, s.colors.value(bond.b));
            } else
                fixed.add(a, b, bond.style->color);
        }
        fixed.show(s.plain);
        s.plainDirty = false;
    }
    if (!s.bondsDirty && scale == s.bondScale && rotation == s.bondRotation)
        return;
    // Only the small camera-facing bond strokes change on rotation, never the isosurface.
    Lines facing;
    vtkNew<vtkPoints> arrowPoints;
    arrowPoints->SetDataTypeToDouble();
    vtkNew<vtkCellArray> arrowCells;
    vtkNew<vtkUnsignedCharArray> arrowColors;
    arrowColors->SetNumberOfComponents(3);
    for (const auto &bond : std::as_const(s.drawnBonds)) {
        if (plain(bond))
            continue;
        const auto &a = s.document.atoms[bond.a], &b = s.document.atoms[bond.b];
        const Vec delta = rotation * (b.position - a.position);
        const double length = std::hypot(delta.x(), delta.y());
        if (length * scale < 1)
            continue;
        const Vec normal = rotation.transpose() * Vec(-delta.y(), delta.x(), 0) / (length * scale);
        const auto &style = *bond.style;
        const auto color = style.color;
        for (const double offset : style.offsets)
            facing.add(a.position + normal * offset, b.position + normal * offset, color);
        if (style.dashed) {
            const Vec beside = normal * BondStyle::DashOffset;
            const int segments = std::clamp(int(length * scale / 7), 1, 256);
            for (int j = 0; j < segments; j += 2)
                facing.add(a.position + (b.position - a.position) * (double(j) / segments) + beside,
                           a.position + (b.position - a.position) * (double(j + 1) / segments) + beside,
                           color);
        }
        if (style.arrow) {
            const Vec head = a.position + (b.position - a.position) * BondStyle::ArrowAt;
            const Vec back = head - rotation.transpose() * Vec(delta.x(), delta.y(), 0) *
                                        (BondStyle::ArrowLength / (length * scale));
            const Vec left = back + normal * BondStyle::ArrowHalfWidth,
                      right = back - normal * BondStyle::ArrowHalfWidth;
            const vtkIdType indices[] = {arrowPoints->InsertNextPoint(head.data()),
                                         arrowPoints->InsertNextPoint(left.data()),
                                         arrowPoints->InsertNextPoint(right.data())};
            arrowCells->InsertNextCell(3, indices);
            appendColor(arrowColors, color);
        }
    }
    facing.show(s.bonds);
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
