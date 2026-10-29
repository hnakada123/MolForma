// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/candidate_controls.h"
#include "ui/editor_chrome.h"
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
namespace mf {
CandidateControls::CandidateControls(bool banner, QWidget *parent) : QFrame(parent) {
    setObjectName(banner ? "candidateBanner" : "candidateBar");
    auto *flow = new FlowLayout(this, 12, 4);
    flow->setContentsMargins(12, 4, 12, 4);
    auto *about = new QWidget;
    about->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *row = new QHBoxLayout(about);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);
    flow->addWidget(about);
    icon = new QLabel;
    icon->setObjectName("candidateIcon");
    row->addWidget(icon);
    auto *texts = new QVBoxLayout;
    texts->setSpacing(1);
    if (banner) {
        title = new QLabel;
        title->setObjectName("candidateBannerTitle");
        auto font = title->font();
        font.setBold(true);
        title->setFont(font);
        title->setWordWrap(true);
        texts->addWidget(title);
    }
    text = new QLabel;
    if (banner)
        text->setObjectName("candidateBannerText");
    text->setWordWrap(true);
    text->setTextFormat(Qt::RichText);
    text->setToolTip(tr("3D画面の印: 緑の円は追加、赤の×は削除（元の位置）、橙の円は移動する原子です。"));
    texts->addWidget(text);
    row->addLayout(texts, 1);
    auto *buttons = new QWidget;
    auto *buttonRow = new QHBoxLayout(buttons);
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(8);
    flow->addWidget(buttons);
    auto *cancel = new QPushButton(tr("候補を取消（Esc）"));
    cancel->setObjectName(banner ? "cancelCandidate" : "candidateBarCancel");
    buttonRow->addWidget(cancel);
    auto *confirm = new QPushButton(tr("変更を確定（Ctrl+Enter）"));
    confirm->setObjectName(banner ? "confirmCandidate" : "candidateBarConfirm");
    confirm->setProperty("primary", true);
    buttonRow->addWidget(confirm);
    connect(confirm, &QPushButton::clicked, this, &CandidateControls::confirmRequested);
    connect(cancel, &QPushButton::clicked, this, &CandidateControls::cancelRequested);
}
void CandidateControls::refresh(const QString &name, const AtomChanges &changes) {
    if (title) {
        title->setText(tr("変更候補: %1").arg(name));
        const auto marker = [](const char *color, const QString &mark) {
            return QString("<span style=\"color:%1\"><b>%2</b></span>").arg(color, mark);
        };
        text->setText(tr("%1 追加 %2　%3 削除 %4　%5 移動 %6 原子")
                          .arg(marker("#25a35a", "○"))
                          .arg(changes.added.size())
                          .arg(marker("#d23c3c", "×"))
                          .arg(changes.removed.size())
                          .arg(marker("#e08a1e", "○"))
                          .arg(changes.moved.size()));
    } else {
        const auto counts = tr("追加 %1・削除 %2・移動 %3 原子")
                                .arg(changes.added.size())
                                .arg(changes.removed.size())
                                .arg(changes.moved.size());
        text->setText(tr("<b>変更候補: %1</b>　%2").arg(name.toHtmlEscaped(), counts));
    }
}
void CandidateControls::applyChromeStyle() {
    setStyleSheet(chromeRules(R"(
        #candidateBanner, #candidateBar { background: @candidate; border: none; }
        #candidateBanner { border-bottom: 1px solid @candidateLine; }
        #candidateBar { border-top: 1px solid @candidateLine; }
        QLabel { color: @candidateText; }
        QPushButton { background: @base; color: @candidateText; border: 1px solid @candidateLine;
                      border-radius: 6px; padding: 5px 12px; }
        QPushButton:hover { background: @hover; }
        QPushButton[primary="true"] { background: @accent; color: @onAccent; border: none; font-weight: bold; }
        QPushButton[primary="true"]:hover { background: @accentHover; }
    )",
                              palette()));
    icon->setPixmap(lineIcon("layers", ChromeColors(palette()).candidateText).pixmap(QSize(22, 22)));
}
} // namespace mf
