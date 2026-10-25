// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/results_panel.h"
#include "editing/coordinates.h"
#include "model/geometry.h"
#include "results/molden_orbitals.h"
#include "ui/editor_chrome.h"
#include "ui/main_window.h"
#include "ui/molecule_view.h"
#include "ui/result_charts.h"
#include "ui/theme.h"
#include <QtWidgets>
#include <algorithm>
#include <cmath>

namespace mf {
namespace {
// The tables of the history: no grid, a muted header, the selection in the tint.
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
    return t;
}
// Numbers in fixed-width digits at the right, so their decimal points line up.
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
constexpr double HartreeToKcal = 627.509474, HartreeToEv = 27.211386245988, EvNanometer = 1239.84198;
QString sourceStamp(const QFileInfo &file) {
    if (!file.exists() || !file.isFile())
        return {};
    return QString::fromUtf8(
        QJsonDocument(
            QJsonArray{
                file.absoluteFilePath(), file.canonicalFilePath(), QString::number(file.size()),
                QString::number(file.lastModified().toMSecsSinceEpoch()),
                QString::number(file.fileTime(QFileDevice::FileMetadataChangeTime).toMSecsSinceEpoch())})
            .toJson(QJsonDocument::Compact));
}
// "bond 1 2" (MolForma's scan title) in the words of the calculation tab.
QString coordinateText(const QString &coordinate) {
    const auto words = coordinate.split(' ', Qt::SkipEmptyParts);
    const QString kind = words.value(0) == "bond" ? "距離" : words.value(0) == "angle" ? "角度" : "二面角";
    return kind + ' ' + words.mid(1).join("–");
}
QString solvationText(const QJsonObject &s) {
    QStringList lines;
    if (s.contains("smd_dg"))
        lines << QString("SMD溶媒和自由エネルギー（1 M基準）: %1 kcal/mol、溶液中の全自由エネルギー %2 Eh")
                     .arg(fixed(s["smd_dg"].toDouble() * HartreeToKcal, 2), real(s["smd_total"].toDouble()));
    else if (s.contains("gas") && s.contains("solution"))
        lines << QString("COSMO溶媒和エネルギー（静電項、溶液 − 気相）: %1 kcal/mol")
                     .arg(fixed((s["solution"].toDouble() - s["gas"].toDouble()) * HartreeToKcal, 2));
    return lines.join('\n');
}
// Summary lines for BSSE, solvation and polarizability (also shown in the 物性 tab).
QString resultPropertyLines(const ResultData &data) {
    QString text;
    if (data.properties.contains("bsse")) {
        const auto b = data.properties.value("bsse").toObject();
        text += QString("BSSE（counterpoise）補正: %1 kcal/mol、補正後 %2 Eh（補正前 %3 Eh）\n")
                    .arg(fixed(b["error"].toDouble() * HartreeToKcal, 3), real(b["corrected"].toDouble()),
                         real(b["supermolecular"].toDouble()));
    }
    if (const auto s = solvationText(data.properties.value("solvation").toObject()); !s.isEmpty())
        text += s + '\n';
    if (data.properties.contains("polarizability")) {
        const auto p = data.properties.value("polarizability").toObject();
        constexpr double BohrCubed = 0.1481847; // Å³ per a.u. of polarizability volume.
        text += QString("静的分極率: 等方 %1 a.u.（%2 Å³）、異方性 %3 a.u.\n")
                    .arg(fixed(p["isotropic"].toDouble(), 4), fixed(p["isotropic"].toDouble() * BohrCubed, 4),
                         fixed(p["anisotropy"].toDouble(), 4));
    }
    if (data.properties.contains("scan"))
        text += QString("座標スキャン: %1点（「経路・スキャン」に表示）\n")
                    .arg(data.properties.value("scan").toArray().size());
    if (data.properties.contains("irc")) {
        const auto irc = data.properties.value("irc").toObject();
        text +=
            QString("IRC: %1点%2（「経路・スキャン」に表示）\n")
                .arg(irc["points"].toArray().size())
                .arg(irc["complete"].toBool() ? "、両方向とも収束" : "、収束メッセージは両方向にありません");
    }
    return text;
}
QString orbitalKey(int orbital, double iso, const QString &normalization, int divisions) {
    return QString::fromUtf8(
        QJsonDocument(QJsonArray{orbital, iso, normalization, divisions}).toJson(QJsonDocument::Compact));
}
QString formatName(const QString &format) {
    return format == "NWChem" ? QString("NWChem 出力") : format;
}
// "C6H12O6" with the counts as subscripts.
QString formulaHtml(const QString &formula) {
    static const QRegularExpression digits("(\\d+)");
    return formula.toHtmlEscaped().replace(digits, "<sub>\\1</sub>");
}
// Style rules that depend on a dynamic property ("tone", "chip") apply once polished again.
void repolishAll(QWidget *widget) {
    auto widgets = widget->findChildren<QWidget *>();
    widgets << widget;
    for (auto *w : std::as_const(widgets)) {
        w->style()->unpolish(w);
        w->style()->polish(w);
    }
}
void setChip(QLabel *chip, const QString &text, const QString &tone, const QString &tip = {}) {
    chip->setText(text);
    chip->setToolTip(tip);
    chip->setVisible(!text.isEmpty());
    if (chip->property("chip").toString() != tone) {
        chip->setProperty("chip", tone);
        repolishAll(chip);
    }
}
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
// A card of the summary: what, the value and a line about it. With a tab it is a button to it.
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
// The IR intensity of a mode as a bar behind its value, relative to the strongest mode.
class IntensityDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        const auto share = index.data(Qt::UserRole);
        if (share.isValid() && !(option.state & QStyle::State_Selected)) {
            const ChromeColors c(option.palette);
            const QRectF r = QRectF(option.rect).adjusted(4, option.rect.height() - 7, -4, -3);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(c.hover);
            painter->drawRoundedRect(r, 2, 2);
            painter->setBrush(c.accent);
            painter->drawRoundedRect(
                QRectF(r.left(), r.top(), std::max(2., r.width() * share.toDouble()), r.height()), 2, 2);
            painter->restore();
        }
        QStyledItemDelegate::paint(painter, option, index);
    }
};
// Shared by the summary cards and the convergence criteria.
const char *TileRules = R"(
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
} // namespace

