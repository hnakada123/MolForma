#include "ui/periodic_table.h"
#include "model/document.h"
#include <QtWidgets>
#include <algorithm>

namespace mf {
namespace {
class ElementButton : public QAbstractButton {
  public:
    explicit ElementButton(const Element &element, QWidget *parent) : QAbstractButton(parent), e(element) {
        setObjectName("periodicElement_" + e.symbol);
        setText(e.symbol);
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setMinimumSize(34, 46); // 18 groups fit beside each other in a 760-pixel dialog.
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        const auto description =
            QString("%1 · %2 / %3\n原子番号 %4").arg(e.symbol, e.nameJa, e.nameEn).arg(e.number);
        setToolTip(description);
        setAccessibleName(description);
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto base = palette().color(QPalette::Base);
        QColor fill = QColor::fromRgbF(base.redF() * .85 + e.color.redF() * .15,
                                       base.greenF() * .85 + e.color.greenF() * .15,
                                       base.blueF() * .85 + e.color.blueF() * .15);
        const auto highlight = palette().color(QPalette::Highlight);
        if (isDown() || isChecked())
            fill = highlight;
        const bool active = isChecked() || isDown();
        const auto ink = palette().color(active ? QPalette::HighlightedText : QPalette::Text);
        if (!isEnabled())
            p.setOpacity(.25);
        p.setBrush(fill);
        p.setPen(QPen(hasFocus() || underMouse() || active ? highlight : palette().color(QPalette::Mid),
                      hasFocus() || active ? 2 : 1));
        p.drawRoundedRect(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5), 6, 6);
        QFont numberFont(font());
        numberFont.setPixelSize(11);
        p.setFont(numberFont);
        p.setPen(ink);
        p.drawText(rect().adjusted(6, 4, -4, -4), Qt::AlignLeft | Qt::AlignTop, QString::number(e.number));
        QFont symbolFont(font());
        symbolFont.setPixelSize(19);
        symbolFont.setWeight(QFont::DemiBold);
        p.setFont(symbolFont);
        p.drawText(rect().adjusted(2, 16, -2, -4), Qt::AlignCenter, e.symbol);
    }

  private:
    const Element &e;
};

QPair<int, int> position(int z) {
    if (z == 1)
        return {1, 1};
    if (z == 2)
        return {1, 18};
    if (z <= 10)
        return {2, z <= 4 ? z - 2 : z + 8};
    if (z <= 18)
        return {3, z <= 12 ? z - 10 : z};
    if (z <= 36)
        return {4, z - 18};
    if (z <= 54)
        return {5, z - 36};
    if (z <= 56)
        return {6, z - 54};
    if (z <= 71)
        return {9, z - 54};
    if (z <= 86)
        return {6, z - 68};
    if (z <= 88)
        return {7, z - 86};
    if (z <= 103)
        return {10, z - 86};
    return {7, z - 100};
}
} // namespace

