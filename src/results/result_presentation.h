// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"

namespace mf {
enum class ResultAnimation { None, Frames, Mode };
// Selection and playback of a result, independent of widgets. The picture, exported reference
// geometry and controls all follow this state; changing a selection always stops playback.
class ResultPresentation {
  public:
    void reset(const ResultData &data);
    void selectFrame(const ResultData &data, int index);
    void selectMode(const ResultData &data, int index); // -1 closes the mode.
    void playFrames(const ResultData &data);
    void playMode(const ResultData &data);
    void stop();
    void advance(const ResultData &data);
    void setAmplitude(double value);
    void setPhase(double value) {
        phase = value;
    } // Samples of a mode when exporting an animation.
    int frame() const {
        return selectedFrame;
    }
    int mode() const {
        return selectedMode;
    }
    double amplitude() const {
        return displacement;
    }
    bool showingMode() const {
        return geometry == Geometry::Mode;
    }
    bool displaced() const {
        return showingMode() && phase != 0;
    }
    ResultAnimation animation() const {
        return playback;
    }
    const Document &referenceGeometry(const ResultData &data) const;
    Document visibleGeometry(const ResultData &data) const;

  private:
    enum class Geometry { Frame, Mode };
    Geometry geometry = Geometry::Frame;
    ResultAnimation playback = ResultAnimation::None;
    int selectedFrame = -1, selectedMode = -1;
    double phase = 0, displacement = .3;
};
} // namespace mf