ResultsPanel::ResultsPanel(MainWindow *owner) : QWidget(owner), window(owner) {
    setObjectName("resultsTab");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    // The bar: which job and file, what kind of file and how the calculation ended, then actions.
    resultBar = new QFrame;
    resultBar->setObjectName("resultBar");
    auto *bar = new FlowLayout(resultBar, 8, 6);
    bar->setContentsMargins(14, 8, 14, 8);
    jobIcon = new QLabel;
    jobIcon->setObjectName("resultsJobIcon");
    jobLabel = new ElidedLabel;
    jobLabel->setObjectName("resultsJob");
    jobLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed); // Its width in the flow.
    jobLabel->setMaximumWidth(300);
    files = new QComboBox;
    files->setObjectName("resultsFiles");
    files->setAccessibleName("結果ファイル");
    files->setToolTip("ジョブフォルダーの結果ファイル。選ぶとすぐに読み込みます。");
    files->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    files->setMinimumContentsLength(20);
    formatChip = new QLabel;
    formatChip->setObjectName("resultsFormat");
    outcomeChip = new QLabel;
    outcomeChip->setObjectName("resultsOutcome");
    emptyHint = new QLabel("結果ファイルまたはジョブフォルダーを開いてください。");
    emptyHint->setObjectName("resultsEmptyHint");
    for (QWidget *w : {static_cast<QWidget *>(jobIcon), static_cast<QWidget *>(jobLabel),
                       static_cast<QWidget *>(files), static_cast<QWidget *>(formatChip),
                       static_cast<QWidget *>(outcomeChip), static_cast<QWidget *>(emptyHint)})
        bar->addWidget(w);
    bar->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum));
    openButton = new QToolButton;
    openButton->setObjectName("resultsOpen");
    openButton->setText("開く…");
    openButton->setIcon(lineIcon("folder"));
    openButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    openButton->setPopupMode(QToolButton::MenuButtonPopup);
    openButton->setToolTip("結果ファイル（NWChem出力・XYZ・CUBE・MOLDEN）を開きます。▾ からジョブフォルダーも"
                           "開けます。");
    auto *openMenu = new QMenu(openButton);
    auto *openFileAction = openMenu->addAction("結果ファイルを開く…");
    auto *openDirectoryAction = openMenu->addAction("ジョブフォルダーを開く…");
    openDirectoryAction->setObjectName("resultsDirectory");
    openButton->setMenu(openMenu);
    reload = new QPushButton(lineIcon("refresh"), QString());
    reload->setObjectName("resultsReload");
    reload->setToolTip("再読込（実行途中のログを更新）");
    reload->setAccessibleName("再読込");
    cancel = new QPushButton(lineIcon("close"), "中止");
    cancel->setObjectName("resultsCancel");
    cancel->setToolTip("読込を中止");
    auto *divider = new QFrame;
    divider->setObjectName("resultsBarDivider");
    divider->setFrameShape(QFrame::VLine);
    divider->setFrameShadow(QFrame::Sunken);
    divider->setFixedHeight(22);
    compareGeometry = new QPushButton(lineIcon("compare"), "分子と比較");
    compareGeometry->setObjectName("resultsCompare");
    compareGeometry->setToolTip(
        "表示中の構造と「1 分子」の分子を重ね合わせ、RMSDを表示します（同じ原子・同じ順番）。");
    applyGeometry = new QPushButton(lineIcon("apply"), "分子へ候補として適用");
    applyGeometry->setObjectName("resultsApplyStructure");
    applyGeometry->setToolTip("表示中の構造の座標を「1 分子」の分子へ候補として適用します。原子の数と順番が同"
                              "じ計算結果に使えます。結合・設定は変わりません。");
    exportButton = new QToolButton;
    exportButton->setObjectName("resultsExport");
    exportButton->setText("書き出し");
    exportButton->setIcon(lineIcon("export"));
    exportButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    exportButton->setPopupMode(QToolButton::InstantPopup);
    auto *exportMenu = new QMenu(exportButton);
    saveXyz = exportMenu->addAction("表示中の構造をXYZで書き出す…");
    saveXyz->setObjectName("resultsSaveXyz");
    savePng = exportMenu->addAction("3D表示をPNG画像で書き出す…");
    savePng->setObjectName("resultsSavePng");
    exportButton->setMenu(exportMenu);
    for (QWidget *w :
         {static_cast<QWidget *>(openButton), static_cast<QWidget *>(reload), static_cast<QWidget *>(cancel),
          static_cast<QWidget *>(divider), static_cast<QWidget *>(compareGeometry),
          static_cast<QWidget *>(applyGeometry), static_cast<QWidget *>(exportButton)})
        bar->addWidget(w);
    layout->addWidget(resultBar);
    auto *body = new QWidget;
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(10, 10, 10, 10);
    auto *split = new QSplitter;
    split->setChildrenCollapsible(false);
    split->setHandleWidth(10);
    bodyLayout->addWidget(split);
    layout->addWidget(body, 1);

    // The 3D view, with what it shows, the view controls and the measurement in its corners.
    auto *viewCard = new QFrame;
    viewCard->setObjectName("resultsViewCard");
    auto *card = new QVBoxLayout(viewCard);
    card->setContentsMargins(1, 1, 1, 1);
    card->setSpacing(0);
    view = new MoleculeView;
    view->setObjectName("resultsView");
    view->setAcceptDrops(false);
    view->setAngles(-25, 25, 0);
    view->setEmptyMessage("NWChem出力・XYZ・CUBE・MOLDENを開いて\n構造・軌道の等値面・振動を確認できます。");
    auto *viewArea = new OverlayHost(view);
    viewArea->setObjectName("resultsViewArea");
    card->addWidget(viewArea, 1);
    contextChip = new QFrame;
    contextChip->setObjectName("resultsContext");
    auto *context = new QVBoxLayout(contextChip);
    context->setContentsMargins(10, 5, 10, 6);
    context->setSpacing(0);
    contextTitle = new QLabel;
    contextTitle->setObjectName("resultsContextTitle");
    auto bold = contextTitle->font();
    bold.setBold(true);
    contextTitle->setFont(bold);
    contextDetail = new QLabel;
    contextDetail->setObjectName("resultsContextDetail");
    context->addWidget(contextTitle);
    context->addWidget(contextDetail);
    contextChip->hide();
    viewArea->addOverlay(contextChip, Qt::AlignTop | Qt::AlignLeft);
    viewControlsFrame = new ViewControls(view, {{"resultsAxisX", "resultsAxisY", "resultsAxisZ"},
                                                "resultsFit",
                                                "resultsRotation",
                                                "resultsViewMenu",
                                                "resultsRotationPopup",
                                                {"resultsYaw", "resultsPitch", "resultsRoll"}});
    viewControlsFrame->setObjectName("resultsViewControls");
    viewControls << viewControlsFrame->controls();
    // What the view shows: the labels, as in the "表示" menu of "1 分子".
    auto *labelMenu = new QMenu(viewControlsFrame->menuButton());
    labelMenu->addSection("ラベル");
    auto *labelGroup = new QActionGroup(labelMenu);
    auto savedLabels = window->settings.value("results/labels", "all").toString();
    if (!QStringList{"all", "heavy", "selection", "none"}.contains(savedLabels))
        savedLabels = "all";
    for (const auto &[label, mode] : {std::pair{"すべて", "all"},
                                      {"水素以外", "heavy"},
                                      {"選択した原子", "selection"},
                                      {"なし", "none"}}) {
        auto *a = labelMenu->addAction(label);
        a->setObjectName(QString("resultsLabel_") + mode);
        a->setCheckable(true);
        a->setData(mode);
        a->setChecked(savedLabels == mode);
        labelGroup->addAction(a);
    }
    viewControlsFrame->menuButton()->setMenu(labelMenu);
    viewControlsFrame->menuButton()->setToolTip("元素記号・番号を表示する原子を選びます。");
    view->setLabelMode(savedLabels);
    connect(labelGroup, &QActionGroup::triggered, this, [this](QAction *a) {
        view->setLabelMode(a->data().toString());
        window->settings.setValue("results/labels", a->data().toString());
    });
    viewArea->addOverlay(viewControlsFrame, Qt::AlignTop | Qt::AlignRight);
    // Distance, angle or dihedral of the atoms clicked in order, and the ones kept on the view.
    measureCard = new QFrame;
    measureCard->setObjectName("resultsMeasureCard");
    measureCard->setMaximumWidth(340);
    auto *measuring = new QVBoxLayout(measureCard);
    measuring->setContentsMargins(12, 8, 12, 9);
    measuring->setSpacing(6);
    measurementLabel = new QLabel("原子をクリックすると距離・角度・二面角を測定します（2〜4原子）。");
    measurementLabel->setObjectName("resultsMeasurement");
    measurementLabel->setWordWrap(true);
    measurementLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    measuring->addWidget(measurementLabel);
    auto *measureActions = new QHBoxLayout;
    measureActions->setSpacing(4);
    auto *pin = new QPushButton("画面に表示");
    pin->setObjectName("resultsPinMeasurement");
    pin->setToolTip("選んだ原子の計測を3D表示に残します。");
    auto *clearMeasure = new QPushButton("消去");
    clearMeasure->setObjectName("resultsClearMeasurement");
    clearMeasure->setToolTip("選択と、画面に表示中の計測をすべて消去します。");
    measureActions->addWidget(pin);
    measureActions->addWidget(clearMeasure);
    measureActions->addStretch();
    measuring->addLayout(measureActions);
    viewControls << pin << clearMeasure;
    measureCard->hide();
    viewArea->addOverlay(measureCard, Qt::AlignBottom | Qt::AlignLeft);
    connect(view, &MoleculeView::atomClicked, this, [this](const QString &id) { pickAtom(id); });
    connect(pin, &QPushButton::clicked, this, [this] {
        const auto &d = view->documentRef();
        Ids ids;
        for (int n : picked)
            if (n < d.atoms.size())
                ids << d.atoms[n].id;
        bool valid = false;
        window->safe([&] {
            if (picked.size() < 2 || ids.size() != picked.size())
                fail("2〜4原子をクリックしてから表示してください。");
            measure(d, ids); // Rejects coincident or collinear atoms before pinning.
            valid = true;
        });
        if (valid) {
            pinned << picked;
            picked.clear();
            setViewDocument(Document(view->documentCopy()));
        }
    });
    connect(clearMeasure, &QPushButton::clicked, this, [this] {
        picked.clear();
        pinned.clear();
        setViewDocument(Document(view->documentCopy()));
    });

    // Under the view: the controls for what it shows. Pages of one height, so the view keeps its size.
    auto *dockFrame = new QFrame;
    dockFrame->setObjectName("resultsDock");
    auto *dockLayout = new QVBoxLayout(dockFrame);
    dockLayout->setContentsMargins(10, 7, 10, 5);
    dockLayout->setSpacing(3);
    dock = new QStackedWidget;
    dock->setObjectName("resultsDockPages");
    dockLayout->addWidget(dock);
    const auto iconButton = [](const QString &icon, const QString &name, const QString &tip) {
        auto *b = new QPushButton(lineIcon(icon), QString());
        b->setObjectName(name);
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        return b;
    };
    // Groups of controls that move to a second row together when the view is narrow.
    const auto group = [](std::initializer_list<QWidget *> widgets, int spacing = 4) {
        auto *g = new QWidget;
        auto *row = new QHBoxLayout(g);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(spacing);
        for (auto *w : widgets)
            row->addWidget(w);
        return g;
    };
    auto *framesPage = new QWidget;
    auto *framesRow = new FlowLayout(framesPage, 10, 4);
    framesRow->setContentsMargins(0, 0, 0, 0);
    firstFrame = iconButton("first", "resultsFirstFrame", "最初の構造");
    playFrames = iconButton("play", "resultsPlayFrames", "構造列を再生");
    playFrames->setProperty("primary", true);
    lastFrame = iconButton("last", "resultsLastFrame", "最後の構造");
    auto *frameLabel = new QLabel("構造");
    frame = new QSpinBox;
    frame->setObjectName("resultsFrame");
    frameLabel->setBuddy(frame);
    timeline = new FrameTimeline;
    timeline->setObjectName("resultsTimeline");
    frameEnergy = new QLabel;
    frameEnergy->setObjectName("resultsFrameEnergy");
    // Room for any value, so that the row keeps its layout while the frames change.
    frameEnergy->setMinimumWidth(frameEnergy->fontMetrics().horizontalAdvance("ΔE −99999.99 kcal/mol") + 4);
    frameEnergy->hide();
    framesRow->addWidget(group({firstFrame, playFrames, lastFrame}));
    framesRow->addWidget(group({frameLabel, frame}, 6));
    framesRow->addWidget(timeline);
    framesRow->addWidget(frameEnergy);
    dock->addWidget(framesPage);
    auto *orbitalPage = new QWidget;
    auto *orbitalRow = new FlowLayout(orbitalPage, 10, 4);
    orbitalRow->setContentsMargins(0, 0, 0, 0);
    const QStringList navigationNames{"First", "Back10", "Previous", "Next", "Forward10", "Last"};
    const QStringList navigationIcons{"first", "back10", "previous", "next", "forward10", "last"};
    const QStringList navigationTips{"最初の軌道へ", "10軌道戻る", "1軌道戻る",
                                     "1軌道進む",    "10軌道進む", "最後の軌道へ"};
    orbitalPosition = new QLabel;
    orbitalPosition->setObjectName("resultsOrbitalPosition");
    orbitalPosition->setAlignment(Qt::AlignCenter);
    // A fixed width, so that the buttons after it stay put while the number changes.
    orbitalPosition->setFixedWidth(orbitalPosition->fontMetrics().horizontalAdvance("Alpha 99999番") + 12);
    for (int i = 0; i < 6; ++i) {
        orbitalNavigation[i] =
            iconButton(navigationIcons[i], "resultsOrbital" + navigationNames[i], navigationTips[i]);
        connect(orbitalNavigation[i], &QPushButton::clicked, this, [this, i] {
            const int last = data.orbitals.size() - 1;
            if (last < 0)
                return;
            const int offsets[]{0, -10, -1, 1, 10, 0};
            renderOrbital(i == 0   ? 0
                          : i == 5 ? last
                                   : std::clamp(data.selectedOrbital + offsets[i], 0, last));
        });
    }
    homo = new QPushButton("HOMO");
    lumo = new QPushButton("LUMO");
    homo->setObjectName("resultsHomo");
    lumo->setObjectName("resultsLumo");
    homo->setToolTip("選択中のスピンのHOMOを表示します。");
    lumo->setToolTip("選択中のスピンのLUMOを表示します。");
    orbitalRow->addWidget(
        group({orbitalNavigation[0], orbitalNavigation[1], orbitalNavigation[2], orbitalPosition,
               orbitalNavigation[3], orbitalNavigation[4], orbitalNavigation[5]}));
    orbitalRow->addWidget(group({homo, lumo}));
    dock->addWidget(orbitalPage);
    auto *modePage = new QWidget;
    auto *modeRow = new FlowLayout(modePage, 10, 4);
    modeRow->setContentsMargins(0, 0, 0, 0);
    playMode = iconButton("play", "resultsPlayMode", "選択したモードを再生");
    playMode->setProperty("primary", true);
    modeLabel = new QLabel;
    modeLabel->setObjectName("resultsModeLabel");
    auto *amplitudeLabel = new QLabel("振幅");
    amplitude = new QDoubleSpinBox;
    amplitude->setObjectName("resultsAmplitude");
    amplitude->setRange(.01, 2);
    amplitude->setSingleStep(.05);
    amplitude->setValue(.3);
    amplitude->setSuffix(" Å");
    amplitude->setToolTip("表示用に規格化した最大変位です。");
    amplitudeLabel->setBuddy(amplitude);
    arrows = new QCheckBox("矢印");
    arrows->setObjectName("resultsModeArrows");
    arrows->setToolTip("選択したモードの変位を基準構造に矢印で重ねます（最大の矢印は振幅の3倍）。");
    modeExport = new QToolButton;
    modeExport->setObjectName("resultsModeExport");
    modeExport->setText("書き出し");
    modeExport->setIcon(lineIcon("export"));
    modeExport->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    modeExport->setPopupMode(QToolButton::InstantPopup);
    auto *modeMenu = new QMenu(modeExport);
    saveModeImages = modeMenu->addAction("振動の連番画像を書き出す…");
    saveModeImages->setObjectName("resultsSaveModeImages");
    saveModeTrajectory = modeMenu->addAction("振動をXYZ（複数構造）で書き出す…");
    saveModeTrajectory->setObjectName("resultsSaveModeXyz");
    modeExport->setMenu(modeMenu);
    closeMode = iconButton("close", "resultsCloseMode", "振動の表示を終えて構造列に戻る");
    modeRow->addWidget(group({playMode, modeLabel}, 8));
    modeRow->addWidget(group({amplitudeLabel, amplitude}, 6));
    modeRow->addWidget(arrows);
    modeRow->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum));
    modeRow->addWidget(group({modeExport, closeMode}));
    dock->addWidget(modePage);
    auto *hint = new ElidedLabel("原子をクリック: 計測（2〜4原子） · " + viewControlsHint() +
                                 " · 結果は編集中の分子から独立");
    hint->setObjectName("resultsHint");
    hint->setToolTip(hint->text());
    auto hintFont = hint->font();
    hintFont.setPointSizeF(hintFont.pointSizeF() * .9);
    hint->setFont(hintFont);
    dockLayout->addWidget(hint);
    card->addWidget(dockFrame);
    split->addWidget(viewCard);

    // The details: a banner when the calculation did not end normally, over the tabs with data.
    auto *side = new QWidget;
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(8);
    banner = new QFrame;
    banner->setObjectName("resultsBanner");
    auto *bannerRow = new QHBoxLayout(banner);
    bannerRow->setContentsMargins(12, 10, 12, 10);
    bannerRow->setSpacing(10);
    bannerIcon = new QLabel;
    bannerRow->addWidget(bannerIcon, 0, Qt::AlignTop);
    auto *bannerTexts = new QVBoxLayout;
    bannerTexts->setSpacing(3);
    bannerTitle = new QLabel;
    bannerTitle->setObjectName("resultsBannerTitle");
    bannerTitle->setFont(bold);
    bannerTitle->setWordWrap(true);
    bannerText = new QLabel;
    bannerText->setObjectName("resultsBannerText");
    bannerText->setWordWrap(true);
    bannerText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bannerTexts->addWidget(bannerTitle);
    bannerTexts->addWidget(bannerText);
    bannerRow->addLayout(bannerTexts, 1);
    auto *bannerLog = new QPushButton("ログの末尾を表示");
    bannerLog->setObjectName("resultsBannerLog");
    bannerRow->addWidget(bannerLog, 0, Qt::AlignTop);
    banner->hide();
    sideLayout->addWidget(banner);
    // Messages of the viewer (how the calculation ended, comparisons, loading) above the details.
    status = new QPlainTextEdit;
    status->setObjectName("resultsStatus");
    status->setAccessibleName("結果ビューアのメッセージ");
    status->setReadOnly(true);
    status->setTabChangesFocus(true);
    status->setFrameShape(QFrame::NoFrame);
    status->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    status->setFixedHeight(2 * status->fontMetrics().lineSpacing() +
                           2 * qCeil(status->document()->documentMargin()) + 14);
    status->hide();
    connect(status, &QPlainTextEdit::textChanged, this,
            [this] { status->setVisible(!status->toPlainText().isEmpty()); });
    sideLayout->addWidget(status);
    auto *tabs = details = new QTabWidget;
    tabs->setObjectName("resultsDetails");
    tabs->setProperty("tabs", "underline");
    tabs->tabBar()->setDrawBase(false);
    tabs->setUsesScrollButtons(true);
    sideLayout->addWidget(tabs, 1);
    split->addWidget(side);
    overviewScroll = new QScrollArea;
    overviewScroll->setObjectName("resultsOverview");
    overviewScroll->setWidgetResizable(true);
    overviewScroll->setFrameShape(QFrame::NoFrame);
    tabs->addTab(overviewScroll, "概要");
    auto *energyPage = new QWidget;
    auto *enl = new QVBoxLayout(energyPage);
    auto *energyRow = new QHBoxLayout;
    enl->addLayout(energyRow);
    energyUnit = new QComboBox;
    energyUnit->setObjectName("resultsEnergyUnit");
    for (const auto &[label, unit] : {std::pair{"ΔE: kcal/mol", "kcal"},
                                      {"ΔE: kJ/mol", "kj"},
                                      {"ΔE: eV", "ev"},
                                      {"ΔE: Hartree", "hartree"},
                                      {"ΔE: cm⁻¹", "wavenumber"}})
        energyUnit->addItem(label, unit);
    energyRow->addWidget(energyUnit);
    energyReference = new QPushButton("選択行をΔEの基準にする");
    energyReference->setObjectName("resultsEnergyReference");
    energyRow->addWidget(energyReference);
    energyRow->addStretch();
    energy = table({"手法", "全エネルギー / Eh", "ΔE", "ログ行"}, "resultsEnergy");
    enl->addWidget(energy, 1);
    energyNote = new QLabel;
    energyNote->setObjectName("resultsEnergyNote");
    energyNote->setWordWrap(true);
    enl->addWidget(energyNote);
    tabs->addTab(energyPage, "エネルギー");
    connect(energyUnit, &QComboBox::currentIndexChanged, this, &ResultsPanel::showEnergies);
    connect(energyReference, &QPushButton::clicked, this, [this] {
        if (energy->currentRow() >= 0 && energy->currentRow() < data.energies.size()) {
            referenceEnergy = energy->currentRow();
            showEnergies();
        }
    });
    auto *orbitalTab = new QWidget;
    auto *ol = new QVBoxLayout(orbitalTab);
    auto *sourceRow = new QHBoxLayout;
    orbitalSource = new QLabel;
    orbitalSource->setObjectName("resultsOrbitalSource");
    orbitalSource->setWordWrap(true);
    orbitalSource->setTextFormat(Qt::PlainText);
    sourceRow->addWidget(orbitalSource, 1);
    openMolden = new QPushButton("形を表示");
    openMolden->setObjectName("resultsOpenMolden");
    openMolden->hide();
    sourceRow->addWidget(openMolden, 0, Qt::AlignTop);
    ol->addLayout(sourceRow);
    auto *orbitalViews = new QHBoxLayout;
    levels = new OrbitalLevels;
    levels->setObjectName("resultsOrbitalLevels");
    orbitalViews->addWidget(levels);
    orbitals = table({"番号", "軌道", "対称性", "スピン", "エネルギー / Eh", "占有数"}, "resultsOrbitals");
    orbitals->verticalHeader()->hide();
    orbitalViews->addWidget(orbitals, 1);
    ol->addLayout(orbitalViews, 1);
    orbitalOptions = new QGroupBox("等値面の設定");
    auto *optionGrid = new QGridLayout(orbitalOptions);
    optionGrid->setVerticalSpacing(3);
    orbitalIso = new QLineEdit("0.03");
    orbitalIso->setObjectName("resultsOrbitalIso");
    orbitalGrid = new QSpinBox;
    orbitalGrid->setObjectName("resultsOrbitalGrid");
    orbitalGrid->setRange(16, 100);
    orbitalGrid->setValue(40);
    orbitalGrid->setToolTip("最も長い辺の分割数です。格子間隔は全軸で同じです。");
    orbitalNorm = new QComboBox;
    orbitalNorm->setObjectName("resultsOrbitalNorm");
    orbitalNorm->addItem("自動判定", "auto");
    orbitalNorm->addItem("標準MOLDEN（NWChemのNONE / JANPA）", "standard");
    orbitalNorm->addItem("NWChem（正規化因子を含む係数）", "nwchem");
    const QList<std::pair<QString, QWidget *>> optionFields{{"等値 / bohr⁻³ᐟ²（±）", orbitalIso},
                                                            {"格子分割数（最長辺）", orbitalGrid},
                                                            {"基底係数の正規化", orbitalNorm}};
    for (int i = 0; i < optionFields.size(); ++i) {
        auto *label = new QLabel(optionFields[i].first);
        label->setBuddy(optionFields[i].second);
        optionGrid->addWidget(label, 0, i);
        optionGrid->addWidget(optionFields[i].second, 1, i);
        optionGrid->setColumnStretch(i, i == 2 ? 2 : 1);
    }
    ol->addWidget(orbitalOptions);
    orbitalStatus = new QLabel;
    orbitalStatus->setObjectName("resultsOrbitalStatus");
    orbitalStatus->setTextFormat(Qt::PlainText);
    orbitalStatus->setWordWrap(true);
    ol->addWidget(orbitalStatus);
    tabs->addTab(orbitalTab, "軌道");
    connect(openMolden, &QPushButton::clicked, this, [this] { openOrbitalShape(orbitals->currentRow()); });
    connect(orbitals, &QTableWidget::cellDoubleClicked, this, [this](int row) {
        if (data.format == "NWChem" && !jobMolden().isEmpty())
            openOrbitalShape(row);
    });
    connect(orbitals, &QTableWidget::currentCellChanged, this, [this](int row, int, int previousRow, int) {
        if (data.format != "MOLDEN")
            levels->setSelected(row);
        if (row != previousRow)
            renderOrbital(row);
        updateActions();
    });
    connect(levels, &OrbitalLevels::orbitalClicked, this, [this](int index) {
        if (index == orbitals->currentRow() && data.format == "MOLDEN" && index != data.selectedOrbital)
            renderOrbital(index);
        else
            orbitals->setCurrentCell(index, 0);
    });
    const auto applyOrbitalOptions = [this] {
        if (loading())
            return;
        bool ok;
        const double iso = orbitalIso->text().toDouble(&ok);
        if (ok && data.selectedOrbital == orbitals->currentRow() && iso == data.isovalue &&
            orbitalGrid->value() == data.orbitalDivisions &&
            orbitalNorm->currentData().toString() == data.normalizationRequest &&
            sourceStamp(QFileInfo(data.source)) == orbitalCacheSource)
            return;
        renderOrbital(orbitals->currentRow());
    };
    connect(orbitalIso, &QLineEdit::editingFinished, this, applyOrbitalOptions);
    connect(orbitalGrid, &QSpinBox::editingFinished, this, applyOrbitalOptions);
    connect(orbitalNorm, &QComboBox::currentIndexChanged, this, applyOrbitalOptions);
    const auto frontier = [this](bool virtualOrbital) {
        const int row = orbitals->currentRow();
        const auto spin = row >= 0 && row < data.orbitals.size() ? data.orbitals[row].spin : QString("Alpha");
        const int target = frontierOrbital(data.orbitals, spin, virtualOrbital);
        if (data.format != "MOLDEN" && target >= 0)
            orbitals->selectRow(target); // Log orbitals have energies only.
        renderOrbital(target);
    };
    connect(homo, &QPushButton::clicked, this, [frontier] { frontier(false); });
    connect(lumo, &QPushButton::clicked, this, [frontier] { frontier(true); });
    auto *vibrations = new QWidget;
    auto *vl = new QVBoxLayout(vibrations);
    modeSummary = new QLabel;
    modeSummary->setObjectName("resultsModeSummary");
    modeSummary->setWordWrap(true);
    vl->addWidget(modeSummary);
    auto *irRow = new QHBoxLayout;
    vl->addLayout(irRow);
    auto *irTitle = new QLabel("IRスペクトル（棒をクリックするとそのモードを表示）");
    irRow->addWidget(irTitle, 1);
    irWidth = new QDoubleSpinBox;
    irWidth->setObjectName("resultsIrWidth");
    irWidth->setRange(1, 200);
    irWidth->setValue(20);
    irWidth->setPrefix("半値全幅 ");
    irWidth->setSuffix(" cm⁻¹");
    irRow->addWidget(irWidth);
    irSpectrum = new ChartView;
    irSpectrum->setObjectName("resultsIrSpectrum");
    irSpectrum->setReversedX(true);
    irSpectrum->setCursor(Qt::PointingHandCursor);
    vl->addWidget(irSpectrum, 1);
    modes = table({"振動数 / cm⁻¹", "IR強度 / km mol⁻¹", "種類"}, "resultsModes");
    modes->setItemDelegateForColumn(1, new IntensityDelegate(modes));
    vl->addWidget(modes, 1);
    showRigid = new QCheckBox("並進・回転のモードも表示");
    showRigid->setObjectName("resultsShowRigid");
    showRigid->setToolTip("振動数がほぼ0の並進・回転（3N−6個以外）は既定で一覧から省きます。");
    vl->addWidget(showRigid);
    connect(showRigid, &QCheckBox::toggled, this, [this](bool on) {
        for (int i = 0; i < modes->rowCount(); ++i)
            modes->setRowHidden(i, !on && rigidMotion.value(i));
    });
    auto *modeHint = new QLabel(
        "モードを選ぶと3Dビューに基準構造を表示し、下の操作で再生・書き出しできます。負の振動数は虚振動です。"
        "振幅は表示用に規格化した最大変位で、アニメーションの速さは実際の振動数を表しません。振動数の絶対値が"
        "最も小さい3N−6個以外（直線分子は3N−5個以外）を並進・回転とみなし、IRスペクトル（ローレンツ関数で表示"
        "用に広げた曲線）から除きます。");
    modeHint->setWordWrap(true);
    vl->addWidget(modeHint);
    tabs->addTab(vibrations, "振動");
    connect(irWidth, &QDoubleSpinBox::valueChanged, this, &ResultsPanel::showIrSpectrum);
    connect(irSpectrum, &ChartView::stickClicked, this, [this](int series, int index) {
        if (series == 1 && index >= 0 && index < irModes.size())
            modes->setCurrentCell(irModes[index], 0);
    });
    connect(arrows, &QCheckBox::toggled, this, &ResultsPanel::showArrows);
    connect(amplitude, &QDoubleSpinBox::valueChanged, this, [this] {
        showArrows();
        showContext();
    });
    connect(saveModeImages, &QAction::triggered, this,
            [this] { window->safe([this] { exportMode(true); }); });
    connect(saveModeTrajectory, &QAction::triggered, this,
            [this] { window->safe([this] { exportMode(false); }); });
    auto *optimizationPage = new QWidget;
    optimizationPage->setObjectName("resultsOptimizationPage");
    auto *opl = new QVBoxLayout(optimizationPage);
    criteria = new QFrame;
    criteria->setObjectName("resultsCriteria");
    auto *criteriaLayout = new QVBoxLayout(criteria);
    criteriaLayout->setContentsMargins(0, 0, 0, 0);
    criteriaLayout->setSpacing(6);
    opl->addWidget(criteria);
    auto *charts = new QHBoxLayout;
    energyChart = new ChartView;
    energyChart->setObjectName("resultsEnergyChart");
    charts->addWidget(energyChart, 1);
    gradientChart = new ChartView;
    gradientChart->setObjectName("resultsGradientChart");
    charts->addWidget(gradientChart, 1);
    opl->addLayout(charts, 1);
    steps = table({"ステップ", "エネルギー / Eh", "ΔE / Eh", "Gmax", "Grms", "Xrms", "Xmax", "経過 / s"},
                  "resultsSteps");
    steps->verticalHeader()->hide();
    opl->addWidget(steps, 1);
    auto *stepHint = new QLabel("NWChem DRIVERの「@」行です。勾配はHartree/Bohr、変位はBohr。破線は収束閾値、"
                                "表の色付きの値は閾値を満たした値です。");
    stepHint->setWordWrap(true);
    opl->addWidget(stepHint);
    tabs->addTab(optimizationPage, "最適化");
    auto *excitedPage = new QWidget;
    auto *el = new QVBoxLayout(excitedPage);
    excitations = table({"状態", "スピン", "対称", "励起エネルギー / eV", "波長 / nm", "振動子強度", "<S²>"},
                        "resultsExcitations");
    el->addWidget(excitations, 1);
    auto *spectrumRow = new QHBoxLayout;
    el->addLayout(spectrumRow);
    spectrumAxis = new QComboBox;
    spectrumAxis->setObjectName("resultsSpectrumAxis");
    spectrumAxis->addItem("横軸: 波長 / nm", "nm");
    spectrumAxis->addItem("横軸: 励起エネルギー / eV", "ev");
    spectrumRow->addWidget(spectrumAxis);
    broadening = new QDoubleSpinBox;
    broadening->setObjectName("resultsBroadening");
    broadening->setRange(.05, 2);
    broadening->setSingleStep(.05);
    broadening->setValue(.3);
    broadening->setPrefix("半値全幅 ");
    broadening->setSuffix(" eV");
    spectrumRow->addWidget(broadening);
    spectrumRow->addStretch();
    spectrum = new ChartView;
    spectrum->setObjectName("resultsSpectrum");
    el->addWidget(spectrum, 1);
    auto *spectrumHint =
        new QLabel("各状態の振動子強度（棒）に、エネルギー軸で指定した半値全幅のガウス関数を掛けて重ね"
                   "た表示用の曲線です。三重項など振動子強度0の状態は棒に現れません。");
    spectrumHint->setWordWrap(true);
    el->addWidget(spectrumHint);
    tabs->addTab(excitedPage, "励起状態");
    connect(spectrumAxis, &QComboBox::currentIndexChanged, this, &ResultsPanel::showSpectrum);
    connect(broadening, &QDoubleSpinBox::valueChanged, this, &ResultsPanel::showSpectrum);
    auto *pathPage = new QWidget;
    auto *pl = new QVBoxLayout(pathPage);
    path = table({"点", "座標・方向", "値", "エネルギー / Eh", "ΔE / kcal mol⁻¹"}, "resultsPath");
    pl->addWidget(path, 1);
    pathChart = new ChartView;
    pathChart->setObjectName("resultsPathChart");
    pl->addWidget(pathChart, 1);
    auto *pathHint =
        new QLabel("座標スキャン: 各点の最後のエネルギー（緩和スキャンは最適化後）で、ΔEは最小値基準です。"
                   "IRC: 遷移状態を0とし、前進方向を正・後退方向を負の反応座標（amu½·Bohr）で表示し、"
                   "ΔEは遷移状態基準です。行を選ぶとその構造を表示します。");
    pathHint->setWordWrap(true);
    pl->addWidget(pathHint);
    tabs->addTab(pathPage, "経路・スキャン");
    connect(path, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (row < 0 || !path->item(row, 0))
            return;
        const int index = path->item(row, 0)->data(Qt::UserRole).toInt();
        if (index >= 0 && index < data.frames.size()) {
            stopAnimation();
            frame->setValue(index + 1);
            showFrame();
        }
    });
    auto *propertyPage = new QWidget;
    auto *ppl = new QVBoxLayout(propertyPage);
    propertySummary = new QLabel;
    propertySummary->setObjectName("resultsPropertySummary");
    propertySummary->setTextFormat(Qt::PlainText);
    propertySummary->setWordWrap(true);
    propertySummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ppl->addWidget(propertySummary);
    atomProperties =
        table({"番号", "元素", "Mulliken電荷", "ESP電荷", "RESP電荷", "NMR遮蔽 / ppm", "異方性 / ppm"},
              "resultsAtomProperties");
    atomProperties->verticalHeader()->hide();
    ppl->addWidget(atomProperties, 1);
    tabs->addTab(propertyPage, "物性");
    auto *surfaces = new QWidget;
    auto *sf = new QFormLayout(surfaces);
    isovalue = new QLineEdit("0.03");
    isovalue->setObjectName("resultsIsovalue");
    sf->addRow("等値（ファイルの単位）", isovalue);
    dataset = new QComboBox;
    dataset->setObjectName("resultsDataset");
    sf->addRow("CUBEデータ", dataset);
    applyIso = new QPushButton("等値面を再生成");
    applyIso->setObjectName("resultsApplyIso");
    sf->addRow(applyIso);
    auto *positive = new QCheckBox("正の面（赤）");
    auto *negative = new QCheckBox("負の面（青）");
    positive->setChecked(true);
    negative->setChecked(true);
    sf->addRow(positive);
    sf->addRow(negative);
    auto *opacity = new QSlider(Qt::Horizontal);
    opacity->setRange(20, 255);
    opacity->setValue(150);
    sf->addRow("不透明度", opacity);
    auto style = [this, positive, negative, opacity] {
        view->setSurfaceStyle(positive->isChecked(), negative->isChecked(), opacity->value());
    };
    connect(positive, &QCheckBox::toggled, this, style);
    connect(negative, &QCheckBox::toggled, this, style);
    connect(opacity, &QSlider::valueChanged, this, style);
    auto *surfaceHint = new QLabel("CUBEとMOLDEN軌道の正負・不透明度を変更できます。CUBEの値の意味は生成"
                                   "条件で確認してください。MOLDENの軌道と等値は「軌道」で選択します。");
    surfaceHint->setWordWrap(true);
    sf->addRow(surfaceHint);
    tabs->addTab(surfaces, "CUBE / 等値面");
    // The file as in "3 実行設定" and "履歴": its name and how much of it, copy, and the text.
    auto *logPage = new QWidget;
    auto *logLayout = new QVBoxLayout(logPage);
    logLayout->setContentsMargins(8, 8, 8, 8);
    logLayout->setSpacing(6);
    auto *logHead = new QHBoxLayout;
    logName = new ElidedLabel;
    logName->setObjectName("resultsLogName");
    logName->setProperty("mono", true);
    logName->setProperty("muted", true);
    logHead->addWidget(logName, 1);
    auto *copyLog = copyButton("resultsCopyLog", "表示中のファイルをコピー");
    logHead->addWidget(copyLog);
    logLayout->addLayout(logHead);
    log = logView("resultsLog");
    logLayout->addWidget(log, 1);
    tabs->addTab(logPage, "元ファイル");
    connect(copyLog, &QToolButton::clicked, this,
            [this] { QApplication::clipboard()->setText(log->toPlainText()); });
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setSizes({780, 540});
    connect(bannerLog, &QPushButton::clicked, this, [this] {
        details->setCurrentWidget(log->parentWidget());
        log->moveCursor(QTextCursor::End);
        log->ensureCursorVisible();
    });

    const auto openResultFile = [this] {
        const auto p = QFileDialog::getOpenFileName(
            this, "結果ファイルを開く", folder,
            "NWChem・可視化 (*.out *.log *.xyz *.cube *.cub *.molden *.mld *.molde);;すべて (*)");
        if (!p.isEmpty())
            openFile(p);
    };
    connect(openButton, &QToolButton::clicked, this, openResultFile);
    connect(openFileAction, &QAction::triggered, this, openResultFile);
    connect(openDirectoryAction, &QAction::triggered, this, [this] {
        const auto p = QFileDialog::getExistingDirectory(this, "ジョブフォルダーを開く", folder);
        if (!p.isEmpty())
            openDirectory(p);
    });
    connect(files, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0)
            window->safe([&] { openEntry(files->itemData(index).toString()); });
    });
    connect(reload, &QPushButton::clicked, this, [this] {
        startLoad(data.source, data.isovalue, data.dataset, data.selectedOrbital, data.normalizationRequest,
                  data.orbitalDivisions);
    });
    connect(cancel, &QPushButton::clicked, this, &ResultsPanel::cancelLoad);
    connect(frame, &QSpinBox::valueChanged, this, [this] {
        if (!animation.isActive())
            showFrame();
    });
    const auto goToFrame = [this](int index) {
        stopAnimation();
        const QSignalBlocker guard(frame);
        frame->setValue(index + 1);
        showFrame();
    };
    connect(timeline, &FrameTimeline::frameSelected, this, goToFrame);
    connect(firstFrame, &QPushButton::clicked, this, [goToFrame] { goToFrame(0); });
    connect(lastFrame, &QPushButton::clicked, this, [this, goToFrame] { goToFrame(data.frames.size() - 1); });
    const auto setPlaying = [this](QPushButton *button, const QString &tip) {
        button->setIcon(lineIcon("pause", ChromeColors(palette()).onAccent));
        button->setToolTip(tip);
        button->setAccessibleName(tip);
    };
    connect(playFrames, &QPushButton::clicked, this, [this, setPlaying] {
        if (animation.isActive()) {
            stopAnimation();
            showFrame();
        } else {
            phase = 0;
            animation.setInterval(180);
            animation.start();
            setPlaying(playFrames, "再生を停止");
        }
    });
    connect(playMode, &QPushButton::clicked, this, [this, setPlaying] {
        if (animation.isActive()) {
            stopAnimation();
            showFrame();
        } else {
            phase = 0;
            animation.setInterval(40);
            animation.start();
            setPlaying(playMode, "再生を停止");
            view->setArrows({}); // Arrows belong to the reference structure, not the moving one.
            showMode();
        }
    });
    connect(closeMode, &QPushButton::clicked, this, [this] {
        stopAnimation();
        {
            const QSignalBlocker guard(modes);
            modes->clearSelection();
            modes->setCurrentCell(-1, -1);
        }
        irSpectrum->setSelectedStick(-1, -1);
        showFrame();
        updateActions();
    });
    connect(modes, &QTableWidget::currentCellChanged, this, [this] {
        stopAnimation();
        updateActions();
        irSpectrum->setSelectedStick(1, irModes.indexOf(modes->currentRow()));
        if (modes->currentRow() >= 0 && modes->currentRow() < data.modes.size() &&
            !data.modes[modes->currentRow()].displacement.isEmpty()) {
            modeDisplayed = true;
            setViewDocument(data.vibrationGeometry);
            view->setSurface({});
        }
        showArrows();
        showContext();
    });
    connect(&animation, &QTimer::timeout, this, [this] {
        if (animation.interval() == 180) {
            frame->setValue(frame->value() % data.frames.size() + 1);
            showFrame();
        } else {
            phase += .18;
            showMode();
        }
    });
    connect(applyIso, &QPushButton::clicked, this, [this] {
        bool ok;
        const double n = isovalue->text().toDouble(&ok);
        if (!ok || !std::isfinite(n) || n <= 0 || n > 1e6) {
            error("等値は0より大きい有限の数で指定してください。");
            return;
        }
        startLoad(data.source, n, dataset->currentIndex());
    });
    connect(applyGeometry, &QPushButton::clicked, this,
            [this] { window->safe([this] { applyStructure(); }); });
    connect(compareGeometry, &QPushButton::clicked, this, [this] {
        window->safe([this] {
            const auto fit = superpose(displayedGeometry(), window->document());
            status->setPlainText(
                QString("編集中の分子との比較: RMSD（座標そのまま）%1 Å、重ね合わせ後 %2 Å（水素以外 "
                        "%3 Å）、最大のずれ %4 Å")
                    .arg(fit.raw, 0, 'f', 4)
                    .arg(fit.rmsd, 0, 'f', 4)
                    .arg(fit.heavyRmsd, 0, 'f', 4)
                    .arg(fit.maximum, 0, 'f', 4));
        });
    });
    connect(saveXyz, &QAction::triggered, this, [this] {
        window->safe([&] {
            const auto p =
                QFileDialog::getSaveFileName(this, "表示中の基準構造を保存", "result.xyz", "XYZ (*.xyz)");
            if (!p.isEmpty())
                saveGeometry(p.endsWith(".xyz", Qt::CaseInsensitive) ? p : p + ".xyz");
        });
    });
    connect(savePng, &QAction::triggered, this, [this] {
        window->safe([&] {
            auto p = QFileDialog::getSaveFileName(this, "結果画像を保存", "result.png", "PNG (*.png)");
            if (p.isEmpty())
                return;
            if (!p.endsWith(".png", Qt::CaseInsensitive))
                p += ".png";
            if (!view->grab().save(p))
                fail("画像を保存できません。");
        });
    });
    display(true);
    updateActions();
}
ResultsPanel::~ResultsPanel() {
    cancelLoad();
}
void ResultsPanel::applyChromeStyle() {
    const auto pal = static_cast<QWidget *>(window)->palette();
    // The rules of the other tabs, and the parts of this one.
    setStyleSheet(chromeRules(settingsRules() + R"(
        #resultsTab { background: @window; }
        #resultBar { background: @base; border-bottom: 1px solid @line; }
        QLabel#resultsJob { color: @text; }
        QLabel#resultsEmptyHint { color: @muted; }
        QLabel[chip] { border-radius: 9px; padding: 2px 9px; }
        QLabel[chip="neutral"] { background: @segment; color: @text; }
        QLabel[chip="good"] { background: @goodTint; color: @good; font-weight: bold; }
        QLabel[chip="warn"] { background: @candidate; color: @candidateText; border: 1px solid @candidateLine;
                              font-weight: bold; }
        QLabel[chip="bad"] { background: @badTint; color: @error; font-weight: bold; }
        QToolButton#resultsOpen, QToolButton#resultsExport, #resultsDock QToolButton {
            border: 1px solid @line; border-radius: 6px; background: @base; color: @text; padding: 5px 10px; }
        QToolButton#resultsOpen:hover, QToolButton#resultsExport:hover, #resultsDock QToolButton:hover {
            background: @hover; }
        QToolButton#resultsOpen:disabled, QToolButton#resultsExport:disabled, #resultsDock QToolButton:disabled {
            color: @muted; border-color: @hover; }
        QToolButton#resultsOpen { padding-right: 24px; }
        QToolButton#resultsOpen::menu-button { border: none; border-left: 1px solid @line; width: 18px; }
        QToolButton#resultsExport, #resultsDock QToolButton { padding-right: 20px; }
        #resultsViewCard { background: @base; border: 1px solid @line; border-radius: 10px; }
        #resultsContext, #resultsMeasureCard { background: @base; border: 1px solid @line; border-radius: 8px; }
        QLabel#resultsContextTitle, QLabel#resultsMeasurement { color: @text; }
        QLabel#resultsContextDetail, QLabel#resultsHint, QLabel#resultsFrameEnergy { color: @muted; }
        #resultsDock { background: @base; border-top: 1px solid @line; border-bottom-left-radius: 10px;
                       border-bottom-right-radius: 10px; }
        #resultsDock QPushButton { padding: 4px 8px; }
        #resultsBanner[tone="warn"] { background: @candidate; border: 1px solid @candidateLine; border-radius: 8px; }
        #resultsBanner[tone="bad"] { background: @badTint; border: 1px solid @error; border-radius: 8px; }
        #resultsBanner[tone="warn"] QLabel { color: @candidateText; }
        #resultsBanner[tone="bad"] QLabel { color: @text; }
        #resultsBanner[tone="bad"] QLabel#resultsBannerTitle { color: @error; }
        QPlainTextEdit#resultsStatus { background: @base; border: 1px solid @line; border-radius: 8px;
                                       color: @text; padding: 2px 8px; }
        QScrollArea#resultsOverview, #resultsOverviewPage { background: @base; border: none; }
        QLabel[role="section"] { color: @text; }
        QLabel[role="file"] { color: @text; }
        QPushButton[role="open"] { padding: 2px 10px; }
        QLabel#resultsLogName { color: @muted; }
    )" + TileRules,
                              pal));
    viewControlsFrame->applyStyle(pal);
    jobIcon->setPixmap(lineIcon("folder", ChromeColors(pal).muted).pixmap(QSize(16, 16)));
    // Icons drawn in fixed colors follow the palette here.
    const ChromeColors c(pal);
    for (auto *b : {playFrames, playMode})
        b->setIcon(
            lineIcon(animation.isActive() && b->toolTip() == "再生を停止" ? "pause" : "play", c.onAccent));
    showOutcome();
    showOptimization();
}
void ResultsPanel::stopAnimation() {
    animation.stop();
    const auto onAccent = ChromeColors(palette()).onAccent;
    playFrames->setIcon(lineIcon("play", onAccent));
    playFrames->setToolTip("構造列を再生");
    playFrames->setAccessibleName("構造列を再生");
    playMode->setIcon(lineIcon("play", onAccent));
    playMode->setToolTip("選択したモードを再生");
    playMode->setAccessibleName("選択したモードを再生");
    showContext();
}
void ResultsPanel::error(const QString &message) {
    status->setPlainText("読込・表示エラー: " + message + "\n前の結果は保持しています。");
    emit loadFailed(message);
}
void ResultsPanel::cancelLoad() {
    if (!process)
        return;
    auto *p = process;
    process = nullptr;
    p->disconnect(this);
    p->kill();
    p->waitForFinished(2000);
    p->deleteLater();
    work.reset();
    pendingPath.clear();
    status->setPlainText("読込を中止しました。前の結果を保持しています。");
    updateActions();
}
void ResultsPanel::openFile(const QString &path) {
    cancelLoad();
    bool ok = true;
    const auto suffix = QFileInfo(path).suffix().toLower();
    const double iso = suffix == "cube" || suffix == "cub" ? isovalue->text().toDouble(&ok) : .03;
    if (!ok || !std::isfinite(iso) || iso <= 0 || iso > 1e6) {
        error("CUBEの等値は0より大きい有限の数で指定してください。");
        return;
    }
    startLoad(path, iso, 0);
}
void ResultsPanel::openDirectory(const QString &path) {
    cancelLoad();
    QDir dir(path);
    if (!dir.exists()) {
        error("フォルダーが見つかりません。");
        return;
    }
    folder = dir.absolutePath();
    fillFiles("output.out");
    if (files->currentIndex() < 0) {
        error("対応する結果ファイルがありません。");
        return;
    }
    window->safe([&] { openEntry(files->currentData().toString()); });
}
void ResultsPanel::fillFiles(const QString &selected) {
    const QSignalBlocker guard(files);
    files->clear();
    animationFrames.clear();
    static const QRegularExpression step(R"(^(.*)\.m-(\d+)\.s-(\d+)\.xyz$)");
    const auto kind = [](const QString &name) {
        const auto suffix = QFileInfo(name).suffix().toLower();
        return suffix == "out" || suffix == "log"    ? 0
               : suffix == "xyz"                     ? 1
               : suffix == "cube" || suffix == "cub" ? 2
                                                     : 3;
    };
    QVector<QStringList> groups(4);
    QMap<QPair<QString, int>, QStringList> modes;
    for (const auto &name : resultFiles(folder)) {
        if (const auto m = step.match(name); m.hasMatch())
            modes[{m.captured(1), m.captured(2).toInt()}] << name;
        else
            groups[kind(name)] << name;
    }
    // Logs, structures, CUBE, MOLDEN, then the animations, separated by kind.
    for (const auto &group : groups) {
        if (group.isEmpty())
            continue;
        if (files->count())
            files->insertSeparator(files->count());
        for (const auto &name : group)
            files->addItem(name, name);
    }
    if (!modes.isEmpty() && files->count())
        files->insertSeparator(files->count());
    for (auto it = modes.begin(); it != modes.end(); ++it) {
        auto list = it.value();
        std::sort(list.begin(), list.end(), [](const QString &a, const QString &b) {
            return step.match(a).captured(3).toInt() < step.match(b).captured(3).toInt();
        });
        const auto key = QString("animation:%1:%2").arg(it.key().first).arg(it.key().second);
        animationFrames[key] = list;
        files->addItem(QString("振動アニメーション %1: モード %2（%3枚）")
                           .arg(QFileInfo(it.key().first).fileName())
                           .arg(it.key().second)
                           .arg(list.size()),
                       key);
    }
    int index = files->findData(selected);
    if (index < 0 && !selected.isEmpty() && !selected.startsWith("animation:")) {
        files->addItem(selected, selected);
        index = files->count() - 1;
    }
    for (int i = 0; index < 0 && i < files->count(); ++i)
        if (!files->itemData(i).toString().isEmpty())
            index = i;
    files->setCurrentIndex(index);
}
void ResultsPanel::openEntry(const QString &entry) {
    if (!entry.startsWith("animation:")) {
        openFile(QDir(folder).filePath(entry));
        return;
    }
    // Join the steps of one mode into a multi-frame XYZ in a private temporary folder.
    const auto list = animationFrames.value(entry);
    if (list.isEmpty() || !animationDir.isValid())
        fail("振動アニメーションのファイルが見つかりません。");
    QByteArray joined;
    for (const auto &name : list) {
        joined += readFile(QDir(folder).filePath(name));
        if (!joined.endsWith('\n'))
            joined += '\n';
        if (joined.size() > 64 * 1024 * 1024)
            fail("振動アニメーションのファイルが大きすぎます。");
    }
    const auto path = animationDir.filePath(QString("animation-%1.xyz").arg(animationSources.size() + 1));
    writeFile(path, joined);
    animationSources[path] = entry;
    openFile(path);
}
QString ResultsPanel::jobMolden() const {
    if (data.format != "NWChem")
        return {};
    for (const auto &name : resultFiles(QFileInfo(data.source).absolutePath()))
        if (QStringList{"molden", "mld", "molde"}.contains(QFileInfo(name).suffix().toLower()) &&
            files->findData(name) >= 0)
            return name;
    return {};
}
void ResultsPanel::openOrbitalShape(int logOrbital) {
    const auto molden = jobMolden();
    const int index = molden.isEmpty() ? -1 : files->findData(molden);
    if (index < 0)
        return;
    if (logOrbital < 0 || logOrbital >= data.orbitals.size())
        logOrbital = frontierOrbital(data.orbitals, "Alpha");
    // The same orbital (spin and NWChem's number) is selected once the MOLDEN file has loaded.
    pendingOrbital = {};
    if (logOrbital >= 0 && data.orbitals[logOrbital].number > 0)
        pendingOrbital = {QFileInfo(QDir(folder).filePath(molden)).absoluteFilePath(),
                          data.orbitals[logOrbital].spin, data.orbitals[logOrbital].number};
    files->setCurrentIndex(index); // Loads it like a choice in the file list.
}
void ResultsPanel::renderOrbital(int orbital) {
    if (data.format != "MOLDEN" || orbital < 0 || orbital >= data.orbitals.size())
        return;
    cancelLoad();
    bool ok;
    const double iso = orbitalIso->text().toDouble(&ok);
    if (!ok || !std::isfinite(iso) || iso <= 0 || iso > 1e6) {
        error("軌道の等値は0より大きい有限の数で指定してください。");
        return;
    }
    startLoad(data.source, iso, 0, orbital, orbitalNorm->currentData().toString(), orbitalGrid->value(),
              true);
}
void ResultsPanel::startLoad(const QString &path, double iso, int channel, int orbital,
                             const QString &normalization, int divisions, bool reuseOrbital) {
    cancelLoad();
    stopAnimation();
    if (path.isEmpty())
        return;
    const QFileInfo before(path);
    const auto stamp = sourceStamp(before);
    const bool sameSource = !stamp.isEmpty() && stamp == orbitalCacheSource;
    if (!reuseOrbital || !sameSource) {
        orbitalCache.clear();
        orbitalCacheSource.clear();
    }
    if (reuseOrbital && !stamp.isEmpty()) {
        if (const auto *cached = orbitalCache.object(orbitalKey(orbital, iso, normalization, divisions))) {
            data.surface = cached->triangles;
            data.grid = cached->grid;
            data.minimum = cached->minimum;
            data.maximum = cached->maximum;
            data.orbitalNormalization = cached->normalization;
            data.warnings = cached->warnings;
            data.orbitalError.clear();
            data.selectedOrbital = orbital;
            data.isovalue = iso;
            data.normalizationRequest = normalization;
            data.orbitalDivisions = divisions;
            display(true);
            emit loaded();
            return;
        }
    }
    work = std::make_unique<QTemporaryDir>();
    if (!work->isValid()) {
        error("一時フォルダーを作成できません。");
        return;
    }
    pendingPath = before.absoluteFilePath();
    const QString request = work->filePath("request.json"), response = work->filePath("result.bin");
    try {
        writeFile(request, QJsonDocument(QJsonObject{{"path", pendingPath},
                                                     {"isovalue", iso},
                                                     {"dataset", channel},
                                                     {"orbital", orbital},
                                                     {"normalization", normalization},
                                                     {"orbital_divisions", divisions}})
                               .toJson());
    } catch (const std::exception &e) {
        error(QString::fromUtf8(e.what()));
        work.reset();
        return;
    }
    process = new QProcess(this);
    auto *p = process;
    p->setProgram(QCoreApplication::applicationFilePath());
    p->setArguments({"--result-worker", request, response});
    p->setStandardOutputFile(QProcess::nullDevice());
    p->setStandardErrorFile(QProcess::nullDevice());
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (process != p || e != QProcess::FailedToStart)
            return;
        const auto message = p->errorString();
        process = nullptr;
        p->deleteLater();
        work.reset();
        pendingOrbital = {};
        updateActions();
        error(message);
    });
    connect(
        p, &QProcess::finished, this,
        [this, p, before, stamp, response, reuseOrbital, sameSource](int code, QProcess::ExitStatus state) {
            if (process != p)
                return;
            process = nullptr;
            p->deleteLater();
            QString failure;
            int follow = -1; // The log's orbital in the job's MOLDEN file, to show next.
            const auto wanted = std::exchange(pendingOrbital, {});
            try {
                if (code != 0 || state != QProcess::NormalExit) {
                    const QString e = QFileInfo::exists(response + ".error")
                                          ? QString::fromUtf8(readFile(response + ".error"))
                                          : "結果の読込プロセスが失敗しました。";
                    fail(e);
                }
                const QFileInfo after(before.absoluteFilePath());
                if (stamp.isEmpty() || sourceStamp(after) != stamp)
                    fail("読込中にファイルが更新されました。再読込してください。");
                auto next = loadResultData(response);
                const bool newSource = data.source != next.source;
                data = std::move(next);
                orbitalCacheSource = stamp;
                cacheOrbital();
                display(reuseOrbital && sameSource && !newSource);
                if (newSource && data.format == "MOLDEN" && !data.orbitals.isEmpty())
                    details->setCurrentIndex(2);
                else if (newSource && data.format == "NWChem" && !data.energies.isEmpty())
                    details->setCurrentIndex(1);
                if (newSource) {
                    // The source path and the selected details tab can resize the view.
                    layout()->activate();
                    view->fit();
                }
                if (data.format == "MOLDEN" && !wanted.path.isEmpty() &&
                    QFileInfo(data.source).absoluteFilePath() == wanted.path)
                    for (int i = 0; i < data.orbitals.size(); ++i)
                        if (data.orbitals[i].spin == wanted.spin &&
                            data.orbitals[i].number == wanted.number) {
                            follow = i != data.selectedOrbital ? i : -1;
                            break;
                        }
            } catch (const std::exception &e) {
                failure = QString::fromUtf8(e.what());
            }
            pendingPath.clear();
            work.reset();
            updateActions();
            if (failure.isEmpty())
                emit loaded();
            else
                error(failure);
            if (follow >= 0)
                orbitals->setCurrentCell(follow, 0); // Renders it like a choice in the list.
        });
    status->setPlainText("読込中: " + pendingPath + "\n完了まで前の結果を表示します。");
    updateActions();
    p->start();
}
void ResultsPanel::cacheOrbital() {
    if (data.format != "MOLDEN" || data.selectedOrbital < 0 || orbitalCacheSource.isEmpty())
        return;
    qsizetype cost = sizeof(OrbitalSurface) + data.surface.capacity() * sizeof(SurfaceTriangle) + 1024;
    for (const auto &warning : data.warnings)
        cost += warning.size() * sizeof(QChar);
    if (cost > orbitalCache.maxCost())
        return;
    orbitalCache.insert(
        orbitalKey(data.selectedOrbital, data.isovalue, data.normalizationRequest, data.orbitalDivisions),
        new OrbitalSurface{data.surface, data.grid, data.minimum, data.maximum, data.orbitalNormalization,
                           data.warnings},
        int(cost));
}
void ResultsPanel::updateActions() {
    const bool ready = !loading() && !data.source.isEmpty();
    for (auto *control : std::as_const(viewControls))
        control->setEnabled(!data.frames.isEmpty());
    cancel->setEnabled(loading());
    reload->setEnabled(ready);
    // Nothing open yet: only "開く…", not a row of buttons that cannot be used.
    const bool shown = !data.source.isEmpty() || loading();
    for (QWidget *w : {static_cast<QWidget *>(reload), static_cast<QWidget *>(compareGeometry),
                       static_cast<QWidget *>(applyGeometry), static_cast<QWidget *>(exportButton),
                       findChild<QWidget *>("resultsBarDivider")})
        w->setVisible(shown);
    cancel->setVisible(loading());
    saveXyz->setEnabled(ready && !data.frames.isEmpty());
    applyGeometry->setEnabled(ready && !data.frames.isEmpty());
    compareGeometry->setEnabled(applyGeometry->isEnabled());
    savePng->setEnabled(ready && !data.frames.isEmpty());
    exportButton->setEnabled(saveXyz->isEnabled() || savePng->isEnabled());
    frame->setEnabled(ready && !data.frames.isEmpty());
    timeline->setEnabled(ready && data.frames.size() > 1);
    playFrames->setEnabled(ready && data.frames.size() > 1);
    firstFrame->setEnabled(playFrames->isEnabled());
    lastFrame->setEnabled(playFrames->isEnabled());
    const int mode = modes->currentRow();
    playMode->setEnabled(ready && mode >= 0 && mode < data.modes.size() &&
                         !data.modes[mode].displacement.isEmpty());
    saveModeImages->setEnabled(playMode->isEnabled());
    saveModeTrajectory->setEnabled(playMode->isEnabled());
    modeExport->setEnabled(playMode->isEnabled());
    energyReference->setEnabled(ready && !data.energies.isEmpty());
    applyIso->setEnabled(ready && data.format == "CUBE");
    dataset->setEnabled(applyIso->isEnabled());
    const int selected = orbitals->currentRow();
    const bool orbitalReady = ready && data.format == "MOLDEN" && data.orbitalBasis;
    orbitalIso->setEnabled(orbitalReady);
    orbitalGrid->setEnabled(orbitalReady);
    orbitalNorm->setEnabled(orbitalReady);
    const auto spin =
        selected >= 0 && selected < data.orbitals.size() ? data.orbitals[selected].spin : QString("Alpha");
    homo->setEnabled(orbitalReady && frontierOrbital(data.orbitals, spin) >= 0);
    lumo->setEnabled(orbitalReady && frontierOrbital(data.orbitals, spin, true) >= 0);
    for (int i = 0; i < 6; ++i)
        orbitalNavigation[i]->setEnabled(
            orbitalReady && data.selectedOrbital >= 0 &&
            (i < 3 ? data.selectedOrbital > 0 : data.selectedOrbital < data.orbitals.size() - 1));
    orbitalPosition->setText(data.selectedOrbital >= 0
                                 ? QString("%1 %2番").arg(data.orbitals[data.selectedOrbital].spin,
                                                          orbitals->item(data.selectedOrbital, 0)->text())
                                 : QString());
}
void ResultsPanel::display(bool orbitalOnly) {
    const auto brief = briefSummary();
    status->setPlainText(data.source.isEmpty() ? QString()
                         : brief.isEmpty()     ? data.status
                                               : data.status + "\n" + brief);
    if (!orbitalOnly && !data.source.isEmpty()) {
        QString selectedFile = animationSources.value(data.source);
        for (int i = 0; selectedFile.isEmpty() && i < files->count(); ++i) {
            const auto entry = files->itemData(i).toString();
            if (!entry.isEmpty() && !entry.startsWith("animation:") &&
                QFileInfo(QDir(folder).filePath(entry)).absoluteFilePath() ==
                    QFileInfo(data.source).absoluteFilePath())
                selectedFile = entry;
        }
        if (selectedFile.isEmpty()) {
            folder = QFileInfo(data.source).absolutePath();
            selectedFile = QFileInfo(data.source).fileName();
        }
        fillFiles(selectedFile);
    }
    // The bar: the job folder and file, the kind of file and how the calculation ended.
    const bool open = !data.source.isEmpty();
    emptyHint->setVisible(!open);
    for (QWidget *w :
         {static_cast<QWidget *>(jobIcon), static_cast<QWidget *>(jobLabel), static_cast<QWidget *>(files)})
        w->setVisible(open);
    jobLabel->setText(QFileInfo(folder).fileName());
    jobLabel->setToolTip(QDir::toNativeSeparators(data.source));
    setChip(formatChip, open ? formatName(data.format) : QString(), "neutral");
    const auto outcome = resultOutcome(data);
    setChip(outcomeChip,
            outcome == "normal"       ? QString("正常終了")
            : outcome == "incomplete" ? QString("正常終了の記録なし")
            : outcome == "error"      ? QString("エラー")
                                      : QString(),
            outcome == "normal"       ? "good"
            : outcome == "incomplete" ? "warn"
                                      : "bad",
            data.status);
    if (!orbitalOnly) {
        log->setPlainText(data.log);
        logName->setText(QFileInfo(data.source).fileName() +
                         (data.log.startsWith("…") ? QString(" · 末尾 128 KiB") : QString()));
        referenceEnergy = -1;
        picked.clear();
        pinned.clear();
        showEnergies();
    }
    {
        const QSignalBlocker guard(orbitals);
        if (!orbitalOnly) {
            orbitals->setRowCount(data.orbitals.size());
            QMap<QString, int> indices;
            QMap<QString, int> homos, lumos;
            for (const auto &spin : {QString("Alpha"), QString("Beta")}) {
                homos[spin] = frontierOrbital(data.orbitals, spin);
                lumos[spin] = frontierOrbital(data.orbitals, spin, true);
            }
            for (int i = 0; i < data.orbitals.size(); ++i) {
                const auto &o = data.orbitals[i];
                const QString frontier = i == homos[o.spin]   ? "HOMO"
                                         : i == lumos[o.spin] ? "LUMO"
                                                              : QString();
                // The orbital's own number, so a log listing only some orbitals matches the MOLDEN file.
                const int number = o.number > 0 ? o.number : indices[o.spin] + 1;
                indices[o.spin] = number;
                row(orbitals, i,
                    {QString::number(number), frontier, o.symmetry, o.spin, fixed(o.energy, 6),
                     real(o.occupation)});
            }
            levels->setOrbitals(data.orbitals, data.selectedOrbital);
            // Columns that would say the same on every row stay hidden (one spin, no symmetry labels).
            QSet<QString> symmetries, spins;
            for (const auto &o : data.orbitals) {
                symmetries << o.symmetry.trimmed().toLower();
                spins << o.spin;
            }
            orbitals->setColumnHidden(2, symmetries.size() <= 1);
            orbitals->setColumnHidden(3, spins.size() <= 1);
        } else
            levels->setSelected(data.selectedOrbital);
        orbitals->clearSelection();
        orbitals->setCurrentCell(data.selectedOrbital, 0);
        if (data.selectedOrbital >= 0)
            orbitals->scrollToItem(orbitals->item(data.selectedOrbital, 0));
    }
    {
        const QSignalBlocker isoGuard(orbitalIso), gridGuard(orbitalGrid), normGuard(orbitalNorm);
        orbitalIso->setText(real(data.isovalue));
        orbitalGrid->setValue(data.orbitalDivisions);
        orbitalNorm->setCurrentIndex(orbitalNorm->findData(data.normalizationRequest));
    }
    // A log has energies only; the shapes come from a MOLDEN file, if the job wrote one.
    const auto molden = jobMolden();
    openMolden->setVisible(!molden.isEmpty());
    openMolden->setText("形を表示: " + molden);
    openMolden->setToolTip(
        QString("選択中の軌道（未選択ならHOMO）を、同じジョブの %1 から表示します。").arg(molden));
    orbitalSource->setText(
        data.format == "NWChem" && !data.orbitals.isEmpty()
            ? (molden.isEmpty()
                   ? QString("ログの軌道エネルギーです。軌道の形を表示するには、「2 計算条件」で"
                             "「計算後に分子軌道をMOLDENファイルに出力」をオンにして計算してください。")
                   : QString("ログの軌道エネルギーです。行をダブルクリックするか「形を表示」で、"
                             "同じジョブの %1 からその軌道の形を表示します。")
                         .arg(molden))
        : data.format == "MOLDEN" ? QString("エネルギーと形: %1").arg(QFileInfo(data.source).fileName())
                                  : QString());
    orbitalSource->setVisible(!orbitalSource->text().isEmpty());
    orbitalOptions->setVisible(data.format == "MOLDEN");
    orbitalStatus->setText(
        data.format == "NWChem" && !data.orbitals.isEmpty()
            ? QString("番号はNWChemの軌道番号です（ログに一部の軌道しかなくてもMOLDENと同じ番号）。")
        : data.format != "MOLDEN"      ? "MOLDENファイルを開くと分子軌道を表示します。"
        : !data.orbitalError.isEmpty() ? data.orbitalError
        : data.selectedOrbital >= 0
            ? QString("表示中: %1 %2番 / 等値 ±%3 / 正規化: "
                      "%4\n準位図か一覧で軌道を選ぶと表示を切り替えます。赤: 正 / 青: 負。")
                  .arg(data.orbitals[data.selectedOrbital].spin,
                       orbitals->item(data.selectedOrbital, 0)->text(), real(data.isovalue),
                       data.orbitalNormalization == "nwchem" ? "NWChem" : "標準MOLDEN")
            : "このファイルには表示可能な分子軌道がありません。");
    if (!orbitalOnly) {
        const QSignalBlocker guard(modes);
        modes->setRowCount(data.modes.size());
        rigidMotion = resultRigidModes(data);
        double strongest = 0;
        for (int i = 0; i < data.modes.size(); ++i)
            if (!rigidMotion.value(i) && std::isfinite(data.modes[i].intensity))
                strongest = std::max(strongest, data.modes[i].intensity);
        int vibrationCount = 0, imaginary = 0;
        for (int i = 0; i < data.modes.size(); ++i) {
            const auto &m = data.modes[i];
            const bool rigid = rigidMotion.value(i);
            vibrationCount += !rigid;
            imaginary += !rigid && m.frequency < -10;
            // Translations and rotations are no vibrations: hidden by default, without an IR intensity.
            row(modes, i,
                {real(m.frequency), rigid ? QString("—") : real(m.intensity),
                 rigid             ? "並進・回転"
                 : m.frequency < 0 ? "虚振動"
                                   : "振動"});
            if (!rigid && strongest > 0 && std::isfinite(m.intensity))
                modes->item(i, 1)->setData(Qt::UserRole, std::clamp(m.intensity / strongest, 0., 1.));
            if (!rigid && m.frequency < -10)
                modes->item(i, 2)->setForeground(errorColor(palette()));
            modes->setRowHidden(i, rigid && !showRigid->isChecked());
        }
        modes->clearSelection();
        modes->setCurrentCell(-1, -1);
        QStringList parts{QString("振動 %1").arg(vibrationCount),
                          imaginary ? QString("虚振動 %1個").arg(imaginary) : QString("虚振動なし")};
        if (const auto thermo = data.properties.value("thermochemistry").toArray(); !thermo.isEmpty())
            parts << QString("ゼロ点補正 %1 kcal/mol")
                         .arg(fixed(thermo[0].toObject()["zpe_kcal"].toDouble(), 3));
        if (data.modes.size() > vibrationCount)
            parts << QString("並進・回転 %1").arg(data.modes.size() - vibrationCount);
        modeSummary->setText(parts.join(" · "));
        showIrSpectrum();
        showPath();
        showProperties();
        showOptimization();
        excitations->setRowCount(data.excitations.size());
        for (int i = 0; i < data.excitations.size(); ++i) {
            const auto &x = data.excitations[i];
            row(excitations, i,
                {QString::number(x.root),
                 x.spin.isEmpty()      ? "非制限"
                 : x.spin == "singlet" ? "一重項"
                                       : "三重項",
                 x.symmetry, fixed(x.ev, 4), fixed(EvNanometer / x.ev, 1), fixed(x.oscillator, 5),
                 std::isfinite(x.s2) ? fixed(x.s2, 4) : "—"});
        }
        showSpectrum();
        // Energies of the frames that scan and IRC points name, against the lowest of them.
        frameEnergies = QVector<double>(data.frames.size(), std::numeric_limits<double>::quiet_NaN());
        const auto setEnergy = [&](int index, double hartree) {
            if (index >= 0 && index < frameEnergies.size() && std::isfinite(hartree))
                frameEnergies[index] = hartree;
        };
        for (const auto &value : data.properties.value("scan").toArray())
            setEnergy(value.toObject()["frame"].toInt(-1), value.toObject()["energy"].toDouble(NAN));
        const auto irc = data.properties.value("irc").toObject();
        for (const auto &value : irc["points"].toArray())
            setEnergy(value.toObject()["frame"].toInt(-1), value.toObject()["energy"].toDouble(NAN));
        if (irc.contains("ts_energy"))
            setEnergy(irc["ts_frame"].toInt(-1), irc["ts_energy"].toDouble(NAN));
        double lowest = INFINITY;
        for (double e : std::as_const(frameEnergies))
            if (std::isfinite(e))
                lowest = std::min(lowest, e);
        for (double &e : frameEnergies)
            e = (e - lowest) * HartreeToKcal;
        timeline->setFrames(data.frames.size(), frameEnergies);
        frameEnergy->setVisible(std::isfinite(lowest));
    }
    {
        const QSignalBlocker guard(frame);
        frame->setRange(data.frames.isEmpty() ? 0 : 1, data.frames.size());
        frame->setValue(data.selectedOrbital >= 0 ? data.orbitalFrame + 1 : data.frames.size());
        frame->setSuffix(QString(" / %1").arg(data.frames.size()));
    }
    dataset->clear();
    for (int n : data.datasets)
        dataset->addItem(QString::number(n));
    dataset->setCurrentIndex(data.dataset);
    isovalue->setText(real(data.isovalue));
    updateTabs();
    showOverview();
    showOutcome();
    showFrame();
    updateActions();
}
void ResultsPanel::updateTabs() {
    const auto p = data.properties;
    const bool loaded = !data.source.isEmpty();
    const bool properties = p.contains("mulliken") || p.contains("esp") || p.contains("resp") ||
                            p.contains("shielding") || p.contains("polarizability") || p.contains("bsse") ||
                            p.contains("solvation");
    const bool visible[]{true,
                         loaded && !data.energies.isEmpty(),
                         loaded && (!data.orbitals.isEmpty() || data.format == "MOLDEN"),
                         loaded && !data.modes.isEmpty(),
                         loaded && !data.steps.isEmpty(),
                         loaded && !data.excitations.isEmpty(),
                         loaded && (p.contains("scan") || p.contains("irc")),
                         loaded && properties,
                         loaded && (data.format == "CUBE" || data.format == "MOLDEN"),
                         loaded && !data.log.isEmpty()};
    // Counts after the names: what each tab lists.
    const int vibrations = int(std::count(rigidMotion.begin(), rigidMotion.end(), false)) +
                           std::max(0, int(data.modes.size() - rigidMotion.size()));
    const int counts[]{0,
                       int(data.energies.size()),
                       int(data.orbitals.size()),
                       loaded ? vibrations : 0,
                       int(data.steps.size()),
                       int(data.excitations.size()),
                       path->rowCount(),
                       0,
                       0,
                       0};
    const QStringList names{"概要",     "エネルギー",     "軌道", "振動",          "最適化",
                            "励起状態", "経路・スキャン", "物性", "CUBE / 等値面", "元ファイル"};
    for (int i = 0; i < details->count() && i < int(std::size(visible)); ++i) {
        details->setTabVisible(i, visible[i]);
        details->setTabText(i, counts[i] > 0 ? QString("%1 %2").arg(names[i]).arg(counts[i]) : names[i]);
    }
}
void ResultsPanel::setOverview(QWidget *page) {
    // The old page goes later: a button on it may be what led here.
    if (auto *old = overviewScroll->takeWidget())
        old->deleteLater();
    overviewScroll->setWidget(page);
}
void ResultsPanel::showOverview() {
    auto *page = new QWidget;
    page->setObjectName("resultsOverviewPage");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);
    const auto label = [&](const QString &text, const char *role, QLayout *into = nullptr) {
        auto *l = new QLabel(text);
        l->setProperty("role", role);
        l->setWordWrap(true);
        l->setTextFormat(Qt::PlainText);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        (into ? into : layout)->addWidget(l);
        return l;
    };
    const auto section = [&](const QString &text) {
        layout->addSpacing(6);
        auto *l = label(text, "section");
        auto font = l->font();
        font.setBold(true);
        l->setFont(font);
    };
    if (data.source.isEmpty()) {
        label(
            "結果ファイル（NWChem出力・MOLDEN・CUBE・XYZ）またはジョブフォルダーを開くと、ここに結果の要点を"
            "表"
            "示し、データのあるタブ（エネルギー・軌道・振動など）が現れます。\n\n「計算を実行」（F5）で計算す"
            "ると、"
            "終了後に自動で読み込みます。過去の計算は「履歴」から開けます。",
            "");
        layout->addStretch();
        setOverview(page);
        return;
    }
    // What was calculated: the formula of the last structure and the file.
    if (!data.frames.isEmpty()) {
        const auto &molecule = data.frames.last().molecule;
        auto *formula = new QLabel(formulaHtml(molecule.formula()));
        formula->setObjectName("resultsFormula");
        formula->setTextFormat(Qt::RichText);
        auto font = formula->font();
        font.setPointSizeF(font.pointSizeF() * 1.5);
        font.setBold(true);
        formula->setFont(font);
        layout->addWidget(formula);
        label(QString("%1原子 · %2 · %3")
                  .arg(molecule.atoms.size())
                  .arg(formatName(data.format), QFileInfo(data.source).fileName()),
              "muted");
    } else
        label(QString("%1 · %2").arg(formatName(data.format), QFileInfo(data.source).fileName()), "muted");
    // The outcomes as cards; a card with a tab opens it.
    auto *grid = new QGridLayout;
    grid->setSpacing(8);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    int cell = 0;
    const auto tile = [&](const QString &name, const QString &title, const QString &value,
                          const QString &caption, const QString &tone, int tab, bool wide = false) {
        const bool button = tab >= 0 && details->isTabVisible(tab);
        auto *t = summaryTile(title, value, caption, tone, button);
        t->setObjectName(name);
        if (button) {
            t->setToolTip(QString("「%1」を表示").arg(details->tabText(tab)));
            connect(static_cast<QPushButton *>(t), &QPushButton::clicked, this,
                    [this, tab] { details->setCurrentIndex(tab); });
        }
        if (wide) {
            cell += cell % 2;
            grid->addWidget(t, cell / 2, 0, 1, 2);
            cell += 2;
        } else {
            grid->addWidget(t, cell / 2, cell % 2);
            ++cell;
        }
    };
    if (const auto final = data.properties["final_energy"].toObject(); !final.isEmpty()) {
        const auto source = final["source"] == "frequency_reference" ? QString("振動数の参照構造")
                            : final["source"] == "bsse_corrected"    ? QString("BSSE補正後")
                                                                     : QString("最後のtask");
        QString caption;
        if (!data.energies.isEmpty() && data.energies.last().hartree != final["hartree"].toDouble())
            caption = QString("出力の最後の記録: %1 %2 Eh")
                          .arg(data.energies.last().method, real(data.energies.last().hartree));
        tile("resultsTileEnergy",
             QString("計算のエネルギー（%1 · %2）").arg(final["method"].toString(), source),
             real(final["hartree"].toDouble()) + " Eh", caption, "neutral", 1, true);
    } else if (!data.energies.isEmpty())
        tile("resultsTileEnergy", QString("最後のエネルギー記録（%1）").arg(data.energies.last().method),
             real(data.energies.last().hartree) + " Eh", "出力順の最後の値です。", "neutral", 1, true);
    if (data.properties.contains("optimization") || !data.steps.isEmpty()) {
        const bool converged = data.properties.value("optimization") == "converged";
        QString caption;
        if (!data.steps.isEmpty()) {
            caption = QString("%1ステップ").arg(data.steps.last().step);
            const auto t = data.properties.value("thresholds").toObject();
            const auto &last = data.steps.last();
            int met = 0, judged = 0;
            for (const auto &[key, value] : {std::pair{"gmax", last.gmax},
                                             {"grms", last.grms},
                                             {"xrms", last.xrms},
                                             {"xmax", last.xmax}})
                if (t.contains(key)) {
                    ++judged;
                    met += value <= t[key].toDouble();
                }
            if (judged)
                caption += QString(" · 最終ステップは判定 %1/%2 を満たす").arg(met).arg(judged);
        }
        tile("resultsTileOptimization", "構造最適化", converged ? "収束" : "収束メッセージなし", caption,
             converged ? "good" : "warn", 4);
    }
    if (!data.modes.isEmpty()) {
        int vibrations = 0;
        QStringList imaginary;
        for (int i = 0; i < data.modes.size(); ++i) {
            if (rigidMotion.value(i))
                continue;
            ++vibrations;
            if (data.modes[i].frequency < -10)
                imaginary << fixed(data.modes[i].frequency, 1);
        }
        const int rigid = int(data.modes.size()) - vibrations;
        tile("resultsTileImaginary", "虚振動（−10 cm⁻¹未満）",
             imaginary.isEmpty() ? QString("なし") : QString("%1個").arg(imaginary.size()),
             imaginary.isEmpty() ? QString("振動 %1%2")
                                       .arg(vibrations)
                                       .arg(rigid ? QString(" · 並進・回転 %1 は除外").arg(rigid) : QString())
                                 : imaginary.join(", ") + " cm⁻¹",
             imaginary.isEmpty() ? "good" : "warn", 3);
    }
    QStringList spins;
    for (const auto &o : data.orbitals)
        if (!spins.contains(o.spin))
            spins << o.spin;
    for (const auto &spin : std::as_const(spins)) {
        const int h = frontierOrbital(data.orbitals, spin), l = frontierOrbital(data.orbitals, spin, true);
        if (h < 0 || l < 0)
            continue;
        tile("resultsTileGap" + spin,
             spins.size() > 1 ? QString("HOMO–LUMOギャップ（%1）").arg(spin) : QString("HOMO–LUMOギャップ"),
             fixed((data.orbitals[l].energy - data.orbitals[h].energy) * HartreeToEv, 2) + " eV",
             QString("HOMO %1 · LUMO %2 eV")
                 .arg(fixed(data.orbitals[h].energy * HartreeToEv, 2),
                      fixed(data.orbitals[l].energy * HartreeToEv, 2)),
             "neutral", 2);
    }
    if (data.properties.contains("dipole")) {
        const auto d = data.properties.value("dipole").toObject();
        tile("resultsTileDipole", "双極子モーメント", fixed(d["debye"].toDouble(), 2) + " D",
             QString("x %1 · y %2 · z %3 a.u.")
                 .arg(fixed(d["x"].toDouble(), 4), fixed(d["y"].toDouble(), 4), fixed(d["z"].toDouble(), 4)),
             "neutral", 7);
    }
    if (!data.excitations.isEmpty()) {
        const auto strongest =
            std::max_element(data.excitations.begin(), data.excitations.end(),
                             [](const auto &a, const auto &b) { return a.oscillator < b.oscillator; });
        tile("resultsTileExcitations", "励起状態", QString("%1件").arg(data.excitations.size()),
             QString("最大の振動子強度 %1（%2 eV / %3 nm）")
                 .arg(fixed(strongest->oscillator, 4), fixed(strongest->ev, 3),
                      fixed(EvNanometer / strongest->ev, 1)),
             "neutral", 5);
    }
    if (path->rowCount())
        tile("resultsTilePath", data.properties.contains("scan") ? "座標スキャン" : "IRC",
             QString("%1点").arg(path->rowCount()), "行を選ぶとその点の構造を表示します。", "neutral", 6);
    if (data.format == "CUBE" || data.selectedOrbital >= 0)
        tile("resultsTileSurface", "等値面", "±" + real(data.isovalue),
             QString("格子 %1 × %2 × %3 · 値 %4 ～ %5 · 三角形 %6")
                 .arg(data.grid[0])
                 .arg(data.grid[1])
                 .arg(data.grid[2])
                 .arg(real(data.minimum), real(data.maximum))
                 .arg(data.surface.size()),
             "neutral", data.format == "CUBE" ? 8 : 2);
    if (data.frames.size() > 1)
        tile("resultsTileFrames", "構造列", QString("%1構造").arg(data.frames.size()),
             "3Dビューの下のタイムラインで切り替えます。", "neutral", -1);
    if (cell)
        layout->addLayout(grid);
    else
        delete grid;
    // Thermochemistry of each temperature.
    for (const auto &value : data.properties.value("thermochemistry").toArray()) {
        const auto t = value.toObject();
        const double temperature = t["temperature"].toDouble(), h = t["enthalpy_kcal"].toDouble(),
                     g = h - temperature * t["entropy_cal"].toDouble() / 1000;
        section(QString("熱化学（%1 K）").arg(fixed(temperature, 2)));
        auto *values = new QGridLayout;
        values->setHorizontalSpacing(16);
        values->setVerticalSpacing(4);
        const QList<std::pair<QString, QString>> rows{
            {"ゼロ点補正", fixed(t["zpe_kcal"].toDouble(), 3) + " kcal/mol"},
            {"熱補正（内部エネルギー）", fixed(t["energy_kcal"].toDouble(), 3) + " kcal/mol"},
            {"熱補正（エンタルピー）", fixed(h, 3) + " kcal/mol"},
            {"エントロピー", fixed(t["entropy_cal"].toDouble(), 3) + " cal/(mol·K)"},
            {"Gibbs補正 G = H − TS（算出値）",
             QString("%1 kcal/mol（%2 Eh）").arg(fixed(g, 3), fixed(g / HartreeToKcal, 6))}};
        for (int r = 0; r < rows.size(); ++r) {
            auto *name = new QLabel(rows[r].first);
            auto *number = new QLabel(rows[r].second);
            number->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            number->setTextInteractionFlags(Qt::TextSelectableByMouse);
            values->addWidget(name, r, 0);
            values->addWidget(number, r, 1);
        }
        values->setColumnStretch(0, 1);
        layout->addLayout(values);
        label(QString(
                  "補正は電子エネルギーに加えて使います。対称数 %1 はNWChemの値です（MolFormaの入力では1）。")
                  .arg(real(t["symmetry_number"].toDouble())),
              "muted");
    }
    if (const auto other = resultPropertyLines(data).trimmed(); !other.isEmpty()) {
        section("ほかの結果");
        label(other, "");
    }
    section("メッセージ");
    label(data.status, "");
    if (!data.warnings.isEmpty())
        label(data.warnings.join("\n\n"), "muted");
    // The other files of the job, to open with one click.
    QList<int> entries;
    for (int i = 0; i < files->count(); ++i)
        if (!files->itemData(i).toString().isEmpty())
            entries << i;
    if (entries.size() > 1) {
        section("このフォルダーの結果ファイル");
        for (int k = 0; k < entries.size() && k < 12; ++k) {
            const int i = entries[k];
            auto *line = new QHBoxLayout;
            line->setSpacing(8);
            auto *name = new QLabel(files->itemText(i));
            name->setProperty("role", "file");
            name->setTextFormat(Qt::PlainText);
            line->addWidget(name, 1);
            if (i == files->currentIndex()) {
                auto *current = new QLabel("表示中");
                current->setProperty("chip", "current");
                line->addWidget(current);
            } else {
                auto *openIt = new QPushButton("開く");
                openIt->setProperty("role", "open");
                openIt->setAccessibleName(files->itemText(i) + " を開く");
                connect(openIt, &QPushButton::clicked, this, [this, entry = files->itemData(i).toString()] {
                    if (const int index = files->findData(entry); index >= 0)
                        files->setCurrentIndex(index);
                });
                line->addWidget(openIt);
            }
            layout->addLayout(line);
        }
        if (entries.size() > 12)
            label(QString("ほか %1 件はバーのファイル一覧から選べます。").arg(entries.size() - 12), "muted");
    }
    layout->addStretch();
    setOverview(page);
}
void ResultsPanel::showOutcome() {
    const auto outcome = resultOutcome(data);
    banner->setVisible(outcome == "incomplete" || outcome == "error");
    if (banner->isHidden())
        return;
    const bool failed = outcome == "error";
    if (banner->property("tone").toString() != (failed ? "bad" : "warn")) {
        banner->setProperty("tone", failed ? "bad" : "warn");
        repolishAll(banner);
    }
    const ChromeColors c(static_cast<QWidget *>(window)->palette());
    bannerIcon->setPixmap(lineIcon("warning", failed ? c.bad : c.candidateText).pixmap(QSize(20, 20)));
    bannerTitle->setText(failed ? "NWChemのエラーを検出しました" : "NWChemの正常終了の記録がありません");
    bannerText->setText(
        failed ? data.status + "。詳細は「概要」のメッセージとログで確認してください。"
               : QString("実行中・中断・途中までのログの可能性があります。%1実行中なら「再読込」で"
                         "更新します。")
                     .arg(data.properties.value("optimization") == "converged" ? "最適化は収束しています。"
                                                                               : ""));
}
void ResultsPanel::showContext() {
    const int i = frame->value() - 1, mode = modes->currentRow();
    int page = 0;
    QString title, detail;
    if (modeDisplayed && mode >= 0 && mode < data.modes.size()) {
        page = 2;
        const auto &m = data.modes[mode];
        title = QString("モード %1 · %2 cm⁻¹").arg(mode + 1).arg(fixed(m.frequency, 1));
        detail = animation.isActive() ? QString("再生中 · 振幅 %1 Å").arg(fixed(amplitude->value(), 2))
                                      : QString("基準構造");
        if (rigidMotion.value(mode))
            detail += " · 並進・回転";
        else if (m.frequency < -10)
            detail += " · 虚振動";
        modeLabel->setText(title);
    } else if (data.format == "MOLDEN" && data.orbitalBasis && !data.orbitals.isEmpty() &&
               (data.frames.size() <= 1 || i == data.orbitalFrame)) {
        page = 1;
        if (data.selectedOrbital >= 0 && i == data.orbitalFrame) {
            const auto &o = data.orbitals[data.selectedOrbital];
            title = QString("%1 · %2 %3番")
                        .arg(frontierName(data.orbitals, data.selectedOrbital), o.spin,
                             orbitals->item(data.selectedOrbital, 0)->text());
            detail = QString("%1 eV · 等値 ±%2").arg(fixed(o.energy * HartreeToEv, 3), real(data.isovalue));
        } else {
            title = "分子軌道";
            detail = data.orbitalError.isEmpty() ? QString("軌道を選ぶと等値面を表示します")
                                                 : data.orbitalError.section('\n', 0, 0).left(80);
        }
    } else if (data.format == "CUBE" && !data.frames.isEmpty()) {
        title = data.datasets.isEmpty() ? QString("CUBE")
                                        : QString("CUBE · データ %1").arg(data.datasets.value(data.dataset));
        detail = QString("等値 ±%1 · 格子 %2 × %3 × %4")
                     .arg(real(data.isovalue))
                     .arg(data.grid[0])
                     .arg(data.grid[1])
                     .arg(data.grid[2]);
    } else if (i >= 0 && i < data.frames.size()) {
        title = data.frames.size() > 1 ? QString("構造 %1 / %2%3")
                                             .arg(i + 1)
                                             .arg(data.frames.size())
                                             .arg(i == data.frames.size() - 1 ? "（最後）" : "")
                                       : QString("構造");
        detail = data.frames[i].label;
    }
    contextChip->setVisible(!title.isEmpty());
    if (contextTitle->text() != title)
        contextTitle->setText(title);
    if (contextDetail->text() != detail)
        contextDetail->setText(detail);
    contextDetail->setVisible(!detail.isEmpty());
    if (dock->currentIndex() != page)
        dock->setCurrentIndex(page);
}
QString ResultsPanel::briefSummary() const {
    QStringList parts;
    if (data.properties.contains("optimization"))
        parts << (data.properties.value("optimization") == "converged"
                      ? QString("最適化: 収束")
                      : QString("最適化: 収束メッセージなし"));
    if (const auto final = data.properties["final_energy"].toObject(); !final.isEmpty())
        parts
            << QString("E = %1 Eh（%2）").arg(real(final["hartree"].toDouble()), final["method"].toString());
    if (!data.modes.isEmpty()) {
        const auto rigid = resultRigidModes(data);
        int imaginary = 0;
        for (int i = 0; i < data.modes.size(); ++i)
            imaginary += !rigid.value(i) && data.modes[i].frequency < -10;
        parts << QString("虚振動 %1個").arg(imaginary);
    }
    if (!data.excitations.isEmpty())
        parts << QString("励起状態 %1件").arg(data.excitations.size());
    return parts.join(" · ");
}
void ResultsPanel::showSpectrum() {
    const bool nm = spectrumAxis->currentData() == "nm";
    const double fwhm = broadening->value(), sigma = fwhm / (2 * std::sqrt(2 * std::log(2.)));
    QVector<QPointF> sticks, curve;
    double lo = INFINITY, hi = -INFINITY;
    for (const auto &x : data.excitations)
        if (x.ev > 0) {
            lo = std::min(lo, x.ev);
            hi = std::max(hi, x.ev);
            if (x.oscillator > 0)
                sticks << QPointF(nm ? EvNanometer / x.ev : x.ev, x.oscillator);
        }
    if (!(hi > 0) || sticks.isEmpty()) {
        spectrum->clear();
        return;
    }
    lo = std::max(.5, lo - 3 * fwhm);
    hi += 3 * fwhm;
    for (int k = 0; k <= 400; ++k) {
        const double e = lo + (hi - lo) * k / 400;
        double y = 0;
        for (const auto &x : data.excitations)
            y += x.oscillator * std::exp(-.5 * std::pow((e - x.ev) / sigma, 2));
        curve << QPointF(nm ? EvNanometer / e : e, y);
    }
    spectrum->setChart("吸収スペクトル（表示用）", nm ? "波長 / nm" : "励起エネルギー / eV", "振動子強度",
                       {{"広がり", curve, QColor("#2f6db5")}, {"各状態", sticks, QColor("#c0392b"), true}});
}
void ResultsPanel::showEnergies() {
    const auto unit = energyUnit->currentData().toString();
    const double factor = unit == "kcal"         ? HartreeToKcal
                          : unit == "kj"         ? 2625.499639
                          : unit == "ev"         ? HartreeToEv
                          : unit == "wavenumber" ? 219474.6313632
                                                 : 1.;
    const QString label = energyUnit->currentText().section(':', 1).trimmed();
    energy->setHorizontalHeaderItem(2, new QTableWidgetItem("ΔE / " + label));
    const int reference = referenceEnergy >= 0 && referenceEnergy < data.energies.size()
                              ? referenceEnergy
                              : int(data.energies.size()) - 1;
    const QSignalBlocker guard(energy);
    energy->setRowCount(data.energies.size());
    for (int i = 0; i < data.energies.size(); ++i) {
        const auto &e = data.energies[i];
        row(energy, i,
            {e.method, fixed(e.hartree, 10),
             fixed((e.hartree - data.energies[reference].hartree) * factor, unit == "hartree" ? 8 : 3),
             e.line ? QString::number(e.line) : "—"});
        if (i == reference)
            for (int c = 0; c < 4; ++c)
                energy->item(i, c)->setFont([&] {
                    auto f = energy->item(i, c)->font();
                    f.setBold(true);
                    return f;
                }());
    }
    energyNote->setText(
        data.energies.isEmpty()
            ? QString()
            : QString("ΔEは %1 行目（太字）を基準にした差です。手法の異なる行どうしの差には意味がありま"
                      "せん。エネルギーは出力順で、最適化ステップとの1対1対応ではありません。"
                      "1 Eh = 627.509 kcal/mol = 2625.50 kJ/mol = 27.2114 eV。")
                  .arg(reference + 1));
}
void ResultsPanel::showPath() {
    const QSignalBlocker guard(path);
    path->setRowCount(0);
    const auto scan = data.properties.value("scan").toArray();
    const auto irc = data.properties.value("irc").toObject();
    struct Point {
        QString label, coordinate;
        double x, energy;
        int frame;
    };
    QVector<Point> points;
    QString xLabel;
    double reference = INFINITY;
    if (!scan.isEmpty()) {
        const auto coordinate = scan[0].toObject()["coordinate"].toString();
        xLabel = coordinate.startsWith("bond") ? "距離 / Å" : "角度 / °";
        for (const auto &value : scan) {
            const auto p = value.toObject();
            points << Point{QString::number(p["point"].toInt()), coordinateText(p["coordinate"].toString()),
                            p["value"].toDouble(), p["energy"].toDouble(), p["frame"].toInt(-1)};
            reference = std::min(reference, p["energy"].toDouble());
        }
    } else if (!irc.isEmpty()) {
        xLabel = "反応座標 s / amu½·Bohr";
        QVector<Point> backward, forward;
        for (const auto &value : irc["points"].toArray()) {
            const auto p = value.toObject();
            const bool ahead = p["direction"] == "forward";
            (ahead ? forward : backward)
                << Point{QString("%1 %2").arg(ahead ? "前進" : "後退").arg(p["point"].toInt()),
                         ahead ? "前進" : "後退", (ahead ? 1 : -1) * p["s"].toDouble(),
                         p["energy"].toDouble(), p["frame"].toInt(-1)};
        }
        std::reverse(backward.begin(), backward.end());
        points = backward;
        if (irc.contains("ts_energy")) {
            reference = irc["ts_energy"].toDouble();
            points << Point{"TS", "遷移状態", 0, reference, irc["ts_frame"].toInt(-1)};
        }
        points += forward;
        if (!std::isfinite(reference) && !points.isEmpty())
            reference = points.first().energy;
    }
    path->setRowCount(points.size());
    QVector<QPointF> curve;
    for (int i = 0; i < points.size(); ++i) {
        const auto &p = points[i];
        const double relative = (p.energy - reference) * HartreeToKcal;
        row(path, i, {p.label, p.coordinate, fixed(p.x, 4), fixed(p.energy, 8), fixed(relative, 3)});
        path->item(i, 0)->setData(Qt::UserRole, p.frame);
        curve << QPointF(p.x, relative);
    }
    if (curve.isEmpty())
        pathChart->clear();
    else
        pathChart->setChart(scan.isEmpty() ? "IRC（遷移状態基準）" : "座標スキャン（最小値基準）", xLabel,
                            "ΔE / kcal mol⁻¹", {{"エネルギー", curve, QColor("#2f6db5")}});
}
void ResultsPanel::showProperties() {
    const auto mulliken = data.properties.value("mulliken").toArray(),
               esp = data.properties.value("esp").toArray(), resp = data.properties.value("resp").toArray(),
               shielding = data.properties.value("shielding").toArray();
    const int count = std::max({mulliken.size(), esp.size(), resp.size(), shielding.size()});
    const Document *molecule = data.frames.isEmpty() ? nullptr : &data.frames.last().molecule;
    atomProperties->setRowCount(count);
    for (int i = 0; i < count; ++i) {
        const auto nmr = i < shielding.size() ? shielding[i].toObject() : QJsonObject();
        const QString symbol =
            molecule && i < molecule->atoms.size() ? molecule->atoms[i].element : nmr["element"].toString();
        const auto charge = [&](const QJsonArray &list) {
            return i < list.size() ? fixed(list[i].toDouble(), 4) : QString("—");
        };
        row(atomProperties, i,
            {QString::number(i + 1), symbol, charge(mulliken), charge(esp), charge(resp),
             nmr.contains("isotropic") ? fixed(nmr["isotropic"].toDouble(), 3) : "—",
             nmr.contains("anisotropy") ? fixed(nmr["anisotropy"].toDouble(), 3) : "—"});
    }
    auto text = resultPropertyLines(data);
    if (data.properties.contains("dipole")) {
        const auto d = data.properties.value("dipole").toObject();
        text += QString("双極子モーメント: %1 D（x %2 / y %3 / z %4 a.u.）\n")
                    .arg(fixed(d["debye"].toDouble(), 4), real(d["x"].toDouble()), real(d["y"].toDouble()),
                         real(d["z"].toDouble()));
    }
    if (data.properties.contains("polarizability")) {
        const auto t = data.properties.value("polarizability").toObject()["tensor"].toArray();
        QStringList rows;
        for (int r = 0; r < 3; ++r)
            rows << QString("%1 %2 %3")
                        .arg(fixed(t[3 * r].toDouble(), 4), fixed(t[3 * r + 1].toDouble(), 4),
                             fixed(t[3 * r + 2].toDouble(), 4));
        text += "分極率テンソル / a.u.: [" + rows.join(" / ") + "]\n";
    }
    if (!shielding.isEmpty())
        text += "NMRは絶対遮蔽定数です。化学シフトは同じ条件で計算した基準物質（TMS等）の値との差です。\n";
    if (text.isEmpty())
        text = "このログには物性の結果がありません。「2 計算条件」の「物性」・溶媒・BSSE補正で計算できます。";
    propertySummary->setText(text.trimmed());
}
void ResultsPanel::showIrSpectrum() {
    QVector<QPointF> sticks, curve;
    irModes.clear();
    double top = 0;
    for (int i = 0; i < data.modes.size(); ++i) {
        const auto &m = data.modes[i];
        if (rigidMotion.value(i) || m.frequency <= 0 || !std::isfinite(m.intensity))
            continue;
        sticks << QPointF(m.frequency, m.intensity);
        irModes << i;
        top = std::max(top, m.frequency);
    }
    if (sticks.isEmpty()) {
        irSpectrum->clear();
        return;
    }
    // Sample at a quarter of the half width (at most 20000 points) so narrow peaks keep their height.
    const double gamma = irWidth->value() / 2, end = top + 200;
    const int samples = std::clamp(int(std::ceil(end / (gamma / 2))), 1000, 20000);
    for (int k = 0; k <= samples; ++k) {
        const double x = end * k / samples;
        double y = 0;
        for (const auto &s : sticks)
            y += s.y() * gamma * gamma / ((x - s.x()) * (x - s.x()) + gamma * gamma);
        curve << QPointF(x, y);
    }
    const ChromeColors c(palette());
    irSpectrum->setChart("IRスペクトル（表示用）", "波数 / cm⁻¹", "IR強度 / km mol⁻¹",
                         {{"広がり", curve, c.muted}, {"各モード", sticks, c.text, true}});
    irSpectrum->setSelectedStick(1, irModes.indexOf(modes->currentRow()));
}
void ResultsPanel::showOptimization() {
    const ChromeColors c(static_cast<QWidget *>(window)->palette());
    const QColor orange =
        darkPalette(static_cast<QWidget *>(window)->palette()) ? QColor("#f0883e") : QColor("#c2570c");
    const auto thresholds = data.properties.value("thresholds").toObject();
    // The four criteria of the last step against their thresholds.
    auto *box = static_cast<QVBoxLayout *>(criteria->layout());
    while (auto *item = box->takeAt(0)) {
        if (item->layout())
            while (auto *inner = item->layout()->takeAt(0)) {
                delete inner->widget();
                delete inner;
            }
        delete item->widget();
        delete item;
    }
    criteria->setVisible(!data.steps.isEmpty());
    if (!data.steps.isEmpty()) {
        const auto &last = data.steps.last();
        auto *title = new QLabel(QString("収束判定（最終ステップ %1）").arg(last.step));
        auto font = title->font();
        font.setBold(true);
        title->setFont(font);
        box->addWidget(title);
        auto *row = new QHBoxLayout;
        row->setSpacing(6);
        for (const auto &[key, name, value] : {std::tuple{"gmax", "Gmax", last.gmax},
                                               {"grms", "Grms", last.grms},
                                               {"xrms", "Xrms", last.xrms},
                                               {"xmax", "Xmax", last.xmax}}) {
            const bool judged = thresholds.contains(key);
            const bool met = judged && value <= thresholds[key].toDouble();
            // Five decimals, as NWChem prints them.
            auto *chip = summaryTile(judged ? QString("%1 %2").arg(name, met ? "✓" : "✗") : QString(name),
                                     fixed(value, 5),
                                     judged ? "閾値 " + fixed(thresholds[key].toDouble(), 5) : QString(),
                                     !judged ? "neutral"
                                     : met   ? "good"
                                             : "warn",
                                     false);
            chip->setObjectName(QString("resultsCriterion") + name);
            row->addWidget(chip, 1);
        }
        box->addLayout(row);
    }
    steps->setRowCount(data.steps.size());
    QVector<QPointF> relative, gmax, grms;
    for (int i = 0; i < data.steps.size(); ++i) {
        const auto &t = data.steps[i];
        row(steps, i,
            {QString::number(t.step), fixed(t.energy, 8), real(t.deltaE), fixed(t.gmax, 5), fixed(t.grms, 5),
             fixed(t.xrms, 5), fixed(t.xmax, 5), real(t.seconds)});
        // Values within their threshold in the color of a met criterion (no step yet at step 0).
        const std::tuple<int, const char *, double> columns[]{
            {3, "gmax", t.gmax}, {4, "grms", t.grms}, {5, "xrms", t.xrms}, {6, "xmax", t.xmax}};
        for (const auto &[column, key, value] : columns)
            if (thresholds.contains(key) && value <= thresholds[key].toDouble() && (column < 5 || t.step > 0))
                steps->item(i, column)->setForeground(c.good);
        relative << QPointF(t.step, (t.energy - data.steps.last().energy) * HartreeToKcal);
        gmax << QPointF(t.step, t.gmax);
        grms << QPointF(t.step, t.grms);
    }
    if (data.steps.isEmpty()) {
        energyChart->clear();
        gradientChart->clear();
        return;
    }
    energyChart->setChart("エネルギー（最終ステップ基準）", "ステップ", "ΔE / kcal mol⁻¹",
                          {{"エネルギー", relative, c.accent}});
    QVector<QPair<QString, double>> levels;
    if (thresholds.contains("gmax"))
        levels.append({"Gmax閾値", thresholds["gmax"].toDouble()});
    if (thresholds.contains("grms"))
        levels.append({"Grms閾値", thresholds["grms"].toDouble()});
    gradientChart->setChart("勾配", "ステップ", "Hartree / Bohr",
                            {{"Gmax", gmax, c.accent}, {"Grms", grms, orange}}, true, levels);
}
void ResultsPanel::showArrows() {
    const int n = modes->currentRow();
    // Only over the reference structure the displacements belong to.
    if (!arrows->isChecked() || !modeDisplayed || n < 0 || n >= data.modes.size() ||
        data.modes[n].displacement.size() != data.vibrationGeometry.atoms.size() || animation.isActive()) {
        view->setArrows({});
        return;
    }
    // Longest arrow: three times the animation amplitude, so small modes stay visible.
    double longest = 0;
    for (const auto &v : data.modes[n].displacement)
        longest = std::max(longest, v.norm());
    QVector<std::pair<Vec, Vec>> list;
    for (int i = 0; i < data.vibrationGeometry.atoms.size() && longest > 0; ++i)
        list.append({data.vibrationGeometry.atoms[i].position,
                     data.modes[n].displacement.value(i) / longest * 3 * amplitude->value()});
    view->setArrows(list);
}
void ResultsPanel::exportMode(bool images) {
    const int n = modes->currentRow();
    if (loading() || n < 0 || n >= data.modes.size() || data.modes[n].displacement.isEmpty())
        fail("振動モードを選択してください。");
    stopAnimation();
    constexpr int steps = 24; // One period.
    const auto name = QString("mode%1_%2cm").arg(n + 1).arg(qRound(data.modes[n].frequency));
    if (images) {
        const auto directory = QFileDialog::getExistingDirectory(this, "連番画像の保存先", folder);
        if (directory.isEmpty())
            return;
        window->checkMcpPath(directory);
        modeDisplayed = true;
        view->setSurface({});
        for (int k = 0; k < steps; ++k) {
            setViewDocument(resultModeGeometry(data, n, 2 * M_PI * k / steps, amplitude->value()));
            view->renderNow();
            const auto file =
                QDir(directory).filePath(QString("%1_%2.png").arg(name).arg(k, 2, 10, QChar('0')));
            if (!view->grab().save(file))
                fail("画像を保存できません: " + file);
        }
        setViewDocument(data.vibrationGeometry);
        window->statusBar()->showMessage(QString("%1枚の画像を保存しました: %2").arg(steps).arg(directory),
                                         8000);
        return;
    }
    auto file = QFileDialog::getSaveFileName(this, "振動をXYZで保存", name + ".xyz", "XYZ (*.xyz)");
    if (file.isEmpty())
        return;
    if (!file.endsWith(".xyz", Qt::CaseInsensitive))
        file += ".xyz";
    window->checkMcpPath(file);
    QString text;
    for (int k = 0; k < steps; ++k) {
        auto frameText = resultModeGeometry(data, n, 2 * M_PI * k / steps, amplitude->value()).xyz();
        // Keep the atom count line and name the frame by its phase.
        auto lines = frameText.split('\n');
        if (lines.size() > 1)
            lines[1] = QString("%1 phase %2/%3").arg(name).arg(k).arg(steps);
        text += lines.join('\n');
        if (!text.endsWith('\n'))
            text += '\n';
    }
    writeFile(file, text.toUtf8());
}
void ResultsPanel::setViewDocument(const Document &d) {
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
    QString text =
        pinned.isEmpty()
            ? QString("原子をクリックすると距離・角度・二面角を測定します（2〜4原子）。")
            : QString("画面に表示中の計測: %1件。原子をクリックすると続けて測定します。").arg(pinned.size());
    if (!picked.isEmpty()) {
        QStringList labels;
        for (int n : picked)
            labels << (n < d.atoms.size() ? d.atoms[n].element : QString("?")) + QString::number(n + 1);
        text = "選択: " + labels.join(" → ");
        if (picked.size() >= 2 && !ids(picked).isEmpty())
            try {
                const double value = measure(d, ids(picked));
                text += picked.size() == 2 ? QString("　距離 %1 Å").arg(value, 0, 'f', 4)
                                           : QString("　%1 %2°")
                                                 .arg(picked.size() == 3 ? "角度" : "二面角")
                                                 .arg(value, 0, 'f', 2);
            } catch (const std::exception &e) {
                text += "　" + QString::fromUtf8(e.what());
            }
    }
    measurementLabel->setText(text);
    measureCard->setVisible(!picked.isEmpty() || !pinned.isEmpty());
}
void ResultsPanel::pickAtom(const QString &id) {
    const auto &d = view->documentRef();
    if (!d.contains(id))
        return;
    const int n = d.index(id);
    if (picked.contains(n))
        picked.removeAll(n);
    else {
        if (picked.size() == 4)
            picked.clear();
        picked << n;
    }
    setViewDocument(Document(d));
}
void ResultsPanel::showFrame() {
    modeDisplayed = false;
    view->setArrows({});
    const int i = frame->value() - 1;
    setViewDocument(i >= 0 && i < data.frames.size() ? data.frames[i].molecule : Document());
    view->setSurface(data.format != "MOLDEN" || i == data.orbitalFrame ? data.surface
                                                                       : QVector<SurfaceTriangle>{});
    timeline->setCurrent(i);
    frameEnergy->setText(std::isfinite(frameEnergies.value(i, NAN))
                             ? QString("ΔE %1 kcal/mol").arg(fixed(frameEnergies[i], 2))
                             : QString("ΔE —"));
    showContext();
}
void ResultsPanel::showMode() {
    modeDisplayed = true;
    const int n = modes->currentRow();
    if (n < 0 || n >= data.modes.size() || data.modes[n].displacement.isEmpty()) {
        stopAnimation();
        return;
    }
    view->setSurface({});
    setViewDocument(resultModeGeometry(data, n, phase, amplitude->value()));
    showContext();
}
const Document &ResultsPanel::displayedGeometry() const {
    if (loading())
        fail("読込完了後に操作してください。");
    const int i = frame->value() - 1;
    if (i < 0 || i >= data.frames.size())
        fail("表示する構造がありません。");
    // During a vibration the reference structure, not a displaced snapshot.
    return modeDisplayed && modes->currentRow() >= 0 ? data.vibrationGeometry : data.frames[i].molecule;
}
void ResultsPanel::saveGeometry(const QString &path) const {
    writeFile(path, displayedGeometry().xyz().toUtf8());
}
void ResultsPanel::applyStructure() {
    const auto result = displayedGeometry();
    std::pair<double, double> moved;
    window->edit("計算結果の構造を適用", [&](Document &d) { moved = applyResultGeometry(d, result); }, true);
    if (!window->hasCandidate())
        fail("編集中の分子と結果の座標は同じです。");
    window->workspaceTabs->setCurrentIndex(0);
    window->statusBar()->showMessage(
        QString("結果の構造を候補にしました（RMSD %1 Å、最大変位 %2 Å）。「変更を確定」"
                "で適用します。")
            .arg(moved.first, 0, 'f', 4)
            .arg(moved.second, 0, 'f', 4),
        15000);
}
} // namespace mf
