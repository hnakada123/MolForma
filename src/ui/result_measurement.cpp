// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/result_measurement.h"
#include "foundation/error.h"
#include "model/geometry.h"
#include "ui/editor_context.h"
#include "ui/molecule_view.h"
#include <QtWidgets>

namespace mf {
ResultMeasurement::ResultMeasurement(MoleculeView *moleculeView, UiServices &uiServices, QWidget *parent)
    : QFrame(parent), view(moleculeView), uiContext(uiServices) {
    // Distance, angle or dihedral of the atoms clicked in order, and the ones kept on the view.
    setObjectName("resultsMeasureCard");
    setMaximumWidth(340);
    auto *measuring = new QVBoxLayout(this);
    measuring->setContentsMargins(12, 8, 12, 9);
    measuring->setSpacing(6);
    measurementLabel = new QLabel(tr("原子をクリックすると距離・角度・二面角を測定します（2〜4原子）。"));
    measurementLabel->setObjectName("resultsMeasurement");
    measurementLabel->setWordWrap(true);
    measurementLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    measuring->addWidget(measurementLabel);
    auto *measureActions = new QHBoxLayout;
    measureActions->setSpacing(4);
    pin = new QPushButton(tr("画面に表示"));
    pin->setObjectName("resultsPinMeasurement");
    pin->setToolTip(tr("選んだ原子の計測を3D表示に残します。"));
    clearMeasure = new QPushButton(tr("消去"));
    clearMeasure->setObjectName("resultsClearMeasurement");
    clearMeasure->setToolTip(tr("選択と、画面に表示中の計測をすべて消去します。"));
    measureActions->addWidget(pin);
    measureActions->addWidget(clearMeasure);
    measureActions->addStretch();
    measuring->addLayout(measureActions);
    hide();
    connect(view, &MoleculeView::atomClicked, this, [this](const QString &id) { pickAtom(id); });
    connect(pin, &QPushButton::clicked, this, [this] {
        const auto &d = view->documentRef();
        Ids ids;
        for (int n : picked)
            if (n < d.atoms.size())
                ids << d.atoms[n].id;
        bool valid = false;
        uiContext.safe([&] {
            if (picked.size() < 2 || ids.size() != picked.size())
                fail(tr("2〜4原子をクリックしてから表示してください。"));
            measure(d, ids); // Rejects coincident or collinear atoms before pinning.
            valid = true;
        });
        if (valid) {
            pinned << picked;
            picked.clear();
            setDocument(Document(view->documentCopy()));
        }
    });
    connect(clearMeasure, &QPushButton::clicked, this, [this] {
        picked.clear();
        pinned.clear();
        setDocument(Document(view->documentCopy()));
    });
}
QList<QWidget *> ResultMeasurement::controls() const {
    return {pin, clearMeasure};
}
void ResultMeasurement::setPicked(const QVector<int> &atoms) {
    picked = atoms;
}
void ResultMeasurement::clear() {
    picked.clear();
    pinned.clear();
}
void ResultMeasurement::setDocument(const Document &d) {
    const auto ids = [&](const QVector<int> &numbers) {
        Ids list;
        for (int n : numbers)
            if (n >= 0 && n < d.atoms.size())
                list << d.atoms[n].id;
        return list.size() == numbers.size() ? list : Ids();
    };
    QVector<Ids> monitors;
    for (const auto &numbers : pinned)
        if (const auto list = ids(numbers); !list.isEmpty())
            monitors << list;
    view->setDocument(d);
    view->setSelection(ids(picked));
    view->setMonitors(monitors);
    QString text = pinned.isEmpty() ? tr("原子をクリックすると距離・角度・二面角を測定します（2〜4原子）。")
                                    : tr("画面に表示中の計測: %n件。原子をクリックすると続けて測定します。",
                                         nullptr, int(pinned.size()));
    if (!picked.isEmpty()) {
        QStringList labels;
        for (int n : picked)
            labels << (n < d.atoms.size() ? d.atoms[n].element : QString("?")) + QString::number(n + 1);
        text = tr("選択: %1").arg(labels.join(" → "));
        if (picked.size() >= 2 && !ids(picked).isEmpty())
            try {
                const double value = measure(d, ids(picked));
                text += picked.size() == 2   ? tr("　距離 %1 Å").arg(value, 0, 'f', 4)
                        : picked.size() == 3 ? tr("　角度 %1°").arg(value, 0, 'f', 2)
                                             : tr("　二面角 %1°").arg(value, 0, 'f', 2);
            } catch (const std::exception &e) {
                text = tr("選択: %1　%2").arg(labels.join(" → "), QString::fromUtf8(e.what()));
            }
    }
    measurementLabel->setText(text);
    setVisible(!picked.isEmpty() || !pinned.isEmpty());
}
void ResultMeasurement::pickAtom(const QString &id) {
    const auto &d = view->documentRef();
    if (!d.contains(id))
        return;
    const int n = d.index(id);
    emit atomClicked(n);
    if (picked.contains(n))
        picked.removeAll(n);
    else {
        if (picked.size() == 4)
            picked.clear();
        picked << n;
    }
    setDocument(Document(d));
}
} // namespace mf
