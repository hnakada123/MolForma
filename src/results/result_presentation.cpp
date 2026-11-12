// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/result_presentation.h"
#include "model/i18n.h"
#include <algorithm>
#include <cmath>

namespace mf {
namespace {
void requireIndex(int index, qsizetype size) {
    if (index < 0 || index >= size)
        fail(tr("結果の選択番号が範囲外です。"));
}
} // namespace
void ResultPresentation::reset(const ResultData &data) {
    stop();
    geometry = Geometry::Frame;
    selectedMode = -1;
    selectedFrame = data.selectedOrbital >= 0 ? data.orbitalFrame : int(data.frames.size()) - 1;
}
void ResultPresentation::selectFrame(const ResultData &data, int index) {
    requireIndex(index, data.frames.size());
    stop();
    selectedFrame = index;
    geometry = Geometry::Frame;
}
void ResultPresentation::selectMode(const ResultData &data, int index) {
    if (index != -1)
        requireIndex(index, data.modes.size());
    stop();
    selectedMode = index;
    geometry = index >= 0 && !data.modes[index].displacement.isEmpty() ? Geometry::Mode : Geometry::Frame;
}
void ResultPresentation::playFrames(const ResultData &data) {
    if (data.frames.size() < 2)
        fail(tr("再生には2つ以上の構造が必要です。"));
    selectFrame(data, selectedFrame);
    playback = ResultAnimation::Frames;
}
void ResultPresentation::playMode(const ResultData &data) {
    requireIndex(selectedMode, data.modes.size());
    if (data.modes[selectedMode].displacement.isEmpty())
        fail(tr("このモードには変位データがありません。"));
    selectMode(data, selectedMode);
    playback = ResultAnimation::Mode;
}
void ResultPresentation::stop() {
    playback = ResultAnimation::None;
    phase = 0;
}
void ResultPresentation::advance(const ResultData &data) {
    if (playback == ResultAnimation::Frames && !data.frames.isEmpty())
        selectedFrame = (selectedFrame + 1) % data.frames.size();
    else if (playback == ResultAnimation::Mode)
        phase += .18;
}
void ResultPresentation::setAmplitude(double value) {
    if (std::isfinite(value))
        displacement = std::clamp(value, .01, 2.);
}
const Document &ResultPresentation::referenceGeometry(const ResultData &data) const {
    if (showingMode())
        return data.vibrationGeometry;
    if (selectedFrame < 0 || selectedFrame >= data.frames.size())
        fail(tr("表示する構造がありません。"));
    return data.frames[selectedFrame].molecule;
}
Document ResultPresentation::visibleGeometry(const ResultData &data) const {
    if (displaced())
        return resultModeGeometry(data, selectedMode, phase, displacement);
    if (!showingMode() && (selectedFrame < 0 || selectedFrame >= data.frames.size()))
        return {};
    return referenceGeometry(data);
}
} // namespace mf
