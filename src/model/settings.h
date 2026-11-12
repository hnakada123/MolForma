// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QSettings>
#include <QString>

namespace mf {
// The format of the user's preferences. Windows and macOS keep them in an INI file (under %APPDATA%;
// main() puts it under ~/Library/Preferences on macOS) instead of the registry or CFPreferences, which
// ignore QSettings::setPath, so that setPath moves them too (tests, XDG_CONFIG_HOME in main()).
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
constexpr auto SettingsFormat = QSettings::IniFormat;
#else
constexpr auto SettingsFormat = QSettings::NativeFormat;
#endif
// The user's preferences ("環境設定" and what the application remembers between sessions): the one place
// that constructs their store. Every such object shares the values of the others in the process, so that
// a value set through one is read through another at once. Follows QSettings::setPath (tests).
inline QSettings userPreferences() {
    return QSettings(SettingsFormat, QSettings::UserScope, QStringLiteral("MolForma"),
                     QStringLiteral("MolForma"));
}
// The keys of the preferences. Their names and the formats of their values are those of the files users
// have: a key is never renamed, and no key of the store is written as a literal elsewhere.
namespace Preference {
// The window and its files.
inline constexpr const char *windowGeometry = "geometry";      // QMainWindow::saveGeometry.
inline constexpr const char *directory = "directory";          // The folder of the file opened last.
inline constexpr const char *recentFiles = "recentFiles";      // Paths, newest first (DocumentStorage).
inline constexpr const char *startupWater = "startupWater";    // A new window starts with a water molecule.
inline constexpr const char *language = "ui/language";         // "ja", "en" or empty: as the system.
inline constexpr const char *xtbExecutable = "xtb/executable"; // The xtb program: a name or a path.
// "1 分子": the tools, the palette and the layout.
inline constexpr const char *keepSelection = "keepSelection";
inline constexpr const char *keepTool = "keepTool";
inline constexpr const char *customParts = "parts"; // The user's own parts (a JSON array).
inline constexpr const char *moleculePalette = "molecule/palette";
inline constexpr const char *moleculeSplit = "molecule/split";   // QSplitter::saveState.
inline constexpr const char *moleculeLayout = "molecule/layout"; // view, both or table.
inline constexpr const char *sequenceCollapsed = "molecule/sequenceCollapsed";
inline constexpr const char *coordinateAttributes = "coordinates/attributes";
inline constexpr const char *symmetryTolerance = "symmetry/tolerance";
inline constexpr const char *solvationSolvent = "solvation/solvent";
inline constexpr const char *solvationCount = "solvation/count";
// The 3D view.
inline constexpr const char *viewRenderer = "view/renderer"; // "painter": QPainter instead of VTK.
inline constexpr const char *viewLabels = "view/labels";
inline constexpr const char *viewStyle = "view/style";
inline constexpr const char *viewHydrogenBonds = "view/hydrogen_bonds";
// "2 計算条件" and "4 入力プレビュー".
inline constexpr const char *nwchemPresets = "nwchem/presets"; // The user's presets (compact JSON).
inline constexpr const char *inputPreviewPaths = "inputPreview/showPaths";
// The results and the history.
inline constexpr const char *resultLabels = "results/labels";
inline constexpr const char *historyRoot = "history/root";
inline constexpr const char *historyFilesOpen = "history/files_open";
// Figures of the 3D view: the options chosen last.
inline constexpr const char *imageScale = "export/image_scale";
inline constexpr const char *imageBackground = "export/image_background";
inline constexpr const char *animationFormat = "export/animation_format";
inline constexpr const char *animationScale = "export/animation_scale";
inline constexpr const char *animationBackground = "export/animation_background";
// Running calculations. The run panel of the window:
inline constexpr const char *openResults = "execution/open_results";
inline constexpr const char *runPanelHeight = "execution/run_panel_height";
// The machine-wide execution environment (nwchem/execution_environment.h reads it as one snapshot):
inline constexpr const char *executionDefaults = "execution/defaults"; // Compact JSON, an object.
inline constexpr const char *remoteHosts = "execution/remotes";        // Compact JSON, an array.
inline constexpr const char *jobPrelude = "execution/job_prelude";
// The header template of the job scripts of a scheduler ("execution/job_header_slurm", "…_pbs").
inline QString jobHeader(const QString &scheduler) {
    return QStringLiteral("execution/job_header_") + scheduler;
}
// The job a session left to its supervisor, for the next session to follow (RunSession).
inline constexpr const char *runningJob = "execution/running";
} // namespace Preference
} // namespace mf
