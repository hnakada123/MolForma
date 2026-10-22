#pragma once
#include "document.h"
#include <QPointF>
#include <QWidget>
namespace mf {
class MoleculeView : public QWidget {
    Q_OBJECT
  public:
    explicit MoleculeView(QWidget *parent = nullptr);
    void setDocument(const Document &d);
    void setSelection(const Ids &ids);
    Ids selection() const {
        return selected;
    }
    void fit();
    void viewFrom(int axis);
    void setAngles(double yaw, double pitch, double roll);
    void setTool(const QString &t) {
        tool = t;
    }
    void setShowHydrogens(bool value) {
        showHydrogens = value;
        update();
    }
    void setShowIndices(bool value) {
        showIndices = value;
        update();
    }
    void setColorMode(const QString &mode) {
        colorMode = mode;
        update();
    }
    void setDepth(double value) {
        depth = value;
    }
    void setAminoHandles(bool value) {
        aminoHandles = value;
        update();
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
    Mat camera() const;
    QPointF project(const Vec &p) const;
    QString hitAtom(const QPointF &p) const;
    const Bond *hitBond(const QPointF &p) const;
    int hitMarker(const QPointF &p) const;
    QColor atomColor(const Atom &a) const;
};
} // namespace mf
Q_DECLARE_METATYPE(mf::Vec)
