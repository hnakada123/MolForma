// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QFile>
#include <QImage>
#include <QString>
#include <functional>
class QWidget;
namespace mf {
class UiServices;
// The background of a picture of the 3D view: as on the screen, white, or transparent (PNG, GIF).
enum class ImageBackground { Screen, White, Transparent };
// A picture of the 3D view: the size of the view times `scale` (whole numbers, so that VTK can render
// it in tiles of the view's size), and its background.
struct ImageOptions {
    int scale = 2;
    ImageBackground background = ImageBackground::Screen;
};
// Asks for the options of a picture of a view of this size. The scale and background chosen last come
// from the preferences of `services` and go back there when the dialog is accepted.
bool imageOptionsDialog(QWidget *parent, UiServices &services, const QSize &view, ImageOptions &options);
// A series of pictures: "gif" (an animated GIF, written by MolForma), "mp4" (H.264 through the ffmpeg
// program found in PATH) or "png" (numbered PNG files in a folder). bounce plays the frames forward and
// back, so that a path or a vibration loops without a jump.
struct AnimationOptions {
    QString format = "gif";
    int scale = 1;
    ImageBackground background = ImageBackground::Screen;
    double fps = 10;
    bool bounce = false;
};
// The ffmpeg program in PATH, or empty.
QString ffmpegExecutable();
// The format, scale and background are remembered like those of a picture; fps and bounce are the
// caller's.
bool animationOptionsDialog(QWidget *parent, UiServices &services, const QSize &view,
                            AnimationOptions &options, double fps, bool bounce);
// Writes `count` frames (frame(i) gives the picture of frame i) to `target`: the file of a GIF or MP4,
// or the folder of PNG files (named prefix_0001.png, ...). Shows its progress and can be cancelled;
// returns false then. Fails with the reason.
bool writeAnimation(QWidget *parent, const QString &target, const QString &prefix,
                    const AnimationOptions &options, int count, const std::function<QImage(int)> &frame);
// Writes an animated GIF frame by frame, each with its own palette of at most 255 colors (median cut) and,
// for pictures with transparent pixels, a transparent color. The frames have the size of the first one.
class GifWriter {
  public:
    // delay: between frames, in hundredths of a second; loop: play again from the start, forever.
    bool open(const QString &path, int delay, bool loop = true);
    bool add(const QImage &frame);
    bool close();

  private:
    QFile file;
    QSize size;
    int delay = 10;
    bool loop = true;
};
// The file dialog filter of figures: PNG, PDF and, where Qt SVG is available, SVG.
QString figureFilter();
// Asks where to save a figure; the suffix of the chosen kind is added when the name has none. Empty when
// cancelled.
QString askFigurePath(QWidget *parent, const QString &title, const QString &name);
// Saves a widget (a chart) as a figure, by the suffix of the path: PNG at twice its size, or the
// vector formats PDF and SVG, where text stays text. Fails with the reason.
void saveFigure(QWidget *figure, const QString &path);
} // namespace mf
