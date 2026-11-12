// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QToolButton>
class QComboBox;
class QHBoxLayout;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;

namespace mf {
// Buttons for the items of a combo box: a segmented control, or chips that wrap. The combo box
// stays the control that holds the value, hidden inside (so callers and MCP clients that set it
// keep working): a click selects its item, and the buttons follow its current item. A value
// without a button ("未対応: …") is named beside them. The buttons take the combo box's name with
// the item's value ("nwchem_task_energy") and follow its enabled state.
class ChoiceButtons : public QFrame {
  public:
    enum Look { Segments, Chips };
    // labels: shorter texts for the buttons, in the order of the items (else the item texts).
    ChoiceButtons(QComboBox *combo, Look look, const QStringList &labels = {}, QWidget *parent = nullptr);
    QToolButton *button(int index) const {
        return buttons.value(index);
    }
    void sync();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QComboBox *combo;
    QList<QToolButton *> buttons;
    QLabel *other;
};

// The list at the left of a settings tab: headings, and entries with a count or a note at the
// right ("変更 4", "使わない"). Muted entries take no part in the current calculation.
// Entries hold their key in Qt::UserRole.
class NavList : public QListWidget {
  public:
    enum Role { HeadingRole = Qt::UserRole + 10, BadgeRole, NoteRole, MutedRole };
    explicit NavList(QWidget *parent = nullptr);
    QListWidgetItem *addHeading(const QString &text);
    QListWidgetItem *addEntry(const QString &text, const QVariant &key);
    QListWidgetItem *entry(const QVariant &key) const;
    // Selects the entry of a key without emitting a click (the list follows the scroll position).
    void setCurrentKey(const QVariant &key);
};

// A card of a settings tab: a bordered box with a title row (the title, then what is added to
// header()) above its contents (body()).
class SettingsCard : public QFrame {
  public:
    explicit SettingsCard(const QString &title, QWidget *parent = nullptr);
    QHBoxLayout *header() const {
        return head;
    }
    QVBoxLayout *body() const {
        return content;
    }
    QLabel *titleLabel() const {
        return heading;
    }
    // An arrow before the title that folds the whole card (for what is seldom needed); while it is
    // folded, `summary` after the title says what is inside. Returns the arrow, for callers that
    // remember the state (its clicked()) or rename it.
    QToolButton *setCollapsible(bool open, const QString &summary = {});
    // Opens or folds a collapsible card as its arrow would.
    void setOpen(bool open);
    void setSummary(const QString &summary);

  private:
    QHBoxLayout *head;
    QVBoxLayout *content;
    QWidget *contents;
    QLabel *heading, *folded = nullptr;
    QToolButton *fold = nullptr;
    void showOpen(bool open);
};

// An arrow and a name in the accent that open what is seldom needed inside a card ("詳細設定",
// "パスの扱いについて"), with what it holds after them in muted text ("21 項目 · 変更 1"). button()
// carries the state and the object name; the caller shows what it opens on its toggled().
// SettingsCard::setCollapsible() folds a whole card instead.
class Disclosure : public QWidget {
  public:
    explicit Disclosure(const QString &text, bool open = false, QWidget *parent = nullptr);
    QToolButton *button() const {
        return toggle;
    }
    bool isOpen() const;
    // Opens or closes it as a click would (toggled() follows).
    void setOpen(bool open);
    void setDetail(const QString &detail);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QToolButton *toggle;
    QLabel *detail;
};

// The outcome of a tab's checks, the same card in "2 計算条件" … "4 入力プレビュー": an icon and a
// headline in the color of the outcome over the details, and optionally "再検査".
class StatusCard : public SettingsCard {
  public:
    enum State { Ok, Warning, Error, Waiting };
    explicit StatusCard(QWidget *parent = nullptr);
    void setState(State state, const QString &headline);
    QLabel *headline() const {
        return text;
    }
    // "再検査": for checks of what can change outside MolForma (files, programs).
    QPushButton *addRecheck(const QString &objectName, const QString &toolTip);

  private:
    QLabel *icon, *text;
    State current = Waiting;
};

// A link to another place ("3 実行設定で変更", a tab named as in the text), with an arrow after it.
QPushButton *linkButton(const QString &text, const QString &objectName = {});
// Read-only text of a log or file: fixed-width, not wrapped, in the frame of the logs.
QPlainTextEdit *logView(const QString &objectName);
// A flat button with the copy icon.
QToolButton *copyButton(const QString &objectName, const QString &toolTip);

// One line of text, shortened with "…" when narrower than the text (the full text in toolTip).
class ElidedLabel : public QLabel {
  public:
    explicit ElidedLabel(const QString &text = {}, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent *event) override;
};
} // namespace mf
