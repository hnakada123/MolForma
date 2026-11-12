// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QStringList>
namespace mf {
// Result files of an NWChem job: omit intermediate NEB paths and include the visualization
// outputs named in its versioned manifest (also in permanent_dir). Generic readers receive
// only this list and do not interpret job metadata.
QStringList nwchemResultFiles(const QString &directory);
} // namespace mf
