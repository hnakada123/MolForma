#include "library.h"
#include "main_window.h"
#include "template_delegate.h"
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QTabWidget>
#include <QTemporaryDir>
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
