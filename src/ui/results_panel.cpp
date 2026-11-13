// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/results_panel.h"
#include "editing/coordinates.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "model/geometry.h"
#include "model/settings.h"
#include "model/units.h"
#include "nwchem/result_files.h"
#include "nwchem/run_record.h"
#include "results/molden_orbitals.h"
#include "results/result_properties.h"
#include "results/spectrum.h"
#include "ui/chrome_layouts.h"
#include "ui/cube_combine_dialog.h"
#include "ui/editor_chrome.h"
#include "ui/editor_context.h"
#include "ui/figure_export.h"
#include "ui/line_icons.h"
#include "ui/molecule_view.h"
#include "ui/property_geometry_view.h"
#include "ui/result_charts.h"
#include "ui/result_detail.h"
#include "ui/result_energies_view.h"
#include "ui/result_excitations_view.h"
#include "ui/result_measurement.h"
#include "ui/result_optimization_view.h"
#include "ui/result_path_view.h"
#include "ui/result_properties_view.h"
#include "ui/settings_widgets.h"
#include "ui/spectroscopy_view.h"
#include "ui/theme.h"
#include "ui/view_controls.h"
#include <QScopeGuard>
#include <QtWidgets>
#include <algorithm>
#include <cmath>

namespace mf {
using namespace result_detail;
namespace {
// Where an animation goes: a file for GIF and MP4, a folder for numbered PNG files; empty when cancelled.
QString animationTarget(QWidget *parent, const AnimationOptions &options, const QString &name,
                        const QString &folder) {
    if (options.format == "png")
        return QFileDialog::getExistingDirectory(parent, tr("連番画像の保存先"), folder);
    const auto suffix = options.format == "mp4" ? QString(".mp4") : QString(".gif");
    auto path = QFileDialog::getSaveFileName(parent, tr("動画を保存"), QDir(folder).filePath(name + suffix),
                                             options.format == "mp4" ? "MP4 (*.mp4)" : "GIF (*.gif)");
    if (!path.isEmpty() && !path.endsWith(suffix, Qt::CaseInsensitive))
        path += suffix;
    return path;
}
QString orbitalKey(int orbital, double iso, const QString &normalization, int divisions) {
    return QString::fromUtf8(
        QJsonDocument(QJsonArray{orbital, iso, normalization, divisions}).toJson(QJsonDocument::Compact));
}
QString formatName(const QString &format) {
    return format == "NWChem" ? tr("NWChem 出力") : format;
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
} // namespace

ResultsPanel::ResultsPanel(EditorContext &editorServices, UiServices &uiServices,
                           WorkspaceActions &workspaceActions, QWidget *parent)
    : QWidget(parent), editorContext(editorServices), uiContext(uiServices), workspace(workspaceActions) {
    setObjectName("resultsTab");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(buildBar());
    auto *body = new QWidget;
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(10, 10, 10, 10);
    auto *split = new QSplitter;
    split->setChildrenCollapsible(false);
    split->setHandleWidth(10);
    bodyLayout->addWidget(split);
    layout->addWidget(body, 1);
    split->addWidget(buildViewCard());
    split->addWidget(buildDetails());
    buildTabs();
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setSizes({780, 540});
    connectActions();
    display(true);
    updateActions();
}
QWidget *ResultsPanel::buildBar() {
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
    files->setAccessibleName(tr("結果ファイル"));
    files->setToolTip(tr("ジョブフォルダーの結果ファイル。選ぶとすぐに読み込みます。"));
    files->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    files->setMinimumContentsLength(20);
    formatChip = new QLabel;
    formatChip->setObjectName("resultsFormat");
    outcomeChip = new QLabel;
    outcomeChip->setObjectName("resultsOutcome");
    emptyHint = new QLabel(tr("結果ファイルまたはジョブフォルダーを開いてください。"));
    emptyHint->setObjectName("resultsEmptyHint");
    for (QWidget *w : {static_cast<QWidget *>(jobIcon), static_cast<QWidget *>(jobLabel),
                       static_cast<QWidget *>(files), static_cast<QWidget *>(formatChip),
                       static_cast<QWidget *>(outcomeChip), static_cast<QWidget *>(emptyHint)})
        bar->addWidget(w);
    bar->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum));
    openButton = new QToolButton;
    openButton->setObjectName("resultsOpen");
    openButton->setText(tr("開く…"));
    openButton->setIcon(lineIcon("folder"));
    openButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    openButton->setPopupMode(QToolButton::MenuButtonPopup);
    openButton->setToolTip(
        tr("結果ファイル（NWChem出力・XYZ・CUBE・MOLDEN）を開きます。▾ からジョブフォルダーも"
           "開けます。"));
    auto *openMenu = new QMenu(openButton);
    auto *openFileAction = openMenu->addAction(lineIcon("document"), tr("結果ファイルを開く…"));
    auto *openDirectoryAction = openMenu->addAction(lineIcon("folder"), tr("ジョブフォルダーを開く…"));
    openDirectoryAction->setObjectName("resultsDirectory");
    openButton->setMenu(openMenu);
    reload = new QPushButton(lineIcon("refresh"), QString());
    reload->setObjectName("resultsReload");
    reload->setToolTip(tr("再読込（実行途中のログを更新）"));
    reload->setAccessibleName(tr("再読込"));
    cancel = new QPushButton(lineIcon("close"), tr("中止"));
    cancel->setObjectName("resultsCancel");
    cancel->setToolTip(tr("読込を中止"));
    auto *divider = new QFrame;
    divider->setObjectName("resultsBarDivider");
    divider->setFrameShape(QFrame::VLine);
    divider->setFrameShadow(QFrame::Sunken);
    divider->setFixedHeight(22);
    compareGeometry = new QPushButton(lineIcon("compare"), tr("分子と比較"));
    compareGeometry->setObjectName("resultsCompare");
    compareGeometry->setToolTip(
        tr("表示中の構造と「1 分子」の分子を重ね合わせ、RMSDを表示します（同じ原子・同じ順番）。"));
    applyGeometry = new QPushButton(lineIcon("apply"), tr("分子へ候補として適用"));
    applyGeometry->setObjectName("resultsApplyStructure");
    applyGeometry->setToolTip(
        tr("表示中の構造の座標を「1 分子」の分子へ候補として適用します。原子の数と順番が同"
           "じ計算結果に使えます。結合・設定は変わりません。"));
    recalculate = new QPushButton(lineIcon("refresh"), tr("再計算を準備"));
    recalculate->setObjectName("resultsPrepareRecalculation");
    recalculate->setToolTip(tr("保存済みの計算条件を読み込み、表示中の構造を候補にします。確定後、新しいジョ"
                               "ブとして実行できます。"));
    connect(recalculate, &QPushButton::clicked, this,
            [this] { uiContext.safe([this] { prepareRecalculation(); }); });
    exportButton = new QToolButton;
    exportButton->setObjectName("resultsExport");
    exportButton->setText(tr("書き出し"));
    exportButton->setIcon(lineIcon("export"));
    exportButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    exportButton->setPopupMode(QToolButton::InstantPopup);
    auto *exportMenu = new QMenu(exportButton);
    saveXyz = exportMenu->addAction(lineIcon("export"), tr("表示中の構造をXYZで書き出す…"));
    saveXyz->setObjectName("resultsSaveXyz");
    savePng = exportMenu->addAction(lineIcon("image"), tr("3D表示を画像で書き出す…"));
    savePng->setObjectName("resultsSavePng");
    saveFrames = exportMenu->addAction(lineIcon("image"), tr("構造列を動画・連番画像で書き出す…"));
    saveFrames->setObjectName("resultsSaveFrames");
    saveFrames->setToolTip(
        tr("座標スキャン・IRC・NEBの点を経路の順に、ほかは構造列のすべてを、GIF・MP4・PNG連番で"
           "書き出します。"));
    exportButton->setMenu(exportMenu);
    for (QWidget *w : {static_cast<QWidget *>(openButton), static_cast<QWidget *>(reload),
                       static_cast<QWidget *>(cancel), static_cast<QWidget *>(divider),
                       static_cast<QWidget *>(compareGeometry), static_cast<QWidget *>(applyGeometry),
                       static_cast<QWidget *>(recalculate), static_cast<QWidget *>(exportButton)})
        bar->addWidget(w);
    // Narrow, the actions keep their icons and the bar its one row.
    barFit = new ToolbarFit(resultBar, [this](bool compact) {
        for (QAbstractButton *b :
             {static_cast<QAbstractButton *>(compareGeometry), static_cast<QAbstractButton *>(applyGeometry),
              static_cast<QAbstractButton *>(recalculate), static_cast<QAbstractButton *>(exportButton)})
            setIconOnly(b, compact);
    });
    const auto openResultFile = [this] {
        const auto p = QFileDialog::getOpenFileName(this, tr("結果ファイルを開く"), folder,
                                                    tr("NWChem・可視化 (*.out *.log *.xyz *.cube *.cub "
                                                       "*.molden *.mld *.molde *.normal);;すべて (*)"));
        if (!p.isEmpty())
            openFile(p);
    };
    connect(openButton, &QToolButton::clicked, this, openResultFile);
    connect(openFileAction, &QAction::triggered, this, openResultFile);
    connect(openDirectoryAction, &QAction::triggered, this, [this] {
        const auto p = QFileDialog::getExistingDirectory(this, tr("ジョブフォルダーを開く"), folder);
        if (!p.isEmpty())
            openDirectory(p);
    });
    return resultBar;
}
QWidget *ResultsPanel::buildViewCard() {
    // The 3D view, with what it shows, the view controls and the measurement in its corners.
    auto *viewCard = new QFrame;
    viewCard->setObjectName("resultsViewCard");
    auto *card = new QVBoxLayout(viewCard);
    card->setContentsMargins(1, 1, 1, 1);
    card->setSpacing(0);
    view = new MoleculeView(rendererChoice(uiContext.preference(Preference::viewRenderer)));
    view->setObjectName("resultsView");
    view->setAcceptDrops(false);
    view->setAngles(-25, 25, 0);
    view->setEmptyMessage(
        tr("NWChem出力・XYZ・CUBE・MOLDENを開いて\n構造・軌道の等値面・振動を確認できます。"));
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
    labelMenu->addSection(tr("ラベル"));
    auto *labelGroup = new QActionGroup(labelMenu);
    auto savedLabels = uiContext.preference(Preference::resultLabels, "all").toString();
    if (!QStringList{"all", "heavy", "selection", "none"}.contains(savedLabels))
        savedLabels = "all";
    for (const auto &[label, mode] : {std::pair{QT_TR_NOOP("すべて"), "all"},
                                      {QT_TR_NOOP("水素以外"), "heavy"},
                                      {QT_TR_NOOP("選択した原子"), "selection"},
                                      {QT_TR_NOOP("なし"), "none"}}) {
        auto *a = labelMenu->addAction(tr(label));
        a->setObjectName(QString("resultsLabel_") + mode);
        a->setCheckable(true);
        a->setData(mode);
        a->setChecked(savedLabels == mode);
        labelGroup->addAction(a);
    }
    viewControlsFrame->menuButton()->setMenu(labelMenu);
    viewControlsFrame->menuButton()->setToolTip(tr("元素記号・番号を表示する原子を選びます。"));
    view->setLabelMode(savedLabels);
    connect(labelGroup, &QActionGroup::triggered, this, [this](QAction *a) {
        view->setLabelMode(a->data().toString());
        uiContext.setPreference(Preference::resultLabels, a->data().toString());
    });
    viewArea->addOverlay(viewControlsFrame, Qt::AlignTop | Qt::AlignRight);
    // Distance, angle or dihedral of the atoms clicked in order, and the ones kept on the view.
    measurement = new ResultMeasurement(view, uiContext);
    viewControls << measurement->controls();
    viewArea->addOverlay(measurement, Qt::AlignBottom | Qt::AlignLeft);
    card->addWidget(buildDock());
    return viewCard;
}
QWidget *ResultsPanel::buildDock() {
    // Under the view: the controls for what it shows. Pages of one height, so the view keeps its size.
    dockFrame = new QFrame;
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
    firstFrame = iconButton("first", "resultsFirstFrame", tr("最初の構造"));
    playFrames = iconButton("play", "resultsPlayFrames", tr("構造列を再生"));
    playFrames->setProperty("primary", true);
    lastFrame = iconButton("last", "resultsLastFrame", tr("最後の構造"));
    auto *frameLabel = new QLabel(tr("構造"));
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
    const QStringList navigationTips{tr("最初の軌道へ"), tr("10軌道戻る"), tr("1軌道戻る"),
                                     tr("1軌道進む"),    tr("10軌道進む"), tr("最後の軌道へ")};
    orbitalPosition = new QLabel;
    orbitalPosition->setObjectName("resultsOrbitalPosition");
    orbitalPosition->setAlignment(Qt::AlignCenter);
    // A fixed width, so that the buttons after it stay put while the number changes.
    orbitalPosition->setFixedWidth(orbitalPosition->fontMetrics().horizontalAdvance(
                                       tr("%1 %2番").arg(QString("Alpha"), QString("99999"))) +
                                   12);
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
    homo->setToolTip(tr("選択中のスピンのHOMOを表示します。"));
    lumo->setToolTip(tr("選択中のスピンのLUMOを表示します。"));
    orbitalRow->addWidget(
        group({orbitalNavigation[0], orbitalNavigation[1], orbitalNavigation[2], orbitalPosition,
               orbitalNavigation[3], orbitalNavigation[4], orbitalNavigation[5]}));
    orbitalRow->addWidget(group({homo, lumo}));
    dock->addWidget(orbitalPage);
    auto *modePage = new QWidget;
    auto *modeRow = new FlowLayout(modePage, 10, 4);
    modeRow->setContentsMargins(0, 0, 0, 0);
    playMode = iconButton("play", "resultsPlayMode", tr("選択したモードを再生"));
    playMode->setProperty("primary", true);
    modeLabel = new QLabel;
    modeLabel->setObjectName("resultsModeLabel");
    auto *amplitudeLabel = new QLabel(tr("振幅"));
    amplitude = new QDoubleSpinBox;
    amplitude->setObjectName("resultsAmplitude");
    amplitude->setRange(.01, 2);
    amplitude->setSingleStep(.05);
    amplitude->setValue(.3);
    amplitude->setSuffix(" Å");
    amplitude->setToolTip(tr("表示用に規格化した最大変位です。"));
    amplitudeLabel->setBuddy(amplitude);
    arrows = new QCheckBox(tr("矢印"));
    arrows->setObjectName("resultsModeArrows");
    arrows->setToolTip(tr("選択したモードの変位を基準構造に矢印で重ねます（最大の矢印は振幅の3倍）。"));
    modeExport = new QToolButton;
    modeExport->setObjectName("resultsModeExport");
    modeExport->setText(tr("書き出し"));
    modeExport->setIcon(lineIcon("export"));
    modeExport->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    modeExport->setPopupMode(QToolButton::InstantPopup);
    auto *modeMenu = new QMenu(modeExport);
    saveModeImages = modeMenu->addAction(lineIcon("image"), tr("振動を動画・連番画像で書き出す…"));
    saveModeImages->setObjectName("resultsSaveModeImages");
    saveModeTrajectory = modeMenu->addAction(lineIcon("export"), tr("振動をXYZ（複数構造）で書き出す…"));
    saveModeTrajectory->setObjectName("resultsSaveModeXyz");
    modeExport->setMenu(modeMenu);
    closeMode = iconButton("close", "resultsCloseMode", tr("振動の表示を終えて構造列に戻る"));
    modeRow->addWidget(group({playMode, modeLabel}, 8));
    modeRow->addWidget(group({amplitudeLabel, amplitude}, 6));
    modeRow->addWidget(arrows);
    modeRow->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum));
    modeRow->addWidget(group({modeExport, closeMode}));
    dock->addWidget(modePage);
    auto *hint = new ElidedLabel(
        tr("原子をクリック: 計測（2〜4原子） · %1 · 結果は編集中の分子から独立").arg(viewControlsHint()));
    hint->setObjectName("resultsHint");
    hint->setToolTip(hint->text());
    auto hintFont = hint->font();
    hintFont.setPointSizeF(hintFont.pointSizeF() * .9);
    hint->setFont(hintFont);
    dockLayout->addWidget(hint);
    return dockFrame;
}
QWidget *ResultsPanel::buildDetails() {
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
    auto bold = bannerTitle->font(); // As the title of what the view shows.
    bold.setBold(true);
    bannerTitle->setFont(bold);
    bannerTitle->setWordWrap(true);
    bannerText = new QLabel;
    bannerText->setObjectName("resultsBannerText");
    bannerText->setWordWrap(true);
    bannerText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bannerTexts->addWidget(bannerTitle);
    bannerTexts->addWidget(bannerText);
    bannerRow->addLayout(bannerTexts, 1);
    auto *bannerLog = new QPushButton(tr("ログの末尾を表示"));
    bannerLog->setObjectName("resultsBannerLog");
    bannerRow->addWidget(bannerLog, 0, Qt::AlignTop);
    banner->hide();
    sideLayout->addWidget(banner);
    auto *tabs = details = new QTabWidget;
    tabs->setObjectName("resultsDetails");
    tabs->setProperty("tabs", "underline");
    tabs->tabBar()->setDrawBase(false);
    tabs->setUsesScrollButtons(true);
    sideLayout->addWidget(tabs, 1);
    // Messages of the viewer (loading, errors, comparisons) under the details, so that showing one
    // moves nothing above it. How the calculation ended is in the bar, the banner and "概要".
    status = new QPlainTextEdit;
    status->setObjectName("resultsStatus");
    status->setAccessibleName(tr("結果ビューアのメッセージ"));
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
    connect(bannerLog, &QPushButton::clicked, this, [this] {
        details->setCurrentWidget(log->parentWidget());
        log->moveCursor(QTextCursor::End);
        log->ensureCursorVisible();
    });
    return side;
}
void ResultsPanel::buildTabs() {
    auto *tabs = details;
    overviewScroll = new QScrollArea;
    overviewScroll->setObjectName("resultsOverview");
    overviewScroll->setWidgetResizable(true);
    overviewScroll->setFrameShape(QFrame::NoFrame);
    tabs->addTab(overviewScroll, tr("概要"));
    // The tabs that only show the result are views of their own; what they ask of the viewer is a
    // signal of theirs.
    energiesView = new ResultEnergiesView;
    tabs->addTab(energiesView, tr("エネルギー"));
    tabs->addTab(buildOrbitalTab(), tr("軌道"));
    tabs->addTab(buildVibrationTab(), tr("振動"));
    optimizationView = new ResultOptimizationView(uiContext);
    tabs->addTab(optimizationView, tr("最適化"));
    excitationsView = new ResultExcitationsView(uiContext, [this] { return jobMolden(); });
    tabs->addTab(excitationsView, tr("励起状態"));
    connect(excitationsView, &ResultExcitationsView::orbitalRequested, this,
            &ResultsPanel::openTransitionOrbital);
    pathView = new ResultPathView(uiContext);
    tabs->addTab(pathView, tr("経路・スキャン"));
    connect(pathView, &ResultPathView::frameRequested, this, &ResultsPanel::selectFrame);
    connect(pathView, &ResultPathView::saddleSearchRequested, this,
            [this] { uiContext.safe([this] { prepareRecalculation("saddle_frequencies"); }); });
    propertiesView = new ResultPropertiesView;
    tabs->addTab(propertiesView, tr("物性"));
    connect(propertiesView, &ResultPropertiesView::atomPairSelected, this, [this](int a, int b) {
        // The last structure, which the analysis is of, with the two atoms picked as for a measurement.
        measurement->setPicked({a, b});
        selectFrame(data.frames.size() - 1);
    });
    tabs->addTab(buildSurfaceTab(), tr("CUBE / 等値面"));
    tabs->addTab(buildLogTab(), tr("元ファイル"));
    propertyGeometry = new PropertyGeometryView(uiContext, this);
    tabs->addTab(propertyGeometry, tr("物性3D・断面"));
    spectroscopy = new SpectroscopyView(uiContext, this);
    tabs->addTab(spectroscopy, tr("スペクトル"));
    connect(spectroscopy, &SpectroscopyView::atomSelected, this, [this](int index) {
        if (index < 0 || data.frames.isEmpty() || index >= data.frames.last().molecule.atoms.size())
            return;
        measurement->setPicked({index});
        selectFrame(data.frames.size() - 1);
    });
    connect(spectroscopy, &SpectroscopyView::modeSelected, this, [this](int index) { selectMode(index); });
    connect(spectroscopy, &SpectroscopyView::excitationSelected, excitationsView,
            &ResultExcitationsView::selectState);
    connect(measurement, &ResultMeasurement::atomClicked, spectroscopy, &SpectroscopyView::selectAtom);
}
QWidget *ResultsPanel::buildOrbitalTab() {
    auto *orbitalTab = new QWidget;
    auto *ol = new QVBoxLayout(orbitalTab);
    auto *sourceRow = new QHBoxLayout;
    orbitalSource = new QLabel;
    orbitalSource->setObjectName("resultsOrbitalSource");
    orbitalSource->setWordWrap(true);
    orbitalSource->setTextFormat(Qt::PlainText);
    sourceRow->addWidget(orbitalSource, 1);
    openMolden = new QPushButton(tr("形を表示"));
    openMolden->setObjectName("resultsOpenMolden");
    openMolden->hide();
    sourceRow->addWidget(openMolden, 0, Qt::AlignTop);
    ol->addLayout(sourceRow);
    auto *orbitalViews = new QHBoxLayout;
    levels = new OrbitalLevels;
    levels->setObjectName("resultsOrbitalLevels");
    orbitalViews->addWidget(levels);
    orbitals = table({tr("番号"), tr("軌道", "orbital table column"), tr("対称性"), tr("スピン"),
                      tr("エネルギー / Eh"), tr("占有数")},
                     "resultsOrbitals");
    orbitals->verticalHeader()->hide();
    orbitalViews->addWidget(orbitals, 1);
    ol->addLayout(orbitalViews, 1);
    orbitalOptions = new QGroupBox(tr("等値面の設定"));
    auto *optionGrid = new QGridLayout(orbitalOptions);
    optionGrid->setVerticalSpacing(3);
    orbitalIso = new QLineEdit("0.03");
    orbitalIso->setObjectName("resultsOrbitalIso");
    orbitalGrid = new QSpinBox;
    orbitalGrid->setObjectName("resultsOrbitalGrid");
    orbitalGrid->setRange(16, 100);
    orbitalGrid->setValue(40);
    orbitalGrid->setToolTip(tr("最も長い辺の分割数です。格子間隔は全軸で同じです。"));
    orbitalNorm = new QComboBox;
    orbitalNorm->setObjectName("resultsOrbitalNorm");
    orbitalNorm->addItem(tr("自動判定"), "auto");
    orbitalNorm->addItem(tr("標準MOLDEN（NWChemのNONE / JANPA）"), "standard");
    orbitalNorm->addItem(tr("NWChem（正規化因子を含む係数）"), "nwchem");
    const QList<std::pair<QString, QWidget *>> optionFields{{tr("等値 / bohr⁻³ᐟ²（±）"), orbitalIso},
                                                            {tr("格子分割数（最長辺）"), orbitalGrid},
                                                            {tr("基底係数の正規化"), orbitalNorm}};
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
            resultSourceStamp(data.source) == orbitalCacheSource)
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
    return orbitalTab;
}
QWidget *ResultsPanel::buildVibrationTab() {
    auto *vibrations = new QWidget;
    auto *vl = new QVBoxLayout(vibrations);
    modeSummary = new QLabel;
    modeSummary->setObjectName("resultsModeSummary");
    modeSummary->setWordWrap(true);
    vl->addWidget(modeSummary);
    auto *irRow = new QHBoxLayout;
    vl->addLayout(irRow);
    auto *irTitle = new QLabel(tr("IRスペクトル（棒をクリックするとそのモードを表示）"));
    irRow->addWidget(irTitle, 1);
    irWidth = new QDoubleSpinBox;
    irWidth->setObjectName("resultsIrWidth");
    irWidth->setRange(1, 200);
    irWidth->setValue(20);
    irWidth->setPrefix(tr("半値全幅 "));
    irWidth->setSuffix(" cm⁻¹");
    irRow->addWidget(irWidth);
    irSpectrum = new ChartView;
    irSpectrum->setObjectName("resultsIrSpectrum");
    irSpectrum->setReversedX(true);
    irSpectrum->setCursor(Qt::PointingHandCursor);
    vl->addWidget(irSpectrum, 1);
    modes = table({tr("振動数 / cm⁻¹"), tr("IR強度 / km mol⁻¹"), tr("種類")}, "resultsModes");
    modes->setItemDelegateForColumn(1, new IntensityDelegate(modes));
    vl->addWidget(modes, 1);
    showRigid = new QCheckBox(tr("並進・回転のモードも表示"));
    showRigid->setObjectName("resultsShowRigid");
    showRigid->setToolTip(tr("振動数がほぼ0の並進・回転（3N−6個以外）は既定で一覧から省きます。"));
    vl->addWidget(showRigid);
    connect(showRigid, &QCheckBox::toggled, this, [this](bool on) {
        for (int i = 0; i < modes->rowCount(); ++i)
            modes->setRowHidden(i, !on && modeKinds.value(i) == ResultModeKind::Rigid);
    });
    auto *modeHint = new QLabel(tr(
        "モードを選ぶと3Dビューに基準構造を表示し、下の操作で再生・書き出しできます。負の振動数は虚振動です。"
        "振幅は表示用に規格化した最大変位で、アニメーションの速さは実際の振動数を表しません。振動数の絶対値が"
        "最も小さい3N−6個以外（直線分子は3N−5個以外）を並進・回転とみなし、IRスペクトル（ローレンツ関数で表示"
        "用に広げた曲線）から除きます。"));
    modeHint->setWordWrap(true);
    vl->addWidget(modeHint);
    connect(irWidth, &QDoubleSpinBox::valueChanged, this, &ResultsPanel::showIrSpectrum);
    connect(irSpectrum, &ChartView::stickClicked, this, [this](int series, int index) {
        if (series == 1 && index >= 0 && index < irModes.size())
            selectMode(irModes[index]);
    });
    connect(arrows, &QCheckBox::toggled, this, &ResultsPanel::showArrows);
    connect(amplitude, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        presentation.setAmplitude(value);
        showPresentation();
    });
    connect(saveModeImages, &QAction::triggered, this,
            [this] { uiContext.safe([this] { exportMode(true); }); });
    connect(saveModeTrajectory, &QAction::triggered, this,
            [this] { uiContext.safe([this] { exportMode(false); }); });
    // Every chart of the viewer saves itself as a figure from its context menu.
    setFigureExport(irSpectrum, uiContext, this);
    return vibrations;
}
QWidget *ResultsPanel::buildSurfaceTab() {
    auto *surfaces = new QWidget;
    auto *sf = new FormLayout(surfaces);
    isovalue = new QLineEdit("0.03");
    isovalue->setObjectName("resultsIsovalue");
    sf->addRow(tr("等値（ファイルの単位）"), isovalue);
    dataset = new QComboBox;
    dataset->setObjectName("resultsDataset");
    sf->addRow(tr("CUBEデータ"), dataset);
    applyIso = new QPushButton(tr("等値面を再生成"));
    applyIso->setObjectName("resultsApplyIso");
    sf->addRow(applyIso);
    // A difference density and the like: this CUBE and others of the same grid, added with factors.
    combineCube = new QPushButton(tr("CUBEの差・和を作る…"));
    combineCube->setObjectName("resultsCombineCubes");
    combineCube->setToolTip(tr("表示中のCUBEと同じ格子のCUBEを係数付きで足し合わせ（ρ(AB) − ρ(A) − ρ(B) "
                               "などの差電子密度）、新しいCUBEとして保存して表示します。"));
    sf->addRow(combineCube);
    connect(combineCube, &QPushButton::clicked, this, &ResultsPanel::showCombineDialog);
    auto *positive = new QCheckBox(tr("正の面（赤）"));
    auto *negative = new QCheckBox(tr("負の面（青）"));
    positive->setChecked(true);
    negative->setChecked(true);
    sf->addRow(positive);
    sf->addRow(negative);
    auto *opacity = new QSlider(Qt::Horizontal);
    opacity->setRange(20, 255);
    opacity->setValue(150);
    sf->addRow(tr("不透明度"), opacity);
    auto style = [this, positive, negative, opacity] {
        view->setSurfaceStyle(positive->isChecked(), negative->isChecked(), opacity->value());
    };
    connect(positive, &QCheckBox::toggled, this, style);
    connect(negative, &QCheckBox::toggled, this, style);
    connect(opacity, &QSlider::valueChanged, this, style);
    auto *surfaceHint = new QLabel(tr("CUBEとMOLDEN軌道の正負・不透明度を変更できます。CUBEの値の意味は生成"
                                      "条件で確認してください。MOLDENの軌道と等値は「軌道」で選択します。"));
    surfaceHint->setWordWrap(true);
    sf->addRow(surfaceHint);
    return surfaces;
}
QWidget *ResultsPanel::buildLogTab() {
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
    auto *copyLog = copyButton("resultsCopyLog", tr("表示中のファイルをコピー"));
    logHead->addWidget(copyLog);
    logLayout->addLayout(logHead);
    log = logView("resultsLog");
    logLayout->addWidget(log, 1);
    connect(copyLog, &QToolButton::clicked, this,
            [this] { QApplication::clipboard()->setText(log->toPlainText()); });
    return logPage;
}
void ResultsPanel::connectActions() {
    connect(files, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0)
            uiContext.safe([&] { openEntry(files->itemData(index).toString()); });
    });
    connect(reload, &QPushButton::clicked, this, &ResultsPanel::reloadResult);
    connect(cancel, &QPushButton::clicked, this, &ResultsPanel::cancelLoad);
    connect(frame, &QSpinBox::valueChanged, this, [this](int value) {
        if (value > 0)
            selectFrame(value - 1);
    });
    connect(timeline, &FrameTimeline::frameSelected, this, &ResultsPanel::selectFrame);
    connect(firstFrame, &QPushButton::clicked, this, [this] { selectFrame(0); });
    connect(lastFrame, &QPushButton::clicked, this, [this] { selectFrame(data.frames.size() - 1); });
    connect(playFrames, &QPushButton::clicked, this, [this] {
        if (presentation.animation() == ResultAnimation::Frames)
            stopAnimation();
        else
            playTrajectory();
    });
    connect(playMode, &QPushButton::clicked, this, [this] {
        if (presentation.animation() == ResultAnimation::Mode)
            stopAnimation();
        else
            selectMode(presentation.mode(), {}, true);
    });
    connect(closeMode, &QPushButton::clicked, this, [this] {
        presentation.selectMode(data, -1);
        showPresentation();
    });
    connect(modes, &QTableWidget::currentCellChanged, this, [this](int row) {
        presentation.selectMode(data, row);
        showPresentation();
    });
    connect(modes, &QTableWidget::cellClicked, this, [this](int row) { selectMode(row); });
    connect(&animation, &QTimer::timeout, this, [this] {
        presentation.advance(data);
        showGeometry();
    });
    connect(applyIso, &QPushButton::clicked, this, [this] {
        bool ok;
        const double n = isovalue->text().toDouble(&ok);
        if (!ok || !std::isfinite(n) || n <= 0 || n > 1e6) {
            error(tr("等値は0より大きい有限の数で指定してください。"));
            return;
        }
        startLoad(data.source, n, dataset->currentIndex());
    });
    connect(applyGeometry, &QPushButton::clicked, this,
            [this] { uiContext.safe([this] { applyStructure(); }); });
    connect(compareGeometry, &QPushButton::clicked, this, [this] {
        uiContext.safe([this] {
            const auto fit = superpose(displayedGeometry(), editorContext.document());
            status->setPlainText(
                tr("編集中の分子との比較: RMSD（座標そのまま）%1 Å、重ね合わせ後 %2 Å（水素以外 "
                   "%3 Å）、最大のずれ %4 Å")
                    .arg(fit.raw, 0, 'f', 4)
                    .arg(fit.rmsd, 0, 'f', 4)
                    .arg(fit.heavyRmsd, 0, 'f', 4)
                    .arg(fit.maximum, 0, 'f', 4));
        });
    });
    connect(saveXyz, &QAction::triggered, this, [this] {
        uiContext.safe([&] {
            const auto p =
                QFileDialog::getSaveFileName(this, tr("表示中の基準構造を保存"), "result.xyz", "XYZ (*.xyz)");
            if (!p.isEmpty())
                saveGeometry(p.endsWith(".xyz", Qt::CaseInsensitive) ? p : p + ".xyz");
        });
    });
    connect(savePng, &QAction::triggered, this, [this] { uiContext.safe([this] { exportImage(); }); });
    connect(saveFrames, &QAction::triggered, this, [this] { uiContext.safe([this] { exportFrames(); }); });
}
ResultsPanel::~ResultsPanel() {
    cancelLoad();
}
void ResultsPanel::applyChromeStyle() {
    const auto pal = palette();
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
    playFrames->setIcon(
        lineIcon(presentation.animation() == ResultAnimation::Frames ? "pause" : "play", c.onAccent));
    playMode->setIcon(
        lineIcon(presentation.animation() == ResultAnimation::Mode ? "pause" : "play", c.onAccent));
    showOutcome();
    optimizationView->setResult(data, palette());
}
void ResultsPanel::stopAnimation() {
    presentation.stop();
    showPresentation();
}
void ResultsPanel::error(const QString &message) {
    status->setPlainText(tr("読込・表示エラー: %1\n前の結果は保持しています。").arg(message));
    emit loadFailed(message);
}
ResultViewerState ResultsPanel::viewerState() const {
    return {loader.pendingPath(), presentation.animation(), presentation.frame(), presentation.mode(),
            presentation.amplitude()};
}
namespace {
void requireResultIndex(int index, qsizetype size) {
    if (index < 0 || index >= size)
        fail(tr("結果の選択番号が範囲外です。"));
}
} // namespace
void ResultsPanel::reloadResult() {
    if (data.source.isEmpty())
        fail(tr("結果ファイルが開かれていません。"));
    startLoad(data.source, data.isovalue, data.dataset, data.selectedOrbital, data.normalizationRequest,
              data.orbitalDivisions, false, animationSource, true);
}
void ResultsPanel::selectFrame(int index) {
    if (loading())
        fail(tr("読込完了後に操作してください。"));
    presentation.selectFrame(data, index);
    showPresentation();
}
void ResultsPanel::selectMode(int index, std::optional<double> value, bool play) {
    if (loading())
        fail(tr("読込完了後に操作してください。"));
    requireResultIndex(index, data.modes.size());
    if (play && data.modes[index].displacement.isEmpty())
        fail(tr("このモードには変位データがありません。"));
    presentation.selectMode(data, index);
    if (value)
        presentation.setAmplitude(*value);
    if (play)
        presentation.playMode(data);
    details->setCurrentWidget(modes->parentWidget());
    showPresentation();
}
void ResultsPanel::selectOrbital(int index) {
    if (loading())
        fail(tr("読込完了後に操作してください。"));
    requireResultIndex(index, data.orbitals.size());
    details->setCurrentWidget(orbitals->parentWidget());
    if (orbitals->currentRow() == index)
        renderOrbital(index);
    else
        orbitals->setCurrentCell(index, 0);
}
void ResultsPanel::playTrajectory() {
    if (loading())
        fail(tr("読込完了後に操作してください。"));
    presentation.playFrames(data);
    showPresentation();
}
void ResultsPanel::cancelLoad() {
    if (!loader.cancel())
        return;
    status->setPlainText(tr("読込を中止しました。前の結果を保持しています。"));
    updateActions();
}
void ResultsPanel::openFile(const QString &path) {
    cancelLoad();
    bool ok = true;
    const auto suffix = QFileInfo(path).suffix().toLower();
    const double iso = suffix == "cube" || suffix == "cub" ? isovalue->text().toDouble(&ok) : .03;
    if (!ok || !std::isfinite(iso) || iso <= 0 || iso > 1e6) {
        error(tr("CUBEの等値は0より大きい有限の数で指定してください。"));
        return;
    }
    startLoad(path, iso, 0);
}
void ResultsPanel::combineCubes(const QVector<CubeTerm> &terms, const QString &output) {
    if (terms.isEmpty())
        fail(tr("組み合わせるCUBEを指定してください。"));
    for (const auto &term : terms)
        uiContext.checkMcpPath(term.path);
    uiContext.checkMcpPath(output);
    bool ok = true;
    const double iso = isovalue->text().toDouble(&ok);
    startLoad(output, ok && std::isfinite(iso) && iso > 0 && iso <= 1e6 ? iso : .03, 0, -1, "auto", 40, false,
              false, false, terms);
}
void ResultsPanel::showCombineDialog() {
    if (data.format != "CUBE" || data.source.isEmpty())
        return;
    if (const auto chosen = askCubeCombination(this, uiContext, data.source, data.dataset))
        uiContext.safe([&] { combineCubes(chosen->terms, chosen->output); });
}
void ResultsPanel::openDirectory(const QString &path) {
    cancelLoad();
    try {
        const auto first = preferredResultFile(path, nwchemResultFiles(path), JobFile::output);
        folder = QFileInfo(first).absolutePath();
        fillFiles(QFileInfo(first).fileName());
        openEntry(files->currentData().toString());
    } catch (const std::exception &e) {
        error(QString::fromUtf8(e.what()));
    }
}
void ResultsPanel::fillFiles(const QString &selected) {
    const QSignalBlocker guard(files);
    files->clear();
    animationFrames.clear();
    const auto kind = [](const QString &name) {
        const auto suffix = QFileInfo(name).suffix().toLower();
        return suffix == "out" || suffix == "log"    ? 0
               : suffix == "xyz"                     ? 1
               : suffix == "cube" || suffix == "cub" ? 2
                                                     : 3;
    };
    QVector<QStringList> groups(4);
    QVector<ResultFileGroup> modes;
    for (const auto &group : resultFileGroups(nwchemResultFiles(folder))) {
        if (group.mode >= 0)
            modes << group;
        else
            groups[kind(group.files.first())] << group.files.first();
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
    for (const auto &group : modes) {
        const auto key = QString("animation:%1:%2").arg(group.animationName).arg(group.mode);
        animationFrames[key] = group.files;
        files->addItem(tr("振動アニメーション %1: モード %2（%3枚）")
                           .arg(QFileInfo(group.animationName).fileName(), QString::number(group.mode),
                                QString::number(group.files.size())),
                       key);
    }
    QString entry = selected;
    for (auto it = animationFrames.cbegin(); it != animationFrames.cend(); ++it)
        if (it.value().contains(selected))
            entry = it.key();
    int index = files->findData(entry);
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
    const auto list = animationFrames.value(entry);
    if (list.isEmpty())
        fail(tr("振動アニメーションのファイルが見つかりません。"));
    startLoad(QDir(folder).filePath(list.first()), .03, 0, -1, "auto", 40, false, true);
}
QString ResultsPanel::jobMolden() const {
    if (data.format != "NWChem")
        return {};
    for (const auto &name : nwchemResultFiles(QFileInfo(data.source).absolutePath()))
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
        error(tr("軌道の等値は0より大きい有限の数で指定してください。"));
        return;
    }
    startLoad(data.source, iso, 0, orbital, orbitalNorm->currentData().toString(), orbitalGrid->value(),
              true);
}
void ResultsPanel::startLoad(const QString &path, double iso, int channel, int orbital,
                             const QString &normalization, int divisions, bool reuseOrbital, bool animation,
                             bool reread, const QVector<CubeTerm> &combine) {
    cancelLoad();
    stopAnimation();
    if (path.isEmpty())
        return;
    try {
        // The file itself: one picked from a folder may be a link that leads out of the allowed ones.
        uiContext.checkMcpPath(path);
    } catch (const std::exception &e) {
        error(QString::fromUtf8(e.what()));
        return;
    }
    const auto stamp = resultSourceStamp(path);
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
    loader.disconnect(this);
    connect(&loader, &ResultLoader::failed, this, [this](const QString &message) {
        pendingOrbital = {};
        updateActions();
        error(message);
    });
    connect(&loader, &ResultLoader::loaded, this,
            [this, reuseOrbital, sameSource, animation](ResultData next, const QString &stamp) {
                QString failure;
                int follow = -1; // The log's orbital in the job's MOLDEN file, to show next.
                const auto wanted = std::exchange(pendingOrbital, {});
                try {
                    const bool newSource = data.source != next.source;
                    data = std::move(next);
                    animationSource = animation;
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
                updateActions();
                if (failure.isEmpty())
                    emit loaded();
                else
                    error(failure);
                if (follow >= 0)
                    orbitals->setCurrentCell(follow, 0); // Renders it like a choice in the list.
            });
    // Another orbital of this file: the message would open under the details at every step and
    // change the height of the level diagram, so that the line under the settings says it instead.
    orbitalLoad = reuseOrbital && sameSource ? orbital : -1;
    try {
        loader.start({path, iso, channel, orbital, normalization, divisions, animation, reread, combine});
        if (orbitalLoad >= 0)
            status->clear();
        else
            status->setPlainText(tr("読込中: %1\n完了まで前の結果を表示します。").arg(loader.pendingPath()));
    } catch (const std::exception &e) {
        pendingOrbital = {};
        error(QString::fromUtf8(e.what()));
    }
    updateActions();
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
                       static_cast<QWidget *>(applyGeometry), static_cast<QWidget *>(recalculate),
                       static_cast<QWidget *>(exportButton), findChild<QWidget *>("resultsBarDivider"),
                       static_cast<QWidget *>(viewControlsFrame), static_cast<QWidget *>(dockFrame)})
        w->setVisible(shown);
    cancel->setVisible(loading());
    saveXyz->setEnabled(ready && !data.frames.isEmpty());
    applyGeometry->setEnabled(ready && !data.frames.isEmpty());
    recalculate->setEnabled(applyGeometry->isEnabled() &&
                            QFileInfo::exists(QFileInfo(data.source).absolutePath() + '/' + JobFile::record));
    compareGeometry->setEnabled(applyGeometry->isEnabled());
    pathView->setSaddleEnabled(recalculate->isEnabled());
    savePng->setEnabled(ready && !data.frames.isEmpty());
    saveFrames->setEnabled(ready && data.frames.size() > 1);
    exportButton->setEnabled(saveXyz->isEnabled() || savePng->isEnabled());
    frame->setEnabled(ready && !data.frames.isEmpty());
    timeline->setEnabled(ready && data.frames.size() > 1);
    playFrames->setEnabled(ready && data.frames.size() > 1);
    firstFrame->setEnabled(playFrames->isEnabled());
    lastFrame->setEnabled(playFrames->isEnabled());
    const int mode = presentation.mode();
    playMode->setEnabled(ready && mode >= 0 && mode < data.modes.size() &&
                         !data.modes[mode].displacement.isEmpty());
    saveModeImages->setEnabled(playMode->isEnabled());
    saveModeTrajectory->setEnabled(playMode->isEnabled());
    modeExport->setEnabled(playMode->isEnabled());
    energiesView->setReferenceEnabled(ready && !data.energies.isEmpty());
    applyIso->setEnabled(ready && data.format == "CUBE");
    combineCube->setEnabled(applyIso->isEnabled());
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
                                 ? tr("%1 %2番").arg(data.orbitals[data.selectedOrbital].spin,
                                                     orbitals->item(data.selectedOrbital, 0)->text())
                                 : QString());
    const bool rendering = loading() && orbitalLoad >= 0 && orbitalLoad < data.orbitals.size();
    orbitalStatus->setText(
        data.format == "NWChem" && !data.orbitals.isEmpty()
            ? tr("番号はNWChemの軌道番号です（ログに一部の軌道しかなくてもMOLDENと同じ番号）。")
        : data.format != "MOLDEN" ? tr("MOLDENファイルを開くと分子軌道を表示します。")
        : rendering ? tr("読込中: %1 %2番 / 等値 ±%3\n準位図か一覧で軌道を選ぶと表示を切り替えます。赤: 正 / "
                         "青: 負。")
                          .arg(data.orbitals[orbitalLoad].spin, orbitals->item(orbitalLoad, 0)->text(),
                               orbitalIso->text())
        : !data.orbitalError.isEmpty() ? data.orbitalError
        : data.selectedOrbital >= 0
            ? tr("表示中: %1 %2番 / 等値 ±%3 / 正規化: "
                 "%4\n準位図か一覧で軌道を選ぶと表示を切り替えます。赤: 正 / 青: 負。")
                  .arg(data.orbitals[data.selectedOrbital].spin,
                       orbitals->item(data.selectedOrbital, 0)->text(), real(data.isovalue),
                       data.orbitalNormalization == "nwchem" ? QString("NWChem") : tr("標準MOLDEN"))
            : tr("このファイルには表示可能な分子軌道がありません。"));
}
void ResultsPanel::display(bool orbitalOnly) {
    if (orbitalOnly && !data.frames.isEmpty())
        presentation.selectFrame(data,
                                 data.selectedOrbital >= 0 ? data.orbitalFrame : data.frames.size() - 1);
    else
        presentation.reset(data);
    summary = resultMessage(data);
    status->clear(); // The outcome is in "概要": no message left from loading.
    if (!orbitalOnly && !data.source.isEmpty()) {
        QString selectedFile;
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
    // A run that did not end normally has the banner over the details instead.
    setChip(outcomeChip, outcome == "normal" ? tr("正常終了") : QString(), "good", data.status);
    if (!orbitalOnly)
        barFit->refit(); // Another file: its names may fit now.
    if (!orbitalOnly) {
        log->setPlainText(data.log);
        logName->setText(QFileInfo(data.source).fileName() +
                         (data.log.startsWith("…") ? tr(" · 末尾 128 KiB") : QString()));
        measurement->clear();
        energiesView->setResult(data);
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
    openMolden->setText(tr("形を表示: %1").arg(molden));
    openMolden->setToolTip(
        tr("選択中の軌道（未選択ならHOMO）を、同じジョブの %1 から表示します。").arg(molden));
    orbitalSource->setText(
        data.format == "NWChem" && !data.orbitals.isEmpty()
            ? (molden.isEmpty()
                   ? tr("ログの軌道エネルギーです。軌道の形を表示するには、「2 計算条件」で"
                        "「計算後に分子軌道をMOLDENファイルに出力」をオンにして計算してください。")
                   : tr("ログの軌道エネルギーです。行をダブルクリックするか「形を表示」で、"
                        "同じジョブの %1 からその軌道の形を表示します。")
                         .arg(molden))
        : data.format == "MOLDEN" ? tr("エネルギーと形: %1").arg(QFileInfo(data.source).fileName())
                                  : QString());
    orbitalSource->setVisible(!orbitalSource->text().isEmpty());
    orbitalOptions->setVisible(data.format == "MOLDEN");
    if (!orbitalOnly) {
        const QSignalBlocker guard(modes);
        modes->setRowCount(data.modes.size());
        modeKinds = resultModeKinds(data);
        const auto counts = resultModeCounts(modeKinds);
        double strongest = 0;
        for (int i = 0; i < data.modes.size(); ++i)
            if (modeKinds[i] != ResultModeKind::Rigid && std::isfinite(data.modes[i].intensity))
                strongest = std::max(strongest, data.modes[i].intensity);
        for (int i = 0; i < data.modes.size(); ++i) {
            const auto &m = data.modes[i];
            const bool rigid = modeKinds[i] == ResultModeKind::Rigid;
            // Translations and rotations are no vibrations: hidden by default, without an IR intensity.
            row(modes, i,
                {real(m.frequency), rigid ? QString("—") : real(m.intensity),
                 rigid                         ? tr("並進・回転")
                 : imaginaryMode(modeKinds[i]) ? tr("虚振動")
                                               : tr("振動", "mode type")});
            if (!rigid && strongest > 0 && std::isfinite(m.intensity))
                modes->item(i, 1)->setData(Qt::UserRole, std::clamp(m.intensity / strongest, 0., 1.));
            if (modeKinds[i] == ResultModeKind::SignificantImaginary)
                modes->item(i, 2)->setForeground(errorColor(palette()));
            modes->setRowHidden(i, rigid && !showRigid->isChecked());
        }
        modes->clearSelection();
        modes->setCurrentCell(-1, -1);
        QStringList parts{tr("振動 %n", nullptr, counts.vibrations), resultImaginaryText(counts)};
        if (const auto thermo = data.properties.value("thermochemistry").toArray(); !thermo.isEmpty())
            parts << tr("ゼロ点補正 %1 kcal/mol").arg(fixed(thermo[0].toObject()["zpe_kcal"].toDouble(), 3));
        if (counts.rigid)
            parts << tr("並進・回転 %1").arg(counts.rigid);
        modeSummary->setText(parts.join(" · "));
        showIrSpectrum();
        pathView->setResult(data);
        propertiesView->setResult(data);
        spectroscopy->setResult(data);
        propertyGeometry->setResult(data);
        optimizationView->setResult(data, palette());
        excitationsView->setResult(data);
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
        for (const auto &value : data.properties.value("neb").toObject()["points"].toArray())
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
        frame->setValue(presentation.frame() + 1);
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
    showPresentation();
}
void ResultsPanel::updateTabs() {
    const auto p = data.properties;
    const bool loaded = !data.source.isEmpty();
    const bool properties = p.contains("mulliken") || p.contains("esp") || p.contains("resp") ||
                            p.contains("shielding") || p.contains("polarizability") || p.contains("bsse") ||
                            p.contains("solvation") || p.contains("bond_indices") || p.contains("valency") ||
                            p.contains("spinspin");
    const bool visible[]{true,
                         loaded && !data.energies.isEmpty(),
                         loaded && (!data.orbitals.isEmpty() || data.format == "MOLDEN"),
                         loaded && !data.modes.isEmpty(),
                         loaded && !data.steps.isEmpty(),
                         loaded && !data.excitations.isEmpty(),
                         loaded && (p.contains("scan") || p.contains("irc") || p.contains("neb")),
                         loaded && properties,
                         loaded && (data.format == "CUBE" || data.format == "MOLDEN"),
                         loaded && !data.log.isEmpty()};
    details->setTabVisible(details->indexOf(spectroscopy),
                           loaded && (p.contains("shielding") || p.contains("raman") || p.contains("ecd") ||
                                      p.contains("ord") || p.contains("epr_gshift_ppt")));
    details->setTabVisible(
        details->indexOf(propertyGeometry),
        loaded && (data.format == "CUBE" || p.contains("dipole") || p.contains("polarizability")));
    // Counts after the names: what each tab lists.
    const int vibrations =
        resultModeCounts(modeKinds).vibrations + std::max(0, int(data.modes.size() - modeKinds.size()));
    const int counts[]{0,
                       int(data.energies.size()),
                       int(data.orbitals.size()),
                       loaded ? vibrations : 0,
                       int(data.steps.size()),
                       int(data.excitations.size()),
                       int(pathView->shown().points.size()),
                       0,
                       0,
                       0};
    const QStringList names{tr("概要"),          tr("エネルギー"), tr("軌道"),           tr("振動"),
                            tr("最適化"),        tr("励起状態"),   tr("経路・スキャン"), tr("物性"),
                            tr("CUBE / 等値面"), tr("元ファイル")};
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
        label(tr("結果ファイル（NWChem出力・MOLDEN・CUBE・XYZ）またはジョブフォルダーを開くと、ここに結果の要"
                 "点を"
                 "表"
                 "示し、データのあるタブ（エネルギー・軌道・振動など）が現れます。\n\n「計算を実行」（F5）で"
                 "計算す"
                 "ると、"
                 "終了後に自動で読み込みます。過去の計算は「履歴」から開けます。"),
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
        label(tr("%n原子 · %1 · %2", nullptr, int(molecule.atoms.size()))
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
            t->setToolTip(tr("「%1」を表示").arg(details->tabText(tab)));
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
    if (const auto final = resultFinalEnergy(data)) {
        const auto source = final->source == ResultEnergySource::FrequencyReference ? tr("振動数の参照構造")
                            : final->source == ResultEnergySource::BsseCorrected    ? tr("BSSE補正後")
                                                                                    : tr("最後のtask");
        QString caption;
        if (!data.energies.isEmpty() && data.energies.last().hartree != final->hartree)
            caption = tr("出力の最後の記録: %1 %2 Eh")
                          .arg(data.energies.last().method, shownReal(data.energies.last().hartree));
        tile("resultsTileEnergy", tr("計算のエネルギー（%1 · %2）").arg(final->method, source),
             shownReal(final->hartree) + " Eh", caption, "neutral", 1, true);
    } else if (!data.energies.isEmpty())
        tile("resultsTileEnergy", tr("最後のエネルギー記録（%1）").arg(data.energies.last().method),
             shownReal(data.energies.last().hartree) + " Eh", tr("出力順の最後の値です。"), "neutral", 1,
             true);
    if (const auto optimization = resultOptimizationConverged(data); optimization || !data.steps.isEmpty()) {
        const bool converged = optimization.value_or(false);
        QString caption;
        if (!data.steps.isEmpty()) {
            caption = tr("%nステップ", nullptr, data.steps.last().step);
            int met = 0, judged = 0;
            for (const auto &criterion :
                 resultCriteria(data.steps.last(), data.properties.value("thresholds").toObject())) {
                judged += criterion.judged();
                met += criterion.met();
            }
            if (judged)
                caption += tr(" · 最終ステップは判定 %1/%2 を満たす").arg(met).arg(judged);
        }
        tile("resultsTileOptimization", tr("構造最適化"),
             converged ? tr("収束", "optimization state") : tr("収束メッセージなし"), caption,
             converged ? "good" : "warn", 4);
    }
    if (!data.modes.isEmpty()) {
        const auto counts = resultModeCounts(modeKinds);
        QStringList values;
        for (int i = 0; i < data.modes.size(); ++i)
            if (imaginaryMode(modeKinds.value(i)))
                values << shownFixed(data.modes[i].frequency, 1);
        // All imaginary frequencies, and how many are significant (the others may be numerical noise).
        tile("resultsTileImaginary", tr("虚振動", "summary card"),
             counts.imaginary ? tr("%n個", "imaginary frequencies", counts.imaginary) : tr("なし"),
             counts.imaginary
                 ? tr("−10 cm⁻¹未満 %n個", nullptr, counts.significant) + " · " + values.join(", ") + " cm⁻¹"
                 : tr("振動 %n%1", nullptr, counts.vibrations)
                       .arg(counts.rigid ? tr(" · 並進・回転 %1 は除外").arg(counts.rigid) : QString()),
             counts.significant ? "warn"
             : counts.imaginary ? "neutral"
                                : "good",
             3);
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
             spins.size() > 1 ? tr("HOMO–LUMOギャップ（%1）").arg(spin) : tr("HOMO–LUMOギャップ"),
             shownFixed((data.orbitals[l].energy - data.orbitals[h].energy) * HartreeToEv, 2) + " eV",
             QString("HOMO %1 · LUMO %2 eV")
                 .arg(shownFixed(data.orbitals[h].energy * HartreeToEv, 2),
                      shownFixed(data.orbitals[l].energy * HartreeToEv, 2)),
             "neutral", 2);
    }
    if (const auto d = resultDipole(data))
        tile("resultsTileDipole", tr("双極子モーメント"), fixed(d->debye, 2) + " D",
             QString("x %1 · y %2 · z %3 a.u.")
                 .arg(shownFixed(d->au.x(), 4), shownFixed(d->au.y(), 4), shownFixed(d->au.z(), 4)),
             "neutral", 7);
    if (!data.excitations.isEmpty()) {
        const auto strongest =
            std::max_element(data.excitations.begin(), data.excitations.end(),
                             [](const auto &a, const auto &b) { return a.oscillator < b.oscillator; });
        tile("resultsTileExcitations", tr("励起状態"),
             tr("%n件", "excited states", int(data.excitations.size())),
             tr("最大の振動子強度 %1（%2 eV / %3 nm）")
                 .arg(fixed(strongest->oscillator, 4), fixed(strongest->ev, 3),
                      fixed(EvNanometer / strongest->ev, 1)),
             "neutral", 5);
    }
    if (const auto &points = pathView->shown().points; !points.isEmpty())
        tile("resultsTilePath",
             data.properties.contains("scan")  ? tr("座標スキャン")
             : data.properties.contains("neb") ? QString("NEB")
                                               : QString("IRC"),
             tr("%n点", nullptr, int(points.size())), tr("行を選ぶとその点の構造を表示します。"), "neutral",
             6);
    if (data.format == "CUBE" || data.selectedOrbital >= 0)
        tile("resultsTileSurface", tr("等値面"), "±" + real(data.isovalue),
             tr("格子 %1 × %2 × %3 · 値 %4 ～ %5 · 三角形 %6")
                 .arg(data.grid[0])
                 .arg(data.grid[1])
                 .arg(data.grid[2])
                 .arg(shownReal(data.minimum), shownReal(data.maximum))
                 .arg(data.surface.size()),
             "neutral", data.format == "CUBE" ? 8 : 2);
    if (data.frames.size() > 1)
        tile("resultsTileFrames", tr("構造列"), tr("%n構造", nullptr, int(data.frames.size())),
             tr("3Dビューの下のタイムラインで切り替えます。"), "neutral", -1);
    if (cell)
        layout->addLayout(grid);
    else
        delete grid;
    // Thermochemistry of each temperature.
    for (const auto &value : data.properties.value("thermochemistry").toArray()) {
        const auto t = value.toObject();
        const double temperature = t["temperature"].toDouble(), h = t["enthalpy_kcal"].toDouble(),
                     g = h - temperature * t["entropy_cal"].toDouble() / 1000;
        section(tr("熱化学（%1 K）").arg(fixed(temperature, 2)));
        auto *values = new QGridLayout;
        values->setHorizontalSpacing(16);
        values->setVerticalSpacing(4);
        const QList<std::pair<QString, QString>> rows{
            {tr("ゼロ点補正"), shownFixed(t["zpe_kcal"].toDouble(), 3) + " kcal/mol"},
            {tr("熱補正（内部エネルギー）"), shownFixed(t["energy_kcal"].toDouble(), 3) + " kcal/mol"},
            {tr("熱補正（エンタルピー）"), shownFixed(h, 3) + " kcal/mol"},
            {tr("エントロピー"), fixed(t["entropy_cal"].toDouble(), 3) + " cal/(mol·K)"},
            {tr("Gibbs補正 G = H − TS（算出値）"),
             tr("%1 kcal/mol（%2 Eh）").arg(shownFixed(g, 3), shownFixed(g / HartreeToKcal, 6))}};
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
        label(tr("補正は電子エネルギーに加えて使います。対称数 %1 はNWChemの値です（MolFormaの入力では1）。")
                  .arg(real(t["symmetry_number"].toDouble())),
              "muted");
    }
    if (const auto other = resultPropertyLines(data).trimmed(); !other.isEmpty()) {
        section(tr("ほかの結果"));
        label(other, "");
    }
    // The banner says how a run that did not end normally ended; the warnings stay here.
    const auto outcome = resultOutcome(data);
    const bool bannered = outcome == "incomplete" || outcome == "error";
    if (!bannered || !data.warnings.isEmpty())
        section(tr("メッセージ"));
    if (!bannered)
        label(data.status, "");
    if (!data.warnings.isEmpty())
        label(data.warnings.join("\n\n"), "muted");
    // The other files of the job, to open with one click.
    QList<int> entries;
    for (int i = 0; i < files->count(); ++i)
        if (!files->itemData(i).toString().isEmpty())
            entries << i;
    if (entries.size() > 1) {
        section(tr("このフォルダーの結果ファイル"));
        for (int k = 0; k < entries.size() && k < 12; ++k) {
            const int i = entries[k];
            auto *line = new QHBoxLayout;
            line->setSpacing(8);
            auto *name = new QLabel(files->itemText(i));
            name->setProperty("role", "file");
            name->setTextFormat(Qt::PlainText);
            line->addWidget(name, 1);
            if (i == files->currentIndex()) {
                auto *current = new QLabel(tr("表示中"));
                current->setProperty("chip", "current");
                line->addWidget(current);
            } else {
                auto *openIt = new QPushButton(tr("開く"));
                openIt->setProperty("role", "open");
                openIt->setAccessibleName(tr("%1 を開く").arg(files->itemText(i)));
                connect(openIt, &QPushButton::clicked, this, [this, entry = files->itemData(i).toString()] {
                    if (const int index = files->findData(entry); index >= 0)
                        files->setCurrentIndex(index);
                });
                line->addWidget(openIt);
            }
            layout->addLayout(line);
        }
        if (entries.size() > 12)
            label(tr("ほか %n 件はバーのファイル一覧から選べます。", nullptr, int(entries.size()) - 12),
                  "muted");
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
    const ChromeColors c(palette());
    bannerIcon->setPixmap(lineIcon("warning", failed ? c.bad : c.candidateText).pixmap(QSize(20, 20)));
    bannerTitle->setText(failed ? tr("NWChemのエラーを検出しました")
                                : tr("NWChemの正常終了の記録がありません"));
    bannerText->setText(
        failed ? tr("%1。詳細は「概要」のメッセージとログで確認してください。").arg(data.status)
               : tr("実行中・中断・途中までのログの可能性があります。%1実行中なら「再読込」で"
                    "更新します。")
                     .arg(resultOptimizationConverged(data).value_or(false) ? tr("最適化は収束しています。")
                                                                            : QString()));
}
void ResultsPanel::showContext() {
    const int i = presentation.frame(), mode = presentation.mode();
    int page = 0;
    QString title, detail;
    if (presentation.showingMode() && mode >= 0 && mode < data.modes.size()) {
        page = 2;
        const auto &m = data.modes[mode];
        title = tr("モード %1 · %2 cm⁻¹").arg(mode + 1).arg(fixed(m.frequency, 1));
        detail = presentation.animation() == ResultAnimation::Mode
                     ? tr("再生中 · 振幅 %1 Å").arg(fixed(presentation.amplitude(), 2))
                     : tr("基準構造");
        if (modeKinds.value(mode) == ResultModeKind::Rigid)
            detail += tr(" · 並進・回転");
        else if (imaginaryMode(modeKinds.value(mode)))
            detail += tr(" · 虚振動");
        modeLabel->setText(title);
    } else if (data.format == "MOLDEN" && data.orbitalBasis && !data.orbitals.isEmpty() &&
               (data.frames.size() <= 1 || i == data.orbitalFrame)) {
        page = 1;
        if (data.selectedOrbital >= 0 && i == data.orbitalFrame) {
            const auto &o = data.orbitals[data.selectedOrbital];
            title = tr("%1 · %2 %3番")
                        .arg(frontierName(data.orbitals, data.selectedOrbital), o.spin,
                             orbitals->item(data.selectedOrbital, 0)->text());
            detail = tr("%1 eV · 等値 ±%2").arg(fixed(o.energy * HartreeToEv, 3), real(data.isovalue));
        } else {
            title = tr("分子軌道");
            detail = data.orbitalError.isEmpty() ? tr("軌道を選ぶと等値面を表示します")
                                                 : data.orbitalError.section('\n', 0, 0).left(80);
        }
    } else if (data.format == "CUBE" && !data.frames.isEmpty()) {
        title = data.datasets.isEmpty() ? QString("CUBE")
                                        : tr("CUBE · データ %1").arg(data.datasets.value(data.dataset));
        detail = tr("等値 ±%1 · 格子 %2 × %3 × %4")
                     .arg(real(data.isovalue))
                     .arg(data.grid[0])
                     .arg(data.grid[1])
                     .arg(data.grid[2]);
    } else if (i >= 0 && i < data.frames.size()) {
        title = data.frames.size() > 1 ? tr("構造 %1 / %2%3")
                                             .arg(i + 1)
                                             .arg(data.frames.size())
                                             .arg(i == data.frames.size() - 1 ? tr("（最後）") : QString())
                                       : tr("構造");
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
QString ResultsPanel::message() const {
    const auto shown = status->toPlainText();
    return shown.isEmpty() ? summary : shown;
}
QString ResultsPanel::briefSummary() const {
    return resultBriefSummary(data);
}
void ResultsPanel::openTransitionOrbital(const QString &spin, int number) {
    if (loading())
        return;
    const auto molden = jobMolden();
    if (const int index = molden.isEmpty() ? -1 : files->findData(molden); index >= 0) {
        // Chosen in the MOLDEN file once it has loaded, like a log orbital whose shape is opened.
        pendingOrbital = {QFileInfo(QDir(folder).filePath(molden)).absoluteFilePath(), spin, number};
        files->setCurrentIndex(index);
        return;
    }
    // Without a MOLDEN file: the orbital's energy in "軌道", if the log's analysis lists it.
    for (int i = 0; i < data.orbitals.size(); ++i)
        if (data.orbitals[i].spin == spin && data.orbitals[i].number == number) {
            details->setCurrentIndex(2);
            orbitals->selectRow(i);
            return;
        }
}
void ResultsPanel::showIrSpectrum() {
    irModes.clear();
    try {
        double top = 0;
        for (int i = 0; i < data.modes.size(); ++i)
            if (modeKinds.value(i) != ResultModeKind::Rigid && std::isfinite(data.modes[i].intensity))
                top = std::max(top, data.modes[i].frequency);
        const auto values = calculateSpectrum(
            data, {{"kind", "ir"}, {"width", irWidth->value()}, {"minimum", 0.}, {"maximum", top + 200}});
        for (const auto &peak : values.peaks)
            irModes << peak.toObject()["mode"].toInt();
        irSpectrum->setToolTip({});
        irSpectrum->setChart(tr("IRスペクトル（表示用）"), tr("波数 / cm⁻¹"), tr("IR強度 / km mol⁻¹"),
                             {{tr("広がり"), values.curve, ChartTone::Muted},
                              {tr("各モード"), values.sticks, ChartTone::Text, true}});
        irSpectrum->setSelectedStick(1, irModes.indexOf(presentation.mode()));
    } catch (const std::exception &e) {
        irSpectrum->clear();
        irSpectrum->setToolTip(QString::fromUtf8(e.what()));
    }
}
void ResultsPanel::showArrows() {
    const int n = presentation.mode();
    // Only over the reference structure the displacements belong to.
    if (!arrows->isChecked() || !presentation.showingMode() || n < 0 || n >= data.modes.size() ||
        data.modes[n].displacement.size() != data.vibrationGeometry.atoms.size() ||
        presentation.animation() != ResultAnimation::None || presentation.displaced()) {
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
                     data.modes[n].displacement.value(i) / longest * 3 * presentation.amplitude()});
    view->setArrows(list);
}
void ResultsPanel::exportMode(bool images) {
    const int n = presentation.mode();
    if (loading() || n < 0 || n >= data.modes.size() || data.modes[n].displacement.isEmpty())
        fail(tr("振動モードを選択してください。"));
    stopAnimation();
    constexpr int steps = 24; // One period.
    const auto name = QString("mode%1_%2cm").arg(n + 1).arg(qRound(data.modes[n].frequency));
    if (images) {
        AnimationOptions options;
        if (!animationOptionsDialog(this, uiContext, view->size(), options, 24, false))
            return;
        const auto target = animationTarget(this, options, name, folder);
        if (target.isEmpty())
            return;
        uiContext.checkMcpPath(target);
        const auto saved = presentation;
        const auto restore = qScopeGuard([&] {
            presentation = saved;
            showPresentation();
        });
        presentation.selectMode(data, n);
        const bool written = writeAnimation(this, target, name, options, steps, [&](int k) {
            presentation.setPhase(2 * M_PI * k / steps);
            showPresentation();
            view->renderNow();
            return view->renderImage(options.scale, options.background);
        });
        if (written)
            uiContext.showMessage(tr("振動の%n枚の画像を書き出しました: %1", nullptr, steps).arg(target),
                                  8000);
        return;
    }
    auto file = QFileDialog::getSaveFileName(this, tr("振動をXYZで保存"), name + ".xyz", "XYZ (*.xyz)");
    if (file.isEmpty())
        return;
    if (!file.endsWith(".xyz", Qt::CaseInsensitive))
        file += ".xyz";
    uiContext.checkMcpPath(file);
    QString text;
    for (int k = 0; k < steps; ++k) {
        auto frameText = resultModeGeometry(data, n, 2 * M_PI * k / steps, presentation.amplitude()).xyz();
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
void ResultsPanel::exportImage() {
    if (loading() || data.frames.isEmpty())
        fail(tr("表示する構造がありません。"));
    ImageOptions options;
    if (!imageOptionsDialog(this, uiContext, view->size(), options))
        return;
    auto path = QFileDialog::getSaveFileName(this, tr("3D表示を画像で保存"),
                                             QDir(folder).filePath("result.png"), "PNG (*.png)");
    if (path.isEmpty())
        return;
    if (!path.endsWith(".png", Qt::CaseInsensitive))
        path += ".png";
    uiContext.checkMcpPath(path);
    if (!view->renderImage(options.scale, options.background).save(path))
        fail(tr("画像を保存できません。"));
    uiContext.showMessage(tr("画像を保存しました: %1").arg(path), 8000);
}
void ResultsPanel::exportFrames() {
    if (loading() || data.frames.size() < 2)
        fail(tr("構造列がありません。"));
    // The points of a path in its order (IRC: backward end, transition state, forward end).
    QVector<int> list;
    for (const auto &point : pathView->shown().points)
        if (point.frame >= 0 && point.frame < data.frames.size())
            list << point.frame;
    if (list.size() < 2) {
        list.clear();
        for (int i = 0; i < data.frames.size(); ++i)
            list << i;
    }
    AnimationOptions options;
    if (!animationOptionsDialog(this, uiContext, view->size(), options, 4, false))
        return;
    const auto target = animationTarget(this, options, "path", folder);
    if (target.isEmpty())
        return;
    uiContext.checkMcpPath(target);
    stopAnimation();
    const auto saved = presentation;
    const auto restore = qScopeGuard([&] {
        presentation = saved;
        showPresentation();
    });
    const bool written = writeAnimation(this, target, "path", options, int(list.size()), [&](int k) {
        selectFrame(list[k]);
        view->renderNow();
        return view->renderImage(options.scale, options.background);
    });
    if (written)
        uiContext.showMessage(tr("%n構造を書き出しました: %1", nullptr, int(list.size())).arg(target), 8000);
}
void ResultsPanel::showPresentation() {
    const auto playing = presentation.animation();
    if (playing == ResultAnimation::None)
        animation.stop();
    else {
        const int interval = playing == ResultAnimation::Frames ? 180 : 40;
        if (!animation.isActive() || animation.interval() != interval)
            animation.start(interval);
    }
    const auto onAccent = ChromeColors(palette()).onAccent;
    const auto button = [&](QPushButton *b, ResultAnimation kind, const QString &idle) {
        const bool active = playing == kind;
        const auto tip = active ? tr("再生を停止") : idle;
        b->setIcon(lineIcon(active ? "pause" : "play", onAccent));
        b->setToolTip(tip);
        b->setAccessibleName(tip);
    };
    button(playFrames, ResultAnimation::Frames, tr("構造列を再生"));
    button(playMode, ResultAnimation::Mode, tr("選択したモードを再生"));
    {
        const QSignalBlocker modeGuard(modes), amplitudeGuard(amplitude);
        if (modes->currentRow() != presentation.mode()) {
            if (presentation.mode() < 0)
                modes->clearSelection();
            modes->setCurrentCell(presentation.mode(), presentation.mode() < 0 ? -1 : 0);
        }
        amplitude->setValue(presentation.amplitude());
    }
    showGeometry();
    updateActions();
}
void ResultsPanel::showGeometry() {
    const int i = presentation.frame();
    {
        const QSignalBlocker guard(frame);
        frame->setValue(i + 1);
    }
    measurement->setDocument(presentation.visibleGeometry(data));
    view->setSurface(!presentation.showingMode() && (data.format != "MOLDEN" || i == data.orbitalFrame)
                         ? data.surface
                         : QVector<SurfaceTriangle>{});
    timeline->setCurrent(i);
    irSpectrum->setSelectedStick(1, irModes.indexOf(presentation.mode()));
    frameEnergy->setText(std::isfinite(frameEnergies.value(i, NAN))
                             ? QString("ΔE %1 kcal/mol").arg(fixed(frameEnergies[i], 2))
                             : QString("ΔE —"));
    showArrows();
    showContext();
}
const Document &ResultsPanel::displayedGeometry() const {
    if (loading())
        fail(tr("読込完了後に操作してください。"));
    return presentation.referenceGeometry(data);
}
void ResultsPanel::saveGeometry(const QString &path) const {
    writeFile(path, displayedGeometry().xyz().toUtf8());
}
void ResultsPanel::applyStructure() {
    const auto result = displayedGeometry();
    std::pair<double, double> moved;
    editorContext.edit(
        tr("計算結果の構造を適用"), [&](Document &d) { moved = applyResultGeometry(d, result); }, true);
    if (!editorContext.hasCandidate())
        fail(tr("編集中の分子と結果の座標は同じです。"));
    workspace.showSetting(SettingTarget::Molecule);
    uiContext.showMessage(tr("結果の構造を候補にしました（RMSD %1 Å、最大変位 %2 Å）。「変更を確定」"
                             "で適用します。")
                              .arg(moved.first, 0, 'f', 4)
                              .arg(moved.second, 0, 'f', 4),
                          15000);
}
void ResultsPanel::prepareRecalculation(const QString &task) {
    emit recalculationRequested(QFileInfo(data.source).absolutePath(), displayedGeometry(), task);
}

} // namespace mf
