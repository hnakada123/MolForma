// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include "results/results.h"
#include <QImage>
#include <QVTKOpenGLNativeWidget.h>
#include <memory>

namespace mf {
// Retained scene owned by the GUI thread. Chemistry and meshing stay in the worker.
class VtkMoleculeRenderer : public QVTKOpenGLNativeWidget {
  public:
    explicit VtkMoleculeRenderer(QWidget *parent);
    ~VtkMoleculeRenderer() override;
    // trace: the line of the backbone style, pairs of atom indices.
    void setMolecule(const Document &document, const QVector<QColor> &colors, bool hydrogens,
                     const QString &style = "ball_and_stick", const QSet<QString> &hidden = {},
                     const QVector<std::pair<int, int>> &trace = {});
    // The same atoms (as the last setMolecule) at new positions: a drag or a vibration frame.
    void setPositions(const Document &document);
    void setSurface(const QVector<SurfaceTriangle> &triangles, const QVector<QColor> &colors = {});
    void setSurfaceStyle(bool positive, bool negative, int opacity);
    void setCamera(const Mat &rotation, const Vec &center, double scale, const QPointF &pan);
    bool draw();
    // The scene `scale` times the size of the view (rendered in tiles), on its background or a uniform
    // one: transparent pixels come from two renderings, on black and on white.
    QImage image(int scale, int background); // 0: as shown, 1: white, 2: transparent.
    // Diagnostics also allow regression tests to detect accidental mesh reconstruction.
    quint64 surfaceBuildCount() const;
    bool depthPeelingUsed() const;
    QPointF screenPosition(const Vec &position) const;

  private:
    struct Scene;
    std::unique_ptr<Scene> scene;
};
} // namespace mf
