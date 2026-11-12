// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/sequence_track.h"
#include "editing/peptide.h"
#include "foundation/i18n.h"
#include "model/settings.h"
#include "ui/editor_chrome.h"
#include "ui/editor_context.h"
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <limits>

namespace mf {
namespace {
QColor mix(const QColor &a, const QColor &b, double share) { // share of a
    return QColor::fromRgbF(float(a.redF() * share + b.redF() * (1 - share)),
                            float(a.greenF() * share + b.greenF() * (1 - share)),
                            float(a.blueF() * share + b.blueF() * (1 - share)));
}
int modulo(int value, int divisor) {
    return (value % divisor + divisor) % divisor;
}
int floorDivision(int value, int divisor) {
    return (value - modulo(value, divisor)) / divisor;
}
// "鎖 A", as in the chains of the structure import.
QString chainLabel(const QString &chain) {
    return tr("鎖 %1").arg(chain);
}
} // namespace

QChar residueLetter(const Residue &residue) {
    static const QHash<QString, QChar> amino{
        {"ALA", u'A'},
        {"ARG", u'R'},
        {"ASN", u'N'},
        {"ASP", u'D'},
        {"CYS", u'C'},
        {"GLN", u'Q'},
        {"GLU", u'E'},
        {"GLY", u'G'},
        {"HIS", u'H'},
        {"ILE", u'I'},
        {"LEU", u'L'},
        {"LYS", u'K'},
        {"MET", u'M'},
        {"PHE", u'F'},
        {"PRO", u'P'},
        {"SER", u'S'},
        {"THR", u'T'},
        {"TRP", u'W'},
        {"TYR", u'Y'},
        {"VAL", u'V'},
        // Protonation states and modified residues that keep their place in the sequence.
        {"HID", u'H'},
        {"HIE", u'H'},
        {"HIP", u'H'},
        {"HSD", u'H'},
        {"HSE", u'H'},
        {"HSP", u'H'},
        {"CYX", u'C'},
        {"CYM", u'C'},
        {"ASH", u'D'},
        {"GLH", u'E'},
        {"LYN", u'K'},
        {"MSE", u'M'},
        {"SEC", u'U'},
        {"PYL", u'O'}};
    if (const auto it = amino.constFind(residue.code); it != amino.constEnd())
        return *it;
    // Nucleotides by their sugar (C1'): the names alone could be an ion's.
    static const QHash<QString, QChar> nucleic{{"A", u'A'},  {"C", u'C'},  {"G", u'G'},  {"U", u'U'},
                                               {"T", u'T'},  {"DA", u'A'}, {"DC", u'C'}, {"DG", u'G'},
                                               {"DT", u'T'}, {"DU", u'U'}};
    if (residue.atoms.contains("C1'"))
        if (const auto it = nucleic.constFind(residue.code); it != nucleic.constEnd())
            return *it;
    return {};
}

QVector<SequenceToken> sequenceTokens(const Document &document) {
    // Chains by the name in the structure file (residues keep an ID of their chain), else by number.
    QStringList chains;
    QHash<QString, int> chainIndex;
    for (const auto &r : document.residues)
        if (!chainIndex.contains(r.chain)) {
            chainIndex.insert(r.chain, chains.size());
            const auto name = r.metadata["pdb_chain"].toString();
            chains << (name.isEmpty() ? QString::number(chains.size() + 1) : name);
        }
    QVector<const Residue *> order;
    order.reserve(document.residues.size());
    for (const auto &r : document.residues)
        order << &r;
    std::stable_sort(order.begin(), order.end(), [&](const Residue *a, const Residue *b) {
        const int ca = chainIndex.value(a->chain), cb = chainIndex.value(b->chain);
        if (ca != cb)
            return ca < cb;
        return a->number != b->number ? a->number < b->number : a->insertionCode < b->insertionCode;
    });
    QVector<SequenceToken> tokens;
    tokens.reserve(order.size() + 2 * chains.size());
    int next = 0;
    for (int c = 0; c < chains.size(); ++c) {
        if (chains.size() > 1) {
            SequenceToken start;
            start.kind = SequenceToken::Chain;
            start.chain = chains[c];
            start.chainIndex = c;
            start.label = chains[c];
            tokens << start;
        }
        SequenceToken waters;
        waters.kind = SequenceToken::Waters;
        waters.chain = chains[c];
        waters.chainIndex = c;
        const Residue *previous = nullptr; // The polymer residue just before, for gaps in the numbers.
        for (; next < order.size() && chainIndex.value(order[next]->chain) == c; ++next) {
            const auto &r = *order[next];
            if (waterResidue(r.code)) {
                if (waters.code.isEmpty())
                    waters.code = r.code;
                waters.atoms += r.atoms.values();
                ++waters.number;
                continue;
            }
            SequenceToken token;
            token.code = r.code;
            token.chain = chains[c];
            token.chainIndex = c;
            token.number = r.number;
            token.label = QString("%1 %2%3").arg(r.code).arg(r.number).arg(r.insertionCode);
            token.letter = residueLetter(r);
            token.polymer = polymerResidue(r);
            token.atoms = r.atoms.values();
            if (token.polymer && previous && r.number > previous->number + 1) {
                SequenceToken gap;
                gap.kind = SequenceToken::Gap;
                gap.chain = chains[c];
                gap.chainIndex = c;
                gap.number = r.number - previous->number - 1;
                gap.label = gap.number == 1 ? QString::number(previous->number + 1)
                                            : QString("%1–%2").arg(previous->number + 1).arg(r.number - 1);
                tokens << gap;
            }
            previous = token.polymer ? &r : nullptr;
            tokens << token;
        }
        if (waters.number) {
            waters.label = QString("%1 ×%2").arg(waters.code).arg(waters.number);
            tokens << waters;
        }
    }
    return tokens;
}

SequenceStrip::SequenceStrip(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName(tr("配列"));
    measure();
}
void SequenceStrip::measure() {
    const auto base = font();
    fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    small = base;
    if (base.pointSizeF() > 0) {
        fixed.setPointSizeF(base.pointSizeF());
        small.setPointSizeF(base.pointSizeF() * .8);
    } else {
        fixed.setPixelSize(base.pixelSize());
        small.setPixelSize(std::max(8, int(base.pixelSize() * .8)));
    }
    tag = small;
    tag.setBold(true);
    const QFontMetrics f(fixed), s(small);
    cell1 = f.horizontalAdvance(u'W') + 4;
    cell3 = f.horizontalAdvance("WWW") + 10;
    cellTop = s.height() + 1;
    cellHeight = f.height() + 4;
    overviewTop = cellTop + cellHeight + 4;
    updateGeometry();
}
QSize SequenceStrip::sizeHint() const {
    return {240, overviewTop + 8};
}
QSize SequenceStrip::minimumSizeHint() const {
    return {60, overviewTop + 8};
}
void SequenceStrip::setTokens(const QVector<SequenceToken> &tokens) {
    list = tokens;
    hover = -1;
    if (anchor >= list.size())
        anchor = -1;
    layoutTokens();
    refreshStates();
    update();
}
void SequenceStrip::layoutTokens() {
    const QFontMetrics f(fixed), t(tag);
    const auto widthOf = [&](const SequenceToken &token, bool threeLetters) {
        switch (token.kind) {
        case SequenceToken::Residue:
            if (!token.letter.isNull())
                return threeLetters ? std::max(cell3, f.horizontalAdvance(token.code) + 10) : cell1;
            return std::max(threeLetters ? cell3 : 2 * cell1, t.horizontalAdvance(token.code) + 12);
        case SequenceToken::Waters:
            return t.horizontalAdvance(token.label) + 14;
        case SequenceToken::Gap:
            return std::max(16, 2 * cell1);
        case SequenceToken::Chain:
            return std::max(2 * cell1, t.horizontalAdvance(token.label) + 12);
        }
        return cell1;
    };
    int whole = 0;
    for (const auto &token : std::as_const(list))
        whole += widthOf(token, true);
    three = letters == Three || (letters == Automatic && whole <= width());
    xs.resize(list.size());
    widths.resize(list.size());
    chainExtents.clear();
    int x = 0;
    for (int i = 0; i < list.size(); ++i) {
        xs[i] = x;
        widths[i] = widthOf(list[i], three);
        if (!i || list[i].chainIndex != list[i - 1].chainIndex)
            chainExtents.append({x, x});
        x += widths[i];
        chainExtents.last().second = x;
    }
    contentWidth = x;
    const int before = scroll;
    setScroll(scroll);
    if (scroll == before)
        emit laidOut();
    update();
}
void SequenceStrip::refreshStates() {
    const QSet<QString> chosen(selectedAtoms.begin(), selectedAtoms.end());
    states.fill(0, list.size());
    for (int i = 0; !chosen.isEmpty() && i < list.size(); ++i) {
        const auto &atoms = list[i].atoms;
        const auto count =
            std::count_if(atoms.begin(), atoms.end(), [&](const auto &id) { return chosen.contains(id); });
        states[i] = !count ? 0 : count == atoms.size() ? 2 : 1;
    }
}
void SequenceStrip::setSelection(const Ids &atoms) {
    const bool changed = atoms != selectedAtoms;
    selectedAtoms = atoms;
    refreshStates();
    // A selection made elsewhere (the 3D view) is brought into sight when none of it is.
    if (changed && !emitting) {
        int first = -1;
        bool seen = false;
        for (int i = 0; i < list.size() && !seen; ++i)
            if (states[i]) {
                if (first < 0)
                    first = i;
                seen = xs[i] + widths[i] > scroll && xs[i] < scroll + width();
            }
        if (first >= 0 && !seen)
            showAround(first);
    }
    update();
}
void SequenceStrip::setLetters(Letters choice) {
    if (letters == choice)
        return;
    // Keep the token at the left edge there.
    const int first = firstVisible();
    letters = choice;
    layoutTokens();
    if (first >= 0)
        scrollToToken(first);
}
QList<int> SequenceStrip::selectedTokens() const {
    QList<int> out;
    for (int i = 0; i < states.size(); ++i)
        if (states[i])
            out << i;
    return out;
}
QRect SequenceStrip::tokenRect(int index) const {
    if (index < 0 || index >= list.size())
        return {};
    return {xs[index] - scroll, cellTop, widths[index], cellHeight};
}
int SequenceStrip::tokenAt(int x) const {
    const int content = x + scroll;
    const auto it = std::upper_bound(xs.begin(), xs.end(), content);
    if (it == xs.begin())
        return -1;
    const int i = int(it - xs.begin()) - 1;
    return content < xs[i] + widths[i] ? i : -1;
}
int SequenceStrip::firstVisible() const {
    if (list.isEmpty())
        return -1;
    return std::max(0, int(std::upper_bound(xs.begin(), xs.end(), scroll) - xs.begin()) - 1);
}
void SequenceStrip::setScroll(int x) {
    x = std::clamp(x, 0, std::max(0, contentWidth - width()));
    if (x == scroll)
        return;
    scroll = x;
    update();
    emit laidOut();
}
void SequenceStrip::scrollToToken(int index) {
    if (index >= 0 && index < list.size())
        setScroll(xs[index]);
}
void SequenceStrip::showAround(int index) {
    setScroll(xs[index] + widths[index] / 2 - width() / 2);
}
void SequenceStrip::scrollOverview(double x) {
    if (width() > 0)
        setScroll(int(x / width() * contentWidth - width() / 2.0));
}
bool SequenceStrip::labelled(int index) const {
    const auto &t = list[index];
    if (t.kind != SequenceToken::Residue)
        return false;
    if (three)
        return true;
    if (!t.polymer || t.letter.isNull())
        return false;
    // Every ten, and the first of a run when the next ten is not too near.
    const int digit = modulo(t.number, 10);
    const auto &before = index ? list[index - 1] : t;
    const bool start =
        !index || before.kind != SequenceToken::Residue || !before.polymer || before.letter.isNull();
    return !digit || (start && digit <= 7);
}

void SequenceStrip::paintEvent(QPaintEvent *) {
    if (list.isEmpty())
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ChromeColors c(palette());
    // The background of the colors drawn on it, whatever the style sheet of the row.
    p.fillRect(rect(), c.base);
    const int first = firstVisible();
    const auto inSight = [&](int i) { return i < list.size() && xs[i] - scroll < width(); };
    // Every other ten residues on a light band, in one-letter codes.
    if (!three) {
        const auto band = mix(c.text, c.base, .045);
        for (int i = first; inSight(i); ++i) {
            const auto &t = list[i];
            if (t.kind == SequenceToken::Residue && t.polymer && !t.letter.isNull() &&
                !modulo(floorDivision(t.number, 10), 2))
                p.fillRect(tokenRect(i), band);
        }
    }
    // The numbers over their residues, where they do not run into the one before.
    p.setFont(small);
    p.setPen(c.muted);
    const QFontMetrics s(small);
    int lastRight = std::numeric_limits<int>::min() / 2;
    for (int i = first; inSight(i); ++i) {
        if (!labelled(i))
            continue;
        const auto text = QString::number(list[i].number);
        const int w = s.horizontalAdvance(text), left = xs[i] - scroll + widths[i] / 2 - w / 2;
        if (left < lastRight + 4)
            continue;
        p.drawText(QRect(left, 0, w, cellTop), Qt::AlignCenter, text);
        lastRight = left + w;
    }
    for (int i = first; inSight(i); ++i) {
        const auto &t = list[i];
        const QRectF r = tokenRect(i);
        const int state = states.value(i);
        const bool hovered = i == hover && !t.atoms.isEmpty();
        if (t.kind == SequenceToken::Residue && !t.letter.isNull()) {
            const QColor fill = state == 2 ? c.accent : state == 1 ? c.tint : hovered ? c.hover : QColor();
            if (fill.isValid()) {
                p.setPen(Qt::NoPen);
                p.setBrush(fill);
                p.drawRoundedRect(r.adjusted(.5, 0, -.5, 0), 3, 3);
            }
            p.setFont(fixed);
            p.setPen(state == 2 ? c.onAccent : state == 1 ? c.accentText : c.text);
            p.drawText(r, Qt::AlignCenter, three ? t.code : QString(t.letter));
        } else if (t.kind == SequenceToken::Residue || t.kind == SequenceToken::Waters) {
            // Ligands, ions, caps and unknown residues by name in amber; the waters of a chain in gray.
            const bool residue = t.kind == SequenceToken::Residue;
            p.setPen(hovered ? QPen(c.accent, 1) : QPen(Qt::NoPen));
            p.setBrush(state == 2 ? c.accent : state == 1 ? c.tint : residue ? c.candidate : c.segment);
            p.drawRoundedRect(r.adjusted(1.5, 1.5, -1.5, -1.5), 4, 4);
            p.setFont(tag);
            p.setPen(state == 2   ? c.onAccent
                     : state == 1 ? c.accentText
                     : residue    ? c.candidateText
                                  : c.text);
            p.drawText(r, Qt::AlignCenter, residue ? t.code : t.label);
        } else if (t.kind == SequenceToken::Gap) {
            p.setPen(QPen(c.line, 1, Qt::DashLine));
            p.drawLine(QPointF(r.left() + 1.5, r.top() + 2), QPointF(r.left() + 1.5, r.bottom() - 2));
            p.drawLine(QPointF(r.right() - 1.5, r.top() + 2), QPointF(r.right() - 1.5, r.bottom() - 2));
            p.setFont(tag);
            p.setPen(c.muted);
            p.drawText(r, Qt::AlignCenter, QString(QChar(0x22ef)));
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(c.text);
            p.drawRoundedRect(r.adjusted(1.5, 1.5, -1.5, -1.5), 4, 4);
            p.setFont(tag);
            p.setPen(c.base);
            p.drawText(r, Qt::AlignCenter, t.label);
        }
    }
    if (!overview())
        return;
    // The whole sequence: its chains in grays, ligands in amber, the selection in the accent and
    // the part in sight framed.
    const double scale = double(width()) / contentWidth;
    const QRectF bar(0, overviewTop, width(), 6);
    QPainterPath shape;
    shape.addRoundedRect(bar, 3, 3);
    p.save();
    p.setClipPath(shape);
    p.fillRect(bar, c.segment);
    for (int k = 0; k < chainExtents.size(); ++k) {
        const auto [from, to] = chainExtents[k];
        p.fillRect(QRectF(from * scale, bar.top(), (to - from) * scale, bar.height()),
                   mix(c.text, c.base, k % 2 ? .32 : .22));
    }
    const auto mark = [&](int i, const QColor &color) {
        p.fillRect(QRectF(xs[i] * scale, bar.top(), std::max(2.0, widths[i] * scale), bar.height()), color);
    };
    for (int i = 0; i < list.size(); ++i)
        if (list[i].kind == SequenceToken::Residue && list[i].letter.isNull() && !list[i].polymer)
            mark(i, c.candidateLine);
    for (int i = 0; i < list.size(); ++i)
        if (states[i])
            mark(i, c.accent);
    p.restore();
    const double frame = std::max(12.0, width() * scale);
    const double left = std::clamp(scroll * scale + width() * scale / 2 - frame / 2, 0.0, width() - frame);
    auto shade = c.text;
    shade.setAlphaF(.18f);
    p.setBrush(shade);
    p.setPen(QPen(c.text, 1.2));
    p.drawRoundedRect(QRectF(left + .6, overviewTop - 1.4, frame - 1.2, 8.8), 2.5, 2.5);
}
void SequenceStrip::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    layoutTokens();
}
void SequenceStrip::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        measure();
        layoutTokens();
    } else if (event->type() == QEvent::PaletteChange)
        update();
}
bool SequenceStrip::event(QEvent *event) {
    if (event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const int i = help->pos().y() < overviewTop - 3 ? tokenAt(help->pos().x()) : -1;
        if (i >= 0)
            QToolTip::showText(help->globalPos(), toolTipOf(i), this, tokenRect(i));
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}
QString SequenceStrip::toolTipOf(int index) const {
    const auto &t = list[index];
    if (t.kind == SequenceToken::Gap)
        return tr("%n 残基（番号 %1）は構造にありません。", nullptr, t.number).arg(t.label);
    if (t.kind == SequenceToken::Chain)
        return chainLabel(t.chain);
    return QStringList{t.label, chainLabel(t.chain), tr("%n 原子", nullptr, int(t.atoms.size()))}.join(
               " · ") +
           "\n" + tr("クリック: 選択 · Shift＋クリック: 範囲 · Ctrl＋クリック: 追加・解除");
}
void SequenceStrip::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton)
        return QWidget::mousePressEvent(event);
    const auto position = event->position();
    if (overview() && position.y() >= overviewTop - 3) {
        draggingOverview = true;
        scrollOverview(position.x());
        return;
    }
    const int i = tokenAt(int(position.x()));
    if (i < 0 || list[i].atoms.isEmpty())
        return;
    if (event->modifiers() & Qt::ShiftModifier && anchor >= 0)
        select(anchor, i);
    else if (event->modifiers() & Qt::ControlModifier) {
        anchor = i;
        toggle(i);
    } else {
        anchor = dragTo = i;
        dragging = true;
        select(i, i);
    }
}
void SequenceStrip::mouseMoveEvent(QMouseEvent *event) {
    const auto position = event->position();
    if (draggingOverview) {
        scrollOverview(position.x());
        return;
    }
    const int i = position.y() < overviewTop - 3 ? tokenAt(int(position.x())) : -1;
    if (i != hover) {
        hover = i;
        setCursor(i >= 0 && !list[i].atoms.isEmpty() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    // Dragging over the residues selects the range from where it started.
    if (dragging && event->buttons() & Qt::LeftButton && i >= 0 && !list[i].atoms.isEmpty() && i != dragTo) {
        dragTo = i;
        select(anchor, i);
    }
}
void SequenceStrip::mouseReleaseEvent(QMouseEvent *event) {
    dragging = draggingOverview = false;
    QWidget::mouseReleaseEvent(event);
}
void SequenceStrip::leaveEvent(QEvent *event) {
    hover = -1;
    update();
    QWidget::leaveEvent(event);
}
void SequenceStrip::wheelEvent(QWheelEvent *event) {
    if (!overview())
        return event->ignore();
    const auto larger = [](const QPoint &d) { return std::abs(d.x()) > std::abs(d.y()) ? d.x() : d.y(); };
    const int delta = !event->pixelDelta().isNull()
                          ? larger(event->pixelDelta())
                          : larger(event->angleDelta()) * (three ? 2 * cell3 : 6 * cell1) / 120;
    setScroll(scroll - delta);
    event->accept();
}
void SequenceStrip::select(int from, int to) {
    Ids atoms;
    for (int i = std::min(from, to); i <= std::max(from, to); ++i)
        atoms += list[i].atoms;
    request(atoms);
}
void SequenceStrip::toggle(int index) {
    auto atoms = selectedAtoms;
    if (states.value(index) == 2) {
        const QSet<QString> remove(list[index].atoms.begin(), list[index].atoms.end());
        atoms.removeIf([&](const QString &id) { return remove.contains(id); });
    } else {
        const QSet<QString> have(atoms.begin(), atoms.end());
        for (const auto &id : list[index].atoms)
            if (!have.contains(id))
                atoms << id;
    }
    request(atoms);
}
void SequenceStrip::request(const Ids &atoms) {
    emitting = true;
    emit selectionRequested(atoms);
    emitting = false;
}

SequenceTrack::SequenceTrack(UiServices &uiServices, QWidget *parent)
    : QFrame(parent), uiContext(uiServices) {
    setObjectName("sequenceTrack");
    setAttribute(Qt::WA_StyledBackground);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(12, 3, 6, 3);
    row->setSpacing(8);
    auto *title = new QLabel(tr("配列"));
    title->setObjectName("sequenceTitle");
    row->addWidget(title);
    chainButton = new QToolButton;
    chainButton->setObjectName("sequenceChain");
    chainButton->setPopupMode(QToolButton::InstantPopup);
    chainButton->setMenu(new QMenu(chainButton));
    chainButton->setToolTip(tr("鎖を選ぶと、その鎖の始まりを表示します。"));
    row->addWidget(chainButton);
    sequence = new SequenceStrip;
    sequence->setObjectName("sequenceStrip");
    row->addWidget(sequence, 1);
    summary = new ElidedLabel;
    summary->setObjectName("sequenceSummary");
    row->addWidget(summary, 1);
    lettersButton = new QToolButton;
    lettersButton->setObjectName("sequenceLetters");
    lettersButton->setText("ABC");
    lettersButton->setCheckable(true);
    lettersButton->setAccessibleName(tr("3文字表記"));
    lettersButton->setToolTip(
        tr("残基を3文字表記と1文字表記で切り替えます。はじめは、配列全体が収まるときだけ3文字表記です。"));
    row->addWidget(lettersButton);
    collapseButton = new QToolButton;
    collapseButton->setObjectName("sequenceCollapse");
    row->addWidget(collapseButton);
    connect(sequence, &SequenceStrip::selectionRequested, this, &SequenceTrack::selectionRequested);
    connect(sequence, &SequenceStrip::laidOut, this, [this] {
        lettersButton->setChecked(sequence->threeLetters());
        refreshChain();
    });
    connect(lettersButton, &QToolButton::clicked, this, [this](bool checked) {
        sequence->setLetters(checked ? SequenceStrip::Three : SequenceStrip::One);
    });
    connect(collapseButton, &QToolButton::clicked, this, [this] {
        setCollapsed(!folded);
        uiContext.setPreference(Preference::sequenceCollapsed, folded);
    });
    setCollapsed(uiContext.preference(Preference::sequenceCollapsed, false).toBool());
    hide();
}
void SequenceTrack::setCollapsed(bool collapsed) {
    folded = collapsed;
    sequence->setVisible(!folded);
    lettersButton->setVisible(!folded);
    chainButton->setVisible(!folded && chainStarts.size() > 1);
    summary->setVisible(folded);
    collapseButton->setIcon(lineIcon(folded ? "down" : "up"));
    collapseButton->setToolTip(folded ? tr("配列を広げる") : tr("配列を折りたたむ"));
    collapseButton->setAccessibleName(collapseButton->toolTip());
    refreshSummary();
}
void SequenceTrack::setDocument(const Document &document) {
    const auto tokens = sequenceTokens(document);
    residues = document.residues.size();
    chainStarts.clear();
    for (int i = 0; i < tokens.size(); ++i)
        if (!i || tokens[i].chainIndex != tokens[i - 1].chainIndex)
            chainStarts << i;
    // The chains with their numbers and residues; choosing one shows its start.
    auto *menu = chainButton->menu();
    menu->clear();
    for (int k = 0; chainStarts.size() > 1 && k < chainStarts.size(); ++k) {
        const int from = chainStarts[k], to = k + 1 < chainStarts.size() ? chainStarts[k + 1] : tokens.size();
        int count = 0, low = std::numeric_limits<int>::max(), high = std::numeric_limits<int>::min();
        for (int i = from; i < to; ++i)
            if (tokens[i].kind == SequenceToken::Residue) {
                ++count;
                low = std::min(low, tokens[i].number);
                high = std::max(high, tokens[i].number);
            } else if (tokens[i].kind == SequenceToken::Waters)
                count += tokens[i].number;
        QStringList details;
        if (low <= high)
            details << (low == high ? QString::number(low) : QString("%1–%2").arg(low).arg(high));
        details << tr("%n 残基", nullptr, count);
        // The chain chosen names the button even where the end of the strip keeps its start from
        // the left edge.
        const auto name = chainLabel(tokens[from].chain);
        connect(menu->addAction(name + "\t" + details.join(" · ")), &QAction::triggered, this,
                [this, from, name] {
                    sequence->scrollToToken(from);
                    chainButton->setText(name);
                });
    }
    chainButton->setVisible(!folded && chainStarts.size() > 1);
    sequence->setTokens(tokens);
    setVisible(!tokens.isEmpty());
    refreshSummary();
}
void SequenceTrack::setSelection(const Ids &atoms) {
    sequence->setSelection(atoms);
    refreshSummary();
}
void SequenceTrack::refreshChain() {
    const int first = sequence->firstVisible();
    if (chainStarts.size() > 1 && first >= 0)
        chainButton->setText(chainLabel(sequence->tokens()[first].chain));
}
void SequenceTrack::refreshSummary() {
    const auto &tokens = sequence->tokens();
    QStringList parts;
    if (chainStarts.size() > 1)
        parts << tr("%n 鎖", nullptr, int(chainStarts.size()));
    parts << tr("%n 残基", nullptr, residues);
    if (const auto chosen = sequence->selectedTokens(); !chosen.isEmpty()) {
        // One run of residues by its ends, else the count.
        const int selectable =
            int(std::count_if(tokens.begin() + chosen.first(), tokens.begin() + chosen.last() + 1,
                              [](const SequenceToken &t) { return !t.atoms.isEmpty(); }));
        const auto &first = tokens[chosen.first()].label, &last = tokens[chosen.last()].label;
        parts << tr("選択: %1")
                     .arg(chosen.size() == 1            ? first
                          : selectable == chosen.size() ? QString(first + "–" + last)
                                                        : tr("%n 残基", nullptr, int(chosen.size())));
    }
    summary->setText(parts.join(" · "));
    summary->setToolTip(summary->text());
}
} // namespace mf
