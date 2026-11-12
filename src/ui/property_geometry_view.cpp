// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/property_geometry_view.h"
#include "foundation/i18n.h"
#include "model/settings.h"
#include "results/field_analysis.h"
#include "results/result_properties.h"
#include "ui/editor_context.h"
#include "ui/molecule_view.h"
#include <QtWidgets>
#include <cmath>
#include <numbers>
namespace mf {
namespace {
QColor fieldColor(double value, double low, double high) {
    const double x = std::clamp((value - low) / (high - low), 0., 1.);
    // Low red, midpoint white, high blue; identical scale for the surface and slice.
    return x < .5 ? QColor::fromRgbF(.85 + .3 * x, .15 + 1.7 * x, .15 + 1.7 * x)
                  : QColor::fromRgbF(1. - 1.7 * (x - .5), 1. - 1.3 * (x - .5), 1. - .3 * (x - .5));
}
} // namespace
class FieldSlice : public QWidget {
  public:
    QImage image;
    QString label;
    double low = 0, high = 1;
    explicit FieldSlice(QWidget *parent) : QWidget(parent) {
        setMinimumHeight(180);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), palette().base());
        if (image.isNull())
            return;
        const QRect area(8, 8, std::max(1, width() - 90), std::max(1, height() - 40));
        p.drawImage(area, image);
        for (int y = 0; y < area.height(); ++y)
            p.fillRect(QRect(area.right() + 10, area.top() + y, 16, 1),
                       fieldColor(high - (high - low) * y / std::max(1, area.height() - 1), low, high));
        p.setPen(palette().text().color());
        p.drawText(area.right() + 30, area.top() + 12, QString::number(high, 'g', 5));
        p.drawText(area.right() + 30, area.bottom(), QString::number(low, 'g', 5));
        p.drawText(QRect(8, height() - 28, width() - 16, 24), Qt::AlignLeft, label);
    }
};
PropertyGeometryView::PropertyGeometryView(UiServices &uiServices, QWidget *parent)
    : QWidget(parent), uiContext(uiServices), loader(this) {
    setObjectName("propertyGeometryView");
    auto *layout = new QVBoxLayout(this);
    view = new MoleculeView(rendererChoice(uiContext.preference(Preference::viewRenderer)));
    view->setObjectName("propertyGeometryMolecule");
    view->setAngles(30, 20, 0);
    layout->addWidget(view, 2);
    source = new QLabel;
    source->setWordWrap(true);
    source->setTextFormat(Qt::PlainText);
    layout->addWidget(source);
    tensorControls = new QWidget;
    auto *tf = new QFormLayout(tensorControls);
    dipole = new QCheckBox(tr("双極子（負 → 正）"));
    dipole->setChecked(true);
    dipoleScale = new QDoubleSpinBox;
    dipoleScale->setRange(.01, 100);
    dipoleScale->setValue(.5);
    tensor = new QCheckBox(tr("分極率の主軸・楕円体"));
    tensor->setChecked(true);
    tensorScale = new QDoubleSpinBox;
    tensorScale->setRange(.01, 10);
    tensorScale->setValue(.3);
    tf->addRow(dipole);
    tf->addRow(tr("双極子の倍率 / Å・D⁻¹"), dipoleScale);
    tf->addRow(tensor);
    tf->addRow(tr("楕円体の倍率 / Å・(a.u.)⁻½"), tensorScale);
    layout->addWidget(tensorControls);
    fieldControls = new QWidget;
    auto *ff = new QFormLayout(fieldControls);
    auto *open = new QPushButton(tr("色付け用CUBEを開く…"));
    open->setObjectName("propertyColorField");
    auto *cancel = new QPushButton(tr("色付け読込を中止"));
    ff->addRow(open, cancel);
    units = new QComboBox;
    units->addItems({tr("ファイルの単位"), "ESP / Eh·e⁻¹", "|E| / Eh·(e·a₀)⁻¹"});
    ff->addRow(tr("格子値の単位（生成条件で確認）"), units);
    low = new QDoubleSpinBox;
    high = new QDoubleSpinBox;
    for (auto *n : {low, high}) {
        n->setDecimals(6);
        n->setRange(-1e9, 1e9);
    }
    low->setValue(-.05);
    high->setValue(.05);
    low->setObjectName("propertyColorMinimum");
    high->setObjectName("propertyColorMaximum");
    ff->addRow(tr("色尺度の下限（赤）"), low);
    ff->addRow(tr("色尺度の上限（青）"), high);
    axis = new QComboBox;
    axis->addItems({tr("格子軸1を固定"), tr("格子軸2を固定"), tr("格子軸3を固定")});
    plane = new QSpinBox;
    ff->addRow(axis, plane);
    layout->addWidget(fieldControls);
    slice = new FieldSlice(this);
    layout->addWidget(slice, 1);
    message = new QLabel;
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto *png = new QPushButton("PNG…");
    layout->addWidget(png);
    connect(png, &QPushButton::clicked, this, [this] {
        uiContext.safe([this] {
            const auto path = QFileDialog::getSaveFileName(this, tr("画像を保存"), {}, "PNG (*.png)");
            if (!path.isEmpty()) {
                uiContext.checkMcpPath(path);
                view->renderNow();
                if (!grab().save(path))
                    fail(tr("画像を保存できません。"));
            }
        });
    });
    connect(open, &QPushButton::clicked, this, [this] {
        uiContext.safe([this] {
            const auto path =
                QFileDialog::getOpenFileName(this, tr("色付け用CUBEを開く"), {}, "CUBE (*.cube *.cub)");
            if (!path.isEmpty())
                openColorField(path);
        });
    });
    connect(cancel, &QPushButton::clicked, &loader, &ResultLoader::cancel);
    connect(&loader, &ResultLoader::failed, message, &QLabel::setText);
    connect(&loader, &ResultLoader::loaded, this, [this](ResultData r) {
        try {
            validateFieldOverlay(data, r);
            field = std::move(r);
            refresh();
        } catch (const std::exception &e) {
            message->setText(QString::fromUtf8(e.what()));
        }
    });
    for (auto *n : {low, high, dipoleScale, tensorScale})
        connect(n, &QDoubleSpinBox::valueChanged, this, &PropertyGeometryView::refresh);
    for (auto *b : {dipole, tensor})
        connect(b, &QCheckBox::toggled, this, &PropertyGeometryView::refresh);
    connect(units, &QComboBox::currentIndexChanged, this, &PropertyGeometryView::refresh);
    connect(axis, &QComboBox::currentIndexChanged, this, &PropertyGeometryView::refresh);
    connect(plane, &QSpinBox::valueChanged, this, &PropertyGeometryView::refresh);
}
void PropertyGeometryView::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (fitPending)
        QTimer::singleShot(0, this, [this] {
            view->fit();
            fitPending = false;
        });
}
void PropertyGeometryView::setResult(const ResultData &r) {
    fitPending = !isVisible();
    loader.cancel();
    data = r;
    field = {};
    const bool cube = r.format == "CUBE";
    fieldControls->setVisible(cube);
    slice->setVisible(cube);
    tensorControls->setVisible(!cube);
    if (!r.frames.isEmpty())
        view->setDocument(r.frames.last().molecule);
    else
        view->setDocument({});
    view->setSurface(r.surface);
    view->fit();
    refresh();
}
void PropertyGeometryView::openColorField(const QString &path) {
    if (data.format != "CUBE")
        fail(tr("色付けには原子座標とCUBE格子が必要です。"));
    uiContext.checkMcpPath(path);
    if (!QStringList{"cube", "cub"}.contains(QFileInfo(path).suffix().toLower()))
        fail(tr("色付けには原子座標とCUBE格子が必要です。"));
    ResultLoadRequest request;
    request.path = path;
    loader.start(request);
}
void PropertyGeometryView::refresh() {
    view->setArrows({});
    view->setOverlayLines({});
    try {
        if (data.format == "CUBE") {
            if (!(low->value() < high->value()))
                fail(tr("色尺度の下限を上限より小さくしてください。"));
            const auto &f = field.source.isEmpty() ? data : field;
            QVector<QColor> colors;
            if (!field.source.isEmpty()) {
                colors.reserve(data.surface.size());
                for (const auto &t : data.surface) {
                    const auto v = sampleField(f, (t.a + t.b + t.c) / 3);
                    if (!v)
                        fail(tr("色付け用の格子が等値面全体を覆っていません。"));
                    colors.append(fieldColor(*v, low->value(), high->value()));
                }
            }
            view->setSurface(data.surface, colors);
            const int k = axis->currentIndex(), i = (k + 1) % 3, j = (k + 2) % 3;
            const QSignalBlocker block(plane);
            plane->setRange(0, std::max(0, f.grid[k] - 1));
            QImage picture(f.grid[i], f.grid[j], QImage::Format_RGB32);
            for (int y = 0; y < f.grid[j]; ++y)
                for (int x = 0; x < f.grid[i]; ++x) {
                    std::array<int, 3> q{};
                    q[k] = plane->value();
                    q[i] = x;
                    q[j] = y;
                    const qsizetype index = (qsizetype(q[0]) * f.grid[1] + q[1]) * f.grid[2] + q[2];
                    if (index >= f.values.size())
                        fail(tr("色付けには原子座標とCUBE格子が必要です。"));
                    picture.setPixelColor(x, f.grid[j] - 1 - y,
                                          fieldColor(f.values[index], low->value(), high->value()));
                }
            slice->image = picture;
            slice->low = low->value();
            slice->high = high->value();
            slice->label = tr("横: 格子軸%1・縦: 格子軸%2（格子番号） / %3")
                               .arg(i + 1)
                               .arg(j + 1)
                               .arg(units->currentText());
            slice->update();
            source->setText(tr("等値面: %1\n色・断面: %2")
                                .arg(QFileInfo(data.source).fileName(), QFileInfo(f.source).fileName()));
            message->setText(tr("同じ座標系の格子を三線形補間します。単位の選択はラベルのみです。電場の大きさ"
                                "からベクトル・力線は生成しません。"));
            return;
        }
        view->setSurface({});
        if (data.frames.isEmpty())
            return;
        const auto d = resultDipole(data);
        const auto p = data.properties["polarizability"].toObject();
        if (dipole->isChecked() && tensor->isChecked() && d && !p.isEmpty() &&
            d->geometry != p["geometry"].toString())
            fail(tr("双極子と分極率の座標が異なります。一方だけを選択してください。"));
        const auto geometry = tensor->isChecked() && !p.isEmpty() ? p["geometry"].toString()
                              : d                                 ? d->geometry
                                                                  : QString();
        const auto molecule = geometry.isEmpty() ? data.frames.last().molecule : Document::fromXyz(geometry);
        view->setDocument(molecule);
        Vec center = Vec::Zero();
        const auto &atoms = molecule.atoms;
        for (const auto &a : atoms)
            center += a.position;
        center /= std::max<qsizetype>(1, atoms.size());
        QVector<std::pair<Vec, Vec>> arrows, lines;
        QStringList values;
        if (dipole->isChecked() && d) {
            const Vec vector = d->au;
            arrows.append({center, vector * 2.541746473 * dipoleScale->value()});
            values << tr("双極子: %1 D").arg(vector.norm() * 2.541746473, 0, 'g', 7);
        }
        if (tensor->isChecked() && !p.isEmpty()) {
            const auto principal = polarizabilityAxes(p["tensor"].toArray());
            const Vec radii = principal.values.cwiseSqrt() * tensorScale->value();
            for (int k = 0; k < 3; ++k) {
                const Vec v = principal.axes.col(k) * radii[k];
                lines.append({center - v, center + v});
                const int i = (k + 1) % 3, j = (k + 2) % 3;
                for (int n = 0; n < 96; ++n) {
                    const auto at = [&](int step) -> Vec {
                        const double angle = step * 2 * std::numbers::pi / 96;
                        return center + principal.axes.col(i) * radii[i] * std::cos(angle) +
                               principal.axes.col(j) * radii[j] * std::sin(angle);
                    };
                    lines.append({at(n), at(n + 1)});
                }
            }
            values << tr("分極率主値: %1, %2, %3 a.u. / 応答周波数: %4 a.u.")
                          .arg(principal.values[0], 0, 'g', 6)
                          .arg(principal.values[1], 0, 'g', 6)
                          .arg(principal.values[2], 0, 'g', 6)
                          .arg(p["frequency"].toDouble(), 0, 'g', 6);
        }
        view->setArrows(arrows);
        view->setOverlayLines(lines);
        source->setText(values.join('\n'));
        message->setText(
            tr("計算の座標系を保持し、矢印・楕円体を分子中心に置きます。矢印は物理学の規約で負から正へ向きま"
               "す。荷電分子の双極子は計算の原点に依存します。楕円体半径は分極率主値の平方根に比例します。"));
    } catch (const std::exception &e) {
        view->setSurface(data.surface);
        slice->image = {};
        slice->update();
        message->setText(QString::fromUtf8(e.what()));
    }
}
} // namespace mf
