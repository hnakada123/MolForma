// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/molecule_view.h"
#include "editing/peptide.h"
#include "model/geometry.h"
#include "model/i18n.h"
#include "model/settings.h"
#include "ui/bond_style.h"
#include "ui/theme.h"
#include <Eigen/Geometry>
#include <QSettings>
#include <limits>
#include <openbabel/elements.h>
#ifdef MOLFORMA_HAS_VTK
#include "ui/vtk_molecule_renderer.h"
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#endif
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>
#include <algorithm>
#include <functional>
#include <optional>
#include <utility>
namespace mf {
namespace {
class ViewOverlay : public QWidget {
  public:
    std::function<void(QPainter &)> draw;
    explicit ViewOverlay(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        draw(painter);
    }
};
} // namespace
void MoleculeView::configureDefaultSurfaceFormat() {
#ifdef MOLFORMA_HAS_VTK
    auto format = QVTKOpenGLNativeWidget::defaultFormat();
    format.setSamples(0);
    format.setAlphaBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
#endif
}
MoleculeView::MoleculeView(QWidget *parent) : QWidget(parent) {
    setObjectName("moleculeView");
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setMouseTracking(true);
    renderTimer.setSingleShot(true);
    connect(&renderTimer, &QTimer::timeout, this, &MoleculeView::renderScene);
#ifdef MOLFORMA_HAS_VTK
    const auto platform = QApplication::platformName();
    if (qEnvironmentVariable("MOLFORMA_RENDERER").toLower() != "painter" &&
        QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma")
                .value("view/renderer")
                .toString() != "painter" &&
        platform != "offscreen" && platform != "minimal") {
        // Probe before adding an OpenGL widget to the top-level window's composition path.
        QOpenGLContext context;
        context.setFormat(QVTKOpenGLNativeWidget::defaultFormat());
        QOffscreenSurface probe;
        if (context.create()) {
            probe.setFormat(context.format());
            probe.create();
            const auto version = context.format().version();
            if (version >= qMakePair(3, 2) && probe.isValid() && context.makeCurrent(&probe)) {
                context.doneCurrent();
                renderer = new VtkMoleculeRenderer(this);
                auto *layer = new ViewOverlay(this);
                layer->draw = [this](QPainter &p) { paintContents(p, true); };
                overlay = layer;
            }
        }
        if (!renderer) {
            qWarning("MolForma: OpenGL 3.2 is unavailable; using QPainter rendering.");
            fallback = true;
        }
    }
#endif
    rendererStatus = new QLabel(this);
    rendererStatus->setObjectName("rendererStatus");
    rendererStatus->setAttribute(Qt::WA_TransparentForMouseEvents);
    rendererStatus->setStyleSheet("QLabel { color: #52677c; background: rgba(248, 250, 252, 230); "
                                  "border: 1px solid #cbd5e1; border-radius: 5px; padding: 4px 8px; }");
    rendererStatus->move(12, 12);
    updateRendererStatus();
}
QString MoleculeView::rendererName() const {
    return renderer ? "vtk" : "painter";
}
void MoleculeView::updateRendererStatus() {
    // Only a fallback is worth a note on the picture; the renderer in use is in "MolFormaについて".
    rendererStatus->setText(renderer ? tr("描画: VTK（OpenGL）")
                                     : tr("描画: QPainter（CPU）· OpenGLを使えないため簡易表示"));
    rendererStatus->setToolTip(
        tr("OpenGL 3.2を使えないため、CPUで描画しています。表示は同じ内容で、大きな等値面は"
           "間引いて描きます。"));
    rendererStatus->setVisible(fallback && !renderer);
    rendererStatus->adjustSize();
    rendererStatus->raise();
}
void MoleculeView::scheduleRender() {
    if (renderer) {
        // Render before scheduling composition, so Qt does not first present the old 3D frame.
        if (!renderTimer.isActive())
            renderTimer.start(0);
    } else {
        update();
    }
}
// Selection, labels, monitors and markers are drawn over the 3D scene: they need no new frame of it.
void MoleculeView::scheduleOverlay() {
    if (renderer && overlay && !renderTimer.isActive())
        overlay->update();
    else
        scheduleRender();
}
void MoleculeView::renderNow() {
    renderTimer.stop();
    if (renderer) {
        renderScene();
        if (overlay)
            overlay->repaint(); // The labels and markers drawn over the scene.
    } else
        repaint();
}
void MoleculeView::setStyle(const QString &style) {
    displayStyle =
        QStringList{"ball_and_stick", "stick", "spacefill", "wireframe", "backbone"}.contains(style)
            ? style
            : QString("ball_and_stick");
    indexTrace();
    moleculeDirty = true;
    scheduleRender();
}
void MoleculeView::setHidden(const QSet<QString> &ids) {
    hidden = ids;
    selected.removeIf([&](const auto &id) { return hidden.contains(id); });
    selectedIds = QSet<QString>(selected.begin(), selected.end());
    moleculeDirty = true;
    scheduleRender();
}
void MoleculeView::setHydrogenBonds(bool on) {
    showHydrogenBonds = on;
    findHydrogenBonds();
    scheduleOverlay();
}
void MoleculeView::findHydrogenBonds() {
    hbonds.clear();
    if (!showHydrogenBonds || doc.atoms.size() > 5000)
        return;
    const auto polar = [](const QString &e) { return e == "N" || e == "O" || e == "F"; };
    QVector<int> acceptors;
    for (int i = 0; i < doc.atoms.size(); ++i)
        if (!doc.atoms[i].ghost && polar(doc.atoms[i].element))
            acceptors << i;
    // The donor of each hydrogen: the other end of its only bond (dative bonds left out).
    QVector<int> degree(doc.atoms.size()), partner(doc.atoms.size(), -1);
    for (const auto &b : doc.bonds) {
        const int ai = atomIndices.value(b.a, -1), bi = atomIndices.value(b.b, -1);
        if (b.kind == "dative" || ai < 0 || bi < 0)
            continue;
        ++degree[ai];
        ++degree[bi];
        partner[ai] = bi;
        partner[bi] = ai;
    }
    for (int k = 0; k < doc.atoms.size(); ++k) {
        const auto &h = doc.atoms[k];
        if (h.element != "H" || h.ghost || degree[k] != 1 || !polar(doc.atoms[partner[k]].element))
            continue;
        const Vec donor = doc.atoms[partner[k]].position;
        for (int i : acceptors) {
            const auto &a = doc.atoms[i];
            if (i == partner[k])
                continue;
            const double distance = (a.position - h.position).norm();
            // H···A 1.5–2.6 Å and D–H···A at least 120°.
            if (distance < 1.5 || distance > 2.6)
                continue;
            const double cosine =
                (donor - h.position).normalized().dot((a.position - h.position).normalized());
            if (cosine <= std::cos(120 * M_PI / 180))
                hbonds.append({h.id, a.id});
        }
    }
}
double MoleculeView::atomRadius(int atom) const {
    const bool hydrogen = doc.atoms[atom].element == "H";
    if (displayStyle == "spacefill")
        return std::max(2., vdwRadii.value(atom, 1.7) * scale);
    if (displayStyle == "stick")
        return std::clamp(scale * .1, 3., 7.);
    if (displayStyle == "wireframe")
        return 2;
    if (displayStyle == "backbone" && traceNodes.value(atom))
        return std::clamp(scale * .1, 3., 7.);
    return std::clamp(scale * (hydrogen ? .12 : .20), hydrogen ? 5. : 8., hydrogen ? 11. : 20.);
}
// The trace of the backbone style: each chain's polymer residues through their Cα (P of nucleotides, else
// C4'), between residues close enough to follow each other, so that gaps in a structure stay open.
void MoleculeView::indexTrace() {
    traced.clear();
    trace.clear();
    traceNodes.fill(false, doc.atoms.size());
    if (displayStyle != "backbone")
        return;
    const auto node = [&](const Residue &r) {
        for (const auto *name : {"CA", "P", "C4'"})
            if (r.atoms.contains(name))
                return atomIndices.value(r.atoms[name], -1);
        return -1;
    };
    QHash<QString, int> residues;
    for (int i = 0; i < doc.residues.size(); ++i) {
        const auto &r = doc.residues[i];
        residues.insert(r.id, i);
        if (!polymerResidue(r))
            continue;
        const int n = node(r);
        for (const auto &id : r.atoms)
            if (atomIndices.value(id, -1) != n)
                traced.insert(id);
    }
    for (const auto &chain : std::as_const(doc.chains)) {
        int previous = -1;
        for (const auto &id : chain) {
            const int r = residues.value(id, -1);
            if (r < 0 || !polymerResidue(doc.residues[r]))
                continue;
            const int n = node(doc.residues[r]);
            // Cα–Cα 3.8 Å; P–P of nucleic acids about 7 Å.
            const double reach =
                n >= 0 && doc.atoms[n].element == "C" && doc.residues[r].atoms.contains("CA") ? 4.3 : 8;
            if (n >= 0 && previous >= 0 &&
                (doc.atoms[n].position - doc.atoms[previous].position).norm() < reach)
                trace.append({previous, n});
            previous = n;
        }
    }
    // The atoms the line runs through, drawn small; a residue on its own keeps its ball, as the VTK scene.
    for (const auto &[a, b] : std::as_const(trace))
        traceNodes[a] = traceNodes[b] = true;
}
void MoleculeView::refreshColors() {
    // By residue: a hue for each residue, the last one that holds the atom. By chain: the polymer residues
    // in a color of each chain, in the order the chains come; ligands, ions and water keep the element
    // colors, so that they stand out.
    static const QColor chainColors[] = {
        QColor("#4e79a7"), QColor("#f28e2b"), QColor("#59a14f"), QColor("#e15759"), QColor("#76b7b2"),
        QColor("#edc948"), QColor("#b07aa1"), QColor("#ff9da7"), QColor("#9c755f"), QColor("#bab0ac")};
    QHash<QString, QColor> colors;
    QHash<QString, int> chains;
    for (const auto &r : doc.residues) {
        QColor color;
        if (colorMode == "residue")
            color = QColor::fromHsv(int(qHash(r.id, 0) % 360), 135, 205);
        else if (colorMode == "chain" && polymerResidue(r)) {
            const int chain = *chains.insert(r.chain, chains.value(r.chain, chains.size()));
            color = chainColors[chain % std::size(chainColors)];
        } else
            continue;
        for (const auto &id : r.atoms)
            colors.insert(id, color);
    }
    atomColors.clear();
    atomColors.reserve(doc.atoms.size());
    for (const auto &atom : doc.atoms)
        atomColors << colors.value(atom.id, element(atom.element).color);
    moleculeDirty = true;
}
void MoleculeView::renderScene() {
#ifdef MOLFORMA_HAS_VTK
    if (!renderer || !isVisible())
        return;
    if (moleculeDirty) {
        renderer->setMolecule(doc, atomColors, showHydrogens, displayStyle, hidden | traced, trace);
        moleculeDirty = positionsDirty = false;
    } else if (positionsDirty) {
        renderer->setPositions(doc);
        positionsDirty = false;
    }
    if (surfaceDirty) {
        renderer->setSurface(surface);
        surfaceDirty = false;
    }
    renderer->setSurfaceStyle(positiveSurface, negativeSurface, surfaceOpacity);
    renderer->setCamera(camera(), center, scale, pan);
    if (renderer->draw()) {
        overlay->update();
    } else {
        qWarning("MolForma: OpenGL widget initialization failed; using QPainter rendering.");
        fallback = true;
        delete overlay;
        overlay = nullptr;
        delete renderer;
        renderer = nullptr;
        updateRendererStatus();
        update();
    }
#endif
}
void MoleculeView::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
#ifdef MOLFORMA_HAS_VTK
    if (renderer) {
        renderer->setGeometry(rect());
        overlay->setGeometry(rect());
        overlay->raise();
    }
#endif
    rendererStatus->raise();
    scheduleRender();
}
void MoleculeView::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (fitPending) // After the layout has given the newly shown tab its size.
        QTimer::singleShot(0, this, [this] {
            if (fitPending && isVisible())
                fit();
        });
    scheduleRender();
}
void MoleculeView::indexAtoms() {
    atomIndices.clear();
    for (int i = 0; i < doc.atoms.size(); ++i)
        atomIndices.insert(doc.atoms[i].id, i);
    bondEnds.clear();
    bondedAtoms.fill(false, doc.atoms.size());
    for (const auto &b : doc.bonds) {
        bondEnds.append({atomIndices.value(b.a, -1), atomIndices.value(b.b, -1)});
        for (int i : {bondEnds.last().first, bondEnds.last().second})
            if (i >= 0)
                bondedAtoms[i] = true;
    }
    vdwRadii.clear();
    for (const auto &a : doc.atoms) {
        double vdw = 1.7;
        try {
            vdw = OpenBabel::OBElements::GetVdwRad(element(a.element).number);
        } catch (const std::exception &) {
        }
        vdwRadii << vdw;
    }
}
void MoleculeView::setDocument(const Document &d) {
    // The window refreshes for changes elsewhere too (settings, the project path): the same atoms,
    // bonds and residues need no new scene.
    const auto shared = [](const auto &a, const auto &b) {
        return a.constData() == b.constData() && a.size() == b.size();
    };
    if (shared(doc.atoms, d.atoms) && shared(doc.bonds, d.bonds) && shared(doc.residues, d.residues)) {
        const bool drawn = shared(doc.centers, d.centers) && shared(doc.pointCharges, d.pointCharges);
        doc = d;
        if (!drawn)
            scheduleRender();
        return;
    }
    // The same atoms elsewhere (a move, a vibration frame) move the scene's atoms: nothing drawn
    // from their other properties changes.
    const bool moved =
        shared(doc.bonds, d.bonds) && shared(doc.residues, d.residues) &&
        doc.atoms.size() == d.atoms.size() &&
        std::equal(doc.atoms.begin(), doc.atoms.end(), d.atoms.begin(), [](const Atom &a, const Atom &b) {
            return a.id == b.id && a.element == b.element && a.ghost == b.ghost && a.isotope == b.isotope &&
                   a.charge == b.charge && a.fixed == b.fixed && a.stereo == b.stereo;
        });
    doc = d;
    if (moved) {
        positionsDirty = true;
        findHydrogenBonds();
        scheduleRender();
        return;
    }
    indexAtoms();
    indexTrace();
    const auto absent = [&](const QString &id) { return !atomIndices.contains(id); };
    selected.removeIf(absent);
    selectedIds = QSet<QString>(selected.begin(), selected.end());
    hidden.removeIf(absent);
    monitors.removeIf([&](const Ids &ids) { return std::any_of(ids.begin(), ids.end(), absent); });
    findHydrogenBonds();
    refreshColors();
    scheduleRender();
}
void MoleculeView::setSelection(const Ids &ids) {
    selected = ids;
    selectedIds = QSet<QString>(ids.begin(), ids.end());
    scheduleOverlay();
}
void MoleculeView::setSurface(const QVector<SurfaceTriangle> &triangles) {
    // Result-frame navigation can supply the same implicitly shared mesh repeatedly.
    if (surface.constData() != triangles.constData() || surface.size() != triangles.size()) {
        surface = triangles;
        surfaceDirty = true;
    }
    scheduleRender();
}
void MoleculeView::setSurfaceStyle(bool positive, bool negative, int opacity) {
    positiveSurface = positive;
    negativeSurface = negative;
    surfaceOpacity = std::clamp(opacity, 0, 255);
    scheduleRender();
}
Mat MoleculeView::camera() const {
    return orientation;
}
Vec MoleculeView::angles() const {
    // orientation = Rz(roll)·Rx(pitch)·Ry(yaw): its third row is (−cos p sin y, sin p, cos p cos y).
    const Mat &r = orientation;
    const double pitch = std::asin(std::clamp(r(2, 1), -1., 1.));
    double yaw, roll;
    if (std::abs(std::cos(pitch)) > 1e-6) {
        yaw = std::atan2(-r(2, 0), r(2, 2));
        roll = std::atan2(-r(0, 1), r(1, 1));
    } else { // Looking along the rotated Y axis: only yaw + roll is defined.
        yaw = 0;
        roll = std::atan2(r(1, 0), r(0, 0));
    }
    return Vec(yaw, pitch, roll) * 180 / M_PI;
}
QPointF MoleculeView::project(const Vec &p) const {
    Vec v = camera() * (p - center);
    return QPointF(width() * .5 + pan.x() + v.x() * scale, height() * .5 + pan.y() - v.y() * scale);
}
Vec MoleculeView::worldPosition(const QPointF &p) const {
    return center + camera().transpose() * Vec((p.x() - width() * .5 - pan.x()) / scale,
                                               -(p.y() - height() * .5 - pan.y()) / scale, depth);
}
QPointF MoleculeView::atomScreenPosition(const QString &id) const {
    const int i = atomIndices.value(id, -1);
    return project(i >= 0 ? doc.atoms[i].position : doc.atom(id).position);
}
void MoleculeView::fit() {
    QVector<Vec> points;
    for (const auto &a : doc.atoms)
        points << a.position;
    for (const auto &triangle : std::as_const(surface))
        points << triangle.a << triangle.b << triangle.c;
    for (const auto &q : doc.pointCharges)
        points << q.position;
    for (const auto &c : doc.centers) {
        if (!doc.contains(c.atom))
            continue;
        for (const auto &site : c.sites)
            points << doc.atom(c.atom).position + site.direction * site.distance;
    }
    center = Vec::Zero();
    for (auto p : points)
        center += p;
    if (!points.isEmpty())
        center /= points.size();
    double radius = 2;
    for (auto p : points)
        radius = std::max(radius, (p - center).norm() + .6);
    scale = std::clamp(std::min(width(), height()) / (2.5 * radius), 4., 100.);
    pan = {};
    // A hidden view (another tab) has no final size yet: fit again when it is shown.
    fitPending = !isVisible();
    scheduleRender();
}
void MoleculeView::keepInView() {
    if (fitPending || !isVisible()) {
        fit();
        return;
    }
    const QRectF area = QRectF(rect()).adjusted(12, 12, -12, -12);
    const auto outside = [&](const Vec &p) { return !area.contains(project(p)); };
    bool fits = true;
    for (const auto &a : doc.atoms)
        fits &= !shown(a) || !outside(a.position);
    for (const auto &q : doc.pointCharges)
        fits &= !outside(q.position);
    if (!fits)
        fit();
}
void MoleculeView::viewFrom(int axis) {
    setAngles(axis == 0 ? -90 : 0, axis == 1 ? 90 : 0, 0);
    fit();
    emit orientationChanged();
}
void MoleculeView::setAngles(double y, double p, double r) {
    orientation = rotation(Vec::UnitZ(), r) * rotation(Vec::UnitX(), p) * rotation(Vec::UnitY(), y);
    scheduleRender();
}
QString MoleculeView::hitAtom(const QPointF &p) const {
    QString result;
    double front = -1e100;
    Mat r = camera();
    for (int i = 0; i < doc.atoms.size(); ++i) {
        const auto &a = doc.atoms[i];
        if (!shown(a))
            continue;
        double radius = displayStyle == "spacefill" ? atomRadius(i)
                        : a.element == "H"          ? showHydrogens ? 10 : 6
                                                    : 14;
        double dist = QLineF(project(a.position), p).length();
        double z = (r * (a.position - center)).z();
        if (dist <= radius && z > front) {
            front = z;
            result = a.id;
        }
    }
    return result;
}
const Bond *MoleculeView::hitBond(const QPointF &p) const {
    const Bond *result = nullptr;
    double best = 8;
    for (const auto &b : doc.bonds) {
        const int ai = atomIndices.value(b.a, -1), bi = atomIndices.value(b.b, -1);
        if (ai < 0 || bi < 0)
            continue;
        const auto &a = doc.atoms[ai], &c = doc.atoms[bi];
        if ((!showHydrogens && (a.element == "H" || c.element == "H")) || !shown(a) || !shown(c) ||
            displayStyle == "spacefill")
            continue;
        QPointF from = project(a.position), to = project(c.position), v = to - from;
        double length = QPointF::dotProduct(v, v);
        if (length < 1)
            continue;
        double t = std::clamp(QPointF::dotProduct(p - from, v) / length, 0., 1.);
        double dist = QLineF(p, from + t * v).length();
        if (dist < best) {
            best = dist;
            result = &b;
        }
    }
    return result;
}
int MoleculeView::hitMarker(const QPointF &p) const {
    for (int i = markers.size() - 1; i >= 0; --i)
        if (QLineF(p, markers[i].point).length() < (markers[i].site < 0 ? 27 : 15))
            return i;
    return -1;
}
void MoleculeView::paintEvent(QPaintEvent *) {
#ifdef MOLFORMA_HAS_VTK
    // QWidget::grab() can paint synchronously before the coalescing timer fires.
    if (renderer && renderer->isValid() && renderTimer.isActive()) {
        renderTimer.stop();
        renderScene();
    }
#endif
    if (renderer)
        return;
    QPainter p(this);
    paintContents(p, false);
}
void MoleculeView::paintContents(QPainter &p, bool overlayOnly) {
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient bg(0, 0, 0, height());
    bg.setColorAt(0, QColor("#f8fafc"));
    bg.setColorAt(1, QColor("#e9eff4"));
    const bool empty = doc.atoms.isEmpty() && surface.isEmpty() && doc.pointCharges.isEmpty();
    // Structures keep the light background in either theme; with nothing to show, a dark theme
    // has its own color here (over the VTK view too), not a bright box.
    const bool dark = empty && darkPalette(palette());
    if (dark)
        p.fillRect(rect(), palette().color(QPalette::Base));
    else if (!overlayOnly)
        p.fillRect(rect(), bg);
    markers.clear();
    if (empty) {
        p.setPen(dark ? palette().color(QPalette::PlaceholderText) : QColor("#617487"));
        QFont f = font();
        f.setPointSize(16);
        p.setFont(f);
        p.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter,
                   emptyMessage.isEmpty()
                       ? tr("分子を作成\n\n左のパレットから原子・骨格を選び、\nこの画面をクリ"
                            "ックしてください。\n\n構造ファイル・SMILESも読み込めます。")
                       : emptyMessage);
        return;
    }
    struct Item {
        double z;
        int kind; // 0: bond, 1: atom, 2: isosurface triangle, 3: a segment of the backbone trace.
        int index;
    };
    QVector<Item> items;
    Mat r = camera();
    // Atoms well outside the view (a zoomed-in protein) are left out: their spheres, rings and
    // labels cannot reach into it.
    QVector<QPointF> screen(doc.atoms.size());
    const QRectF reach = QRectF(rect()).adjusted(-100, -100, 100, 100);
    for (int i = 0; i < doc.atoms.size(); ++i) {
        screen[i] = project(doc.atoms[i].position);
        const double margin = displayStyle == "spacefill" ? atomRadius(i) : 0;
        if (reach.adjusted(-margin, -margin, margin, margin).contains(screen[i]) && shown(doc.atoms[i]))
            items << Item{(r * (doc.atoms[i].position - center)).z(), true, i};
    }
    const auto crosses = [&](const QPointF &a, const QPointF &b) {
        return std::max(a.x(), b.x()) >= reach.left() && std::min(a.x(), b.x()) <= reach.right() &&
               std::max(a.y(), b.y()) >= reach.top() && std::min(a.y(), b.y()) <= reach.bottom();
    };
    for (int i = 0; !overlayOnly && displayStyle != "spacefill" && i < doc.bonds.size(); ++i) {
        const auto [ai, bi] = bondEnds.value(i, {-1, -1});
        if (ai >= 0 && bi >= 0 && shown(doc.atoms[ai]) && shown(doc.atoms[bi]) &&
            crosses(screen[ai], screen[bi]))
            items << Item{(r * ((doc.atoms[ai].position + doc.atoms[bi].position) * .5 - center)).z(), false,
                          i};
    }
    for (int i = 0; !overlayOnly && i < trace.size(); ++i) {
        const auto [ai, bi] = trace[i];
        if (crosses(screen[ai], screen[bi]))
            items << Item{(r * ((doc.atoms[ai].position + doc.atoms[bi].position) * .5 - center)).z(), 3, i};
    }
    // The painter draws at most about 150 000 triangles; larger surfaces are thinned evenly.
    const int stride = int(std::max<qsizetype>(1, (surface.size() + 149999) / 150000));
    for (int i = 0; !overlayOnly && i < surface.size(); i += stride) {
        const auto &t = surface.at(i);
        if ((t.positive && positiveSurface) || (!t.positive && negativeSurface))
            items << Item{(r * ((t.a + t.b + t.c) / 3 - center)).z(), 2, i};
    }
    std::sort(items.begin(), items.end(), [](auto a, auto b) { return a.z < b.z; });
    struct Label {
        QPointF center;
        double radius;
        QString text;
        double z;
        bool selected, hydrogen, metal;
        int atom;
    };
    QVector<Label> labels;
    QSet<QString> centers; // Coordination centers: their labels come before their ligands'.
    for (const auto &c : doc.centers)
        centers.insert(c.atom);
    for (auto item : items) {
        if (item.kind == 3) {
            // A tube in the colors of its two residues.
            const auto [ai, bi] = trace[item.index];
            const QPointF middle = (screen[ai] + screen[bi]) / 2;
            const double width = std::clamp(scale * .35, 4., 16.);
            p.setPen(QPen(atomColors[ai], width, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(screen[ai], middle);
            p.setPen(QPen(atomColors[bi], width, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(middle, screen[bi]);
        } else if (item.kind == 2) {
            const auto &t = surface.at(item.index);
            const Vec smooth = t.na + t.nb + t.nc;
            const Vec normal = r * (smooth.isZero() ? (t.b - t.a).cross(t.c - t.a) : smooth).normalized();
            QColor color = surfaceColor(t.positive);
            color = color.lighter(80 + qRound(30 * std::abs(normal.z())));
            color.setAlpha(surfaceOpacity);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawPolygon(QPolygonF{project(t.a), project(t.b), project(t.c)});
        } else if (item.kind == 0) {
            const auto &b = doc.bonds[item.index];
            const auto [ai, bi] = bondEnds[item.index];
            const auto &a = doc.atoms[ai], &c = doc.atoms[bi];
            if (!showHydrogens && (a.element == "H" || c.element == "H"))
                continue;
            QPointF from = screen[ai], to = screen[bi], v = to - from;
            double len = QLineF(from, to).length();
            if (len < 1)
                continue;
            QPointF normal(-v.y() / len, v.x() / len);
            if (halfBonds(displayStyle)) {
                // Half bonds in the atom colors; multiple bonds as one line.
                const double width = displayStyle == "stick" ? std::clamp(scale * .2, 5., 12.) : 2.;
                const QPointF middle = (from + to) / 2;
                p.setPen(QPen(atomColors[ai], width, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(from, middle);
                p.setPen(QPen(atomColors[bi], width, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(middle, to);
                continue;
            }
            const auto &style = bondStyle(b.kind);
            p.setPen(QPen(style.color, 3.5, Qt::SolidLine, Qt::RoundCap));
            for (const double offset : style.offsets)
                p.drawLine(from + normal * offset, to + normal * offset);
            if (style.dashed) {
                p.setPen(QPen(style.color, 1.7, Qt::DashLine));
                p.drawLine(from + normal * BondStyle::DashOffset, to + normal * BondStyle::DashOffset);
            }
            if (style.arrow) {
                QPointF head = from + v * BondStyle::ArrowAt, back = head - v / len * BondStyle::ArrowLength;
                p.setBrush(style.color);
                p.setPen(Qt::NoPen);
                p.drawPolygon(QPolygonF{head, back + normal * BondStyle::ArrowHalfWidth,
                                        back - normal * BondStyle::ArrowHalfWidth});
            }
        } else {
            const auto &a = doc.atoms[item.index];
            QPointF pos = screen[item.index];
            bool hydrogen = a.element == "H";
            if (hydrogen && !showHydrogens) {
                p.setPen(QPen(QColor("#4c9a6c"), 1.5));
                p.setBrush(selectedIds.contains(a.id) ? QColor("#f6d57b") : QColor("#cfe9db"));
                p.drawEllipse(pos, 4, 4);
                continue;
            }
            const double radius = atomRadius(item.index);
            QColor color = atomColors[item.index];
            if (selectedIds.contains(a.id)) {
                p.setPen(QPen(QColor("#de9c25"), 3));
                p.setBrush(QColor(250, 199, 85, 80));
                p.drawEllipse(pos, radius + 5, radius + 5);
            }
            if ((!overlayOnly || a.ghost) &&
                (displayStyle != "wireframe" || a.ghost || !bondedAtoms.value(item.index))) {
                QRadialGradient sphere(pos - QPointF(radius * .35, radius * .4), radius * 1.5);
                sphere.setColorAt(0, color.lighter(160));
                sphere.setColorAt(.6, color);
                sphere.setColorAt(1, color.darker(160));
                p.setBrush(a.ghost ? QBrush(Qt::NoBrush) : QBrush(sphere));
                p.setPen(QPen(color.darker(150), a.ghost ? 2 : 1, a.ghost ? Qt::DashLine : Qt::SolidLine));
                p.drawEllipse(pos, radius, radius);
            }
            const bool chosen = selectedIds.contains(a.id);
            // The hundreds of Cα of a backbone trace carry labels only when selected.
            if (labelMode != "none" && (chosen || !traceNodes.value(item.index)) &&
                (labelMode == "all" || chosen || (labelMode == "heavy" && !hydrogen))) {
                QString label = a.isotope == 2 && hydrogen ? "D"
                                : a.isotope == 3 && hydrogen
                                    ? "T"
                                    : (a.isotope ? QString::number(a.isotope) : "") + a.element;
                if (showIndices)
                    label += QString::number(item.index + 1);
                if (a.charge)
                    label += QString("(%1%2)").arg(a.charge > 0 ? "+" : "").arg(a.charge);
                if (a.stereo == "R" || a.stereo == "S") // The recorded configuration of a stereocenter.
                    label += " (" + a.stereo + ")";
                // Placed after all atoms, so labels stay readable in front of the drawing.
                labels << Label{pos,       radius, label, item.z, chosen, hydrogen, centers.contains(a.id),
                                item.index};
            }
            if (a.fixed) {
                p.setPen(QColor("#c47b12"));
                p.drawText(pos + QPointF(-radius, radius + 14), "■");
            }
        }
    }
    // The recorded E/Z of double bonds, at the middle of the bond (after the atom labels).
    if (labelMode != "none")
        for (int i = 0; i < doc.bonds.size(); ++i) {
            const auto &b = doc.bonds[i];
            if (b.stereo != "E" && b.stereo != "Z")
                continue;
            const auto [ai, bi] = bondEnds.value(i, {-1, -1});
            if (ai >= 0 && bi >= 0 && shown(doc.atoms[ai]) && shown(doc.atoms[bi]))
                labels << Label{project((doc.atoms[ai].position + doc.atoms[bi].position) / 2),
                                2,
                                "(" + b.stereo + ")",
                                -std::numeric_limits<double>::infinity(),
                                false,
                                false,
                                false,
                                -1};
        }
    // Labels: selected atoms first, then coordination centers and other heavy atoms, front to back.
    // Each tries the four corners beside its atom and takes the first that covers neither another
    // label nor another atom; one that only covers atoms is the fallback, and one that would
    // cover a label is left out.
    std::stable_sort(labels.begin(), labels.end(), [](const Label &a, const Label &b) {
        if (a.selected != b.selected)
            return a.selected;
        if (a.metal != b.metal)
            return a.metal;
        if (a.hydrogen != b.hydrogen)
            return !a.hydrogen;
        return a.z > b.z;
    });
    constexpr double cell = 40; // Atoms by screen cell, so the overlap test stays local.
    QHash<QPoint, QVector<int>> grid;
    QVector<std::pair<QPointF, double>> disks(doc.atoms.size(), {QPointF(), 0.});
    if (!labels.isEmpty())
        for (const auto &item : std::as_const(items))
            if (item.kind == 1) {
                const auto &a = doc.atoms[item.index];
                const double radius = a.element == "H" && !showHydrogens ? 4 : atomRadius(item.index);
                disks[item.index] = {screen[item.index], radius};
                grid[QPoint(int(std::floor(disks[item.index].first.x() / cell)),
                            int(std::floor(disks[item.index].first.y() / cell)))]
                    << item.index;
            }
    const auto coversAtom = [&](const QRectF &box, int own) {
        const double reach = 20; // The largest atom radius drawn.
        for (int x = int(std::floor((box.left() - reach) / cell));
             x <= int(std::floor((box.right() + reach) / cell)); ++x)
            for (int y = int(std::floor((box.top() - reach) / cell));
                 y <= int(std::floor((box.bottom() + reach) / cell)); ++y)
                for (int i : grid.value(QPoint(x, y))) {
                    if (i == own)
                        continue;
                    const auto &[center, radius] = disks[i];
                    const QPointF nearest(std::clamp(center.x(), box.left(), box.right()),
                                          std::clamp(center.y(), box.top(), box.bottom()));
                    if (QLineF(nearest, center).length() < radius)
                        return true;
                }
        return false;
    };
    // The labels placed so far, by the screen cells they cover, so each test stays local too.
    QVector<QRectF> placed;
    QHash<QPoint, QVector<int>> placedCells;
    const auto cells = [&](const QRectF &box, const auto &visit) {
        for (int x = int(std::floor(box.left() / cell)); x <= int(std::floor(box.right() / cell)); ++x)
            for (int y = int(std::floor(box.top() / cell)); y <= int(std::floor(box.bottom() / cell)); ++y)
                if (visit(QPoint(x, y)))
                    return true;
        return false;
    };
    const auto coversLabel = [&](const QRectF &box) {
        return cells(box, [&](const QPoint &c) {
            const auto it = placedCells.constFind(c);
            return it != placedCells.cend() &&
                   std::any_of(it->begin(), it->end(), [&](int i) { return placed[i].intersects(box); });
        });
    };
    const auto metrics = p.fontMetrics();
    QHash<QString, QRectF> extents; // Most labels repeat ("C", "H"); measure each text once.
    p.setPen(QColor("#20364b"));
    for (const auto &label : labels) {
        auto extent = extents.constFind(label.text);
        if (extent == extents.cend())
            extent = extents.insert(label.text, metrics.boundingRect(label.text));
        const QRectF text = *extent;
        const double r = label.radius;
        const QPointF corners[]{{r + 3, -r - 1},
                                {r + 3, r + metrics.ascent()},
                                {-r - 3 - text.width(), -r - 1},
                                {-r - 3 - text.width(), r + metrics.ascent()}};
        std::optional<QPointF> chosen, fallback;
        for (const auto &corner : corners) {
            const QPointF at = label.center + corner;
            // Two pixels apart, so neighbouring labels never read as one ("H17N14").
            const QRectF box = text.translated(at).adjusted(-2, -1, 2, 1);
            if (coversLabel(box))
                continue;
            if (!coversAtom(box, label.atom)) {
                chosen = at;
                break;
            }
            if (!fallback)
                fallback = at;
        }
        if (!chosen)
            chosen = fallback;
        if (!chosen)
            continue;
        placed << text.translated(*chosen).adjusted(-2, -1, 2, 1);
        cells(placed.last(), [&](const QPoint &c) {
            placedCells[c] << int(placed.size()) - 1;
            return false;
        });
        p.drawText(*chosen, label.text);
    }
    // Candidate changes: removed atoms at their old place (red), added (green) and moved (orange).
    for (const auto &[position, symbol] : removedAtoms) {
        const QPointF at = project(position);
        p.setPen(QPen(QColor("#d23c3c"), 2, Qt::DashLine));
        p.setBrush(QColor(210, 60, 60, 40));
        p.drawEllipse(at, 12, 12);
        p.setPen(QPen(QColor("#d23c3c"), 2));
        p.drawLine(at + QPointF(-6, -6), at + QPointF(6, 6));
        p.drawLine(at + QPointF(-6, 6), at + QPointF(6, -6));
        p.drawText(at + QPointF(13, 13), symbol);
    }
    for (int i = 0; (!addedAtoms.isEmpty() || !movedAtoms.isEmpty()) && i < doc.atoms.size(); ++i) {
        const auto &a = doc.atoms[i];
        const bool added = addedAtoms.contains(a.id), moved = movedAtoms.contains(a.id);
        if ((!added && !moved) || !shown(a))
            continue;
        p.setPen(QPen(added ? QColor("#25a35a") : QColor("#e08a1e"), 2.5));
        p.setBrush(Qt::NoBrush);
        const double r = atomRadius(i) + 4;
        p.drawEllipse(project(a.position), r, r);
    }
    for (const auto &c : doc.centers) {
        const auto &id = c.atom;
        if (!atomIndices.contains(id))
            continue;
        Vec origin = doc.atoms[atomIndices[id]].position;
        for (int i = 0; i < c.sites.size(); ++i) {
            const auto &s = c.sites[i];
            if (!s.donor.isEmpty())
                continue;
            QPointF pos = project(origin + s.direction * s.distance);
            p.setPen(QPen(QColor("#9978bd"), 1, Qt::DashLine));
            p.drawLine(project(origin), pos);
            p.setBrush(QColor("#f1e8fc"));
            p.setPen(QPen(QColor("#9978bd"), 2));
            p.drawEllipse(pos, 12, 12);
            p.drawText(QRectF(pos - QPointF(12, 12), QSizeF(24, 24)), Qt::AlignCenter,
                       QString::number(i + 1));
            markers << Marker{pos, id, i, false};
        }
    }
    if (aminoHandles)
        for (const auto &res : doc.residues) {
            for (bool n : {true, false}) {
                const auto &state = n ? res.nTerminal : res.cTerminal;
                if (state != "neutral" && state != "charged")
                    continue;
                const int atom = atomIndices.value(res.atoms.value(n ? "N" : "C"), -1);
                if (atom < 0)
                    continue;
                QPointF pos = project(doc.atoms[atom].position) + QPointF(n ? -38 : 38, -32);
                p.setPen(QPen(QColor("#168473"), 1.5));
                p.setBrush(QColor("#e2f6ee"));
                p.drawRoundedRect(QRectF(pos - QPointF(27, 12), QSizeF(54, 24)), 8, 8);
                p.drawText(QRectF(pos - QPointF(27, 12), QSizeF(54, 24)), Qt::AlignCenter,
                           n ? tr("＋ N") : tr("＋ C"));
                markers << Marker{pos, res.id, -1, n};
            }
        }
    for (const auto &q : doc.pointCharges) {
        auto pos = project(q.position);
        QColor c = q.charge > 0 ? QColor("#c75162") : QColor("#407fcc");
        p.setPen(QPen(c, 2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(pos, 9, 9);
        p.drawLine(pos - QPointF(5, 0), pos + QPointF(5, 0));
        if (q.charge > 0)
            p.drawLine(pos - QPointF(0, 5), pos + QPointF(0, 5));
        p.drawText(pos + QPointF(12, 4), QString::number(q.charge, 'g', 3));
    }
    if (!hbonds.isEmpty()) {
        p.setPen(QPen(QColor("#2a7bd6"), 1.6, Qt::DashLine));
        for (const auto &[h, acceptor] : hbonds) {
            const int hi = atomIndices.value(h, -1), ai = atomIndices.value(acceptor, -1);
            if (hi < 0 || ai < 0 || !shown(doc.atoms[hi]) || !shown(doc.atoms[ai]))
                continue;
            p.drawLine(project(doc.atoms[hi].position), project(doc.atoms[ai].position));
        }
    }
    for (const auto &ids : monitors) {
        if (std::any_of(ids.begin(), ids.end(),
                        [&](const auto &id) { return !doc.contains(id) || !shown(doc.atom(id)); }))
            continue;
        QPolygonF points;
        for (const auto &id : ids)
            points << project(doc.atom(id).position);
        p.setPen(QPen(QColor("#8a4fb8"), 1.6, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(points);
        QPointF middle(0, 0);
        for (const auto &point : points)
            middle += point;
        middle /= points.size();
        // Coincident or collinear atoms have no angle or dihedral; never throw from painting.
        QString text = "—";
        try {
            const double value = measure(doc, ids);
            text = ids.size() == 2 ? QString::number(value, 'f', 3) + " Å"
                                   : QString::number(value, 'f', 1) + "°";
        } catch (const std::exception &) {
        }
        const QRectF box = QRectF(p.fontMetrics().boundingRect(text)).adjusted(-4, -2, 4, 2);
        const QRectF placed(middle - QPointF(box.width() / 2, box.height() / 2), box.size());
        p.setPen(QPen(QColor("#8a4fb8"), 1));
        p.setBrush(QColor(255, 255, 255, 220));
        p.drawRoundedRect(placed, 4, 4);
        p.setPen(QColor("#4b2a66"));
        p.drawText(placed, Qt::AlignCenter, text);
    }
    for (const auto &[from, vector] : arrows) {
        const QPointF a = project(from), b = project(from + vector), v = b - a;
        const double length = QLineF(a, b).length();
        if (length < 3)
            continue;
        const QPointF unit = v / length, normal(-unit.y(), unit.x());
        const QColor color("#178a55");
        p.setPen(QPen(color, 2.5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a, b - unit * 6);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPolygon(QPolygonF{b, b - unit * 10 + normal * 5, b - unit * 10 - normal * 5});
    }
    QPointF origin(width() - 48, height() - 48);
    for (int axis = 0; axis < 3; ++axis) {
        Vec v = r * Vec::Unit(axis);
        p.setPen(QPen(QColor(axis == 0 ? "#c44d53" : axis == 1 ? "#278660" : "#317bb4"), 2));
        p.drawLine(origin, origin + QPointF(v.x() * 28, -v.y() * 28));
        p.drawText(origin + QPointF(v.x() * 35, -v.y() * 35), QString("XYZ")[axis]);
    }
    if (boxing) {
        p.setPen(QPen(QColor("#247a96"), 1, Qt::DashLine));
        p.setBrush(QColor(43, 131, 160, 25));
        p.drawRect(QRectF(press, last).normalized());
    }
}
void MoleculeView::mousePressEvent(QMouseEvent *e) {
    setFocus();
    press = last = e->position();
    pressedButton = e->button();
    pressModifiers = e->modifiers();
    pressedAtom = hitAtom(press);
    dragging = false;
    boxing = false;
    dragAtoms.clear();
    if (tool == EditorTool::Move && !pressedAtom.isEmpty() && pressedButton == Qt::LeftButton &&
        !pressModifiers.testFlag(Qt::AltModifier))
        emit dragStarted(pressedAtom);
}
void MoleculeView::mouseMoveEvent(QMouseEvent *e) {
    if (pressedButton == Qt::NoButton)
        return;
    QPointF delta = e->position() - last;
    if (QLineF(press, e->position()).length() > 4)
        dragging = true;
    if (dragging) {
        if (pressedButton == Qt::RightButton)
            pan += delta;
        else if (pressedButton == Qt::LeftButton &&
                 (pressModifiers & (Qt::ShiftModifier | Qt::ControlModifier)) && tool == EditorTool::Select)
            boxing = true;
        else if (tool != EditorTool::Move || pressedAtom.isEmpty() ||
                 pressModifiers.testFlag(Qt::AltModifier) || pressedButton == Qt::MiddleButton) {
            // Trackball: turn about the screen's vertical and horizontal axes, whatever the
            // current orientation (no gimbal lock), and keep the matrix orthonormal.
            orientation =
                rotation(Vec::UnitY(), delta.x() * .6) * rotation(Vec::UnitX(), delta.y() * .6) * orientation;
            orientation = Eigen::Quaterniond(orientation).normalized().toRotationMatrix();
            emit orientationChanged();
        } else if (pressedButton == Qt::LeftButton) {
            // The atoms follow the cursor while dragging; the edit itself happens on release. A
            // drag that includes a fixed atom is refused then, so nothing moves meanwhile either.
            // The carried atoms are found once; each move then sets only their positions.
            if (!dragOrigin) {
                dragOrigin = doc;
                dragMoving.clear();
                for (int i = 0; i < doc.atoms.size(); ++i)
                    if (dragAtoms.contains(doc.atoms[i].id) || doc.atoms[i].id == pressedAtom)
                        dragMoving << i;
                dragFixed = std::any_of(dragMoving.begin(), dragMoving.end(),
                                        [&](int i) { return doc.atoms[i].fixed; });
            }
            if (doc.atoms.size() != dragOrigin->atoms.size()) // Replaced during the drag.
                doc = *dragOrigin;
            if (!dragFixed) {
                const Vec shift = worldPosition(e->position()) - worldPosition(press);
                for (int i : dragMoving)
                    doc.atoms[i].position = dragOrigin->atoms[i].position + shift;
            }
            positionsDirty = true;
        }
    }
    last = e->position();
    if (boxing)
        scheduleOverlay(); // Only the rectangle moved.
    else if (dragging)
        scheduleRender();
}
void MoleculeView::mouseReleaseEvent(QMouseEvent *e) {
    auto button = pressedButton;
    pressedButton = Qt::NoButton;
    if (dragOrigin) { // The edit replaces the preview (or the document stays as it was).
        doc = *dragOrigin;
        dragOrigin.reset();
        positionsDirty = true;
        scheduleRender();
    }
    if (dragging) {
        if (boxing) {
            Ids ids;
            auto box = QRectF(press, e->position()).normalized();
            for (const auto &a : doc.atoms)
                if ((showHydrogens || a.element != "H") && box.contains(project(a.position)))
                    ids << a.id;
            emit rectangleSelected(ids, pressModifiers.testFlag(Qt::ControlModifier));
            boxing = false;
            scheduleOverlay();
        } else if (button == Qt::LeftButton && tool == EditorTool::Move && !pressedAtom.isEmpty() &&
                   !pressModifiers.testFlag(Qt::AltModifier))
            emit atomDragged(pressedAtom, worldPosition(e->position()) - worldPosition(press));
        return;
    }
    if (button == Qt::RightButton) {
        emit contextRequested(e->globalPosition().toPoint(), hitAtom(e->position()));
        return;
    }
    if (button != Qt::LeftButton)
        return;
    int marker = hitMarker(e->position());
    if (marker >= 0) {
        auto m = markers[marker];
        if (m.site >= 0)
            emit siteClicked(m.owner, m.site);
        else
            emit terminusClicked(m.owner, m.n);
        return;
    }
    auto id = hitAtom(e->position());
    if (!id.isEmpty())
        emit atomClicked(id, e->modifiers());
    else if (auto b = hitBond(e->position()))
        emit bondClicked(b->a, b->b);
    else
        emit blankClicked(worldPosition(e->position()));
}
void MoleculeView::wheelEvent(QWheelEvent *e) {
    // Zoom about the cursor: the point under it stays in place.
    const double next = std::clamp(scale * std::pow(1.0015, e->angleDelta().y()), .5, 600.);
    const QPointF cursor = e->position(), middle(width() * .5, height() * .5);
    pan = cursor - middle - (cursor - middle - pan) * (next / scale);
    scale = next;
    scheduleRender();
    e->accept();
}
void MoleculeView::keyPressEvent(QKeyEvent *e) {
    if (QList<int>{Qt::Key_Delete, Qt::Key_Escape, Qt::Key_H, Qt::Key_1, Qt::Key_2, Qt::Key_3}.contains(
            e->key())) {
        emit shortcut(e->key());
        e->accept();
    } else
        QWidget::keyPressEvent(e);
}
void MoleculeView::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasFormat("application/x-molforma-template"))
        e->acceptProposedAction();
}
void MoleculeView::dropEvent(QDropEvent *e) {
    QString key = QString::fromUtf8(e->mimeData()->data("application/x-molforma-template"));
    QString c, res;
    int site = -1;
    bool n = false;
    int hit = hitMarker(e->position());
    if (hit >= 0) {
        auto m = markers[hit];
        if (m.site >= 0) {
            c = m.owner;
            site = m.site;
        } else {
            res = m.owner;
            n = m.n;
        }
    }
    emit templateDropped(key, hitAtom(e->position()), worldPosition(e->position()), c, site, res, n);
    e->acceptProposedAction();
}
} // namespace mf
