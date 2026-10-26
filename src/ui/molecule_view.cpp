#include "ui/molecule_view.h"
#include "model/geometry.h"
#include "model/i18n.h"
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
        QSettings("MolForma", "MolForma").value("view/renderer").toString() != "painter" &&
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
    displayStyle = QStringList{"ball_and_stick", "stick", "spacefill", "wireframe"}.contains(style)
                       ? style
                       : QString("ball_and_stick");
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
    scheduleRender();
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
    for (const auto &h : doc.atoms) {
        if (h.element != "H" || h.ghost)
            continue;
        const auto neighbors = doc.neighbors(h.id, false);
        if (neighbors.size() != 1 || !polar(doc.atom(neighbors[0]).element))
            continue;
        const Vec donor = doc.atom(neighbors[0]).position;
        for (int i : acceptors) {
            const auto &a = doc.atoms[i];
            if (a.id == neighbors[0])
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
double MoleculeView::atomRadius(const Atom &a) const {
    const bool hydrogen = a.element == "H";
    if (displayStyle == "spacefill") {
        double vdw = 1.7;
        try {
            vdw = OpenBabel::OBElements::GetVdwRad(element(a.element).number);
        } catch (const std::exception &) {
        }
        return std::max(2., vdw * scale);
    }
    if (displayStyle == "stick")
        return std::clamp(scale * .1, 3., 7.);
    if (displayStyle == "wireframe")
        return 2;
    return std::clamp(scale * (hydrogen ? .12 : .20), hydrogen ? 5. : 8., hydrogen ? 11. : 20.);
}
void MoleculeView::refreshColors() {
    atomColors.clear();
    atomColors.reserve(doc.atoms.size());
    for (const auto &atom : doc.atoms)
        atomColors << atomColor(atom);
    moleculeDirty = true;
}
void MoleculeView::renderScene() {
#ifdef MOLFORMA_HAS_VTK
    if (!renderer || !isVisible())
        return;
    if (moleculeDirty) {
        renderer->setMolecule(doc, atomColors, showHydrogens, displayStyle, hidden);
        moleculeDirty = false;
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
void MoleculeView::setDocument(const Document &d) {
    doc = d;
    atomIndices.clear();
    for (int i = 0; i < doc.atoms.size(); ++i)
        atomIndices.insert(doc.atoms[i].id, i);
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
    scheduleRender();
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
    return project(doc.atom(id).position);
}
void MoleculeView::fit() {
    QVector<Vec> points;
    for (const auto &a : doc.atoms)
        points << a.position;
    for (const auto &triangle : std::as_const(surface))
        points << triangle.a << triangle.b << triangle.c;
    for (auto v : doc.data["point_charges"].toArray())
        points << vectorFromJson(v.toObject()["position"]);
    for (auto v : doc.data["centers"].toArray()) {
        auto c = v.toObject();
        if (!doc.contains(c["atom"].toString()))
            continue;
        for (auto s : c["sites"].toArray())
            points << doc.atom(c["atom"].toString()).position +
                          vectorFromJson(s.toObject()["direction"]) * s.toObject()["distance"].toDouble();
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
    for (const auto &v : doc.data["point_charges"].toArray())
        fits &= !outside(vectorFromJson(v.toObject()["position"]));
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
    for (const auto &a : doc.atoms) {
        if (!shown(a))
            continue;
        double radius = displayStyle == "spacefill" ? atomRadius(a)
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
QColor MoleculeView::atomColor(const Atom &a) const {
    QString group;
    if (colorMode == "residue") {
        for (auto v : doc.data["residues"].toArray()) {
            auto r = v.toObject();
            for (auto id : r["atoms"].toObject())
                if (id == a.id)
                    group = r["id"].toString();
        }
    } else if (colorMode != "element")
        group =
            doc.data["assignments"].toObject()[colorMode].toObject()["members"].toObject()[a.id].toString();
    if (!group.isEmpty())
        return QColor::fromHsv(int(qHash(group, 0) % 360), 135, 205);
    return element(a.element).color;
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
    const bool empty =
        doc.atoms.isEmpty() && surface.isEmpty() && doc.data["point_charges"].toArray().isEmpty();
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
        int kind; // 0: bond, 1: atom, 2: isosurface triangle.
        int index;
    };
    QVector<Item> items;
    QSet<QString> bonded; // Wireframe draws atoms only where no bond line shows them.
    if (displayStyle == "wireframe")
        for (const auto &b : doc.bonds)
            bonded << b.a << b.b;
    Mat r = camera();
    for (int i = 0; i < doc.atoms.size(); ++i)
        if (shown(doc.atoms[i]))
            items << Item{(r * (doc.atoms[i].position - center)).z(), true, i};
    for (int i = 0; !overlayOnly && displayStyle != "spacefill" && i < doc.bonds.size(); ++i) {
        const auto &b = doc.bonds[i];
        const int ai = atomIndices.value(b.a, -1), bi = atomIndices.value(b.b, -1);
        if (ai >= 0 && bi >= 0 && shown(doc.atoms[ai]) && shown(doc.atoms[bi]))
            items << Item{(r * ((doc.atoms[ai].position + doc.atoms[bi].position) * .5 - center)).z(), false,
                          i};
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
    for (const auto &v : doc.data["centers"].toArray())
        centers.insert(v.toObject()["atom"].toString());
    for (auto item : items) {
        if (item.kind == 2) {
            const auto &t = surface.at(item.index);
            const Vec smooth = t.na + t.nb + t.nc;
            const Vec normal = r * (smooth.isZero() ? (t.b - t.a).cross(t.c - t.a) : smooth).normalized();
            QColor color(t.positive ? "#ce5268" : "#397cce");
            color = color.lighter(80 + qRound(30 * std::abs(normal.z())));
            color.setAlpha(surfaceOpacity);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawPolygon(QPolygonF{project(t.a), project(t.b), project(t.c)});
        } else if (item.kind == 0) {
            const auto &b = doc.bonds[item.index];
            const auto &a = doc.atoms[atomIndices.value(b.a)], &c = doc.atoms[atomIndices.value(b.b)];
            if (!showHydrogens && (a.element == "H" || c.element == "H"))
                continue;
            QPointF from = project(a.position), to = project(c.position), v = to - from;
            double len = QLineF(from, to).length();
            if (len < 1)
                continue;
            QPointF normal(-v.y() / len, v.x() / len);
            if (displayStyle == "stick" || displayStyle == "wireframe") {
                // Half bonds in the atom colors; multiple bonds as one line.
                const double width = displayStyle == "stick" ? std::clamp(scale * .2, 5., 12.) : 2.;
                const QPointF middle = (from + to) / 2;
                p.setPen(QPen(atomColors[atomIndices.value(b.a)], width, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(from, middle);
                p.setPen(QPen(atomColors[atomIndices.value(b.b)], width, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(middle, to);
                continue;
            }
            QColor color = b.kind == "dative"     ? QColor("#2879bd")
                           : b.kind == "aromatic" ? QColor("#a36a2b")
                                                  : QColor("#748595");
            int count = b.kind == "double" ? 2 : b.kind == "triple" ? 3 : 1;
            p.setPen(QPen(color, 3.5, Qt::SolidLine, Qt::RoundCap));
            for (int j = 0; j < count; ++j) {
                auto offset = normal * ((j - (count - 1) * .5) * 5);
                p.drawLine(from + offset, to + offset);
            }
            if (b.kind == "aromatic") {
                p.setPen(QPen(color, 1.7, Qt::DashLine));
                p.drawLine(from + normal * 5, to + normal * 5);
            }
            if (b.kind == "dative") {
                QPointF head = from + v * .72, back = head - v / len * 9;
                p.setBrush(color);
                p.setPen(Qt::NoPen);
                p.drawPolygon(QPolygonF{head, back + normal * 4, back - normal * 4});
            }
        } else {
            const auto &a = doc.atoms[item.index];
            QPointF pos = project(a.position);
            bool hydrogen = a.element == "H";
            if (hydrogen && !showHydrogens) {
                p.setPen(QPen(QColor("#4c9a6c"), 1.5));
                p.setBrush(selectedIds.contains(a.id) ? QColor("#f6d57b") : QColor("#cfe9db"));
                p.drawEllipse(pos, 4, 4);
                continue;
            }
            const double radius = atomRadius(a);
            QColor color = atomColors[item.index];
            if (selectedIds.contains(a.id)) {
                p.setPen(QPen(QColor("#de9c25"), 3));
                p.setBrush(QColor(250, 199, 85, 80));
                p.drawEllipse(pos, radius + 5, radius + 5);
            }
            if ((!overlayOnly || a.ghost) &&
                (displayStyle != "wireframe" || a.ghost || !bonded.contains(a.id))) {
                QRadialGradient sphere(pos - QPointF(radius * .35, radius * .4), radius * 1.5);
                sphere.setColorAt(0, color.lighter(160));
                sphere.setColorAt(.6, color);
                sphere.setColorAt(1, color.darker(160));
                p.setBrush(a.ghost ? QBrush(Qt::NoBrush) : QBrush(sphere));
                p.setPen(QPen(color.darker(150), a.ghost ? 2 : 1, a.ghost ? Qt::DashLine : Qt::SolidLine));
                p.drawEllipse(pos, radius, radius);
            }
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
            const bool chosen = selectedIds.contains(a.id);
            if (labelMode != "none" && (labelMode == "all" || chosen || (labelMode == "heavy" && !hydrogen)))
                // Placed after all atoms, so labels stay readable in front of the drawing.
                labels << Label{pos,       radius, label, item.z, chosen, hydrogen, centers.contains(a.id),
                                item.index};
            if (a.fixed) {
                p.setPen(QColor("#c47b12"));
                p.drawText(pos + QPointF(-radius, radius + 14), "■");
            }
        }
    }
    // The recorded E/Z of double bonds, at the middle of the bond (after the atom labels).
    if (labelMode != "none")
        for (const auto &b : doc.bonds) {
            const int ai = atomIndices.value(b.a, -1), bi = atomIndices.value(b.b, -1);
            if ((b.stereo == "E" || b.stereo == "Z") && ai >= 0 && bi >= 0 && shown(doc.atoms[ai]) &&
                shown(doc.atoms[bi]))
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
                const double radius = a.element == "H" && !showHydrogens ? 4 : atomRadius(a);
                disks[item.index] = {project(a.position), radius};
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
    QVector<QRectF> placed;
    const auto metrics = p.fontMetrics();
    p.setPen(QColor("#20364b"));
    for (const auto &label : labels) {
        const QRectF text = metrics.boundingRect(label.text);
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
            if (std::any_of(placed.begin(), placed.end(), [&](const QRectF &q) { return q.intersects(box); }))
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
    for (const auto &a : doc.atoms) {
        const bool added = addedAtoms.contains(a.id), moved = movedAtoms.contains(a.id);
        if ((!added && !moved) || !shown(a))
            continue;
        p.setPen(QPen(added ? QColor("#25a35a") : QColor("#e08a1e"), 2.5));
        p.setBrush(Qt::NoBrush);
        const double r = atomRadius(a) + 4;
        p.drawEllipse(project(a.position), r, r);
    }
    for (auto v : doc.data["centers"].toArray()) {
        auto c = v.toObject();
        QString id = c["atom"].toString();
        if (!doc.contains(id))
            continue;
        Vec origin = doc.atom(id).position;
        auto sites = c["sites"].toArray();
        for (int i = 0; i < sites.size(); ++i) {
            auto s = sites[i].toObject();
            if (!s["donor"].toString().isEmpty())
                continue;
            QPointF pos = project(origin + vectorFromJson(s["direction"]) * s["distance"].toDouble());
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
        for (auto v : doc.data["residues"].toArray()) {
            auto res = v.toObject();
            for (bool n : {true, false}) {
                auto state = res[n ? "n_terminal" : "c_terminal"].toString();
                auto id = res["atoms"].toObject()[n ? "N" : "C"].toString();
                if (!doc.contains(id) || !QStringList{"neutral", "charged"}.contains(state))
                    continue;
                QPointF pos = project(doc.atom(id).position) + QPointF(n ? -38 : 38, -32);
                p.setPen(QPen(QColor("#168473"), 1.5));
                p.setBrush(QColor("#e2f6ee"));
                p.drawRoundedRect(QRectF(pos - QPointF(27, 12), QSizeF(54, 24)), 8, 8);
                p.drawText(QRectF(pos - QPointF(27, 12), QSizeF(54, 24)), Qt::AlignCenter,
                           n ? tr("＋ N") : tr("＋ C"));
                markers << Marker{pos, res["id"].toString(), -1, n};
            }
        }
    for (auto v : doc.data["point_charges"].toArray()) {
        auto q = v.toObject();
        auto pos = project(vectorFromJson(q["position"]));
        QColor c = q["charge"].toDouble() > 0 ? QColor("#c75162") : QColor("#407fcc");
        p.setPen(QPen(c, 2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(pos, 9, 9);
        p.drawLine(pos - QPointF(5, 0), pos + QPointF(5, 0));
        if (q["charge"].toDouble() > 0)
            p.drawLine(pos - QPointF(0, 5), pos + QPointF(0, 5));
        p.drawText(pos + QPointF(12, 4), QString::number(q["charge"].toDouble(), 'g', 3));
    }
    if (!hbonds.isEmpty()) {
        p.setPen(QPen(QColor("#2a7bd6"), 1.6, Qt::DashLine));
        for (const auto &[h, acceptor] : hbonds) {
            if (!doc.contains(h) || !doc.contains(acceptor) || !shown(doc.atom(h)) ||
                !shown(doc.atom(acceptor)))
                continue;
            p.drawLine(project(doc.atom(h).position), project(doc.atom(acceptor).position));
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
    if (tool == "move" && !pressedAtom.isEmpty() && pressedButton == Qt::LeftButton &&
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
                 (pressModifiers & (Qt::ShiftModifier | Qt::ControlModifier)) && tool == "select")
            boxing = true;
        else if (tool != "move" || pressedAtom.isEmpty() || pressModifiers.testFlag(Qt::AltModifier) ||
                 pressedButton == Qt::MiddleButton) {
            // Trackball: turn about the screen's vertical and horizontal axes, whatever the
            // current orientation (no gimbal lock), and keep the matrix orthonormal.
            orientation =
                rotation(Vec::UnitY(), delta.x() * .6) * rotation(Vec::UnitX(), delta.y() * .6) * orientation;
            orientation = Eigen::Quaterniond(orientation).normalized().toRotationMatrix();
            emit orientationChanged();
        } else if (pressedButton == Qt::LeftButton) {
            // The atoms follow the cursor while dragging; the edit itself happens on release. A
            // drag that includes a fixed atom is refused then, so nothing moves meanwhile either.
            if (!dragOrigin)
                dragOrigin = doc;
            doc = *dragOrigin;
            const auto moves = [&](const Atom &a) { return dragAtoms.contains(a.id) || a.id == pressedAtom; };
            if (std::none_of(doc.atoms.begin(), doc.atoms.end(),
                             [&](const Atom &a) { return a.fixed && moves(a); })) {
                const Vec shift = worldPosition(e->position()) - worldPosition(press);
                for (auto &a : doc.atoms)
                    if (moves(a))
                        a.position += shift;
            }
            moleculeDirty = true;
        }
    }
    last = e->position();
    scheduleRender();
}
void MoleculeView::mouseReleaseEvent(QMouseEvent *e) {
    auto button = pressedButton;
    pressedButton = Qt::NoButton;
    if (dragOrigin) { // The edit replaces the preview (or the document stays as it was).
        doc = *dragOrigin;
        dragOrigin.reset();
        moleculeDirty = true;
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
            scheduleRender();
        } else if (button == Qt::LeftButton && tool == "move" && !pressedAtom.isEmpty() &&
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
