#include "editing/chemistry.h"
#include "editing/coordinates.h"
#include "editing/coordination.h"
#include "editing/editor_session.h"
#include "editing/peptide.h"
#include "editing/stereochemistry.h"
#include "io/structure_io.h"
#include "model/geometry.h"
#include "nwchem/nwchem.h"
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace mf;
class CoreTest : public QObject {
    Q_OBJECT
  private slots:
    void sessionTransactionsAndProvenance() {
        EditorSession session;
        auto d = Document::fromXyz("2\nH2\nH 0 0 0\nH 0 0 .74\n");
        d.atoms[0].fixed = true;
        d.envelope["future_field"] = "preserved";
        session.reset(d);
        QSignalSpy applied(&session, &EditorSession::applied);
        const auto before = session.document().project();
        QVERIFY_EXCEPTION_THROWN(
            session.edit("invalid", [](Document &next) { next.atoms[0].position.x() = 1; }),
            std::runtime_error);
        QCOMPARE(session.document().project(), before);
        QCOMPARE(session.history().count(), 0);
        QVERIFY(!session.candidate());
        QVERIFY(session.edit("move", [](Document &next) { next.atoms[1].position.z() = 1; }, true, true));
        QCOMPARE(session.document().project(), before);
        QVERIFY(session.candidateRemote());
        QVERIFY_EXCEPTION_THROWN(session.edit("blocked", [](Document &) {}), std::runtime_error);
        QVERIFY(session.confirm());
        QCOMPARE(session.document().envelope, d.envelope);
        QCOMPARE(session.history().count(), 1);
        session.navigate("undo");
        QCOMPARE(session.document().project(), before);
        session.navigate("redo");
        QCOMPARE(session.document().atoms[1].position.z(), 1.0);
        QCOMPARE(applied.size(), 3);
        for (const auto &event : applied)
            QVERIFY(event[2].toBool()); // Confirmation and undo/redo keep remote provenance.
        QVERIFY(!session.edit("no-op", [](Document &) {}));
        QCOMPARE(session.history().count(), 1);
        QVERIFY(session.edit("candidate", [](Document &next) { next.atoms[1].position.z() = 2; }, true));
        session.cancel();
        QCOMPARE(session.document().atoms[1].position.z(), 1.0);
        session.reset(d);
        QVERIFY(session.history().isClean());
        QVERIFY(!session.history().canUndo());
    }

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
        // From XYZ, which has neither charges nor residues: the caps, the charges the hydrogens imply
        // and the side-chain states come back.
        const auto fromXyz = [](const QString &sequence, const QString &n, const QString &c,
                                const QString &state) {
            Document built;
            buildPeptide(built, parseSequence(sequence), n, c, "L", state);
            auto read = xyzStructure(built.xyz());
            recognizeResidues(read);
            return std::pair{built, read};
        };
        const auto states = [](const Document &doc) {
            QStringList out;
            QMap<int, QString> byNumber;
            for (auto v : doc.data["residues"].toArray())
                byNumber[v.toObject()["number"].toInt()] =
                    v.toObject()["code"].toString() + ":" + v.toObject()["state"].toString() + ":" +
                    v.toObject()["n_terminal"].toString() + "/" + v.toObject()["c_terminal"].toString();
            return byNumber.values();
        };
        auto [built, read] = fromXyz("AHE", "ACE", "NME", "default");
        QCOMPARE(read.formalCharge(), built.formalCharge());
        QCOMPARE(read.formalCharge(), -1);
        QCOMPARE(states(read), QStringList({"ACE:default:internal/internal", "ALA:default:ACE/internal",
                                            "HIS:HIE:internal/internal", "GLU:default:internal/NME",
                                            "NME:default:internal/internal"}));
        QCOMPARE(read.data["chains"].toObject().begin()->toArray().size(), 5);
        // HIP: an imidazolium ring, whose bond orders from XYZ differ from the template's.
        for (const auto &[n, c] : {std::pair{"ACE", "NME"}, {"charged", "charged"}}) {
            Document hip;
            buildPeptide(hip, parseSequence("H"), n, c, "L", "HIP");
            auto again = xyzStructure(hip.xyz());
            recognizeResidues(again);
            QCOMPARE(again.formalCharge(), hip.formalCharge());
            QVERIFY(states(again).join(' ').contains("HIS:HIP:"));
        }
        std::tie(built, read) = fromXyz("KDE", "neutral", "neutral", "default");
        QCOMPARE(read.formalCharge(), built.formalCharge());
        QCOMPARE(states(read), QStringList({"LYS:default:neutral/internal", "ASP:default:internal/internal",
                                            "GLU:default:internal/neutral"}));
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
        // A replaced residue inside a chain keeps its amide hydrogen in the peptide plane, away from
        // the previous carbonyl carbon (the template's free amino group once put it 0.66 Å from it).
        for (const auto *code : {"ALA", "ARG", "ASN", "ASP", "CYS", "GLN", "GLU", "GLY", "HIS", "ILE", "LEU",
                                 "LYS", "MET", "PHE", "SER", "THR", "TRP", "TYR", "VAL"}) {
            Document p;
            buildPeptide(p, parseSequence("AHE"), "ACE", "NME");
            QString his;
            for (auto value : p.data["residues"].toArray())
                if (value.toObject()["code"] == "HIS")
                    his = value.toObject()["id"].toString();
            mutateResidue(p, his, code, "default", "L");
            const auto r = residue(p, his);
            const auto n = r["atoms"].toObject()["N"].toString();
            QString previous;
            for (const auto &neighbor : p.neighbors(n))
                if (p.atom(neighbor).element == "C" && residueForAtom(p, neighbor) != his)
                    previous = neighbor;
            QVERIFY(!previous.isEmpty());
            for (const auto &h : p.neighbors(n))
                if (p.atom(h).element == "H")
                    QVERIFY2((p.atom(h).position - p.atom(previous).position).norm() > 1.8, code);
            // Every amide N–H of the chain, the one next to the ACE cap included.
            for (auto value : p.data["residues"].toArray()) {
                const auto atoms = value.toObject()["atoms"].toObject();
                if (!atoms.contains("N") || !atoms.contains("CA"))
                    continue;
                const auto nitrogen = atoms["N"].toString();
                for (const auto &c : p.neighbors(nitrogen))
                    if (p.atom(c).element == "C" && c != atoms["CA"].toString())
                        for (const auto &h : p.neighbors(nitrogen))
                            if (p.atom(h).element == "H")
                                QVERIFY2((p.atom(h).position - p.atom(c).position).norm() > 1.8,
                                         qPrintable(value.toObject()["code"].toString()));
            }
        }
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
    // The practice data of the recipes (docs/recipes): what the pages say they are, and inputs that
    // generate.
    void recipeSamples() {
        struct Sample {
            const char *name, *formula;
            int atoms, charge, multiplicity;
            const char *input;
        };
        const Sample samples[]{{"ethanol", "C2H6O", 9, 0, 1, "task dft frequencies"},
                               {"ethanol_opt", "C2H6O", 9, 0, 1, "task dft"},
                               {"ethanol_od", "C2H6O", 9, 0, 1, "mass 2.014101778"},
                               {"co_en_complex", "C4H16Cl2CoN4", 27, 1, 1, "library \"def2-svp\""},
                               {"p_nitroaniline", "C6H6N2O2", 16, 0, 1, "task dft frequencies"},
                               {"o_nitroaniline", "C6H6N2O2", 16, 0, 1, "task dft optimize"},
                               {"m_nitroaniline", "C6H6N2O2", 16, 0, 1, "task dft optimize"},
                               {"p_nitroaniline_tddft", "C6H6N2O2", 16, 0, 1, "task tddft energy"},
                               {"caffeine", "C8H10N4O2", 24, 0, 1, "task dft optimize"},
                               {"butane_anti", "C4H10", 14, 0, 1, "geometry"},
                               {"butane_gauche", "C4H10", 14, 0, 1, "geometry"},
                               {"butane_scan", "C4H10", 14, 0, 1, "MolForma scan 13/13"},
                               {"lactic_R", "C3H6O3", 12, 0, 1, "geometry"},
                               {"lactic_S", "C3H6O3", 12, 0, 1, "geometry"},
                               {"butene_E", "C4H8", 12, 0, 1, "task dft frequencies"},
                               {"butene_Z", "C4H8", 12, 0, 1, "task dft frequencies"},
                               {"ace_ahe_nme", "C17H25N6O6", 54, -1, 1, "cosmo"},
                               {"ace_afe_nme", "C20H27N4O6", 57, -1, 1, "cosmo"},
                               {"ace_ae_nme", "C11H18N3O5", 37, -1, 1, "geometry"},
                               {"benzene_water", "C6H8O", 15, 0, 1, "mon m2 13 14 15"},
                               {"phenol", "C6H6O", 13, 0, 1, "task dft frequencies"},
                               {"phenoxyl", "C6H5O", 12, 0, 2, "mult 2"},
                               {"phenolate", "C6H5O", 12, -1, 1, "charge -1"},
                               {"sf6", "F6S", 7, 0, 1, "task dft frequencies"},
                               {"pf5", "F5P", 6, 0, 1, "task dft frequencies"},
                               {"na_h2o4", "H8NaO4", 13, 1, 1, "charge 1"},
                               {"na_h2o4_charges", "H8NaO4", 13, 1, 1, "bq units angstrom"},
                               {"na_h2o5", "H10NaO5", 16, 1, 1, "charge 1"},
                               {"sn2_ts_guess", "CH3ClF", 6, -1, 1, "saddle"},
                               {"sn2_reactant", "CH3ClF", 6, -1, 1, "task dft optimize"},
                               {"tms_benzene", "C9H14Si", 24, 0, 1, "task dft optimize"}};
        for (const auto &s : samples) {
            const auto d = Document::load(QString(RECIPES_DIR) + "/" + s.name + ".molforma.json");
            QVERIFY2(d.formula() == s.formula, s.name);
            QVERIFY2(d.atoms.size() == s.atoms, s.name);
            QVERIFY2(d.job()["charge"].toInt() == s.charge, s.name);
            QVERIFY2(d.job()["multiplicity"].toInt() == s.multiplicity, s.name);
            QVERIFY2(validateNwchem(d).errors.isEmpty(),
                     qPrintable(QString(s.name) + ": " + validateNwchem(d).errors.join(';')));
            QVERIFY2(nwchemInput(d).contains(s.input), s.name);
            QVERIFY2(!d.data.contains("nwchem_execution"), s.name); // No local run settings or paths.
        }
        QCOMPARE(Document::load(RECIPES_DIR "/lactic_R.molforma.json").atoms[0].stereo, QString("R"));
        QCOMPARE(Document::load(RECIPES_DIR "/lactic_S.molforma.json").atoms[0].stereo, QString("S"));
        // The part of recipe 16 replaces a hydrogen of benzene; the 2D file of recipe 4 has no hydrogens.
        QFile file(RECIPES_DIR "/trimethylsilyl.molforma-part.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto part = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(part["category"].toString(), QString("substituent"));
        Document benzene;
        placeTemplate(benzene, findTemplate("skeleton:benzene"), Vec::Zero());
        QString hydrogen;
        for (const auto &a : benzene.atoms)
            if (a.element == "H")
                hydrogen = a.id;
        attachPart(benzene, hydrogen, part);
        QCOMPARE(benzene.formula(), QString("C9H14Si"));
        QCOMPARE(importStructure(RECIPES_DIR "/caffeine_2d.sdf").formula(), QString("C8N4O2"));
    }
    void projectRoundTrip() {
        for (auto name : {"benzoic_acid", "co_en", "ala_gly_ser", "water_dimer_counterpoise"}) {
            auto d = Document::load(QString(FIXTURE_DIR) + "/" + name + ".molforma.json");
            QTemporaryDir temp;
            QString file = temp.filePath("test.molforma.json");
            d.save(file);
            const auto saved = parseJson(readFile(file));
            QCOMPARE(saved["format"].toString(), QString("molforma-project"));
            QCOMPARE(saved["version"].toInt(), 1);
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
        // Errors name the line, so a broken file can be fixed.
        try {
            Document::fromXyz("2\nbroken\nO 0 0 0\nH 1 x 0\n");
            QFAIL("accepted a broken coordinate");
        } catch (const std::exception &e) {
            QVERIFY(QString::fromUtf8(e.what()).contains("4行目"));
        }
    }
    void bondKindChangesInPlace() {
        // Setting the kind a bond already has changes nothing (no reordering, no history entry).
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH .96 0 0\nH -.24 .93 0\n");
        const auto before = d.toJson();
        d.setBond(d.bonds[0].a, d.bonds[0].b, d.bonds[0].kind);
        QCOMPARE(d.toJson(), before);
        const auto a = d.bonds[0].a, b = d.bonds[0].b;
        d.setBond(b, a, "double");
        QCOMPARE(d.bonds[0].kind, QString("double"));
        QCOMPARE(d.bonds[0].a, a);
        d.setBond(a, b, "none");
        QCOMPARE(d.bonds.size(), 1);
    }
    void xyzBondOrders() {
        // XYZ has no bond orders; perceived orders keep neutral organic templates saturated as drawn.
        int checked = 0;
        for (auto v : templates()) {
            const auto t = v.toObject();
            Document d;
            instantiate(d, t);
            const bool plain = std::all_of(d.atoms.begin(), d.atoms.end(), [](const Atom &a) {
                return !a.charge && !a.radical && element(a.element).number <= 36 &&
                       !QStringList{"Li", "Be", "Na", "Mg", "Al", "K", "Ca"}.contains(a.element) &&
                       (element(a.element).number < 21 || element(a.element).number > 30);
            });
            if (!plain || d.atoms.size() < 2)
                continue;
            auto copy = xyzStructure(d.xyz());
            completeHydrogens(copy);
            QVERIFY2(copy.formula() == d.formula(), qPrintable(t["id"].toString() + " " + copy.formula()));
            ++checked;
        }
        QVERIFY(checked > 40);
        auto benzene = xyzStructure([] {
            Document d;
            instantiate(d, findTemplate("skeleton:benzene"));
            return d.xyz();
        }());
        QVERIFY(std::none_of(benzene.bonds.begin(), benzene.bonds.end(), [&](const Bond &b) {
            return b.kind == "single" && benzene.atom(b.a).element == "C" && benzene.atom(b.b).element == "C";
        }));
    }
    void fragmentsAndSkeletonParts() {
        // A second molecule lands 3 Å beyond the first along +X; the first does not move.
        Document d;
        placeTemplate(d, findTemplate("skeleton:water"), Vec::Zero());
        d.atoms[0].fixed = true;
        const auto first = d.atoms[0].position;
        Document other;
        placeTemplate(other, findTemplate("skeleton:water"), Vec(10, 10, 10));
        const auto added = insertFragment(d, other);
        QCOMPARE(added.size(), 3);
        QCOMPARE(d.atoms.size(), 6);
        QCOMPARE(d.bonds.size(), 4);
        QCOMPARE(d.atoms[0].position, first);
        double right = -1e9, left = 1e9;
        for (int i = 0; i < 3; ++i) {
            right = std::max(right, d.atoms[i].position.x());
            left = std::min(left, d.atoms[i + 3].position.x());
        }
        QVERIFY(std::abs(left - right - 3) < 1e-12);
        QVERIFY(d.validate().isEmpty());
        QVERIFY_EXCEPTION_THROWN(insertFragment(d, other), std::runtime_error); // Same IDs again.
        // A total charge that matched the formal charges follows the added ion.
        auto chloride = Document::fromXyz("1\nchloride\nCl 0 0 0\n");
        chloride.atoms[0].charge = -1;
        insertFragment(d, chloride);
        QCOMPARE(d.job()["charge"].toInt(), -1);
        // Skeleton parts are whole molecules without connection points, placed by clicking.
        Document water;
        placeTemplate(water, findTemplate("skeleton:water"), Vec::Zero());
        const auto part = makePart(water, water.ids(), "my water", "skeleton");
        QVERIFY(part["connections"].toArray().isEmpty());
        QCOMPARE(part["category"].toString(), QString("skeleton"));
        validateTemplate(part);
        Document placed;
        placeTemplate(placed, part, Vec(1, 2, 3));
        QCOMPARE(placed.formula(), QString("H2O"));
        QVERIFY_EXCEPTION_THROWN(makePart(water, {water.atoms[0].id, water.atoms[1].id}, "piece", "skeleton"),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(makePart(water, water.ids(), "bad", "fragment"), std::runtime_error);
    }
    void symmetryNumber() {
        // Rotational symmetry numbers for thermochemistry hints (NWChem runs with noautosym).
        for (const auto &[id, sigma] : {std::pair{"skeleton:water", 2},
                                        {"skeleton:ammonia", 3},
                                        {"skeleton:methane", 12},
                                        {"skeleton:carbon_dioxide", 2},
                                        {"skeleton:ethane", 6},
                                        {"skeleton:benzene", 12},
                                        {"skeleton:cyclohexane", 6},
                                        {"skeleton:imidazole", 1},
                                        {"ligand:carbonyl", 1},
                                        {"ligand:en", 2}}) {
            Document d;
            instantiate(d, findTemplate(id));
            QVERIFY2(rotationalSymmetryNumber(d) == sigma,
                     qPrintable(QString("%1: %2").arg(id).arg(rotationalSymmetryNumber(d))));
        }
        Document hdo;
        instantiate(hdo, findTemplate("skeleton:water"));
        hdo.atoms[1].isotope = 2;
        QCOMPARE(rotationalSymmetryNumber(hdo), 0);
    }
    void pointGroups() {
        for (const auto &[id, expected] : {std::pair{"skeleton:water", "C2v"},
                                           {"skeleton:ammonia", "C3v"},
                                           {"skeleton:methane", "Td"},
                                           {"skeleton:carbon_dioxide", "D∞h"},
                                           {"skeleton:acetylene", "D∞h"},
                                           {"skeleton:ethylene", "D2h"},
                                           {"skeleton:benzene", "D6h"},
                                           {"skeleton:naphthalene", "D2h"},
                                           {"skeleton:pyridine", "C2v"},
                                           {"skeleton:imidazole", "Cs"}}) {
            Document d;
            instantiate(d, findTemplate(id));
            const auto group = pointGroup(d);
            QVERIFY2(group.name == expected, qPrintable(QString("%1: %2").arg(id, group.name)));
        }
        // Built from exact coordinates: octahedral SF6, icosahedral B12, allene (D2d), HCN (C∞v),
        // a twisted H2O2 (C2), CHFClBr (C1), an inversion-only center (Ci) and S4.
        const auto build = [](const QVector<std::pair<QString, Vec>> &atoms) {
            Document d;
            for (const auto &[symbol, position] : atoms) {
                Atom a;
                a.element = symbol;
                a.position = position;
                d.atoms << a;
            }
            return d;
        };
        QVector<std::pair<QString, Vec>> sf6{{"S", Vec::Zero()}};
        for (int axis = 0; axis < 3; ++axis)
            for (double sign : {1., -1.})
                sf6.append({"F", Vec::Unit(axis) * 1.56 * sign});
        QCOMPARE(pointGroup(build(sf6)).name, QString("Oh"));
        QVector<std::pair<QString, Vec>> icosahedron;
        const double phi = (1 + std::sqrt(5.)) / 2;
        for (double a : {1., -1.})
            for (double b : {phi, -phi}) {
                icosahedron.append({"B", Vec(0, a, b)});
                icosahedron.append({"B", Vec(a, b, 0)});
                icosahedron.append({"B", Vec(b, 0, a)});
            }
        const auto ih = pointGroup(build(icosahedron));
        QCOMPARE(ih.name, QString("Ih"));
        QCOMPARE(ih.operations.size(), 120);
        QCOMPARE(pointGroup(build({{"C", Vec(0, 0, 0)},
                                   {"C", Vec(0, 0, 1.31)},
                                   {"C", Vec(0, 0, -1.31)},
                                   {"H", Vec(.93, 0, 1.87)},
                                   {"H", Vec(-.93, 0, 1.87)},
                                   {"H", Vec(0, .93, -1.87)},
                                   {"H", Vec(0, -.93, -1.87)}}))
                     .name,
                 QString("D2d"));
        QCOMPARE(
            pointGroup(build({{"H", Vec(0, 0, -1.06)}, {"C", Vec(0, 0, 0)}, {"N", Vec(0, 0, 1.15)}})).name,
            QString("C∞v"));
        QCOMPARE(pointGroup(build({{"O", Vec(.7, 0, 0)},
                                   {"O", Vec(-.7, 0, 0)},
                                   {"H", Vec(.9, .9, .2)},
                                   {"H", Vec(-.9, -.9, .2)}}))
                     .name,
                 QString("C2"));
        QCOMPARE(pointGroup(build({{"C", Vec(0, 0, 0)},
                                   {"H", Vec(0, 0, 1.09)},
                                   {"F", Vec(1.3, 0, -.4)},
                                   {"Cl", Vec(-.8, 1.4, -.5)},
                                   {"Br", Vec(-.9, -1.5, -.6)}}))
                     .name,
                 QString("C1"));
        QCOMPARE(pointGroup(build({{"C", Vec(1, .2, .3)},
                                   {"C", Vec(-1, -.2, -.3)},
                                   {"F", Vec(.4, 1.1, -.7)},
                                   {"F", Vec(-.4, -1.1, .7)},
                                   {"Cl", Vec(.2, -.9, 1.3)},
                                   {"Cl", Vec(-.2, .9, -1.3)}}))
                     .name,
                 QString("Ci"));
        // Symmetrization averages a slightly distorted water onto C2v; fixed atoms must not move.
        Document water;
        instantiate(water, findTemplate("skeleton:water"));
        // Stretch one O–H bond by 0.02 Å.
        water.atoms[1].position += (water.atoms[1].position - water.atoms[0].position).normalized() * .02;
        const auto group = pointGroup(water);
        QCOMPARE(group.name, QString("C2v"));
        QVERIFY(group.deviation > .01);
        auto fixed = water;
        fixed.atoms[1].fixed = true;
        QVERIFY_THROWS_EXCEPTION(std::exception, symmetrize(fixed, group));
        QVERIFY(symmetrize(water, group) > 0);
        const auto exact = pointGroup(water, 1e-6);
        QCOMPARE(exact.name, QString("C2v"));
        QVERIFY(exact.deviation < 1e-9);
        QVERIFY(pointGroup(water, .001).name == "C2v");
    }
    void conformersAndSuperposition() {
        const auto butane = fromSmiles("CCCC");
        const auto conformers = searchConformers(butane, 5);
        QVERIFY(conformers.size() >= 2); // anti and gauche
        for (int i = 1; i < conformers.size(); ++i)
            QVERIFY(conformers[i].energy >= conformers[i - 1].energy);
        const auto &best = conformers.first().document;
        QCOMPARE(best.atoms.size(), butane.atoms.size());
        const auto carbons = [](const Document &d) {
            Ids ids;
            for (const auto &a : d.atoms)
                if (a.element == "C")
                    ids << a.id;
            return ids;
        };
        // The lowest conformer is anti (C–C–C–C near 180°).
        QVERIFY(std::abs(measure(best, carbons(best))) > 150);
        auto fixed = butane;
        fixed.atoms[0].fixed = true;
        QVERIFY_THROWS_EXCEPTION(std::exception, searchConformers(fixed));
        // Superposition removes rigid motion; different conformers keep a residual RMSD.
        auto moved = best;
        transform(moved, moved.ids(), Eigen::AngleAxisd(.7, Vec(1, 2, 3).normalized()).toRotationMatrix(),
                  Vec(3, -1, 2));
        const auto fit = superpose(moved, best);
        QVERIFY(fit.raw > 1);
        QVERIFY(fit.rmsd < 1e-9 && fit.maximum < 1e-9);
        QVERIFY(superpose(conformers[1].document, best).heavyRmsd > .25);
        QVERIFY_THROWS_EXCEPTION(std::exception, superpose(best, fromSmiles("CCC")));
    }
    void zMatrixRows() {
        Document d;
        instantiate(d, findTemplate("skeleton:ethane"));
        const auto rows = zMatrix(d);
        QCOMPARE(rows.size(), d.atoms.size());
        QCOMPARE(rows[0].bond, -1);
        for (int i = 1; i < rows.size(); ++i) {
            const auto &r = rows[i];
            QVERIFY(r.bond >= 0 && r.bond < i);
            QVERIFY(std::abs(r.distance - measure(d, {d.atoms[i].id, d.atoms[r.bond].id})) < 1e-12);
            // Bonded references first: every atom of ethane hangs on a bonded earlier atom.
            QVERIFY(d.bond(d.atoms[i].id, d.atoms[r.bond].id));
            if (i >= 2) {
                QVERIFY(r.angle >= 0 && r.angle < i && r.angle != r.bond);
                QVERIFY(std::abs(r.angleValue - measure(d, {d.atoms[i].id, d.atoms[r.bond].id,
                                                            d.atoms[r.angle].id})) < 1e-9);
            }
            if (i >= 3) {
                QVERIFY(r.dihedral >= 0 && r.dihedral < i);
                QVERIFY(std::abs(r.dihedralValue -
                                 measure(d, {d.atoms[i].id, d.atoms[r.bond].id, d.atoms[r.angle].id,
                                             d.atoms[r.dihedral].id})) < 1e-9);
            }
        }
        const auto text = zMatrixText(d).split('\n', Qt::SkipEmptyParts);
        QCOMPARE(text.size(), d.atoms.size());
        QCOMPARE(text[0], d.atoms[0].element);
        QCOMPARE(text[3].split(' ').size(), 7);
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
    void atomIntoBond() {
        const auto angle = [](const Document &d, const QString &a, const QString &b, const QString &c) {
            const Vec u = d.atom(a).position - d.atom(b).position,
                      v = d.atom(c).position - d.atom(b).position;
            return std::acos(u.dot(v) / u.norm() / v.norm()) * 180 / M_PI;
        };
        // Ethane → dimethyl ether: covalent bond lengths and a bent C–O–C, not O on the C–C midpoint
        // (a straight 180° angle the force field cannot open).
        Document d;
        auto c1 = addAtom(d, "C", Vec::Zero());
        auto c2 = addAtom(d, "C", Vec::Zero(), d.neighbors(c1).first());
        auto o = insertAtom(d, c1, c2, "O");
        QCOMPARE(d.formula(), QString("C2H6O"));
        QVERIFY(!d.bond(c1, c2) && d.bond(c1, o) && d.bond(o, c2));
        QVERIFY(std::abs((d.atom(o).position - d.atom(c1).position).norm() - 1.42) < .05);
        QVERIFY(std::abs((d.atom(o).position - d.atom(c2).position).norm() - 1.42) < .05);
        QVERIFY(std::abs(angle(d, c1, o, c2) - 109.47) < .5);
        QCOMPARE(refine(d).formula(), QString("C2H6O"));
        // A carbon becomes CH2 (propane), and fixed atoms on one side keep that side in place.
        Document fixed;
        auto a = addAtom(fixed, "C", Vec::Zero());
        auto b = addAtom(fixed, "C", Vec::Zero(), fixed.neighbors(a).first());
        for (const auto &id : fixed.component(a, a, b))
            fixed.atom(id).fixed = true;
        const auto before = fixed;
        auto c = insertAtom(fixed, a, b, "C");
        QCOMPARE(fixed.formula(), QString("C3H8"));
        for (const auto &atom : before.atoms)
            if (atom.fixed)
                QVERIFY((fixed.atom(atom.id).position - atom.position).norm() < 1e-12);
        QVERIFY(std::abs(angle(fixed, a, c, b) - 109.47) < .5);
        // In a ring nothing else moves, and the new atom is off the bond line.
        auto ring = fromSmiles("C1CCCCC1");
        QString r1, r2;
        for (const auto &bond : ring.bonds)
            if (ring.atom(bond.a).element == "C" && ring.atom(bond.b).element == "C") {
                r1 = bond.a;
                r2 = bond.b;
                break;
            }
        const auto old = ring;
        auto n = insertAtom(ring, r1, r2, "N");
        QCOMPARE(ring.formula(), QString("C6H13N"));
        for (const auto &atom : old.atoms)
            QVERIFY((ring.atom(atom.id).position - atom.position).norm() < 1e-12);
        QVERIFY(angle(ring, r1, n, r2) < 170);
        QVERIFY_EXCEPTION_THROWN(insertAtom(ring, r1, r2, "O"), std::runtime_error); // No bond any more.
    }
    void aromaticHydrogens() {
        // Aromatic rings count by their Kekulé form, so completion keeps every template intact.
        for (auto v : templates()) {
            Document d;
            instantiate(d, v.toObject());
            const auto formula = d.formula();
            try {
                completeHydrogens(d);
            } catch (const std::exception &e) {
                QFAIL(qPrintable(v.toObject()["id"].toString() + ": " + e.what()));
            }
            QVERIFY2(d.formula() == formula, qPrintable(v.toObject()["id"].toString() + " " + d.formula()));
            QVERIFY2(!structureReport(d).contains("結合次数合計"),
                     qPrintable(v.toObject()["id"].toString() + "\n" + structureReport(d)));
        }
        // A clean structure says so; a finding replaces that line.
        Document water;
        instantiate(water, findTemplate("skeleton:water"));
        QVERIFY(structureReport(water).endsWith("問題は見つかりませんでした。"));
        water.setJobValue("multiplicity", 2);
        QVERIFY(!structureReport(water).contains("問題は見つかりませんでした"));
        // Complexes: free coordination sites, and a charge that disagrees with the oxidation number.
        Document zinc;
        addCenter(zinc, "Zn", "tetrahedral", Vec::Zero(), 2.1, 2);
        QCOMPARE(zinc.job()["charge"].toInt(), 2); // The total charge followed the oxidation number.
        zinc.setJobValue("charge", 0);
        auto report = structureReport(zinc);
        QVERIFY2(report.contains("Zn1: 配位サイト 4 のうち 4 が空いています"), qPrintable(report));
        QVERIFY(report.contains("見込まれる全電荷は 2"));
        QVERIFY(!report.contains("形式電荷の合計と設定電荷が異なります")); // The center line says it.
        zinc.setJobValue("charge", 2);
        QVERIFY(!structureReport(zinc).contains("見込まれる全電荷"));
        // Without its N–H, pyrrole gets the hydrogen back on nitrogen rather than an sp3 carbon.
        for (const auto &[id, element] : {std::pair{"skeleton:pyrrole", "N"}, {"skeleton:imidazole", "N"}}) {
            Document d;
            instantiate(d, findTemplate(id));
            const auto formula = d.formula();
            for (const auto &a : d.atoms)
                if (a.element == element)
                    for (const auto &n : d.neighbors(a.id))
                        if (d.atom(n).element == "H")
                            d.remove({n});
            completeHydrogens(d);
            QCOMPARE(d.formula(), formula);
            int nh = 0;
            for (const auto &a : d.atoms)
                if (a.element == "N")
                    for (const auto &n : d.neighbors(a.id))
                        nh += d.atom(n).element == "H";
            QCOMPARE(nh, 1);
        }
        // A thiophene carbon that lost its H gets it back; sulfur stays divalent.
        Document thiophene;
        instantiate(thiophene, findTemplate("skeleton:thiophene"));
        for (const auto &a : thiophene.atoms)
            if (a.element == "H") {
                thiophene.remove({a.id});
                break;
            }
        completeHydrogens(thiophene);
        QCOMPARE(thiophene.formula(), QString("C4H4S"));
        // Residue repair completes hydrogens too; aromatic side chains keep their protonation.
        for (const auto &code : {"HIS", "TRP", "PHE", "TYR"}) {
            Document d;
            buildPeptide(d, {"GLY", code, "GLY"}, "charged", "charged");
            const auto formula = d.formula();
            completeHydrogens(d);
            QCOMPARE(d.formula(), formula);
            for (auto v : d.data["residues"].toArray())
                repairResidue(d, v.toObject()["id"].toString());
            QCOMPARE(d.formula(), formula);
        }
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
        // Intermolecular distance, angle and dihedral move the molecule of the later atoms.
        Document dimer;
        placeTemplate(dimer, findTemplate("skeleton:water"), Vec::Zero());
        placeTemplate(dimer, findTemplate("skeleton:water"), Vec(0, 0, 4));
        const auto o1 = dimer.atoms[0].id, h1 = dimer.atoms[1].id, o2 = dimer.atoms[3].id,
                   h2 = dimer.atoms[4].id;
        const auto first = dimer.atoms[0].position;
        setDimension(dimer, {o1, o2}, 2.9);
        QVERIFY(std::abs(measure(dimer, {o1, o2}) - 2.9) < 1e-10);
        setDimension(dimer, {h1, o1, o2}, 100);
        QVERIFY(std::abs(measure(dimer, {h1, o1, o2}) - 100) < 1e-8);
        setDimension(dimer, {h1, o1, o2, h2}, 45);
        QVERIFY(std::abs(measure(dimer, {h1, o1, o2, h2}) - 45) < 1e-8);
        QVERIFY(std::abs(measure(dimer, {o2, h2}) - measure(dimer, {o1, h1})) < 1e-8);
        QVERIFY((dimer.atoms[0].position - first).norm() < 1e-12);
        // Within one molecule, non-bonded atoms have no side to move.
        QVERIFY_EXCEPTION_THROWN(setDimension(dimer, {h1, dimer.atoms[2].id}, 2), std::runtime_error);
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
        // [ZnCl4]2−: the total charge followed Zn(II) and the chlorides; it keeps a chosen value.
        QCOMPARE(d.job()["charge"].toInt(), -2);
        QCOMPARE(d.expectedCharge(), -2);
        QCOMPARE(d.formalCharge(), -4);
        adjustCenter(d, zinc, "tetrahedral", 2.5, 3);
        QCOMPARE(d.job()["charge"].toInt(), -1);
        d.setJobValue("charge", 5);
        adjustCenter(d, zinc, "tetrahedral", 2.5, 2);
        QCOMPARE(d.job()["charge"].toInt(), 5);
        // trans-[Co(en)2Cl2]+ and SF6 (a main-group center).
        Document co;
        auto cobalt = addCenter(co, "Co", "octahedral", Vec::Zero(), 2.0, 3);
        placeLigand(co, cobalt, {0, 1}, findTemplate("ligand:en"));
        placeLigand(co, cobalt, {2, 3}, findTemplate("ligand:en"));
        placeLigand(co, cobalt, {4}, findTemplate("ligand:chloride"));
        placeLigand(co, cobalt, {5}, findTemplate("ligand:chloride"));
        QCOMPARE(co.formula(), QString("C4H16Cl2CoN4"));
        QCOMPARE(co.job()["charge"].toInt(), 1);
        QVERIFY(validateNwchem(co).warnings.filter("見込まれる").isEmpty());
        Document sf6;
        auto sulfur = addCenter(sf6, "S", "octahedral", Vec::Zero(), 1.56, 6);
        for (int i = 0; i < 6; ++i)
            placeLigand(sf6, sulfur, {i}, findTemplate("ligand:fluoride"));
        QCOMPARE(sf6.formula(), QString("F6S"));
        QCOMPARE(sf6.job()["charge"].toInt(), 0);
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
        auto ids = d.ids();
        std::reverse(ids.begin(), ids.end());
        waterToCharges(d, ids);
        QVERIFY(d.atoms.isEmpty());
        QCOMPARE(d.data["point_charges"].toArray().size(), 3);
        QCOMPARE(d.data["point_charges"].toArray()[0].toObject()["charge"].toDouble(), -.834);
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
        // Heavy main-group elements are refined; only metals and ghosts are refused.
        for (const auto &[smiles, formula] :
             {std::pair{"c1ccccc1Br", "C6H5Br"}, {"CI", "CH3I"}, {"C[Se]C", "C2H6Se"}})
            QCOMPARE(fromSmiles(smiles).formula(), QString(formula));
        // UFF (no MMFF94 types for B, Se, ...): Open Babel 3.2 copies of UFF used to abort.
        for (int i = 0; i < 5; ++i)
            QCOMPARE(fromSmiles("CB(C)C").data["metadata"].toObject()["molforma_force_field"].toString(),
                     QString("UFF"));
        auto metal = fromSmiles("CCO");
        metal.atoms[0].element = "Fe";
        QVERIFY_EXCEPTION_THROWN(refine(metal), std::runtime_error);
        QTemporaryDir tmp;
        exportStructure(d, tmp.filePath("ethanol.sdf"));
        auto copy = importStructure(tmp.filePath("ethanol.sdf"));
        QCOMPARE(copy.formula(), d.formula());
        QCOMPARE(copy.bonds.size(), d.bonds.size());
    }
};
QTEST_APPLESS_MAIN(CoreTest)
#include "test_core.moc"
