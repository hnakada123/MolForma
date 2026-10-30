// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include "results/results.h"
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
    void setSurface(const QVector<SurfaceTriangle> &triangles);
    void setSurfaceStyle(bool positive, bool negative, int opacity);
    void setCamera(const Mat &rotation, const Vec &center, double scale, const QPointF &pan);
    bool draw();
    // Diagnostics also allow regression tests to detect accidental mesh reconstruction.
    quint64 surfaceBuildCount() const;
    bool depthPeelingUsed() const;
    QPointF screenPosition(const Vec &position) const;

  private:
    struct Scene;
    std::unique_ptr<Scene> scene;
};
} // namespace mf