PeriodicTableDialog::PeriodicTableDialog(const QString &current, const QString &query, QWidget *parent)
    : QDialog(parent), selected(current) {
    setObjectName("periodicTableDialog");
    setWindowTitle("周期表から原子を選択 — H〜Og");
    auto *layout = new QVBoxLayout(this);
    auto *heading = new QLabel("元素をクリックして選択し、分子画面に配置します。", this);
    layout->addWidget(heading);
    search = new QLineEdit(this);
    search->setObjectName("periodicTableSearch");
    search->setPlaceholderText("元素記号・日本語名・英語名・原子番号で検索");
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *table = new QWidget(scroll);
    auto *grid = new QGridLayout(table);
    grid->setObjectName("periodicTableGrid");
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setSpacing(4);
    auto label = [&](const QString &text, int row, int column, int span = 1) {
        auto *widget = new QLabel(text, table);
        widget->setAlignment(Qt::AlignCenter);
        grid->addWidget(widget, row, column, 1, span);
    };
    for (int group = 1; group <= 18; ++group) {
        label(QString::number(group), 0, group);
        grid->setColumnStretch(group, 1);
    }
    for (int period = 1; period <= 7; ++period)
        label(QString::number(period), period, 0);
    label("57–71\n↓", 6, 3);
    label("89–103\n↓", 7, 3);
    label("ランタノイド", 9, 0, 3);
    label("アクチノイド", 10, 0, 3);
    grid->setRowMinimumHeight(8, 12);
    for (const auto &e : elements()) {
        auto *button = new ElementButton(e, table);
        const auto [row, column] = position(e.number);
        button->setProperty("periodicRow", row);
        button->setProperty("periodicColumn", column);
        grid->addWidget(button, row, column);
        button->setChecked(e.symbol == current);
        button->installEventFilter(this);
        buttons << button;
        connect(button, &QAbstractButton::clicked, this, [this, symbol = e.symbol] {
            selected = symbol;
            accept();
        });
    }
    scroll->setWidget(table);
    scroll->setObjectName("periodicTableScroll");
    layout->addWidget(scroll, 1);
    details = new QLabel(this);
    details->setObjectName("periodicElementDetails");
    details->setMinimumHeight(details->fontMetrics().height() * 2 + 6);
    layout->addWidget(details);
    auto *footer = new QHBoxLayout;
    matches = new QLabel(this);
    footer->addWidget(matches);
    footer->addStretch();
    auto *close = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    close->button(QDialogButtonBox::Cancel)->setText("キャンセル");
    close->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    footer->addWidget(close);
    layout->addLayout(footer);
    connect(search, &QLineEdit::textChanged, this, &PeriodicTableDialog::filter);
    connect(search, &QLineEdit::returnPressed, this, [this] {
        QAbstractButton *match = nullptr;
        for (auto *button : buttons)
            if (button->isEnabled()) {
                if (match)
                    return;
                match = button;
            }
        if (match)
            match->click();
    });
    describe(current);
    search->setText(query);
    filter();
    search->setFocus();
    // All 18 groups stay visible: only the periods may need scrolling on small screens.
    const auto margins = layout->contentsMargins();
    setMinimumWidth(table->minimumSizeHint().width() + margins.left() + margins.right() +
                    style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 4);
    resize(QSize(990, 730).boundedTo(screen()->availableGeometry().size() - QSize(40, 60)));
}

void PeriodicTableDialog::describe(const QString &symbol) {
    const auto &e = element(symbol);
    details->setText(QString("%1  %2 / %3\n原子番号 %4").arg(e.symbol, e.nameJa, e.nameEn).arg(e.number));
}

void PeriodicTableDialog::filter() {
    const auto query = search->text().trimmed();
    bool numeric = false;
    const int number = query.toInt(&numeric);
    const bool exactSymbol = std::any_of(elements().begin(), elements().end(), [&](const auto &e) {
        return e.symbol.compare(query, Qt::CaseInsensitive) == 0;
    });
    int count = 0;
    QString last;
    for (auto *button : buttons) {
        const auto symbol = button->text();
        const auto &e = element(symbol);
        const bool match =
            query.isEmpty() ||
            (numeric ? number == e.number
             : exactSymbol
                 ? e.symbol.compare(query, Qt::CaseInsensitive) == 0
                 : (e.symbol + ' ' + e.nameJa + ' ' + e.nameEn).contains(query, Qt::CaseInsensitive));
        button->setEnabled(match);
        count += match;
        if (match)
            last = symbol;
    }
    matches->setText(query.isEmpty() ? "118 元素 · H〜Og" : QString("%1 / 118 元素が一致").arg(count));
    // The details follow the search when it names one element (Enter chooses it).
    describe(count == 1 ? last : selected);
}

bool PeriodicTableDialog::eventFilter(QObject *watched, QEvent *event) {
    auto *button = qobject_cast<QAbstractButton *>(watched);
    if (!button)
        return QDialog::eventFilter(watched, event);
    if (event->type() == QEvent::Enter || event->type() == QEvent::FocusIn)
        describe(button->text());
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave)
        button->update();
    if (event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            button->click();
            return true;
        }
        const int dx = key == Qt::Key_Left ? -1 : key == Qt::Key_Right ? 1 : 0;
        const int dy = key == Qt::Key_Up ? -1 : key == Qt::Key_Down ? 1 : 0;
        if (dx || dy) {
            int row = button->property("periodicRow").toInt();
            int column = button->property("periodicColumn").toInt();
            while ((row += dy) >= 1 && row <= 10 && (column += dx) >= 1 && column <= 18)
                for (auto *next : buttons)
                    if (next->isEnabled() && next->property("periodicRow").toInt() == row &&
                        next->property("periodicColumn").toInt() == column) {
                        next->setFocus(Qt::TabFocusReason);
                        return true;
                    }
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}
} // namespace mf
