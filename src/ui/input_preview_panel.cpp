// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/input_preview_panel.h"
#include "editing/coordinates.h"
#include "model/i18n.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_execution.h"
#include "ui/editor_chrome.h"
#include "ui/input_highlighter.h"
#include "ui/main_window.h"
#include "ui/theme.h"
#include <QtWidgets>

namespace mf {
namespace {
QColor advancedMark(const QPalette &palette) {
    return accentColor(palette, "#8250df", "#b392f0");
}
QColor extraMark(const QPalette &palette) {
    return accentColor(palette, "#d97706", "#f0b35a");
}
QLabel *caption(const QString &text) {
    auto *label = new QLabel(text);
    label->setProperty("caption", true);
    label->setWordWrap(true);
    return label;
}
// A diagnostic as a link to where it is set: "tab:<index>", or "field:<key>" of "2 計算条件".
QString sourceLink(const QString &target, const QString &text) {
    return QString("<a href=\"%1\">%2</a>").arg(target.toHtmlEscaped(), text.toHtmlEscaped());
}
// The titles of the tabs that set a block, by number (from 1).
QString tabName(int tab) {
    static const char *const names[]{QT_TR_NOOP("1 分子"), QT_TR_NOOP("2 計算条件"),
                                     QT_TR_NOOP("3 実行設定")};
    return tab >= 1 && tab <= 3 ? tr(names[tab - 1]) : QString();
}
} // namespace

// The input, read only, with line numbers in a gutter (so copied or saved input never contains
// them) and the lines from the details and the additional input marked at its edge.
class InputEditor : public QPlainTextEdit {
    class Gutter : public QWidget {
        InputEditor *editor;

      public:
        explicit Gutter(InputEditor *owner) : QWidget(owner), editor(owner) {}
        void paintEvent(QPaintEvent *event) override {
            editor->paintNumbers(event);
        }
    };
    Gutter *gutter;
    int gutterWidth() const {
        return 22 + fontMetrics().horizontalAdvance('9') * QString::number(blockCount()).size();
    }
    void paintNumbers(QPaintEvent *event) {
        QPainter painter(gutter);
        const ChromeColors c(palette());
        painter.fillRect(event->rect(), c.window);
        const int current = textCursor().blockNumber();
        auto block = firstVisibleBlock();
        while (block.isValid()) {
            const auto geometry = blockBoundingGeometry(block).translated(contentOffset());
            if (geometry.top() > event->rect().bottom())
                break;
            const int line = block.blockNumber();
            if (block.isVisible()) {
                const QColor mark = advanced.contains(line) ? advancedMark(palette())
                                    : extra.contains(line)  ? extraMark(palette())
                                                            : QColor();
                if (mark.isValid())
                    painter.fillRect(QRectF(0, geometry.top(), 3, geometry.height()), mark);
                auto font = this->font();
                font.setBold(line == current);
                painter.setFont(font);
                painter.setPen(line == current ? c.accentText : c.muted);
                painter.drawText(0, qRound(geometry.top()), gutter->width() - 8, fontMetrics().height(),
                                 Qt::AlignRight, QString::number(line + 1));
            }
            block = block.next();
        }
    }
    void updateGutter() {
        setViewportMargins(gutterWidth(), 0, 0, 0);
        const auto area = contentsRect();
        gutter->setGeometry(area.left(), area.top(), gutterWidth(), area.height());
        gutter->update();
    }
    void resizeEvent(QResizeEvent *event) override {
        QPlainTextEdit::resizeEvent(event);
        updateGutter();
    }

