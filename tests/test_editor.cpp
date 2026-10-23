#include "library.h"
#include "main_window.h"
#include "periodic_table.h"
#include "template_delegate.h"
#include <QComboBox>
#include <QDir>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
using namespace mf;
class InspectableTemplateDelegate : public TemplateDelegate {
  public:
    using TemplateDelegate::initStyleOption;
};
class EditorTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, "/tmp/molforma-test-settings");
    }
    void candidateUndoRedo() {
        MainWindow w;
        Document d;
        placeTemplate(d, findTemplate("skeleton:water"), Vec::Zero());
        w.loadDocument(d);
        auto id = d.atoms[0].id;
        w.edit("削除", [id](Document &d) { d.remove({id}); }, true);
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.document().atoms.size(), 3);
        QCOMPARE(w.displayDocument().atoms.size(), 2);
        w.cancelCandidate();
        QCOMPARE(w.document().atoms.size(), 3);
        w.edit("削除", [id](Document &d) { d.remove({id}); }, true);
        w.confirmCandidate();
        QCOMPARE(w.document().atoms.size(), 2);
        w.history()->undo();
        QCOMPARE(w.document().atoms.size(), 3);
        w.history()->redo();
        QCOMPARE(w.document().atoms.size(), 2);
    }
    void paletteAndCanvas() {
        MainWindow w;
        w.loadDocument(Document());
        w.show();
        QTest::qWait(40);
        auto *tool = w.findChild<QComboBox *>("toolSelector"),
             *element = w.findChild<QComboBox *>("elementSelector");
        element->setCurrentIndex(element->findData("C"));
        tool->setCurrentIndex(tool->findData("atom"));
        QTest::mouseClick(w.canvas(), Qt::LeftButton, Qt::NoModifier, w.canvas()->rect().center());
        QCOMPARE(w.document().formula(), QString("CH4"));
        auto id = w.document().atoms.first().id;
        tool->setCurrentIndex(0);
        QTest::mouseClick(w.canvas(), Qt::LeftButton, Qt::NoModifier,
                          w.canvas()->atomScreenPosition(id).toPoint());
        QVERIFY(w.canvas()->selection().contains(id));
        QTest::keyClick(w.canvas(), Qt::Key_Delete);
        QVERIFY(w.hasCandidate());
        QTest::keyClick(w.canvas(), Qt::Key_Escape);
        QVERIFY(!w.hasCandidate());
        QCOMPARE(w.document().formula(), QString("CH4"));
    }
    void rotationControlsLayout_data() {
        QTest::addColumn<int>("tab");
        QTest::addColumn<int>("width");
        for (int tab : {0, 6})
            for (int width : {900, 1380, 2200})
                QTest::newRow(qPrintable(QString("%1-%2").arg(tab == 0 ? "molecule" : "results").arg(width)))
                    << tab << width;
    }
    void rotationControlsLayout() {
        QFETCH(int, tab);
        QFETCH(int, width);
        MainWindow w;
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        tabs->setCurrentIndex(tab);
        w.resize(width, 900);
        w.show();
        QTest::qWait(40);
        const auto labels = tabs->currentWidget()->findChildren<QLabel *>();
        QRect previous, controls;
        int sliderWidth = 0;
        for (const auto &text : {QString("左右回転"), QString("上下回転"), QString("画面内回転")}) {
            QLabel *label = nullptr;
            for (auto *candidate : labels)
                if (candidate->text() == text && qobject_cast<QSlider *>(candidate->buddy()))
                    label = candidate;
            QVERIFY(label);
            auto *slider = qobject_cast<QSlider *>(label->buddy());
            QVERIFY(label->isVisible());
            QVERIFY(slider->isVisible());
            const QRect labelRect(label->mapTo(&w, QPoint()), label->size());
            const QRect sliderRect(slider->mapTo(&w, QPoint()), slider->size());
            QCOMPARE(labelRect.left(), sliderRect.left());
            QVERIFY(labelRect.bottom() < sliderRect.top());
            QVERIFY(sliderRect.top() - labelRect.bottom() <= 6);
            QVERIFY(label->width() >= label->sizeHint().width());
            if (!previous.isNull()) {
                QVERIFY(sliderRect.left() - previous.right() >= 12);
                QVERIFY(std::abs(slider->width() - sliderWidth) <= 1);
            }
            previous = sliderRect;
            sliderWidth = slider->width();
            controls = controls.united(labelRect).united(sliderRect);
        }
        const auto artifacts = qEnvironmentVariable("MOLFORMA_ROTATION_ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            const auto path = QDir(artifacts).filePath(QString::fromLatin1(QTest::currentDataTag()));
            QVERIFY(w.grab().save(path + ".png"));
            QVERIFY(w.grab(controls.adjusted(-4, -4, 4, 4)).save(path + "-controls.png"));
        }
    }
    void skeletonPalette() {
        MainWindow w;
        w.loadDocument(Document());
        w.show();
        auto *category = w.findChild<QComboBox *>("paletteCategory");
        auto *search = w.findChild<QLineEdit *>("paletteSearch");
        auto *palette = w.findChild<QListWidget *>("templatePalette");
        category->setCurrentIndex(category->findData("skeleton"));
        QTest::qWait(40);
        QCOMPARE(palette->count(), 19);
        for (int row = 0; row < palette->count(); ++row) {
            QVERIFY(!palette->item(row)->text().contains('\n'));
            QCOMPARE(palette->visualItemRect(palette->item(row)).height(), 48);
        }
        search->setText("benzene");
        QCOMPARE(palette->count(), 1);
        QCOMPARE(palette->item(0)->text(), QString("ベンゼン"));
        palette->clearSelection();
        auto image = palette->viewport()->grab().toImage();
        int atomPixels = 0;
        for (int y = 0; y < qRound(48 * image.devicePixelRatio()); ++y)
            for (int x = 0; x < qRound(64 * image.devicePixelRatio()); ++x)
                if (image.pixelColor(x, y) == QColor("#475569"))
                    ++atomPixels;
        QVERIFY2(atomPixels > 20, "The molecule thumbnail is missing from the palette row");
        QVERIFY(w.document().atoms.isEmpty());
        QTest::mouseClick(palette->viewport(), Qt::LeftButton, Qt::NoModifier,
                          palette->visualItemRect(palette->item(0)).center());
        QTest::mouseClick(w.canvas(), Qt::LeftButton, Qt::NoModifier, w.canvas()->rect().center());
        QCOMPARE(w.document().formula(), QString("C6H6"));
        w.history()->undo();
        QVERIFY(w.document().atoms.isEmpty());
        category->setCurrentIndex(category->findData("amino"));
        search->setText("Histidine");
        QCOMPARE(palette->count(), 3);
        QSet<QString> names;
        for (int row = 0; row < palette->count(); ++row)
            names.insert(palette->item(row)->text());
        QCOMPARE(names, QSet<QString>({"ヒスチジン（HIE）", "ヒスチジン（HID）", "ヒスチジン（HIP, +1）"}));
    }
    void periodicTableLayoutAndSearch() {
        PeriodicTableDialog dialog("C");
        dialog.show();
        QTest::qWait(40);
        const auto buttons = dialog.findChildren<QAbstractButton *>(QRegularExpression("^periodicElement_"));
        QCOMPARE(buttons.size(), 118);
        auto *grid = dialog.findChild<QGridLayout *>("periodicTableGrid");
        QVERIFY(grid);
        auto check = [&](const QString &symbol, int row, int column) {
            auto *button = dialog.findChild<QAbstractButton *>("periodicElement_" + symbol);
            QVERIFY(button);
            QCOMPARE(grid->itemAtPosition(row, column)->widget(), button);
            QVERIFY(button->isEnabled());
            QVERIFY(button->width() >= 44);
        };
        check("H", 1, 1);
        check("He", 1, 18);
        check("C", 2, 14);
        check("Fe", 4, 8);
        check("La", 9, 3);
        check("Lu", 9, 17);
        check("Ac", 10, 3);
        check("U", 10, 6);
        check("Lr", 10, 17);
        check("Og", 7, 18);
        auto *search = dialog.findChild<QLineEdit *>("periodicTableSearch");
        auto enabled = [&] {
            QStringList symbols;
            for (auto *button : buttons)
                if (button->isEnabled())
                    symbols << button->text();
            return symbols;
        };
        for (const auto &query : {"ウラン", "uranium", "92", "u"}) {
            search->setText(QString::fromUtf8(query));
            QCOMPARE(enabled(), QStringList{"U"});
        }
        search->setText("118");
        QCOMPARE(enabled(), QStringList{"Og"});
        search->setText("存在しない元素");
        QVERIFY(enabled().isEmpty());
        QTest::keyClick(search, Qt::Key_Return);
        QVERIFY(dialog.isVisible());
        search->clear();
        QCOMPARE(enabled().size(), 118);
        auto *h = dialog.findChild<QAbstractButton *>("periodicElement_H");
        auto *he = dialog.findChild<QAbstractButton *>("periodicElement_He");
        h->setFocus();
        QTest::keyClick(h, Qt::Key_Right);
        QCOMPARE(dialog.focusWidget(), he);
        if (auto path = qEnvironmentVariable("MOLFORMA_PERIODIC_SCREENSHOT"); !path.isEmpty())
            QVERIFY(dialog.grab().save(path));
        search->setText("118");
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.selectedElement(), QString("Og"));
    }
    void periodicTablePlacement() {
        MainWindow w;
        w.loadDocument(Document());
        w.show();
        QTest::qWait(40);
        auto *category = w.findChild<QComboBox *>("paletteCategory");
        auto *palette = w.findChild<QListWidget *>("templatePalette");
        auto *open = w.findChild<QPushButton *>("openPeriodicTable");
        category->setCurrentIndex(category->findData("atom"));
        QVERIFY(open->isVisible());
        QCOMPARE(palette->count(), 1);
        QTimer::singleShot(0, &w, [&] {
            auto *dialog = w.findChild<PeriodicTableDialog *>("periodicTableDialog");
            if (dialog)
                dialog->findChild<QAbstractButton *>("periodicElement_U")->click();
        });
        open->click();
        QCOMPARE(palette->count(), 1);
        QCOMPARE(palette->item(0)->data(Qt::UserRole).toString(), QString("atom:U"));
        QCOMPARE(palette->item(0)->text(), QString("ウラン"));
        QVERIFY(palette->visualItemRect(palette->item(0)).height() >= 48);
        QCOMPARE(w.findChild<QComboBox *>("elementSelector")->currentData().toString(), QString("U"));
        QVERIFY(w.document().atoms.isEmpty());
        QTest::mouseClick(w.canvas(), Qt::LeftButton, Qt::NoModifier, w.canvas()->rect().center());
        QCOMPARE(w.document().atoms.size(), 1);
        QCOMPARE(w.document().atoms[0].element, QString("U"));
        w.history()->undo();
        QVERIFY(w.document().atoms.isEmpty());
        w.history()->redo();
        QCOMPARE(w.document().atoms[0].element, QString("U"));
        QTimer::singleShot(0, &w, [&] {
            if (auto *dialog = w.findChild<PeriodicTableDialog *>("periodicTableDialog"))
                QTest::keyClick(dialog, Qt::Key_Escape);
        });
        open->click();
        QCOMPARE(palette->item(0)->data(Qt::UserRole).toString(), QString("atom:U"));
        QCOMPARE(w.document().atoms.size(), 1);
        category->setCurrentIndex(category->findData("skeleton"));
        QVERIFY(!open->isVisible());
        QCOMPARE(palette->count(), 19);
    }
    void customPartThumbnailRefresh() {
        QStandardItemModel model(1, 1);
        auto t = findTemplate("skeleton:benzene");
        t["id"] = "my:preview";
        auto index = model.index(0, 0);
        model.setData(index, t, TemplateDelegate::TemplateRole);
        InspectableTemplateDelegate delegate;
        QWidget widget;
        QStyleOptionViewItem option;
        option.initFrom(&widget);
        option.widget = &widget;
        delegate.initStyleOption(&option, index);
        const auto ratio = widget.devicePixelRatioF();
        const auto before = option.icon.pixmap(option.decorationSize, ratio).toImage();
        QVERIFY(!before.isNull());
        QCOMPARE(before.size(), QSize(qRound(60 * ratio), qRound(44 * ratio)));
        QCOMPARE(before.pixelColor(0, 0).alpha(), 0);
        // Reimporting a custom part can change its connection without changing its ID.
        t["connections"] = QJsonArray{0};
        model.setData(index, t, TemplateDelegate::TemplateRole);
        delegate.initStyleOption(&option, index);
        const auto after = option.icon.pixmap(option.decorationSize, ratio).toImage();
        QVERIFY(after != before);
        bool hasConnection = false;
        for (int y = 0; y < after.height(); ++y)
            for (int x = 0; x < after.width(); ++x)
                hasConnection |= after.pixelColor(x, y) == QColor("#16a34a");
        QVERIFY(hasConnection);
    }
    void sequenceUi() {
        MainWindow w;
        w.loadDocument(Document());
        w.show();
        auto *tabs = w.findChild<QTabWidget *>("propertyTabs");
        tabs->setCurrentIndex(2);
        auto *input = w.findChild<QLineEdit *>("sequenceInput");
        input->setText("AGS");
        auto *build = w.findChild<QPushButton *>("buildSequence");
        QVERIFY(build);
        build->click();
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.document().atoms.size(), 0);
        QCOMPARE(w.displayDocument().formula(), QString("C8H15N3O5"));
        w.findChild<QPushButton *>("confirmCandidate")->click();
        QCOMPARE(w.document().atoms.size(), 31);
        w.history()->undo();
        QVERIFY(w.document().atoms.isEmpty());
    }
    void failedEditIsAtomic() {
        MainWindow w;
        Document d;
        placeTemplate(d, findTemplate("skeleton:water"), Vec::Zero());
        w.loadDocument(d);
        QVERIFY_EXCEPTION_THROWN(w.edit("invalid",
                                        [](Document &candidate) {
                                            candidate.atoms.clear();
                                            fail("reject");
                                        }),
                                 std::runtime_error);
        QCOMPARE(w.document().atoms.size(), 3);
        QCOMPARE(w.history()->count(), 0);
    }
};
QTEST_MAIN(EditorTest)
#include "test_editor.moc"
