// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Everything about runs on a remote destination, for code outside src/nwchem: the destinations, the
// header templates of job scripts, what the record of a remote run says, its supervisor and the probe of
// a destination. Files of src/nwchem include the header of what they use.
#include "nwchem/job_script.h"
#include "nwchem/remote_host.h"
#include "nwchem/remote_probe.h"
#include "nwchem/remote_run.h"
#include "nwchem/remote_supervisor.h"
