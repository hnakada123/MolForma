// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>
#include <QStringList>
class QTableWidget;
class QWidget;
namespace mf {
class ChartView;
class UiServices;
} // namespace mf

// Internal to the result viewer: what results_panel.cpp and the views of its tabs
// (result_energies_view, result_optimization_view, result_path_view, result_properties_view,
// result_excitations_view) share with one another, each with one definition in result_detail.cpp.
namespace mf::result_detail {
// The tables of the history: no grid, a muted header, the selection in the tint. The chosen row stays
// in sight as the table gets lower.
QTableWidget *table(const QStringList &labels, const QString &name);
// Numbers in fixed-width digits at the right, so their decimal points line up.
void row(QTableWidget *t, int index, const QStringList &values);
QString real(double value);
QString fixed(double value, int decimals);
// A value to read (the overview, the summary) with the minus sign of the history and the run panel; the
// cells of the tables keep "-", so that what is taken from them stays a number.
QString shownReal(double value);
QString shownFixed(double value, int decimals);
// A card of the summary: what, the value and a line about it. With a tab it is a button to it.
QWidget *summaryTile(const QString &title, const QString &value, const QString &caption, const QString &tone,
                     bool button);
// The style rules of those cards (chromeRules tokens): of the summary and of the convergence criteria.
extern const char *const TileRules;
// A chart of the viewer saves itself as a figure from its context menu; `parent` is the dialog's.
void setFigureExport(ChartView *chart, UiServices &ui, QWidget *parent);
} // namespace mf::result_detail
