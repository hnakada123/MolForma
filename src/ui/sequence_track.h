// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <QFrame>
#include <QSet>
class QLabel;
class QSettings;
class QToolButton;
namespace mf {
class ElidedLabel;

// One entry of the sequence row: a residue (amino acids and nucleotides as their one-letter code,
// other residues as a tag with their name), the waters of a chain as one tag, a gap where the
// numbers of a polymer chain jump, or the start of a chain (when there are several).
struct SequenceToken {
    enum Kind { Residue, Waters, Gap, Chain };
    Kind kind = Residue;
    // chain: its name in the structure file, else its place (1, 2, …); label: "GLY 2", "HOH ×430",
    // the missing numbers, the chain's name.
    QString code, chain, label;
    QChar letter; // None for ligands, ions, caps and unknown residues.
    int number = 0, chainIndex = 0;
    bool polymer = false; // An amino acid or nucleotide of a chain (N, CA, C or C1').
    Ids atoms;            // What a click selects.
};
// The residues N → C by chain and number (caps added after the chain was built, ACE 0 and NME n+1,
// take their place), the waters of each chain at its end.
QVector<SequenceToken> sequenceTokens(const Document &document);
// The one-letter code of an amino acid (also HID, CYX, MSE, …) or a nucleotide, else none.
QChar residueLetter(const Residue &residue);

// The residues in a strip that scrolls sideways: their numbers above them, the selected ones in the
// accent (partly selected ones tinted), and while the sequence is wider than the strip an overview
// of all of it under them, with the part in sight framed.
class SequenceStrip : public QWidget {
    Q_OBJECT
  public:
    // Automatic: three-letter codes when the whole sequence fits in them, else one letter.
    enum Letters { Automatic, One, Three };
    explicit SequenceStrip(QWidget *parent = nullptr);
    void setTokens(const QVector<SequenceToken> &tokens);
    // Shows the selection; a selection that changed and is out of sight is scrolled to.
    void setSelection(const Ids &atoms);
    void setLetters(Letters letters);
    bool threeLetters() const {
        return three;
    }
    const QVector<SequenceToken> &tokens() const {
        return list;
    }
    // The tokens with selected atoms, in their order.
    QList<int> selectedTokens() const;
    // Where a token is drawn (outside the strip when scrolled out of sight).
    QRect tokenRect(int index) const;
    int firstVisible() const;
    int offset() const {
        return scroll;
    }
    void scrollToToken(int index);
    bool overview() const {
        return contentWidth > width();
    }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
  signals:
    void selectionRequested(mf::Ids atoms);
    void laidOut(); // The letters or the part in sight changed.

  protected:
    bool event(QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;

  private:
    QVector<SequenceToken> list;
    QVector<int> xs, widths;
    QVector<uchar> states; // 0 unselected, 1 partly, 2 wholly selected.
    QVector<std::pair<int, int>> chainExtents;
    Ids selectedAtoms;
    Letters letters = Automatic;
    bool three = false, emitting = false, dragging = false, draggingOverview = false;
    int contentWidth = 0, scroll = 0, hover = -1, anchor = -1, dragTo = -1;
    QFont fixed, small, tag;
    int cell1 = 12, cell3 = 34, cellTop = 13, cellHeight = 20, overviewTop = 37;
    void measure();
    void layoutTokens();
    void refreshStates();
    int tokenAt(int x) const; // x in the strip.
    bool labelled(int index) const;
    void setScroll(int x);
    void showAround(int index);
    void scrollOverview(double x);
    void select(int from, int to);
    void toggle(int index);
    void request(const Ids &atoms);
    QString toolTipOf(int index) const;
};

// "配列" between the options of the tool and the 3D view: the chain in sight (a menu of the chains),
// the strip, the choice of one- or three-letter codes and folding it into a line with the counts.
class SequenceTrack : public QFrame {
    Q_OBJECT
  public:
    SequenceTrack(QSettings &settings, QWidget *parent = nullptr);
    // Shown while the document has residues.
    void setDocument(const Document &document);
    void setSelection(const Ids &atoms);
    SequenceStrip *strip() const {
        return sequence;
    }
    bool collapsed() const {
        return folded;
    }
    void setCollapsed(bool collapsed);
  signals:
    void selectionRequested(mf::Ids atoms);

  private:
    QSettings &settings;
    SequenceStrip *sequence = nullptr;
    QToolButton *chainButton = nullptr, *lettersButton = nullptr, *collapseButton = nullptr;
    ElidedLabel *summary = nullptr;
    QVector<int> chainStarts; // The first token of each chain.
    int residues = 0;
    bool folded = false;
    void refreshChain();
    void refreshSummary();
};
} // namespace mf
