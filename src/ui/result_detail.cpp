// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/result_detail.h"
#include "foundation/i18n.h"
#include "ui/editor_context.h"
#include "ui/figure_export.h"
#include "ui/result_charts.h"
#include <QtWidgets>
#include <cmath>

namespace mf::result_detail {
namespace {
// Keeps the chosen row in sight as a table gets lower (the run panel opens below the tabs). A row
// that the user scrolled away stays away, and the sideways position stays.
class ChosenRowInSight : public QObject {
  public:
    explicit ChosenRowInSight(QTableWidget *table) : QObject(table), view(table) {
        table->viewport()->installEventFilter(this);
    }
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() != QEvent::Resize)
            return false;
        const auto *resize = static_cast<QResizeEvent *>(event);
        const int before = resize->oldSize().height(), after = resize->size().height();
        const auto row = view->visualRect(view->currentIndex());
        if (after < before && row.isValid() && row.top() < before && row.bottom() >= after)
            // Once the table has its new scroll range.
            QMetaObject::invokeMethod(
                this,
                [this] {
                    const int sideways = view->horizontalScrollBar()->value();
                    view->scrollTo(view->currentIndex(), QAbstractItemView::PositionAtCenter);
                    view->horizontalScrollBar()->setValue(sideways);
                },
                Qt::QueuedConnection);
        return false;
    }

  private:
    QTableWidget *view;
};
// A button as large as the labels laid out in it.
class TileButton : public QPushButton {
  public:
    QSize sizeHint() const override {
        return layout() ? layout()->sizeHint() : QPushButton::sizeHint();
    }
    QSize minimumSizeHint() const override {
        return layout() ? layout()->minimumSize() : QPushButton::minimumSizeHint();
    }
    bool hasHeightForWidth() const override {
        return layout() && layout()->hasHeightForWidth();
    }
    int heightForWidth(int width) const override {
        return layout() ? layout()->totalHeightForWidth(width) : QPushButton::heightForWidth(width);
    }
};
} // namespace
QTableWidget *table(const QStringList &labels, const QString &name) {
    auto *t = new QTableWidget(0, labels.size());
    t->setObjectName(name);
    t->setProperty("plain", true);
    t->setShowGrid(false);
    t->setHorizontalHeaderLabels(labels);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->horizontalHeader()->setStretchLastSection(true);
    t->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    t->verticalHeader()->setDefaultSectionSize(26);
    new ChosenRowInSight(t);
    return t;
}
void row(QTableWidget *t, int index, const QStringList &values) {
    static const auto digits = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (int i = 0; i < values.size(); ++i) {
        auto *item = new QTableWidgetItem(values[i]);
        bool number = false;
        values[i].toDouble(&number);
        if (number) {
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            item->setFont(digits);
        }
        t->setItem(index, i, item);
    }
}
QString real(double value) {
    return std::isfinite(value) ? QString::number(value, 'g', 12) : "—";
}
QString fixed(double value, int decimals) {
    return std::isfinite(value) ? QString::number(value, 'f', decimals) : "—";
}
QString shownReal(double value) {
    return real(value).replace('-', "−");
}
QString shownFixed(double value, int decimals) {
    return fixed(value, decimals).replace('-', "−");
}
QWidget *summaryTile(const QString &title, const QString &value, const QString &caption, const QString &tone,
                     bool button) {
    QWidget *tile = button ? static_cast<QWidget *>(new TileButton) : new QFrame;
    tile->setProperty("tile", true);
    tile->setProperty("tone", tone);
    auto *layout = new QVBoxLayout(tile);
    layout->setContentsMargins(12, 9, 12, 10);
    layout->setSpacing(2);
    for (const auto &[role, text] : {std::pair{"title", title}, {"value", value}, {"caption", caption}}) {
        if (text.isEmpty())
            continue;
        auto *label = new QLabel(text);
        label->setProperty("role", role);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setWordWrap(QString(role) != "value");
        if (QString(role) == "value") {
            auto font = label->font();
            font.setPointSizeF(font.pointSizeF() * 1.35);
            font.setBold(true);
            label->setFont(font);
        }
        layout->addWidget(label);
    }
    tile->setAccessibleName(QStringList{title, value, caption}.join(' ').trimmed());
    tile->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    return tile;
}
const char *const TileRules = R"(
    QPushButton[tile="true"], QFrame[tile="true"] { background: @base; border: 1px solid @line;
                                                    border-radius: 8px; text-align: left; padding: 0; }
    QPushButton[tile="true"]:hover { background: @hover; }
    QPushButton[tile="true"]:focus { border-color: @accent; }
    QLabel[role="title"], QLabel[role="caption"], QLabel[role="muted"] { color: @muted; }
    QLabel[role="value"] { color: @text; }
    *[tone="good"] QLabel[role="value"] { color: @good; }
    *[tone="warn"] QLabel[role="value"] { color: @candidateText; }
    *[tone="warn"][tile="true"] { background: @candidate; border-color: @candidateLine; }
    *[tone="bad"] QLabel[role="value"] { color: @error; }
    QLabel[chip="current"] { background: @tint; color: @accentText; border-radius: 8px; padding: 1px 8px; }
)";
void setFigureExport(ChartView *chart, UiServices &ui, QWidget *parent) {
    chart->setExport([chart, &ui, parent] {
        ui.safe([&] {
            const auto path = askFigurePath(parent, tr("グラフを保存"), chart->objectName().mid(7).toLower());
            if (path.isEmpty())
                return;
            ui.checkMcpPath(path);
            saveFigure(chart, path);
        });
    });
}
} // namespace mf::result_detail
