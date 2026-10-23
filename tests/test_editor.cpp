#include "editing/library.h"
#include "editing/operations.h"
#include "nwchem/nwchem.h"
#include "ui/main_window.h"
#include "ui/periodic_table.h"
#include "ui/template_delegate.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
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
    void coordinationCenterPalette_data() {
        QTest::addColumn<QString>("categoryId");
        QTest::newRow("centers") << QString("center");
        QTest::newRow("all") << QString("all");
    }
    void coordinationCenterPalette() {
        QFETCH(QString, categoryId);
        MainWindow w;
        w.loadDocument(Document());
        w.show();
        auto *category = w.findChild<QComboBox *>("paletteCategory");
        auto *search = w.findChild<QLineEdit *>("paletteSearch");
        auto *palette = w.findChild<QListWidget *>("templatePalette");
        category->setCurrentIndex(category->findData(categoryId));
        QTest::qWait(40);
        int centers = 0;
        for (int row = 0; row < palette->count(); ++row) {
            auto *item = palette->item(row);
            const auto id = item->data(Qt::UserRole).toString();
            if (!id.startsWith("center:"))
                continue;
            ++centers;
            const auto &e = element(id.mid(7));
            QCOMPARE(item->text(), e.nameJa);
            QVERIFY(palette->visualItemRect(item).height() >= 48);
            QVERIFY(item->toolTip().contains(e.nameEn));
            QVERIFY(item->data(Qt::AccessibleTextRole).toString().contains("配位中心"));
        }
        QCOMPARE(centers, 25);
        search->setText("存在しない配位中心");
        QCOMPARE(palette->count(), 0);
        for (const auto &query : {"亜鉛", "zinc", "zN", "30"}) {
            search->setText(QString::fromUtf8(query));
            QCOMPARE(palette->count(), 1);
            QCOMPARE(palette->item(0)->data(Qt::UserRole).toString(), QString("center:Zn"));
            QCOMPARE(palette->item(0)->text(), QString("亜鉛"));
        }
        palette->clearSelection();
        const auto image = palette->viewport()->grab().toImage();
        int colorPixels = 0;
        for (int y = 0; y < qRound(48 * image.devicePixelRatio()); ++y)
            for (int x = 0; x < qRound(64 * image.devicePixelRatio()); ++x)
                if (image.pixelColor(x, y) == element("Zn").color)
                    ++colorPixels;
        QVERIFY2(colorPixels > 100, "The coordination center element icon is missing");

        const auto rowRect = palette->visualItemRect(palette->item(0));
        QTest::mouseClick(palette->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(rowRect.left() + 30, rowRect.center().y()));
        QCOMPARE(w.findChild<QComboBox *>("toolSelector")->currentData().toString(), QString("center"));
        QCOMPARE(w.findChild<QComboBox *>("elementSelector")->currentData().toString(), QString("Zn"));
        QCOMPARE(w.findChild<QTabWidget *>("propertyTabs")->currentIndex(), 1);
        QVERIFY(w.document().atoms.isEmpty());
        QCOMPARE(w.history()->count(), 0);
        QTest::mouseClick(w.canvas(), Qt::LeftButton, Qt::NoModifier, w.canvas()->rect().center());
        QCOMPARE(w.document().atoms.size(), 1);
        QCOMPARE(w.document().atoms[0].element, QString("Zn"));
        const auto centerRows = w.document().data["centers"].toArray();
        QCOMPARE(centerRows.size(), 1);
        const auto center = centerRows[0].toObject();
        QCOMPARE(center["atom"].toString(), w.document().atoms[0].id);
        QCOMPARE(center["geometry"].toString(), QString("octahedral"));
        QCOMPARE(center["sites"].toArray().size(), 6);
        w.history()->undo();
        QVERIFY(w.document().atoms.isEmpty());
        QVERIFY(w.document().data["centers"].toArray().isEmpty());
        w.history()->redo();
        QCOMPARE(w.document().atoms[0].element, QString("Zn"));
        QCOMPARE(w.document().data["centers"].toArray(), centerRows);

        search->clear();
        QCOMPARE(palette->currentItem()->data(Qt::UserRole).toString(), QString("center:Zn"));
        const auto artifacts = qEnvironmentVariable("MOLFORMA_CENTER_ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            for (int width : {1380, 900}) {
                w.resize(width, 900);
                palette->scrollToItem(palette->currentItem(), QAbstractItemView::PositionAtCenter);
                QTest::qWait(40);
                const auto path = QDir(artifacts).filePath(QString("%1-%2").arg(categoryId).arg(width));
                QVERIFY(w.grab().save(path + ".png"));
                QVERIFY(palette->grab().save(path + "-palette.png"));
            }
        }
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
    void nwchemMoleculeControls() {
        MainWindow w;
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
        d.atoms[0].fixed = true;
        w.loadDocument(d);
        auto *tabs = w.findChild<QTabWidget *>("propertyTabs");
        QStringList names;
        for (int i = 0; i < tabs->count(); ++i)
            names << tabs->tabText(i);
        QCOMPARE(names, QStringList({"編集", "配位", "残基", "電荷"}));
        for (auto *b : tabs->findChildren<QPushButton *>()) {
            QVERIFY(!b->text().contains("FMO"));
            QVERIFY(!b->text().contains("QM/MM"));
            QVERIFY(!b->text().contains("スキャン"));
            QVERIFY(!b->text().contains("拘束に追加"));
            QVERIFY(!b->text().contains("KAGAMI最適化"));
        }
        auto *color = w.findChild<QComboBox *>("moleculeColorMode");
        QCOMPARE(color->count(), 2);
        QVERIFY(w.findChild<QPushButton *>("preparePdb"));

        auto *optimize = w.findChild<QPushButton *>("setNwchemOptimization");
        QVERIFY(optimize);
        optimize->click();
        QVERIFY(nwchemInput(w.document()).endsWith("task scf optimize\n"));
        QCOMPARE(w.document().job(), d.job());
        QCOMPARE(w.findChild<QComboBox *>("nwchem_task")->currentData().toString(), "optimize");
        w.history()->undo();
        QVERIFY(nwchemInput(w.document()).endsWith("task scf energy\n"));
        w.history()->redo();

        // Mixed selections must be fixed explicitly regardless of the first atom's state.
        w.setSelection({d.atoms[0].id, d.atoms[1].id});
        w.findChild<QPushButton *>("fixSelectedAtoms")->click();
        QVERIFY(nwchemInput(w.document()).contains("fix atom 1 2\n"));
        for (int i = 0; i < d.atoms.size(); ++i)
            QCOMPARE(w.document().atoms[i].position, d.atoms[i].position);
        w.history()->undo();
        QVERIFY(w.document().atoms[0].fixed);
        QVERIFY(!w.document().atoms[1].fixed);
        w.history()->redo();
        const auto fixedId = d.atoms[0].id;
        w.edit("reorder", [](Document &next) { std::swap(next.atoms[0], next.atoms[2]); });
        QVERIFY(nwchemInput(w.document()).contains("fix atom 2 3\n"));
        w.setSelection({fixedId});
        w.findChild<QPushButton *>("unfixSelectedAtoms")->click();
        QVERIFY(nwchemInput(w.document()).contains("fix atom 2\n"));
        QVERIFY(!w.document().atom(fixedId).fixed);
        w.history()->undo();
        QVERIFY(w.document().atom(fixedId).fixed);

        const auto artifacts = qEnvironmentVariable("MOLFORMA_MOLECULE_ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            w.resize(1380, 1000);
            w.show();
            for (int i = 0; i < tabs->count(); ++i) {
                tabs->setCurrentIndex(i);
                QTest::qWait(40);
                QVERIFY(w.grab().save(QDir(artifacts).filePath(QString("molecule-%1.png").arg(i))));
            }
        }
    }
    void moleculeElectronicState() {
        MainWindow w;
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
        w.loadDocument(d);
        auto *charge = w.findChild<QSpinBox *>("moleculeTotalCharge");
        auto *mult = w.findChild<QSpinBox *>("moleculeMultiplicity");
        auto *apply = w.findChild<QPushButton *>("applyMoleculeCharge");
        auto *summary = w.findChild<QLabel *>("moleculeChargeSummary");
        QVERIFY(charge && mult && apply && summary);
        charge->setValue(1);
        apply->click();
        QVERIFY(summary->text().contains("整合しません"));
        QVERIFY(!w.findChild<QPushButton *>("calculationCopy")->isEnabled());
        mult->setValue(2);
        apply->click();
        QVERIFY(summary->text().contains("電子数 9"));
        QVERIFY(summary->text().contains("α 5 / β 4"));
        QVERIFY(nwchemInput(w.document()).contains("charge 1\n"));
        QVERIFY(nwchemInput(w.document()).contains("nopen 1\n"));
        QCOMPARE(w.findChild<QSpinBox *>("nwchem_charge")->value(), 1);
        QCOMPARE(w.findChild<QSpinBox *>("nwchem_multiplicity")->value(), 2);
        w.history()->undo();
        QCOMPARE(mult->value(), 1);
        w.history()->undo();
        QCOMPARE(charge->value(), 0);
        w.history()->redo();
        w.history()->redo();
        w.findChild<QSpinBox *>("nwchem_charge")->setValue(0);
        w.findChild<QSpinBox *>("nwchem_multiplicity")->setValue(1);
        QCOMPARE(charge->value(), 0);
        QCOMPARE(mult->value(), 1);
        w.edit("candidate", [](Document &next) { next.setJobValue("charge", 2); }, true);
        QVERIFY(!apply->isEnabled());
        QCOMPARE(charge->value(), 2);
        QVERIFY(summary->text().contains("未確定候補"));
        w.cancelCandidate();
        QVERIFY(apply->isEnabled());
        QCOMPARE(charge->value(), 0);

        // ECP electrons and ghost centers use the same validation as the input generator.
        d = Document::fromXyz("2\nRb ghost\nRb 0 0 0\nH 0 0 3\n");
        d.atoms[0].charge = 1;
        d.atoms[1].ghost = true;
        d.data["nwchem"] = QJsonObject{{"basis", "def2-svp"}};
        w.loadDocument(d);
        w.findChild<QPushButton *>("applyMoleculeFormalCharge")->click();
        QCOMPARE(charge->value(), 1);
        QVERIFY(summary->text().contains("電子数 8（ECP適用後）"));
        QVERIFY(summary->text().contains("α 4 / β 4"));
        QVERIFY(validateNwchem(w.document()).errors.isEmpty());
        w.findChild<QPushButton *>("moleculeInputPreview")->click();
        QCOMPARE(w.findChild<QTabWidget *>("workspaceTabs")->currentIndex(), 5);
    }
    void moleculePointChargesAndCompatibility() {
        MainWindow w;
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
        addReference(d, {d.atoms[0].id}, "freeze");
        d.data["assignments"] = QJsonObject{{"legacy", QJsonObject{{"note", "preserve"}}}};
        d.setJobValue("sections", QJsonObject{{"qmmm", QJsonObject{{"enabled", "t"}}}});
        d.envelope["external_field"] = "preserve";
        d.data["nwchem"] = QJsonObject{{"geometry_units", "bohr"}, {"future_setting", 42}};
        w.loadDocument(d);
        const auto original = w.document().project();
        auto *enabled = w.findChild<QCheckBox *>("moleculeIncludePointCharges");
        auto *calculation = w.findChild<QCheckBox *>("nwchem_include_point_charges");
        auto *x = w.findChild<QDoubleSpinBox *>("pointChargeX");
        auto *q = w.findChild<QDoubleSpinBox *>("pointChargeValue");
        QVERIFY(enabled && calculation && x && q);
        QVERIFY(!enabled->isChecked()); // The legacy QM/MM flag is not an NWChem switch.
        x->setValue(5);
        q->setValue(-.5);
        w.findChild<QPushButton *>("addPointCharge")->click();
        QCOMPARE(w.document().data["point_charges"].toArray().size(), 1);
        QVERIFY(!nwchemInput(w.document()).contains("\nbq units"));
        enabled->click();
        QVERIFY(calculation->isChecked());
        QVERIFY(nwchemInput(w.document()).contains("\nbq units angstrom\n"));
        QVERIFY(nwchemInput(w.document()).contains("5 0 0 -0.5"));
        QCOMPARE(validateNwchem(w.document()).electrons, 10);
        QCOMPARE(w.document().job(), d.job());
        QCOMPARE(w.document().data["references"], d.data["references"]);
        QCOMPARE(w.document().data["assignments"], d.data["assignments"]);
        QCOMPARE(w.document().data["nwchem"].toObject()["future_setting"].toInt(), 42);
        w.history()->undo();
        QVERIFY(!enabled->isChecked());
        QVERIFY(!calculation->isChecked());
        w.history()->redo();
        QVERIFY(enabled->isChecked());
        calculation->click();
        QVERIFY(!enabled->isChecked());
        w.history()->undo();
        QVERIFY(enabled->isChecked());

        auto *list = w.findChild<QListWidget *>("pointChargeList");
        list->setCurrentRow(0);
        const auto id = w.document().data["point_charges"].toArray()[0].toObject().value("id");
        x->setValue(6);
        q->setValue(.25);
        w.findChild<QPushButton *>("updatePointCharge")->click();
        QCOMPARE(w.document().data["point_charges"].toArray()[0].toObject()["id"], id);
        QVERIFY(nwchemInput(w.document()).contains("6 0 0 0.25"));
        w.findChild<QPushButton *>("deletePointCharge")->click();
        QVERIFY(w.document().data["point_charges"].toArray().isEmpty());
        QVERIFY(!nwchemInput(w.document()).contains("\nbq units"));
        w.history()->undo();
        QVERIFY(nwchemInput(w.document()).contains("6 0 0 0.25"));
        w.edit("candidate", [](Document &next) { next.data["point_charges"] = QJsonArray(); }, true);
        QVERIFY(!enabled->isEnabled());
        w.cancelCandidate();
        QVERIFY(enabled->isEnabled());

        QTemporaryDir dir;
        const auto path = dir.filePath("legacy.kgui.json");
        w.document().save(path);
        const auto loaded = Document::load(path);
        QCOMPARE(loaded.project(), w.document().project());
        QCOMPARE(loaded.envelope["external_field"].toString(), "preserve");
        while (w.history()->canUndo())
            w.history()->undo();
        QCOMPARE(w.document().project(), original);
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
