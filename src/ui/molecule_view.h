#pragma once
#include "model/document.h"
#include "results/results.h"
#include <QHash>
#include <QPointF>
#include <QTimer>
#include <QWidget>
class QLabel;
class QPainter;
namespace mf {
class VtkMoleculeRenderer;
class MoleculeView : public QWidget {
    Q_OBJECT
  public:
    explicit MoleculeView(QWidget *parent = nullptr);
    static void configureDefaultSurfaceFormat();
    QString rendererName() const;
    void setDocument(const Document &d);
    void setSelection(const Ids &ids);
    void setSurface(const QVector<SurfaceTriangle> &triangles);
    void setSurfaceStyle(bool positive, bool negative, int opacity);
    // Arrows drawn over the molecule: (origin, vector) in Å, e.g. normal-mode displacements.
    void setArrows(const QVector<std::pair<Vec, Vec>> &list) {
        arrows = list;
        scheduleRender();
    }
    // Renders pending changes at once (VTK), so the widget can be grabbed as an image.
    void renderNow();
    // "ball_and_stick" (default), "stick", "spacefill" or "wireframe".
    void setStyle(const QString &style);
    QString style() const {
        return displayStyle;
    }
    // Atoms left out of the picture (view only; they stay in the molecule and calculations).
    void setHidden(const QSet<QString> &ids);
    const QSet<QString> &hiddenAtoms() const {
        return hidden;
    }
    // Measurements kept on screen: 2–4 ordered atom IDs each, drawn with their current value.
    void setMonitors(const QVector<Ids> &list) {
        monitors = list;
        scheduleRender();
    }
    QVector<Ids> monitorList() const {
        return monitors;
    }
    void setHydrogenBonds(bool on);
    // Hydrogen bonds X–H···Y (X, Y = N, O, F) found by distance and angle in the shown molecule.
    QVector<std::pair<QString, QString>> hydrogenBonds() const {
        return hbonds;
    }
    void setEmptyMessage(const QString &message) {
        emptyMessage = message;
        scheduleRender();
    }
    Ids selection() const {
        return selected;
    }
    const Document &documentRef() const {
        return doc;
    }
    Document documentCopy() const {
        return doc;
    }
    void fit();
    void viewFrom(int axis);
    void setAngles(double yaw, double pitch, double roll);
    void setTool(const QString &t) {
        tool = t;
    }
    void setShowHydrogens(bool value) {
        showHydrogens = value;
        moleculeDirty = true;
        scheduleRender();
    }
    void setShowIndices(bool value) {
        showIndices = value;
        scheduleRender();
    }
    void setColorMode(const QString &mode) {
        colorMode = mode;
        refreshColors();
        scheduleRender();
    }
    void setDepth(double value) {
        depth = value;
    }
    void setAminoHandles(bool value) {
        aminoHandles = value;
        scheduleRender();
    }
    QPointF atomScreenPosition(const QString &id) const;
    Vec worldPosition(const QPointF &p) const;
    QSize minimumSizeHint() const override {
        return {280, 240};
    }
  signals:
    void atomClicked(QString id, Qt::KeyboardModifiers modifiers);
    void blankClicked(mf::Vec position);
    void bondClicked(QString a, QString b);
    void siteClicked(QString center, int site);
    void terminusClicked(QString residue, bool nTerminal);
    void atomDragged(QString id, mf::Vec delta);
    void rectangleSelected(mf::Ids ids, bool append);
    void templateDropped(QString key, QString atom, mf::Vec position, QString center, int site,
                         QString residue, bool nTerminal);
    void contextRequested(QPoint globalPosition, QString atom);
    void shortcut(int key);

  protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

  private:
    struct Marker {
        QPointF point;
        QString owner;
        int site = -1;
        bool n = false;
    };
    Document doc;
    Ids selected;
    QString tool = "select", colorMode = "element";
    Vec center = Vec::Zero();
    double scale = 65, yaw = 0, pitch = 0, roll = 0, depth = 0;
    QPointF pan, press, last;
    QString pressedAtom;
    bool showHydrogens = true, showIndices = true, aminoHandles = false, dragging = false, boxing = false;
    Qt::MouseButton pressedButton = Qt::NoButton;
    Qt::KeyboardModifiers pressModifiers;
    QVector<Marker> markers;
    QVector<SurfaceTriangle> surface;
    QVector<std::pair<Vec, Vec>> arrows;
    QString displayStyle = "ball_and_stick";
    QSet<QString> hidden;
    QVector<Ids> monitors;
    bool showHydrogenBonds = false;
    QVector<std::pair<QString, QString>> hbonds; // (hydrogen, acceptor)
    void findHydrogenBonds();
    bool shown(const Atom &a) const {
        return !hidden.contains(a.id);
    }
    double atomRadius(const Atom &a) const;
    bool positiveSurface = true, negativeSurface = true;
    int surfaceOpacity = 150;
    QString emptyMessage;
    VtkMoleculeRenderer *renderer = nullptr;
    QWidget *overlay = nullptr;
    QLabel *rendererStatus = nullptr;
    QTimer renderTimer;
    bool moleculeDirty = true, surfaceDirty = true;
    QVector<QColor> atomColors;
    QHash<QString, int> atomIndices;
    mutable Vec cachedAngles = Vec::Constant(std::numeric_limits<double>::infinity());
    mutable Mat cachedCamera = Mat::Identity();
    void scheduleRender();
    void renderScene();
    void refreshColors();
    void updateRendererStatus();
    void paintContents(QPainter &painter, bool overlayOnly);
    Mat camera() const;
    QPointF project(const Vec &p) const;
    QString hitAtom(const QPointF &p) const;
    const Bond *hitBond(const QPointF &p) const;
    int hitMarker(const QPointF &p) const;
    QColor atomColor(const Atom &a) const;
};
} // namespace mf
Q_DECLARE_METATYPE(mf::Vec)
