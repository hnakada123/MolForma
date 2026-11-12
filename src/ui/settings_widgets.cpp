// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/settings_widgets.h"
#include "foundation/i18n.h"
#include "ui/chrome_layouts.h"
#include "ui/line_icons.h"
#include "ui/theme.h"
#include <QButtonGroup>
#include <QComboBox>
#include <QEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <algorithm>

namespace mf {
QPushButton *linkButton(const QString &text, const QString &objectName) {
    auto *b = new QPushButton(text);
    if (!objectName.isEmpty())
        b->setObjectName(objectName);
    b->setProperty("link", true);
    b->setIcon(lineIcon("arrow"));
    b->setLayoutDirection(Qt::RightToLeft); // The arrow after the text.
    b->setCursor(Qt::PointingHandCursor);
    b->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed); // As wide as its text, also in a form row.
    return b;
}
QPlainTextEdit *logView(const QString &objectName) {
    auto *text = new QPlainTextEdit;
    text->setObjectName(objectName);
    text->setReadOnly(true);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    text->setProperty("log", true);
    return text;
}
QToolButton *copyButton(const QString &objectName, const QString &toolTip) {
    auto *b = new QToolButton;
    b->setObjectName(objectName);
    b->setIcon(lineIcon("copy"));
    b->setProperty("flat", true);
    b->setToolTip(toolTip);
    b->setAccessibleName(toolTip);
    return b;
}

ChoiceButtons::ChoiceButtons(QComboBox *box, Look look, const QStringList &labels, QWidget *parent)
    : QFrame(parent), combo(box), other(new QLabel) {
    setProperty("choice", look == Segments ? "segments" : "chips");
    QLayout *row;
    if (look == Segments) {
        auto *h = new QHBoxLayout(this);
        h->setContentsMargins(2, 2, 2, 2);
        h->setSpacing(2);
        row = h;
        setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed); // As wide as its buttons.
    } else {
        row = new FlowLayout(this, 6, 6);
        row->setContentsMargins(0, 0, 0, 0);
    }
    combo->setParent(this);
    combo->hide();
    auto *group = new QButtonGroup(this);
    for (int i = 0; i < combo->count(); ++i) {
        auto *b = new QToolButton;
        b->setObjectName(combo->objectName() + '_' + combo->itemData(i).toString());
        b->setText(labels.value(i, combo->itemText(i)));
        if (b->text() != combo->itemText(i))
            b->setToolTip(combo->itemText(i));
        b->setCheckable(true);
        b->setAutoRaise(true);
        row->addWidget(b);
        group->addButton(b, i);
        buttons << b;
    }
    if (look == Segments)
        static_cast<QHBoxLayout *>(row)->addWidget(other);
    else
        row->addWidget(other);
    other->hide();
    connect(group, &QButtonGroup::idClicked, this, [this](int index) {
        if (combo->currentIndex() != index)
            combo->setCurrentIndex(index);
        sync();
    });
    connect(combo, &QComboBox::currentIndexChanged, this, [this] { sync(); });
    combo->installEventFilter(this);
    sync();
}
void ChoiceButtons::sync() {
    const int index = combo->currentIndex();
    for (int i = 0; i < buttons.size(); ++i) {
        buttons[i]->setChecked(i == index);
        buttons[i]->setEnabled(combo->isEnabled());
    }
    other->setVisible(index >= buttons.size());
    other->setText(index >= buttons.size() ? combo->currentText() : QString());
}
bool ChoiceButtons::eventFilter(QObject *watched, QEvent *event) {
    if (watched == combo && event->type() == QEvent::EnabledChange)
        for (auto *b : std::as_const(buttons))
            b->setEnabled(combo->isEnabled());
    return QFrame::eventFilter(watched, event);
}

