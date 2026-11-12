// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>

namespace mf {
// The supervisor mode of the executable ("--run-job <job folder>"): runs the job that folder was just
// prepared for, records its states in run.json and stops it when stop.request appears in the folder
// (or on SIGTERM), so that the calculation goes on when the application that started it ends.
// Returns -1 when the arguments ask for something else.
int runJobMain(int argc, char **argv);
// The program that supervises runs: this executable, or the AppImage it was started from (which
// stays mounted for as long as one of its processes runs).
QString runSupervisorProgram();
} // namespace mf
