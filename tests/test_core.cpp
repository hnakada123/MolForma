#include "chemistry.h"
#include "coordination.h"
#include "peptide.h"
#include "stereochemistry.h"
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace mf;
class CoreTest : public QObject {
    Q_OBJECT
  private slots:
    void namedElements() {
        QCOMPARE(elements().size(), 118);
        QCOMPARE(elements().first().symbol, QString("H"));
        QCOMPARE(elements().last().symbol, QString("Og"));
        for (int i = 0; i < elements().size(); ++i) {
            const auto &e = elements()[i];
            QCOMPARE(e.number, i + 1);
            QVERIFY(e.mass > 0 && e.radius > 0 && e.color.isValid());
            QVERIFY(!e.nameJa.isEmpty() && !e.nameEn.isEmpty());
            auto d = Document::fromXyz(QString("1\nelement\n%1 1.2 -3.4 5.6\n").arg(e.symbol));
            d.envelope["custom"] = "preserved";
            const auto restored = Document::fromProject(d.project());
            QCOMPARE(restored.atoms[0].element, e.symbol);
            QCOMPARE(restored.atoms[0].id, d.atoms[0].id);
            QCOMPARE(restored.envelope["custom"], d.envelope["custom"]);
            QCOMPARE(Document::fromXyz(d.xyz()).atoms[0].element, e.symbol);
        }
        QCOMPARE(element("U").number, 92);
        QCOMPARE(element("Nh").number, 113);
        QVERIFY_EXCEPTION_THROWN(element("Uue"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(element("Ubn"), std::runtime_error);
    }
    void nativeStereochemistry() {
        if (!hasStereochemistryBackend())
            QSKIP("RDKit C++ backend not installed");
        Document d;
        buildPeptide(d, {"ALA"}, "neutral", "neutral");
        recordStereo(d);
        auto r = d.data["residues"].toArray().first().toObject();
        auto ca = r["atoms"].toObject()["CA"].toString();
        QCOMPARE(d.atom(ca).stereo, QString("S"));
        invertCenter(d, ca);
        recordStereo(d);
        QCOMPARE(d.atom(ca).stereo, QString("R"));
        auto refined = refine(d);
        recordStereo(refined);
        QCOMPARE(refined.atom(ca).stereo, QString("R"));
    }
    void residueRecognition() {
        if (!hasStereochemistryBackend())
            QSKIP("RDKit C++ backend not installed");
        Document d;
        buildPeptide(d, {"ALA", "GLY", "SER"}, "charged", "charged");
        d.data["residues"] = QJsonArray();
        d.data["chains"] = QJsonObject();
        for (auto &a : d.atoms)
            a.name.clear();
        recognizeResidues(d);
        QCOMPARE(d.data["residues"].toArray().size(), 3);
        QStringList codes;
        for (auto v : d.data["residues"].toArray())
            codes << v.toObject()["code"].toString();
        QCOMPARE(codes, QStringList({"ALA", "GLY", "SER"}));
        QVERIFY(d.validate().isEmpty());
    }
    void missingSidechainRepair() {
        Document d;
        buildPeptide(d, {"ALA"}, "charged", "charged");
        auto r = d.data["residues"].toArray().first().toObject();
        QString rid = r["id"].toString(), cb = r["atoms"].toObject()["CB"].toString();
        auto ca = r["atoms"].toObject()["CA"].toString();
        Vec original = d.atom(ca).position;
        Ids removed{cb};
        for (auto n : d.neighbors(cb))
            if (d.atom(n).element == "H")
                removed << n;
        d.remove(removed);
        QVERIFY(residueDiagnostics(d, rid).join(' ').contains("CB"));
        repairResidue(d, rid);
        QCOMPARE(d.formula(), QString("C3H7NO2"));
        QVERIFY((d.atom(ca).position - original).norm() < 1e-12);
        QVERIFY2(d.validate().isEmpty(), qPrintable(d.validate().join(';')));
    }
    void capsAndCopies() {
        Document d;
        buildPeptide(d, {"ALA", "GLY", "SER"}, "ACE", "NME");
        QCOMPARE(d.formula(), QString("C11H20N4O5"));
        QCOMPARE(d.data["residues"].toArray().size(), 5);
        QVERIFY2(d.validate().isEmpty(), qPrintable(d.validate().join(';')));
        duplicate(d, d.ids(), Vec(30, 0, 0));
        QCOMPARE(d.data["chains"].toObject().size(), 2);
        for (auto sequence : d.data["chains"].toObject())
            QCOMPARE(sequence.toArray().size(), 5);
        QVERIFY(d.validate().isEmpty());
    }
    void cappedResidueEdits() {
        Document d;
        buildPeptide(d, {"ALA"}, "ACE", "NME");
        QString rid;
        for (auto value : d.data["residues"].toArray())
            if (value.toObject()["code"] == "ALA")
                rid = value.toObject()["id"].toString();
        mutateResidue(d, rid, "GLY", "default", "L");
        QCOMPARE(d.data["residues"].toArray().size(), 3);
        QCOMPARE(d.formula(), QString("C5H10N2O2"));
        QVERIFY2(d.validate().isEmpty(), qPrintable(d.validate().join(';')));
        auto copy = d;
        deleteResidue(copy, rid);
        QVERIFY(copy.atoms.isEmpty());
        QVERIFY(copy.data["chains"].toObject().isEmpty());
        setTerminals(d, rid, "charged", "charged");
        QCOMPARE(d.data["residues"].toArray().size(), 1);
        QCOMPARE(d.formula(), QString("C2H5NO2"));
        QCOMPARE(d.formalCharge(), 0);
        QVERIFY2(d.validate().isEmpty(), qPrintable(d.validate().join(';')));
    }
    void optionalJobSections() {
        Document d;
        const auto sections = d.emittedJob()["sections"].toObject();
        QVERIFY(!sections.contains("qmmm"));
        for (auto value : sections)
            QVERIFY(value.isObject());
    }
    void pdbRoundTrip() {
        Document d;
        buildPeptide(d, {"ALA", "GLY", "SER"}, "charged", "charged");
        QTemporaryDir temp;
        auto path = temp.filePath("peptide.pdb");
        exportStructure(d, path);
        auto copy = importStructure(path);
        QCOMPARE(copy.atoms.size(), 31);
        QCOMPARE(copy.data["residues"].toArray().size(), 3);
        QVERIFY(copy.validate().isEmpty());
    }
    void templateLibrary() {
        QCOMPARE(templates().size(), 101);
        for (auto v : templates()) {
            Document d;
            instantiate(d, v.toObject());
            QVERIFY2(d.validate().isEmpty(),
                     qPrintable(v.toObject()["id"].toString() + d.validate().join(';')));
            QCOMPARE(Document::fromProject(d.project()).atoms.size(), d.atoms.size());
        }
    }
    void projectRoundTrip() {
        for (auto name : {"benzoic_acid", "co_en", "ala_gly_ser", "water_dimer_counterpoise"}) {
            auto d = Document::load(QString(FIXTURE_DIR) + "/" + name + ".kgui.json");
            QTemporaryDir temp;
            QString file = temp.filePath("test.kgui.json");
            d.save(file);
            auto copy = Document::load(file);
            QCOMPARE(copy.ids(), d.ids());
            QCOMPARE(copy.bonds.size(), d.bonds.size());
            QCOMPARE(copy.data["residues"], d.data["residues"]);
            QCOMPARE(copy.data["centers"], d.data["centers"]);
            QCOMPARE(copy.data["assignments"], d.data["assignments"]);
            for (int i = 0; i < d.atoms.size(); ++i)
                QVERIFY((copy.atoms[i].position - d.atoms[i].position).norm() < 1e-12);
        }
    }
    void xyzValidation() {
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nD .96 0 0\nH -.24 .93 0\n");
        QCOMPARE(d.bonds.size(), 2);
        QCOMPARE(d.atoms[1].isotope, 2);
        QCOMPARE(Document::fromXyz(d.xyz()).atoms[1].isotope, 2);
        QVERIFY_EXCEPTION_THROWN(Document::fromXyz("1\ninvalid\nC nan 0 0\n"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(Document::fromXyz("2\nshort\nC 0 0 0\n"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(Document::fromXyz("1\nunknown\nXx 0 0 0\n"), std::runtime_error);
    }
    void hydrogenAndAttachment() {
        Document d;
        auto carbon = addAtom(d, "C", Vec::Zero());
        QCOMPARE(d.formula(), QString("CH4"));
        QCOMPARE(d.atoms.size(), 5);
        auto hydrogen = d.neighbors(carbon).first();
        addAtom(d, "O", Vec::Zero(), hydrogen);
        QCOMPARE(d.formula(), QString("CH4O"));
        QCOMPARE(d.atoms.size(), 6);
        auto saved = d;
        completeHydrogens(d);
        QCOMPARE(d.atoms.size(), saved.atoms.size());
    }
    void substituent() {
        Document d;
        placeTemplate(d, findTemplate("skeleton:benzene"), Vec::Zero());
        QString h;
        for (auto a : d.atoms)
            if (a.element == "H") {
                h = a.id;
                break;
            }
        attachPart(d, h, findTemplate("substituent:methyl"));
        QCOMPARE(d.formula(), QString("C7H8"));
        QVERIFY(d.validate().isEmpty());
    }
    void dimensions() {
        auto d = Document::fromXyz("4\nchain\nC 0 1 0\nC 0 0 0\nC 1.5 0 0\nC 1.5 -1 0\n");
        d.bonds.clear();
        for (int i = 0; i < 3; ++i)
            d.setBond(d.atoms[i].id, d.atoms[i + 1].id, "single");
        auto ids = d.ids();
        QVERIFY(std::abs(measure(d, ids) - 180) < 1e-10);
        for (double angle : {-170., -60., 0., 60., 179.}) {
            setDimension(d, ids, angle);
            QVERIFY(std::abs(measure(d, ids) - angle) < 1e-8);
        }
        setDimension(d, {ids[0], ids[1], ids[2]}, 110);
        QVERIFY(std::abs(measure(d, {ids[0], ids[1], ids[2]}) - 110) < 1e-8);
        setDimension(d, {ids[1], ids[2]}, 1.8);
        QVERIFY(std::abs(measure(d, {ids[1], ids[2]}) - 1.8) < 1e-10);
        d.atom(ids[3]).fixed = true;
        QVERIFY_EXCEPTION_THROWN(setDimension(d, ids, 90), std::runtime_error);
        d.atom(ids[3]).fixed = false;
        d.setBond(ids[0], ids[3], "single");
        QVERIFY_EXCEPTION_THROWN(d.bondSide(ids[1], ids[2]), std::runtime_error);
    }
    void wholePartReplacement() {
        Document d;
        auto ring = placeTemplate(d, findTemplate("skeleton:benzene"), Vec::Zero());
        auto h = *std::find_if(ring.atoms.begin(), ring.atoms.end(),
                               [&](const auto &id) { return d.atom(id).element == "H"; });
        auto methyl = attachPart(d, h, findTemplate("substituent:methyl"));
        QCOMPARE(d.formula(), QString("C7H8"));
        QString parent;
        for (auto id : d.neighbors(methyl.connections.first()))
            if (!methyl.atoms.contains(id))
                parent = id;
        const auto position = d.atom(parent).position;
        auto oldIndex = d.index(methyl.connections.first());
        auto hydroxyl = replacePart(d, methyl.atoms, findTemplate("substituent:hydroxy"));
        QCOMPARE(d.formula(), QString("C6H6O"));
        QCOMPARE(d.index(hydroxyl.connections.first()), oldIndex);
        QVERIFY((d.atom(parent).position - position).norm() < 1e-12);
        QVERIFY(d.validate().isEmpty());
        QVERIFY_EXCEPTION_THROWN(replacePart(d, d.ids(), findTemplate("substituent:methyl")),
                                 std::runtime_error);
    }
    void ionFragmentMerge() {
        Document d;
        placeTemplate(d, findTemplate("skeleton:water"), Vec::Zero());
        auto sodium = addAtom(d, "Na", Vec(0, 0, 2.4), {}, "single", false);
        d.atom(sodium).charge = 1;
        placeTemplate(d, findTemplate("skeleton:water"), Vec(20, 0, 0));
        proposeFragments(d, "fmo", "component");
        const auto before = d.xyz();
        const auto bonds = d.bonds.size();
        mergeIonContacts(d);
        auto a = d.data["assignments"].toObject()["fmo"].toObject();
        QCOMPARE(a["charges"].toObject().size(), 2);
        QCOMPARE(a["members"].toObject()[sodium], a["members"].toObject()[d.atoms.first().id]);
        QCOMPARE(d.job()["charge"].toInt(), 1);
        QCOMPARE(d.xyz(), before);
        QCOMPARE(d.bonds.size(), bonds);
    }
    void peptide() {
        Document d;
        buildPeptide(d, parseSequence("AGS"), "charged", "charged");
        QCOMPARE(d.formula(), QString("C8H15N3O5"));
        QCOMPARE(d.atoms.size(), 31);
        QCOMPARE(d.formalCharge(), 0);
        QCOMPARE(d.data["residues"].toArray().size(), 3);
        QVERIFY(d.validate().isEmpty());
        auto seq = idsFromJson(d.data["chains"].toObject().begin().value());
        QCOMPARE(seq.size(), 3);
        setResidueTorsion(d, seq[1], "phi", -60);
        setResidueTorsion(d, seq[1], "psi", -40);
        QVERIFY(d.validate().isEmpty());
        auto next = appendResidue(d, "ALA", seq.last());
        QCOMPARE(d.data["residues"].toArray().size(), 4);
        QVERIFY(!next.isEmpty());
        auto before = d.atoms.size();
        setTerminals(d, seq.first(), "neutral", {});
        QCOMPARE(d.atoms.size(), before - 1);
        QCOMPARE(d.formalCharge(), -1);
    }
    void peptidePrependMutateDelete() {
        Document d;
        buildPeptide(d, {"ALA"}, "charged", "charged");
        auto r = d.data["residues"].toArray().first().toObject()["id"].toString();
        auto first = prependResidue(d, "GLY", r);
        QCOMPARE(d.formula(), QString("C5H10N2O3"));
        mutateResidue(d, r, "SER", "default", "L");
        QCOMPARE(d.formula(), QString("C5H10N2O4"));
        QVERIFY2(d.validate().isEmpty(), qPrintable(d.validate().join(';')));
        deleteResidue(d, first);
        QCOMPARE(d.data["residues"].toArray().size(), 1);
        QVERIFY(d.validate().isEmpty());
    }
    void coordination() {
        Document d;
        QString zinc = addCenter(d, "Zn", "tetrahedral", Vec::Zero(), 2.3, 2);
        for (int i = 0; i < 4; ++i)
            placeLigand(d, zinc, {i}, findTemplate("ligand:chloride"));
        QCOMPARE(d.atoms.size(), 5);
        QCOMPARE(d.bonds.size(), 4);
        for (auto b : d.bonds) {
            QCOMPARE(b.kind, QString("dative"));
            QCOMPARE(b.b, zinc);
            QVERIFY(std::abs((d.atom(b.a).position - d.atom(zinc).position).norm() - 2.3) < 1e-10);
        }
        QVERIFY_EXCEPTION_THROWN(placeLigand(d, zinc, {0}, findTemplate("ligand:chloride")),
                                 std::runtime_error);
        adjustCenter(d, zinc, "tetrahedral", 2.5, 2);
        QCOMPARE(d.atoms.size(), 5);
        for (auto b : d.bonds)
            QVERIFY(std::abs((d.atom(b.a).position - d.atom(zinc).position).norm() - 2.5) < 1e-8);
        QVERIFY(d.validate().isEmpty());
    }
    void coordinationSiteEdits() {
        Document d;
        auto zn = addCenter(d, "Zn", "tetrahedral", Vec::Zero(), 2.3, 2);
        auto a = placeLigand(d, zn, {0}, findTemplate("ligand:chloride"));
        auto b = placeLigand(d, zn, {1}, findTemplate("ligand:bromide"));
        auto before = d.ids();
        Vec old = d.atom(a.connections.first()).position;
        swapLigands(d, zn, 0, 1);
        QCOMPARE(d.ids(), before);
        QVERIFY((d.atom(b.connections.first()).position - old).norm() < 1e-9);
        setSiteDirection(d, zn, {0}, Vec(0, 0, -1), 2.5);
        QVERIFY((d.atom(b.connections.first()).position - Vec(0, 0, -2.5)).norm() < 1e-9);
        moveLigand(d, a.id, {3});
        QCOMPARE(d.ids(), before);
        QVERIFY(d.validate().isEmpty());
    }
    void chelate() {
        Document d;
        QString co = addCenter(d, "Co", "octahedral", Vec::Zero(), 2.1, 3);
        QVERIFY_EXCEPTION_THROWN(placeLigand(d, co, {0, 2}, findTemplate("ligand:en")), std::runtime_error);
        auto en = placeLigand(d, co, {0, 1}, findTemplate("ligand:en"));
        QCOMPARE(en.connections.size(), 2);
        QVERIFY(d.validate().isEmpty());
    }
    void fragmentAssignments() {
        Document d;
        buildPeptide(d, {"ALA", "GLY", "SER"}, "charged", "charged");
        proposeFragments(d, "fmo", "residue2");
        auto a = d.data["assignments"].toObject()["fmo"].toObject();
        QCOMPARE(a["charges"].toObject().size(), 2);
        QCOMPARE(a["cuts"].toArray().size(), 1);
        auto cut = idsFromJson(a["cuts"].toArray().first());
        QCOMPARE(d.atom(cut[0]).name, QString("CA"));
        QCOMPARE(d.atom(cut[1]).name, QString("C"));
        QCOMPARE(a["charges"].toObject()["1"].toInt(), 1);
        QCOMPARE(a["charges"].toObject()["2"].toInt(), -1);
        auto sections = d.emittedJob()["sections"].toObject();
        QVERIFY(!sections["fmo"].toObject()["fragments"].toString().isEmpty());
        Document water;
        placeTemplate(water, findTemplate("skeleton:water"), Vec::Zero());
        duplicate(water, water.ids(), Vec(0, 0, 4));
        proposeFragments(water, "counterpoise", "component");
        QCOMPARE(water.data["assignments"].toObject()["counterpoise"].toObject()["charges"].toObject().size(),
                 2);
    }
    void referenceRemapping() {
        Document d;
        placeTemplate(d, findTemplate("skeleton:water"), Vec::Zero());
        auto ids = d.ids();
        addReference(d, {ids[0], ids[1]}, "constraint", .96);
        addReference(d, {ids[1]}, "freeze");
        addReference(d, {ids[1], ids[0], ids[2]}, "scan", 100, 120, 10);
        std::reverse(d.atoms.begin(), d.atoms.end());
        auto sections = d.emittedJob()["sections"].toObject();
        QVERIFY(sections["constraint"].toObject()["list"].toString().contains("bond=3,2"));
        QCOMPARE(sections["constraint"].toObject()["freeze"].toString(), QString("2"));
        QCOMPARE(sections["scan"].toObject()["angle"].toString(), QString("2,3,1"));
        d.remove({ids[1]});
        QVERIFY(!d.data["invalid_settings"].toArray().isEmpty());
        QVERIFY(!d.emittedJob()["sections"].toObject()["constraint"].toObject().contains("freeze"));
        QVERIFY(!d.emittedJob()["sections"].toObject()["constraint"].toObject().contains("list"));
    }
    void charges() {
        Document d;
        placeTemplate(d, findTemplate("skeleton:water"), Vec::Zero());
        waterToCharges(d, d.ids());
        QVERIFY(d.atoms.isEmpty());
        QCOMPARE(d.data["point_charges"].toArray().size(), 3);
        double sum = 0;
        for (auto q : d.data["point_charges"].toArray())
            sum += q.toObject()["charge"].toDouble();
        QVERIFY(std::abs(sum) < 1e-12);
        auto j = d.job();
        j["sections"] = QJsonObject{{"qmmm", QJsonObject{{"enabled", "t"}}}};
        d.data["job"] = j;
        QCOMPARE(
            d.emittedJob()["sections"].toObject()["qmmm"].toObject()["charges"].toString().split(';').size(),
            3);
    }
    void smilesAndRefine() {
        auto d = fromSmiles("CCO");
        QCOMPARE(d.formula(), QString("C2H6O"));
        QCOMPARE(d.atoms.size(), 9);
        auto first = d.atoms[0].position;
        d.atoms[0].fixed = true;
        auto out = refine(d);
        QVERIFY((out.atoms[0].position - first).norm() < 1e-12);
        QCOMPARE(out.ids(), d.ids());
        QVERIFY_EXCEPTION_THROWN(fromSmiles("not a smiles"), std::runtime_error);
        QTemporaryDir tmp;
        exportStructure(d, tmp.filePath("ethanol.sdf"));
        auto copy = importStructure(tmp.filePath("ethanol.sdf"));
        QCOMPARE(copy.formula(), d.formula());
        QCOMPARE(copy.bonds.size(), d.bonds.size());
    }
};
QTEST_APPLESS_MAIN(CoreTest)
#include "test_core.moc"