namespace {
class NavDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const int line = option.fontMetrics.height();
        return {120, index.data(NavList::HeadingRole).toBool() ? line + 16 : line + 14};
    }
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const ChromeColors c(option.palette);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = option.rect.adjusted(6, 1, -6, -1);
        auto font = option.font;
        if (index.data(NavList::HeadingRole).toBool()) {
            font.setBold(true);
            font.setPointSizeF(font.pointSizeF() * .88);
            p->setFont(font);
            p->setPen(c.muted);
            p->drawText(r.adjusted(6, 0, 0, -3), Qt::AlignLeft | Qt::AlignBottom, index.data().toString());
            p->restore();
            return;
        }
        const bool selected = option.state & QStyle::State_Selected,
                   hover = option.state & QStyle::State_MouseOver,
                   muted = index.data(NavList::MutedRole).toBool();
        if (selected || hover) {
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? c.tint : c.hover);
            p->drawRoundedRect(r, 6, 6);
        }
        int right = r.right() - 8;
        auto small = option.font;
        small.setPointSizeF(small.pointSizeF() * .86);
        const auto badge = index.data(NavList::BadgeRole).toString(),
                   note = index.data(NavList::NoteRole).toString();
        const QFontMetrics sm(small);
        if (!badge.isEmpty()) {
            p->setFont(small);
            const int w = sm.horizontalAdvance(badge) + 12, h = sm.height() + 2;
            const QRect pill(right - w + 1, r.center().y() - h / 2, w, h);
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? c.base : c.segment);
            p->drawRoundedRect(pill, h / 2., h / 2.);
            p->setPen(selected ? c.accentText : c.muted);
            p->drawText(pill, Qt::AlignCenter, badge);
            right = pill.left() - 6;
        } else if (!note.isEmpty()) {
            p->setFont(small);
            const int w = sm.horizontalAdvance(note);
            p->setPen(c.muted);
            p->drawText(QRect(right - w + 1, r.top(), w, r.height()), Qt::AlignVCenter | Qt::AlignRight,
                        note);
            right -= w + 6;
        }
        font.setBold(selected);
        p->setFont(font);
        p->setPen(selected ? c.accentText : muted ? c.muted : c.text);
        const QRect text(r.left() + 10, r.top(), std::max(0, right - r.left() - 12), r.height());
        p->drawText(text, Qt::AlignVCenter | Qt::AlignLeft,
                    QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, text.width()));
        p->restore();
    }
};
} // namespace

NavList::NavList(QWidget *parent) : QListWidget(parent) {
    setItemDelegate(new NavDelegate(this));
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setProperty("nav", true);
}
QListWidgetItem *NavList::addHeading(const QString &text) {
    auto *item = new QListWidgetItem(text, this);
    item->setData(HeadingRole, true);
    item->setFlags(Qt::NoItemFlags);
    return item;
}
QListWidgetItem *NavList::addEntry(const QString &text, const QVariant &key) {
    auto *item = new QListWidgetItem(text, this);
    item->setData(Qt::UserRole, key);
    return item;
}
QListWidgetItem *NavList::entry(const QVariant &key) const {
    for (int i = 0; i < count(); ++i)
        if (!item(i)->data(HeadingRole).toBool() && item(i)->data(Qt::UserRole) == key)
            return item(i);
    return nullptr;
}
void NavList::setCurrentKey(const QVariant &key) {
    if (auto *e = entry(key); e && e != currentItem()) {
        const QSignalBlocker blocker(this);
        setCurrentItem(e);
        viewport()->update();
    }
}

SettingsCard::SettingsCard(const QString &title, QWidget *parent) : QFrame(parent) {
    setProperty("card", true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 14);
    layout->setSpacing(10);
    head = new QHBoxLayout;
    head->setSpacing(8);
    heading = new QLabel(title);
    heading->setProperty("cardTitle", true);
    head->addWidget(heading);
    head->addStretch();
    layout->addLayout(head);
    contents = new QWidget;
    content = new QVBoxLayout(contents);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(8);
    layout->addWidget(contents);
    heading->setVisible(!title.isEmpty());
}
QToolButton *SettingsCard::setCollapsible(bool open, const QString &summary) {
    fold = new QToolButton;
    fold->setObjectName(objectName().isEmpty() ? QString() : objectName() + "Toggle");
    fold->setProperty("fold", true);
    fold->setCheckable(true);
    fold->setChecked(open);
    fold->setAccessibleName(heading->text());
    head->insertWidget(0, fold);
    folded = new ElidedLabel(summary); // Shortened in a narrow card, in full as a tooltip.
    folded->setProperty("muted", true);
    folded->setToolTip(summary);
    head->insertWidget(2, folded, 1);
    connect(fold, &QToolButton::toggled, this, [this](bool on) { showOpen(on); });
    showOpen(open);
    return fold;
}
void SettingsCard::setOpen(bool open) {
    if (fold)
        fold->setChecked(open);
}
void SettingsCard::setSummary(const QString &summary) {
    if (!folded)
        return;
    folded->setText(summary);
    folded->setToolTip(summary);
    showOpen(fold->isChecked());
}
void SettingsCard::showOpen(bool open) {
    contents->setVisible(open);
    fold->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    fold->setToolTip(open ? tr("閉じる", "fold") : tr("開く", "fold"));
    folded->setVisible(!open && !folded->text().isEmpty());
}

