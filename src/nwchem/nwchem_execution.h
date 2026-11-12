// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Everything about running a calculation from this computer, for code outside src/nwchem: the settings
// and their checks, the plan and bundle of a job, its job script, the processes of a run, the supervisor
// and NwchemRun. Files of src/nwchem include the header of what they use.
#include "nwchem/execution_plan.h"
#include "nwchem/execution_settings.h"
#include "nwchem/job_script.h"
#include "nwchem/nwchem_run.h"
#include "nwchem/run_process.h"
#include "nwchem/run_supervisor.h"
