// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"

// What test executables of every area share. The helpers of one area are in its own header:
// nwchem_support.h (calculation settings), run_support.h (runs and their job folders),
// result_support.h (result files) and mcp_support.h (MCP messages).

// Executables carry .exe on Windows.
#ifdef Q_OS_WIN
#define EXE ".exe"
#else
#define EXE ""
#endif

inline mf::Document water() {
    return mf::Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
}
