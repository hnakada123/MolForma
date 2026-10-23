#pragma once
#include "model/document.h"
namespace mf {
bool hasStereochemistryBackend();
void recordStereo(Document &d);
void recognizeResidues(Document &d);
} // namespace mf