  public:
    QSet<int> advanced, extra;
    InputEditor() : gutter(new Gutter(this)) {
        setReadOnly(true);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        setFrameShape(QFrame::NoFrame);
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { updateGutter(); });
        connect(this, &QPlainTextEdit::updateRequest, gutter, [this] { gutter->update(); });
        connect(this, &QPlainTextEdit::cursorPositionChanged, gutter, [this] { gutter->update(); });
        highlightInput(document());
        updateGutter();
    }
    // A tint behind the lines of a block (from 0), stronger on the cursor's line.
    void markBlock(int first, int count) {
        QList<QTextEdit::ExtraSelection> selections;
        auto color = ChromeColors(palette()).accent;
        for (int line = first; line < first + count; ++line) {
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(document()->findBlockByNumber(line));
            color.setAlpha(line == textCursor().blockNumber() ? 46 : 20);
            s.format.setBackground(color);
            s.format.setProperty(QTextFormat::FullWidthSelection, true);
            selections << s;
        }
        setExtraSelections(selections);
    }
};

InputPreviewPanel::InputPreviewPanel(MainWindow *owner) : QWidget(owner), window(owner) {
    setObjectName("inputPreviewTab");
    setAttribute(Qt::WA_StyledBackground);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Which input, search, and copy or save it.
    auto *toolbar = new QFrame;
    toolbar->setObjectName("inputPreviewToolbar");
    toolbar->setProperty("toolbar", true);
    auto *tools = new FlowLayout(toolbar, 8, 6);
    tools->setContentsMargins(12, 8, 12, 8);
    outer->addWidget(toolbar);
    auto *modeBox = new QWidget;
    auto *modeRow = new QHBoxLayout(modeBox);
    modeRow->setContentsMargins(0, 0, 0, 0);
    modeRow->addWidget(caption(tr("表示する入力")));
    mode = new QComboBox;
    mode->setObjectName("inputPreviewMode");
    mode->addItem(tr("書出し用入力（設定どおりのパス）"), "input");
    mode->addItem(tr("実行時の入力（入力パスを解決）"), "execution");
    modes = new ChoiceButtons(mode, ChoiceButtons::Segments, {tr("書出し用"), tr("実行時（パスを解決）")});
    modes->setObjectName("inputPreviewModes");
    modeRow->addWidget(modes);
    tools->addWidget(modeBox);
    auto *gap = new QWidget;
    gap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tools->addWidget(gap);
    auto *find = new QWidget;
    auto *findRow = new QHBoxLayout(find);
    findRow->setContentsMargins(0, 0, 0, 0);
    findRow->setSpacing(4);
    search = new QLineEdit;
    search->setObjectName("inputPreviewSearch");
    search->setProperty("search", true);
    search->setPlaceholderText(tr("入力内を検索（Ctrl+F）"));
    search->setClearButtonEnabled(true);
    search->addAction(lineIcon("search"), QLineEdit::LeadingPosition);
    search->setFixedWidth(280);
    findRow->addWidget(search);
    for (bool backwards : {true, false}) {
        auto *button = new QPushButton;
        button->setObjectName(backwards ? "inputPreviewPrevious" : "inputPreviewNext");
        button->setIcon(lineIcon(backwards ? "previous" : "next"));
        button->setToolTip(backwards ? tr("前を検索") : tr("次を検索"));
        findRow->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, backwards] { findText(backwards); });
    }
    find->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed); // As wide as its parts.
    search->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    tools->addWidget(find);
    copy = new QPushButton(tr("全文をコピー"));
    save = new QPushButton(tr("入力ファイルを書き出す…"));
    copy->setObjectName("inputPreviewCopy");
    save->setObjectName("inputPreviewSave");
    copy->setIcon(lineIcon("copy"));
    save->setIcon(lineIcon("apply"));
    tools->addWidget(copy);
    tools->addWidget(save);

    banner = new QFrame;
    banner->setObjectName("inputPreviewBanner");
    banner->setProperty("banner", true);
    auto *bannerRow = new QHBoxLayout(banner);
    bannerRow->setContentsMargins(12, 6, 12, 6);
    hint = new QLabel;
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::PlainText);
    hint->setObjectName("inputPreviewHint");
    bannerRow->addWidget(hint, 1);
    banner->hide();
    outer->addWidget(banner);

    auto *body = new QHBoxLayout;
    body->setSpacing(0);
    outer->addLayout(body, 1);
    // The blocks of the input, each with the tabs that set it.
    auto *outlineColumn = new QFrame;
    outlineColumn->setObjectName("inputPreviewOutlineColumn");
    outlineColumn->setFixedWidth(270);
    auto *outlineLayout = new QVBoxLayout(outlineColumn);
    outlineLayout->setContentsMargins(8, 12, 8, 12);
    outlineLayout->setSpacing(6);
    auto *outlineHead = new QHBoxLayout;
    outlineHead->setContentsMargins(8, 0, 8, 0);
    outlineHead->addWidget(caption(tr("入力の構成")));
    outlineHead->addStretch();
    outlineHead->addWidget(caption(tr("編集元")));
    outlineLayout->addLayout(outlineHead);
    outlineList = new QListWidget;
    outlineList->setObjectName("inputPreviewOutline");
    outlineList->setProperty("nav", true);
    outlineList->setFrameShape(QFrame::NoFrame);
    outlineList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outlineLayout->addWidget(outlineList, 1);
    outlineLayout->addWidget(caption(
        tr("番号はその行を設定するタブ、「詳」は「2 計算条件」の詳細設定です。押すとそこへ移ります。")));
    connect(outlineList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || row >= outline.blocks.size())
            return;
        QTextCursor cursor(input->document()->findBlockByNumber(outline.blocks[row].line));
        input->setTextCursor(cursor);
        input->centerCursor();
    });
    body->addWidget(outlineColumn);

    // The input with the tabs of the block at the cursor over it, and a status line.
    auto *center = new QWidget;
    center->setObjectName("inputPreviewCenter");
    center->setProperty("canvas", true);
    auto *centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(16, 16, 16, 16);
    auto *code = new QFrame;
    code->setObjectName("inputPreviewCode");
    auto *codeLayout = new QVBoxLayout(code);
    codeLayout->setContentsMargins(1, 1, 1, 1);
    codeLayout->setSpacing(0);
    input = new InputEditor;
    input->setObjectName("inputPreviewText");
    input->setPlaceholderText(tr("分子と計算条件を設定すると、NWChem入力を自動表示します。"));
    auto *host = new OverlayHost(input);
    codeLayout->addWidget(host, 1);
    sourceChip = new QFrame;
    sourceChip->setObjectName("inputPreviewSource");
    auto *chipLayout = new QHBoxLayout(sourceChip);
    chipLayout->setContentsMargins(10, 5, 6, 5);
    chipLayout->setSpacing(8);
    sourceTitle = new QLabel;
    sourceTitle->setObjectName("inputPreviewSourceTitle");
    chipLayout->addWidget(sourceTitle);
    sourceLinks = new QWidget;
    auto *linksLayout = new QHBoxLayout(sourceLinks);
    linksLayout->setContentsMargins(0, 0, 0, 0);
    linksLayout->setSpacing(4);
    chipLayout->addWidget(sourceLinks);
    sourceChip->hide();
    host->addOverlay(sourceChip, Qt::AlignTop | Qt::AlignRight);
    auto *footer = new QFrame;
    footer->setObjectName("inputPreviewFooter");
    auto *footerRow = new QHBoxLayout(footer);
    footerRow->setContentsMargins(12, 5, 12, 5);
    footerRow->setSpacing(14);
    position = new QLabel;
    position->setObjectName("inputPreviewPosition");
    footerRow->addWidget(position);
    stats = new QLabel;
    stats->setObjectName("inputPreviewStats");
    footerRow->addWidget(stats);
    searchStatus = new QLabel;
    searchStatus->setObjectName("inputPreviewSearchStatus");
    footerRow->addWidget(searchStatus, 1);
    // The legend of the marks: a short bar in the color of each.
    for (const auto &[name, text] : {std::pair{"inputPreviewLegendAdvanced", QT_TR_NOOP("詳細設定から")},
                                     {"inputPreviewLegendExtra", QT_TR_NOOP("追加入力から")}}) {
        auto *swatch = new QLabel;
        swatch->setObjectName(name);
        swatch->setFixedSize(4, 13);
        footerRow->addWidget(swatch);
        footerRow->addSpacing(-8);
        footerRow->addWidget(new QLabel(tr(text)));
    }
    codeLayout->addWidget(footer);
    centerLayout->addWidget(code, 1);
    body->addWidget(center, 1);

    // The input in brief, its checks, and how its paths are written.
    auto *rail = new QWidget;
    rail->setObjectName("inputPreviewRail");
    rail->setProperty("canvas", true);
    rail->setFixedWidth(330);
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(0, 16, 16, 16);
    railLayout->setSpacing(12);
    body->addWidget(rail);
    summaryCard = new CalculationSummary("inputPreview", tr("この入力"), [this](const QString &link) {
        if (link == "memory")
            window->showSetting(2, "memory_mb");
    });
    railLayout->addWidget(summaryCard);
    checkCard = new StatusCard;
    checkCard->setObjectName("inputPreviewCheck");
    checkCard->headline()->setObjectName("inputPreviewStatus");
    auto *reload = checkCard->addRecheck("inputPreviewRefresh",
                                         tr("表示は編集に追従します。参照ファイル・保存先など、MolFormaの外で"
                                            "変わったものを確認し直すときに使います。"));
    diagnostics = new QTextBrowser;
    diagnostics->setObjectName("inputPreviewDiagnostics");
    diagnostics->setOpenLinks(false);
    diagnostics->setOpenExternalLinks(false);
    diagnostics->setFrameShape(QFrame::NoFrame);
    checkCard->body()->addWidget(diagnostics);
    railLayout->addWidget(checkCard);
    // How the paths of the input are written, folded with a summary (and remembered).
    pathCard = new SettingsCard(tr("書き出し後のパス"));
    pathCard->setObjectName("inputPreviewPathCard");
    auto *pathsToggle = pathCard->setCollapsible(window->preference("inputPreview/showPaths", true).toBool());
    pathsToggle->setObjectName("inputPreviewPathsToggle");
    connect(pathsToggle, &QToolButton::clicked, this,
            [this](bool on) { window->setPreference("inputPreview/showPaths", on); });
    paths = new QLabel;
    paths->setObjectName("inputPreviewPaths");
    paths->setWordWrap(true);
    paths->setTextFormat(Qt::RichText);
    pathCard->body()->addWidget(paths);
    railLayout->addWidget(pathCard);
    railLayout->addStretch();
    const auto openLink = [this](const QString &link) {
        const QUrl url(link);
        bool ok = false;
        const int tab = url.path().toInt(&ok);
        if (url.scheme() == "tab" && ok)
            navigate(tab);
        else if (url.scheme() == "field")
            window->showSetting(1, url.path());
        else if (url.scheme() == "execution")
            window->showSetting(2, url.path());
    };
    connect(paths, &QLabel::linkActivated, this, openLink);
    connect(diagnostics, &QTextBrowser::anchorClicked, this,
            [openLink](const QUrl &url) { openLink(url.toString()); });

    connect(mode, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(reload, &QPushButton::clicked, this, [this] { refresh(); });
    connect(copy, &QPushButton::clicked, this, [this] { exportInput(false); });
    connect(save, &QPushButton::clicked, this, [this] { exportInput(true); });
    connect(search, &QLineEdit::returnPressed, this, [this] { findText(false); });
    connect(search, &QLineEdit::textChanged, searchStatus, &QLabel::clear);
    connect(input, &QPlainTextEdit::cursorPositionChanged, this, &InputPreviewPanel::followCursor);
    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QShortcut::activated, this, [this] {
        search->setFocus();
        search->selectAll();
    });
}
void InputPreviewPanel::showLine(int line) {
    window->showPanel(this);
    window->refreshPanel(this);
    QTextCursor cursor(input->document()->findBlockByNumber(line));
    input->setTextCursor(cursor);
    input->centerCursor();
    input->setFocus();
}
void InputPreviewPanel::navigate(int tab) {
    window->showPanel(tab);
}
void InputPreviewPanel::openSource(int tab, const QString &setting, const QString &group) {
    window->showSetting(tab - 1, setting, group);
}
QList<InputPreviewPanel::Source> InputPreviewPanel::sources(const NwchemInputBlock &b) {
    QList<Source> result;
    for (const int tab : b.tabs)
        if (tab != 2 || !b.setting.isEmpty() || b.group.isEmpty()) // Not when only the details set it.
            result << Source{tab, tab == 1 ? QString() : b.setting, {}};
    if (!b.group.isEmpty())
        result << Source{2, {}, b.group};
    return result;
}
void InputPreviewPanel::rebuildOutline() {
    const QSignalBlocker blocker(outlineList);
    outlineList->clear();
    const ChromeColors c(palette());
    for (int i = 0; i < outline.blocks.size(); ++i) {
        const auto &b = outline.blocks[i];
        auto *item = new QListWidgetItem(outlineList);
        item->setData(Qt::UserRole, b.line);
        item->setToolTip(b.title);
        auto *row = new QWidget;
        row->setObjectName(QString("inputPreviewBlock%1").arg(b.line + 1));
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(8, 3, 6, 3);
        layout->setSpacing(6);
        auto *number = new QLabel(QString::number(b.line + 1));
        number->setProperty("mono", true);
        number->setProperty("muted", true);
        number->setMinimumWidth(number->fontMetrics().horizontalAdvance("9999"));
        layout->addWidget(number);
        auto *title = new ElidedLabel(b.title);
        title->setProperty("blockTitle", true);
        layout->addWidget(title, 1);
        for (const auto &source : sources(b)) {
            const bool details = !source.group.isEmpty();
            auto *badge = new QToolButton;
            badge->setObjectName(QString("inputPreviewSource%1_%2%3")
                                     .arg(b.line + 1)
                                     .arg(source.tab)
                                     .arg(details ? "_details" : ""));
            badge->setText(details ? tr("詳") : QString::number(source.tab));
            badge->setProperty(
                "source", details                                                                ? "advanced"
                          : (source.setting == "extra_input" || source.setting == "extra_tasks") ? "extra"
                                                                                                 : "tab");
            const auto where = tabName(source.tab);
            badge->setToolTip(details ? tr("「%1」の詳細設定で編集").arg(where)
                                      : tr("「%1」で編集").arg(where));
            badge->setAccessibleName(details ? tr("%1の詳細設定で編集").arg(where)
                                             : tr("%1で編集").arg(where));
            badge->setFixedSize(22, 22);
            layout->addWidget(badge);
            connect(badge, &QToolButton::clicked, this,
                    [this, source] { openSource(source.tab, source.setting, source.group); });
        }
        item->setSizeHint(QSize(0, row->sizeHint().height()));
        outlineList->setItemWidget(item, row);
    }
}
void InputPreviewPanel::followCursor() {
    const auto cursor = input->textCursor();
    const int line = cursor.blockNumber();
    position->setText(tr("行 %1 / 列 %2").arg(line + 1).arg(cursor.positionInBlock() + 1));
    int found = -1;
    for (int i = 0; i < outline.blocks.size(); ++i)
        if (line >= outline.blocks[i].line && line < outline.blocks[i].line + outline.blocks[i].lines)
            found = i;
    {
        const QSignalBlocker blocker(outlineList);
        outlineList->setCurrentRow(found);
    }
    if (found < 0 || contents.isEmpty()) {
        input->setExtraSelections({});
        sourceChip->hide();
        return;
    }
    const auto &b = outline.blocks[found];
    input->markBlock(b.line, b.lines);
    // Where this block is set, as links to the tabs.
    sourceTitle->setText(tr("%1 ブロックの編集元").arg(b.keyword));
    while (auto *item = sourceLinks->layout()->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const auto &source : sources(b)) {
        QString text = tabName(source.tab);
        if (!source.group.isEmpty()) {
            for (const auto &g : nwchemAdvancedGroups())
                if (g.value == source.group)
                    text = tr("%1 › 詳細設定（%2）")
                               .arg(text, g.value == "visualization" ? QString("CUBE / MOLDEN") : g.label);
        } else if ((source.setting == "extra_input" || source.setting == "extra_tasks"))
            text = tr("%1 › 追加入力").arg(text);
        else if (source.tab == 2 && !source.setting.isEmpty())
            if (const auto section = window->settingSectionTitle(source.setting); !section.isEmpty())
                text += " › " + section;
        auto *link = new QPushButton(text);
        link->setProperty("link", true);
        link->setCursor(Qt::PointingHandCursor);
        sourceLinks->layout()->addWidget(link);
        connect(link, &QPushButton::clicked, this,
                [this, source] { openSource(source.tab, source.setting, source.group); });
    }
    sourceChip->setVisible(!b.tabs.isEmpty());
    sourceChip->adjustSize();
}
void InputPreviewPanel::refresh() {
    const auto &d = window->displayDocument();
    const auto report = validateNwchem(d);
    const bool resolved = mode->currentData() == "execution";
    modes->sync();
    QStringList errors = report.errors;
    QString html;
    // The setting a message is about in "2 計算条件" or "3 実行設定", else its tab.
    const auto target = [](const Diagnostic &d) {
        if (d.setting.isEmpty() || (d.tab != 1 && d.tab != 2))
            return QString("tab:%1").arg(d.tab);
        return (d.tab == 1 ? "field:" : "execution:") + d.setting;
    };
    auto append = [&](const QString &label, const QString &message, const QString &link,
                      const QColor &color) {
        html += QString("<p><b style=\"color:%1\">%2</b> %3</p>")
                    .arg(color.name(), label, sourceLink(link, message));
    };
    const ChromeColors c(palette());
    for (const auto &diagnostic : report.diagnostics)
        if (diagnostic.severity == DiagnosticSeverity::Error)
            append(tr("エラー:"), diagnostic.message, target(diagnostic), c.bad);
    QString text;
    NwchemInputOutline generatedOutline;
    try {
        if (resolved) {
            const auto base = window->projectPath().isEmpty()
                                  ? QDir::currentPath()
                                  : QFileInfo(window->projectPath()).absolutePath();
            const auto plan = executionPreviewPlan(d, base);
            text = plan.input;
            generatedOutline = plan.inputOutline;
        } else if (errors.isEmpty()) {
            const auto generated = generateNwchemInput(d);
            text = generated.text;
            generatedOutline = generated.outline;
        }
    } catch (const ValidationFailure &e) {
        // The execution settings in resolved mode: each error points at its setting.
        for (const auto &diagnostic : e.report.diagnostics)
            if (diagnostic.severity == DiagnosticSeverity::Error && !errors.contains(diagnostic.message)) {
                errors << diagnostic.message;
                append(tr("エラー:"), diagnostic.message, target(diagnostic), c.bad);
            }
    } catch (const std::exception &e) {
        errors << QString::fromUtf8(e.what());
        append(tr("エラー:"), errors.last(), "tab:2", c.bad);
    }
    for (const auto &diagnostic : report.diagnostics) {
        if (diagnostic.severity != DiagnosticSeverity::Warning)
            continue;
        const auto &warning = diagnostic.message;
        // The line a check is about, when the input names it ("frequencies numerical").
        QString where;
        if (diagnostic.code == "frequencies.numerical")
            for (const int line : generatedOutline.settings.value("numerical_frequencies"))
                where = tr("（%1 行目）").arg(line + 1);
        append(tr("確認:"), warning + where, target(diagnostic), c.muted);
    }
    if (errors.isEmpty())
        html.prepend(tr("<p style=\"color:%1\">分子・計算条件・詳細設定の検証を通過しました。</p>")
                         .arg(c.muted.name()));
    diagnostics->document()->setDefaultStyleSheet(
        QString("a { color: %1; text-decoration: none; }").arg(c.accentText.name()));
    diagnostics->setHtml(html);
    // As tall as its messages, up to a limit.
    diagnostics->document()->setTextWidth(diagnostics->width() > 60 ? diagnostics->viewport()->width() : 280);
    diagnostics->setFixedHeight(std::min(300, int(diagnostics->document()->size().height()) + 4));
    pathCard->titleLabel()->setText(resolved ? tr("実行時のパス") : tr("書き出し後のパス"));
    pathCard->setSummary(resolved ? tr("ジョブフォルダー基準に解決") : tr("相対パスは変更せず出力"));
    paths->setText((resolved
                        ? tr("入力movecsは参照基準から絶対パスへ変換します。"
                             "相対出力先・scratch・permanentは新しいジョブフォルダーを基準にします。"
                             "この画面を開いてもフォルダーは作成しません。")
                        : tr("相対パスは変更せず出力します。外部で実行するときは、"
                             "NWChemの作業ディレクトリを基準に必要なファイル・保存先を用意してください。")) +
                   tr("<br><br>実行ファイル・MPI・基底ライブラリ・参照ファイルの存在と書込み権限は "
                      "<a href=\"tab:2\" style=\"color:%1\">「3 実行設定」</a>"
                      " で確認してください。ここではNWChemを起動しません。")
                       .arg(c.accentText.name()));
    summaryCard->update(d);
    const bool pending = window->hasCandidate() || window->chemistryRunning();
    hint->setText(window->hasCandidate()
                      ? tr("未確定の分子候補を表示しています。確定または取消後にコピー・保存できます。")
                  : window->chemistryRunning() ? tr("分子の整形中です。終了後にコピー・保存できます。")
                                               : QString());
    banner->setVisible(pending);
    if (pending)
        checkCard->setState(StatusCard::Waiting, tr("候補・整形の完了待ち（出力不可）"));
    else if (!errors.isEmpty())
        checkCard->setState(StatusCard::Error,
                            tr("入力を生成できません（エラー %n 件）", nullptr, int(errors.size())));
    else
        checkCard->setState(StatusCard::Ok, report.warnings.isEmpty()
                                                ? tr("入力を生成できます")
                                                : tr("入力を生成できます（確認 %n 件）", nullptr,
                                                     int(report.warnings.size())));
    contents = errors.isEmpty() ? text : QString();
    stats->setText(contents.isEmpty() ? QString()
                                      : tr("%n 行 · UTF-8 · %1 バイト", nullptr, int(contents.count('\n')))
                                            .arg(QLocale().toString(contents.toUtf8().size())));
    const auto nextOutline = contents.isEmpty() ? NwchemInputOutline{} : generatedOutline;
    if (input->toPlainText() != contents || outline != nextOutline) {
        const int scroll = input->verticalScrollBar()->value();
        // As numbers: a cursor kept across setPlainText would follow the inserted text to its end.
        const int anchor = input->textCursor().anchor(), at = input->textCursor().position();
        // Marks first: the text change repaints the gutter.
        outline = nextOutline;
        input->advanced = outline.advanced;
        input->extra = outline.extra;
        input->setPlainText(contents);
        QTextCursor restored(input->document());
        restored.setPosition(std::min(anchor, int(contents.size())));
        restored.setPosition(std::min(at, int(contents.size())), QTextCursor::KeepAnchor);
        input->setTextCursor(restored);
        input->verticalScrollBar()->setValue(scroll);
        searchStatus->clear();
        rebuildOutline();
    }
    followCursor();
    copy->setEnabled(!pending && !contents.isEmpty());
    save->setEnabled(copy->isEnabled());
    setBadge(!d.atoms.isEmpty() && !errors.isEmpty(), checkCard->headline()->text());
}
void InputPreviewPanel::refreshBadge() {
    const auto &d = window->displayDocument();
    const auto errors = validateNwchem(d).errors;
    setBadge(!d.atoms.isEmpty() && !errors.isEmpty(),
             errors.isEmpty() ? tr("入力を生成できます")
                              : tr("入力を生成できません（エラー %n 件）", nullptr, int(errors.size())));
}
void InputPreviewPanel::setBadge(bool failed, const QString &tip) {
    window->setPanelDiagnostics(this, failed, tip);
}
QString InputPreviewPanel::prepareExport() {
    window->commitPendingSettings();
    if (window->hasCandidate() || window->chemistryRunning())
        fail(tr("分子候補の確定・取消、または整形の終了後に出力してください。"));
    refresh();
    if (contents.isEmpty())
        fail(tr("入力を生成できません。検証結果のエラーを修正してください。"));
    return contents;
}
void InputPreviewPanel::saveInput(const QString &path) {
    const auto text = prepareExport();
    writeFile(path, text.toUtf8());
}
void InputPreviewPanel::exportInput(bool toFile) {
    window->safe([&] {
        const auto text = prepareExport();
        if (toFile) {
            auto path = QFileDialog::getSaveFileName(
                this, tr("NWChem入力ファイルを書き出す"),
                nwchemSettings(window->document())["start"].toString() + ".nw", "NWChem (*.nw)");
            if (path.isEmpty())
                return;
            if (!path.endsWith(".nw", Qt::CaseInsensitive))
                path += ".nw";
            writeFile(path, text.toUtf8());
        } else
            QApplication::clipboard()->setText(text);
        window->statusBar()->showMessage(
            toFile ? tr("NWChem入力を書き出しました。") : tr("NWChem入力をコピーしました。"), 5000);
    });
}
void InputPreviewPanel::findText(bool backwards) {
    if (search->text().isEmpty())
        return;
    const auto original = input->textCursor();
    const auto flags = backwards ? QTextDocument::FindBackward : QTextDocument::FindFlags{};
    bool found = input->find(search->text(), flags);
    if (!found) {
        input->moveCursor(backwards ? QTextCursor::End : QTextCursor::Start);
        found = input->find(search->text(), flags);
    }
    if (!found)
        input->setTextCursor(original);
    searchStatus->setText(found ? tr("一致する文字列を選択しました。") : tr("一致する文字列はありません。"));
}
void InputPreviewPanel::applyChromeStyle() {
    const auto pal = static_cast<QWidget *>(window)->palette();
    const auto advanced = advancedMark(pal).name(), extra = extraMark(pal).name();
    setStyleSheet(chromeRules(settingsRules() + R"(
        #inputPreviewTab { background: @window; }
        #inputPreviewOutlineColumn { background: @base; border: none; border-right: 1px solid @line; }
        QListWidget#inputPreviewOutline::item { border-radius: 6px; }
        QListWidget#inputPreviewOutline::item:hover { background: @hover; }
        QListWidget#inputPreviewOutline::item:selected { background: @tint; }
        QListWidget#inputPreviewOutline QLabel[blockTitle="true"] { font-family: monospace; color: @text; }
        QListWidget#inputPreviewOutline QToolButton { border: 1px solid @line; border-radius: 11px; background: @base;
                                                      color: @text; font-size: 11px; font-weight: bold; }
        QListWidget#inputPreviewOutline QToolButton:hover { background: @hover; }
        QListWidget#inputPreviewOutline QToolButton[source="advanced"] { border-color: ADVANCED; color: ADVANCED; }
        QListWidget#inputPreviewOutline QToolButton[source="extra"] { border-color: EXTRA; color: EXTRA; }
        #inputPreviewCode { background: @base; border: 1px solid @line; border-radius: 10px; }
        #inputPreviewFooter { background: @window; border: none; border-top: 1px solid @line;
                              border-bottom-left-radius: 10px; border-bottom-right-radius: 10px; }
        #inputPreviewFooter QLabel { color: @muted; }
        #inputPreviewSource { background: @base; border: 1px solid @line; border-radius: 8px; }
        QLabel#inputPreviewSourceTitle { color: @muted; }
        QTextBrowser#inputPreviewDiagnostics { background: transparent; }
        #inputPreviewRail { background: @window; }
        QLabel#inputPreviewLegendAdvanced { background: ADVANCED; border-radius: 1px; }
        QLabel#inputPreviewLegendExtra { background: EXTRA; border-radius: 1px; }
    )",
                              pal)
                      .replace("ADVANCED", advanced)
                      .replace("EXTRA", extra));
}
} // namespace mf