Disclosure::Disclosure(const QString &text, bool open, QWidget *parent) : QWidget(parent) {
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    toggle = new QToolButton;
    toggle->setText(text);
    toggle->setProperty("disclosure", true);
    toggle->setCheckable(true);
    toggle->setChecked(open);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    toggle->setCursor(Qt::PointingHandCursor);
    row->addWidget(toggle);
    detail = new QLabel;
    detail->setProperty("disclosureDetail", true);
    detail->setCursor(Qt::PointingHandCursor);
    detail->installEventFilter(this); // The count opens it too.
    detail->hide();
    row->addWidget(detail);
    connect(toggle, &QToolButton::toggled, this,
            [this](bool on) { toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow); });
}
bool Disclosure::isOpen() const {
    return toggle->isChecked();
}
void Disclosure::setOpen(bool open) {
    toggle->setChecked(open);
}
void Disclosure::setDetail(const QString &text) {
    detail->setText(text);
    detail->setVisible(!text.isEmpty());
}
bool Disclosure::eventFilter(QObject *watched, QEvent *event) {
    if (watched == detail && event->type() == QEvent::MouseButtonRelease)
        toggle->click();
    return QWidget::eventFilter(watched, event);
}

StatusCard::StatusCard(QWidget *parent) : SettingsCard({}, parent) {
    icon = new QLabel;
    icon->setFixedSize(18, 18);
    text = new QLabel;
    text->setWordWrap(true);
    header()->insertWidget(0, icon, 0, Qt::AlignVCenter);
    header()->insertWidget(1, text, 1);
    setState(Waiting, {});
}
void StatusCard::setState(State state, const QString &headline) {
    static const char *names[]{"ok", "warn", "error", "waiting"};
    current = state;
    text->setText(headline);
    const ChromeColors c(palette());
    const QColor color = state == Ok        ? c.good
                         : state == Warning ? c.candidateText
                         : state == Error   ? c.bad
                                            : c.muted;
    icon->setPixmap(lineIcon(state == Ok        ? "check"
                             : state == Error   ? "error"
                             : state == Warning ? "warning"
                                                : "info",
                             color)
                        .pixmap(QSize(18, 18)));
    if (property("state").toString() != names[state] ||
        text->property("headline").toString() != names[state]) {
        setProperty("state", names[state]);
        text->setProperty("headline", names[state]);
        repolish(this);
        repolish(text);
    }
}
QPushButton *StatusCard::addRecheck(const QString &objectName, const QString &toolTip) {
    auto *b = new QPushButton(tr("再検査"));
    b->setObjectName(objectName);
    b->setIcon(lineIcon("refresh"));
    b->setToolTip(toolTip);
    header()->addWidget(b, 0, Qt::AlignVCenter);
    return b;
}

ElidedLabel::ElidedLabel(const QString &text, QWidget *parent) : QLabel(text, parent) {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}
QSize ElidedLabel::minimumSizeHint() const {
    return {0, fontMetrics().height()};
}
QSize ElidedLabel::sizeHint() const {
    return {fontMetrics().horizontalAdvance(text()), fontMetrics().height()};
}
void ElidedLabel::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setPen(palette().color(foregroundRole()));
    const auto shown = fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width());
    p.drawText(contentsRect(), int(alignment()) | Qt::TextSingleLine, shown);
}
} // namespace mf
