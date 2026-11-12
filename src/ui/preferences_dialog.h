// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonObject>
#include <functional>
class QWidget;
namespace mf {
class UiServices;
// "環境設定": what MolForma keeps machine-wide. The execution settings new documents start with (the form
// of "3 実行設定", with the remote destinations), the preparation lines and scheduler headers of job
// scripts, the xtb program, and what a new window shows (the molecule at startup, the renderer, the
// language). OK checks the values with the rules of "3 実行設定" and saves them: the execution's through
// nwchem/execution_environment.h, the others through the preferences of `services`.
// currentExecution: the execution settings of the open document, for "現在の実行設定を読み込む".
// parent: the window, whose palette colors the dialog. MCP cannot operate it (mcpProtected).
void preferencesDialog(QWidget *parent, UiServices &services,
                       const std::function<QJsonObject()> &currentExecution);
} // namespace mf
