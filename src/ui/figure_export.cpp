// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/figure_export.h"
#include "model/document.h"
#include "model/i18n.h"
#include "model/settings.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#ifdef MOLFORMA_HAS_QTSVG
#include <QSvgGenerator>
#endif

namespace mf {
namespace {
QSettings preferences() {
    return QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma");
}
ImageBackground backgroundOf(const QString &key) {
    return key == "white"         ? ImageBackground::White
           : key == "transparent" ? ImageBackground::Transparent
                                  : ImageBackground::Screen;
}
QString backgroundKey(ImageBackground background) {
    return background == ImageBackground::White         ? "white"
           : background == ImageBackground::Transparent ? "transparent"
                                                        : "screen";
}
QComboBox *backgroundChoice(ImageBackground chosen) {
    auto *combo = new QComboBox;
    combo->setObjectName("figureBackground");
    combo->addItem(tr("画面と同じ"), "screen");
    combo->addItem(tr("白"), "white");
    combo->addItem(tr("透明"), "transparent");
    combo->setCurrentIndex(std::max(0, combo->findData(backgroundKey(chosen))));
    return combo;
}
// The largest side of a picture, which VTK and most image viewers handle.
constexpr int LargestSide = 16384;
QSpinBox *scaleChoice(const QSize &view, int chosen, int most) {
    auto *spin = new QSpinBox;
    spin->setObjectName("figureScale");
    const int fits = std::max(1, LargestSide / std::max({1, view.width(), view.height()}));
    spin->setRange(1, std::min(most, fits));
    spin->setValue(std::clamp(chosen, 1, spin->maximum()));
    spin->setSuffix(" ×");
    return spin;
}
} // namespace
bool imageOptionsDialog(QWidget *parent, const QSize &view, ImageOptions &options) {
    auto settings = preferences();
    QDialog dialog(parent);
    dialog.setObjectName("imageOptionsDialog");
    dialog.setWindowTitle(tr("3D画面の画像"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *scale = scaleChoice(view, settings.value("export/image_scale", 2).toInt(), 8);
    form->addRow(tr("倍率（画面の大きさに対して）"), scale);
    auto *background = backgroundChoice(backgroundOf(settings.value("export/image_background").toString()));
    form->addRow(tr("背景"), background);
    auto *size = new QLabel;
    size->setObjectName("figureSize");
    form->addRow(tr("画像の大きさ"), size);
    const auto showSize = [&] {
        size->setText(
            tr("%1 × %2 ピクセル").arg(view.width() * scale->value()).arg(view.height() * scale->value()));
    };
    QObject::connect(scale, &QSpinBox::valueChanged, &dialog, showSize);
    showSize();
    auto *note =
        new QLabel(tr("選択・候補・配位サイトなどの編集用の印は描きません。ラベル・計測値・矢印・等値面は"
                      "画面のとおりに描きます。透明な背景はPNGで保存します。"));
    note->setWordWrap(true);
    note->setProperty("muted", true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    options.scale = scale->value();
    options.background = backgroundOf(background->currentData().toString());
    settings.setValue("export/image_scale", options.scale);
    settings.setValue("export/image_background", backgroundKey(options.background));
    return true;
}
QString ffmpegExecutable() {
    return QStandardPaths::findExecutable("ffmpeg");
}
bool animationOptionsDialog(QWidget *parent, const QSize &view, AnimationOptions &options, double fps,
                            bool bounce) {
    auto settings = preferences();
    QDialog dialog(parent);
    dialog.setObjectName("animationOptionsDialog");
    dialog.setWindowTitle(tr("動画・連番画像"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *format = new QComboBox;
    format->setObjectName("animationFormat");
    format->addItem(tr("GIFアニメーション"), "gif");
    const bool ffmpeg = !ffmpegExecutable().isEmpty();
    format->addItem(ffmpeg ? tr("MP4動画（ffmpeg）") : tr("MP4動画（ffmpegが見つかりません）"), "mp4");
    format->addItem(tr("PNG連番（フォルダー）"), "png");
    if (auto *items = qobject_cast<QStandardItemModel *>(format->model()); items && !ffmpeg)
        items->item(1)->setEnabled(false);
    format->setCurrentIndex(
        std::max(0, format->findData(settings.value("export/animation_format", "gif").toString())));
    if (!ffmpeg && format->currentData() == "mp4")
        format->setCurrentIndex(0);
    form->addRow(tr("形式"), format);
    auto *scale = scaleChoice(view, settings.value("export/animation_scale", 1).toInt(), 4);
    form->addRow(tr("倍率（画面の大きさに対して）"), scale);
    auto *background =
        backgroundChoice(backgroundOf(settings.value("export/animation_background").toString()));
    form->addRow(tr("背景"), background);
    auto *rate = new QDoubleSpinBox;
    rate->setObjectName("animationFps");
    rate->setRange(1, 60);
    rate->setDecimals(1);
    rate->setValue(fps);
    rate->setSuffix(tr(" 枚/秒"));
    form->addRow(tr("速さ"), rate);
    auto *back = new QCheckBox(tr("往復させる（最後まで進んだら戻る）"));
    back->setObjectName("animationBounce");
    back->setChecked(bounce);
    form->addRow(QString(), back);
    auto *note = new QLabel;
    note->setWordWrap(true);
    note->setProperty("muted", true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    const auto update = [&] {
        const auto kind = format->currentData().toString();
        // MP4 has no transparency; GIF keeps a transparent color (its edges are not smoothed).
        if (kind == "mp4" && background->currentData() == "transparent")
            background->setCurrentIndex(background->findData("white"));
        if (auto *items = qobject_cast<QStandardItemModel *>(background->model()))
            items->item(2)->setEnabled(kind != "mp4");
        note->setText(kind == "gif"
                          ? tr("GIFは1枚ごとに最大255色です。透明な背景では輪郭がぎざぎざになります。")
                      : kind == "mp4" ? tr("H.264（yuv420p）のMP4です。ffmpegで作成し、透明な背景には"
                                           "できません。")
                                      : tr("番号付きのPNGをフォルダーに保存します。"));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(kind != "mp4" || ffmpeg);
    };
    QObject::connect(format, &QComboBox::currentIndexChanged, &dialog, update);
    QObject::connect(background, &QComboBox::currentIndexChanged, &dialog, update);
    update();
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    options.format = format->currentData().toString();
    options.scale = scale->value();
    options.background = backgroundOf(background->currentData().toString());
    options.fps = rate->value();
    options.bounce = back->isChecked();
    settings.setValue("export/animation_format", options.format);
    settings.setValue("export/animation_scale", options.scale);
    settings.setValue("export/animation_background", backgroundKey(options.background));
    return true;
}
bool writeAnimation(QWidget *parent, const QString &target, const QString &prefix,
                    const AnimationOptions &options, int count, const std::function<QImage(int)> &frame) {
    if (count < 1)
        fail(tr("書き出す構造がありません。"));
    QVector<int> order;
    for (int i = 0; i < count; ++i)
        order << i;
    for (int i = count - 2; options.bounce && i > 0; --i)
        order << i;
    QProgressDialog progress(tr("書き出しています…"), tr("中止"), 0, int(order.size()) + 1, parent);
    progress.setObjectName("animationProgress");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    const auto picture = [&](int k) {
        auto image = frame(order[k]);
        if (options.format == "mp4" && image.hasAlphaChannel()) { // No transparency in H.264.
            QImage opaque(image.size(), QImage::Format_RGB32);
            opaque.fill(Qt::white);
            QPainter(&opaque).drawImage(0, 0, image);
            image = opaque;
        }
        return image;
    };
    if (options.format == "png") {
        if (!QDir().mkpath(target))
            fail(tr("フォルダーを作成できません: %1").arg(target));
        for (int k = 0; k < order.size(); ++k) {
            progress.setValue(k);
            if (progress.wasCanceled())
                return false;
            const auto file =
                QDir(target).filePath(QString("%1_%2.png").arg(prefix).arg(k + 1, 4, 10, QChar('0')));
            if (!picture(k).save(file))
                fail(tr("画像を保存できません: %1").arg(file));
        }
    } else if (options.format == "gif") {
        GifWriter gif;
        if (!gif.open(target, std::max(2, qRound(100 / options.fps))))
            fail(tr("ファイルを保存できません: %1").arg(target));
        for (int k = 0; k < order.size(); ++k) {
            progress.setValue(k);
            if (progress.wasCanceled()) {
                gif.close();
                QFile::remove(target);
                return false;
            }
            if (!gif.add(picture(k)))
                fail(tr("ファイルを保存できません: %1").arg(target));
        }
        if (!gif.close())
            fail(tr("ファイルを保存できません: %1").arg(target));
    } else if (options.format == "mp4") {
        const auto ffmpeg = ffmpegExecutable();
        if (ffmpeg.isEmpty())
            fail(tr("ffmpegが見つかりません。MP4にはPATHにffmpegが必要です。"));
        QTemporaryDir frames;
        if (!frames.isValid())
            fail(tr("一時ディレクトリを作成できません。"));
        for (int k = 0; k < order.size(); ++k) {
            progress.setValue(k);
            if (progress.wasCanceled())
                return false;
            if (!picture(k).save(frames.filePath(QString("frame_%1.png").arg(k + 1, 5, 10, QChar('0')))))
                fail(tr("画像を保存できません: %1").arg(frames.path()));
        }
        progress.setLabelText(tr("ffmpegで動画を作成しています…"));
        QProcess process;
        process.setProgram(ffmpeg);
        // Even sizes for yuv420p; the frames as they are, at the chosen rate.
        process.setArguments({"-y", "-loglevel", "error", "-framerate", QString::number(options.fps), "-i",
                              frames.filePath("frame_%05d.png"), "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2",
                              "-c:v", "libx264", "-pix_fmt", "yuv420p", "-movflags", "+faststart", target});
        process.start();
        if (!process.waitForStarted())
            fail(tr("ffmpegを起動できません: %1").arg(process.errorString()));
        while (!process.waitForFinished(100)) {
            QApplication::processEvents();
            if (progress.wasCanceled()) {
                process.kill();
                process.waitForFinished(2000);
                QFile::remove(target);
                return false;
            }
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            fail(tr("ffmpegで動画を作れません: %1")
                     .arg(QString::fromLocal8Bit(process.readAllStandardError()).trimmed().right(600)));
    } else
        fail(tr("書き出しの形式が不正です。"));
    progress.setValue(progress.maximum());
    return true;
}
namespace {
void put16(QByteArray &out, int value) {
    out.append(char(value & 0xff));
    out.append(char((value >> 8) & 0xff));
}
// The picture's colors reduced to at most 255 by median cut over a histogram of 5 bits per channel; each
// pixel becomes the index of its color, 255 for transparent pixels.
struct Indexed {
    QByteArray pixels;
    std::array<QRgb, 256> palette{};
    bool transparent = false;
};
Indexed indexed(const QImage &source) {
    const auto image = source.convertToFormat(QImage::Format_ARGB32);
    struct Bin {
        qint64 count = 0, r = 0, g = 0, b = 0;
    };
    QVector<Bin> bins(32768);
    Indexed result;
    const auto binOf = [](QRgb c) { return (qRed(c) >> 3) << 10 | (qGreen(c) >> 3) << 5 | (qBlue(c) >> 3); };
    for (int y = 0; y < image.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) < 128) {
                result.transparent = true;
                continue;
            }
            auto &bin = bins[binOf(line[x])];
            ++bin.count;
            bin.r += qRed(line[x]);
            bin.g += qGreen(line[x]);
            bin.b += qBlue(line[x]);
        }
    }
    // Boxes of occupied bins, split at the median of their longest side until there are 255.
    using Box = QVector<int>;
    QVector<Box> boxes(1);
    for (int i = 0; i < bins.size(); ++i)
        if (bins[i].count)
            boxes[0] << i;
    const auto channel = [](int bin, int axis) { return (bin >> (10 - 5 * axis)) & 31; };
    while (boxes.size() < 255) {
        int best = -1, axis = 0;
        qint64 weight = 0;
        for (int i = 0; i < boxes.size(); ++i) {
            if (boxes[i].size() < 2)
                continue;
            qint64 count = 0;
            int widest = -1, along = 0;
            for (int a = 0; a < 3; ++a) {
                int low = 31, high = 0;
                for (int bin : boxes[i]) {
                    low = std::min(low, channel(bin, a));
                    high = std::max(high, channel(bin, a));
                }
                if (high - low > widest) {
                    widest = high - low;
                    along = a;
                }
            }
            for (int bin : boxes[i])
                count += bins[bin].count;
            if (widest > 0 && count * (widest + 1) > weight) {
                weight = count * (widest + 1);
                best = i;
                axis = along;
            }
        }
        if (best < 0)
            break;
        auto box = boxes[best];
        std::sort(box.begin(), box.end(), [&](int a, int b) { return channel(a, axis) < channel(b, axis); });
        qint64 total = 0, half = 0;
        for (int bin : box)
            total += bins[bin].count;
        int cut = 1;
        for (int i = 0; i + 1 < box.size(); ++i) {
            half += bins[box[i]].count;
            if (half * 2 >= total) {
                cut = i + 1;
                break;
            }
        }
        boxes[best] = box.mid(0, cut);
        boxes << box.mid(cut);
    }
    QVector<quint8> lookup(bins.size(), 0);
    for (int i = 0; i < boxes.size(); ++i) {
        qint64 count = 0, r = 0, g = 0, b = 0;
        for (int bin : boxes[i]) {
            lookup[bin] = quint8(i);
            count += bins[bin].count;
            r += bins[bin].r;
            g += bins[bin].g;
            b += bins[bin].b;
        }
        if (count)
            result.palette[i] = qRgb(int(r / count), int(g / count), int(b / count));
    }
    result.pixels.resize(qsizetype(image.width()) * image.height());
    auto *out = reinterpret_cast<quint8 *>(result.pixels.data());
    for (int y = 0; y < image.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            *out++ = qAlpha(line[x]) < 128 ? 255 : lookup[binOf(line[x])];
    }
    return result;
}
// GIF's LZW for 8-bit indices, codes packed from the lowest bit up.
QByteArray lzw(const QByteArray &pixels) {
    constexpr int minimum = 8, clear = 1 << minimum;
    QByteArray bytes;
    quint32 buffer = 0;
    int bits = 0, size = minimum + 1, last = clear + 1;
    const auto write = [&](int code, int width) {
        buffer |= quint32(code) << bits;
        bits += width;
        while (bits >= 8) {
            bytes.append(char(buffer & 0xff));
            buffer >>= 8;
            bits -= 8;
        }
    };
    std::vector<quint16> tree(4096 * 256, 0); // The code of (code, next index), 0 for none.
    write(clear, size);
    int current = -1;
    for (const char c : pixels) {
        const int next = quint8(c);
        if (current < 0) {
            current = next;
            continue;
        }
        if (const int known = tree[size_t(current) * 256 + next]) {
            current = known;
            continue;
        }
        write(current, size);
        tree[size_t(current) * 256 + next] = quint16(++last);
        if (last >= (1 << size))
            ++size;
        if (last == 4095) { // A full dictionary starts again.
            write(clear, size);
            std::fill(tree.begin(), tree.end(), 0);
            size = minimum + 1;
            last = clear + 1;
        }
        current = next;
    }
    write(current, size);
    write(clear, size);
    write(clear + 1, minimum + 1);
    if (bits > 0)
        bytes.append(char(buffer & 0xff));
    return bytes;
}
} // namespace
bool GifWriter::open(const QString &path, int frameDelay, bool repeat) {
    file.setFileName(path);
    size = {};
    delay = frameDelay;
    loop = repeat;
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate);
}
bool GifWriter::add(const QImage &frame) {
    if (!file.isOpen() || frame.isNull())
        return false;
    QByteArray out;
    if (size.isEmpty()) {
        size = frame.size();
        if (size.width() > 65535 || size.height() > 65535)
            return false;
        out.append("GIF89a");
        put16(out, size.width());
        put16(out, size.height());
        // No global colors; the background is the transparent index of the frames (255), so that the
        // place a frame clears shows through where the next one is transparent.
        out.append(char(0)).append(char(255)).append(char(0));
        if (loop) {
            out.append("\x21\xff\x0b", 3).append("NETSCAPE2.0").append("\x03\x01", 2);
            put16(out, 0); // Forever.
            out.append(char(0));
        }
    }
    const auto image = frame.size() == size ? frame : frame.scaled(size);
    const auto colors = indexed(image);
    // Each frame replaces the one before: the place is cleared after it (disposal 2), so that the
    // transparent pixels of the next one show the background, not this frame. Every frame names index 255
    // transparent (unused by opaque ones): decoders such as Qt's clear to the transparent color only then.
    out.append("\x21\xf9\x04", 3);
    out.append(char(2 << 2 | 1));
    put16(out, delay);
    out.append(char(255)).append(char(0));
    out.append(char(0x2c));
    put16(out, 0);
    put16(out, 0);
    put16(out, size.width());
    put16(out, size.height());
    out.append(char(0x87)); // A table of 256 colors of its own.
    for (const auto color : colors.palette)
        out.append(char(qRed(color))).append(char(qGreen(color))).append(char(qBlue(color)));
    out.append(char(8));
    const auto data = lzw(colors.pixels);
    for (qsizetype i = 0; i < data.size(); i += 255) {
        const auto block = data.mid(i, 255);
        out.append(char(block.size())).append(block);
    }
    out.append(char(0));
    return file.write(out) == out.size();
}
bool GifWriter::close() {
    if (!file.isOpen())
        return false;
    const bool written = file.write(";", 1) == 1;
    file.close();
    return written && !size.isEmpty();
}
QString figureFilter() {
#ifdef MOLFORMA_HAS_QTSVG
    return "PNG (*.png);;PDF (*.pdf);;SVG (*.svg)";
#else
    return "PNG (*.png);;PDF (*.pdf)";
#endif
}
QString askFigurePath(QWidget *parent, const QString &title, const QString &name) {
    QString chosen;
    auto path = QFileDialog::getSaveFileName(parent, title, name, figureFilter(), &chosen);
    if (!path.isEmpty() && QFileInfo(path).suffix().isEmpty())
        path += chosen.startsWith("PDF") ? ".pdf" : chosen.startsWith("SVG") ? ".svg" : ".png";
    return path;
}
void saveFigure(QWidget *figure, const QString &path) {
    const auto suffix = QFileInfo(path).suffix().toLower();
    // Units of the widget's own pixels, so that text keeps its size beside the lines.
    const int dpi = std::max(1, figure->logicalDpiX());
    if (suffix == "pdf") {
        QPdfWriter writer(path);
        writer.setResolution(dpi);
        writer.setPageSize(
            QPageSize(QSizeF(figure->width() * 72. / dpi, figure->height() * 72. / dpi), QPageSize::Point));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setCreator("MolForma");
        QPainter painter;
        if (!painter.begin(&writer))
            fail(tr("図を保存できません: %1").arg(path));
        figure->render(&painter);
        painter.end();
        if (!QFileInfo(path).isFile())
            fail(tr("図を保存できません: %1").arg(path));
        return;
    }
#ifdef MOLFORMA_HAS_QTSVG
    if (suffix == "svg") {
        QSvgGenerator generator;
        generator.setFileName(path);
        generator.setSize(figure->size());
        generator.setViewBox(QRect(QPoint(), figure->size()));
        generator.setResolution(dpi);
        generator.setTitle("MolForma");
        QPainter painter;
        if (!painter.begin(&generator))
            fail(tr("図を保存できません: %1").arg(path));
        figure->render(&painter);
        painter.end();
        if (!QFileInfo(path).isFile())
            fail(tr("図を保存できません: %1").arg(path));
        return;
    }
#endif
    if (suffix != "png")
        fail(tr("図はPNG・PDF%1で保存できます。")
                 .arg(figureFilter().contains("svg") ? tr("・SVG") : QString()));
    QImage image(figure->size() * 2, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(2);
    image.fill(Qt::transparent);
    figure->render(&image);
    if (!image.save(path))
        fail(tr("図を保存できません: %1").arg(path));
}
} // namespace mf
