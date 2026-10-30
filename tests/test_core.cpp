// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/chemistry.h"
#include "editing/coordinates.h"
#include "editing/coordination.h"
#include "editing/editor_session.h"
#include "editing/mmcif.h"
#include "editing/operations.h"
#include "editing/peptide.h"
#include "editing/site.h"
#include "editing/stereochemistry.h"
#include "gzip.h"
#include "io/structure_io.h"
#include "model/geometry.h"
#include "model/structure_file.h"
#include "model/topology.h"
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
        // Edits made now are told apart from undo/redo (and loads), which never trust settings.
        QCOMPARE(applied[0][3].value<EditorSession::Change>(), EditorSession::Change::Edit);
        QCOMPARE(applied[1][3].value<EditorSession::Change>(), EditorSession::Change::History);
        QCOMPARE(applied[2][3].value<EditorSession::Change>(), EditorSession::Change::History);
        QVERIFY(!session.edit("no-op", [](Document &) {}));
        QCOMPARE(session.history().count(), 1);
        QVERIFY(session.edit("candidate", [](Document &next) { next.atoms[1].position.z() = 2; }, true));
        session.cancel();
        QCOMPARE(session.document().atoms[1].position.z(), 1.0);
        session.reset(d);
        QVERIFY(session.history().isClean());
        QVERIFY(!session.history().canUndo());
    }

    void topologyMatchesDocumentLookups() {
        Document d;
        buildPeptide(d, {"ALA", "SER"}, "charged", "charged");
        auto water = Document::fromXyz("3\n\nO 0 0 20\nH .76 0 20.6\nH -.76 0 20.6\n");
        d.atoms += water.atoms;
        d.bonds += water.bonds;
        d.bonds.last().kind = "dative";
        const Topology topology(d);
        QCOMPARE(topology.size(), d.atoms.size());
        QCOMPARE(topology.index("missing"), -1);
        for (int i = 0; i < d.atoms.size(); ++i) {
            const auto &id = d.atoms[i].id;
            QCOMPARE(topology.index(id), d.index(id));
            QCOMPARE(topology.neighbors(d, id), d.neighbors(id));
            QCOMPARE(topology.neighbors(d, id, false), d.neighbors(id, false));
            QCOMPARE(topology.neighbors(i).size(), size_t(d.neighbors(id).size()));
            for (const auto &n : d.neighbors(id)) {
                QCOMPARE(topology.bond(d, id, n), d.bond(id, n));
                QVERIFY(topology.bonded(i, d.index(n)));
            }
        }
        QCOMPARE(topology.bond(d, d.atoms[0].id, d.atoms.last().id), nullptr);
        // Breadth-first order from the root, as Document::component, and the far side of a bond.
        const auto chain = topology.component(0);
        QCOMPARE(chain.size(), d.atoms.size() - 3);
        Ids ids;
        for (int i : chain)
            ids << d.atoms[i].id;
        QCOMPARE(d.component(d.atoms[0].id), ids);
        const auto &n = d.residues[1].atoms["N"], &c = d.residues[0].atoms["C"];
        const auto side = d.bondSide(c, n);
        QCOMPARE(side.size(), d.residues[1].atoms.size());
        QCOMPARE(QSet<QString>(side.begin(), side.end()),
                 QSet<QString>(d.residues[1].atoms.begin(), d.residues[1].atoms.end()));
        const auto molecule = topology.molecules();
        QCOMPARE(molecule.first(), 0);
        QCOMPARE(molecule.last(), 1);
        QCOMPARE(std::count(molecule.begin(), molecule.end(), 1), 3);
        // Indices of a list of atoms keep its order; an unknown atom fails.
        QCOMPARE(d.indices({d.atoms[4].id, d.atoms[1].id, d.atoms[4].id}), (QVector<int>{4, 1, 4}));
        QVERIFY_EXCEPTION_THROWN(d.indices({d.atoms[1].id, "missing"}), std::runtime_error);
    }

    void counterpoiseMonomersInAtomOrder() {
        // A ghost atom bonded to a molecule belongs to its monomer; a molecule of ghosts only is none.
        auto d = Document::fromXyz("7\n\nO 0 0 0\nH .76 0 .6\nO 0 0 5\nH .76 0 5.6\nH -.76 0 .6\n"
                                   "H -.76 0 5.6\nHe 0 0 9\n");
        d.atoms[4].ghost = true;
        d.atoms[6].ghost = true;
        d.setBond(d.atoms[0].id, d.atoms[4].id, "single");
        QCOMPARE(nwchemBsseMonomers(d), (QVector<QVector<int>>{{0, 1, 4}, {2, 3, 5}}));
    }

    void largeStructureEdits() {
        // A 500-residue chain: building it and editing it stay linear in its size.
        QStringList codes;
        for (int i = 0; i < 500; ++i)
            codes << QStringList{"ALA", "GLY", "SER", "LYS", "GLU"}[i % 5];
        Document d;
        QElapsedTimer timer;
        timer.start();
        buildPeptide(d, codes, "charged", "charged");
        QVERIFY(d.validate().isEmpty());
        QCOMPARE(d.residues.size(), 500);
        QCOMPARE(d.chains.size(), 1);
        QCOMPARE(d.chains.first().size(), 500);
        for (int i = 0; i < d.residues.size(); ++i) {
            QCOMPARE(d.residues[i].number, i + 1);
            QCOMPARE(d.chains.first()[i], d.residues[i].id);
        }
        QCOMPARE(d.residues.first().nTerminal, QString("charged"));
        QCOMPARE(d.residues.last().cTerminal, QString("charged"));
        QCOMPARE(d.residues[250].nTerminal, QString("internal"));
        QCOMPARE(d.parts.size(), 500);
        QCOMPARE(d.component(d.atoms.first().id).size(), d.atoms.size());
        QCOMPARE(nwchemBsseMonomers(d).size(), 1);
        auto stripped = d;
        Ids hydrogens;
        for (const auto &a : stripped.atoms)
            if (a.element == "H")
                hydrogens << a.id;
        stripped.remove(hydrogens);
        completeHydrogens(stripped);
        QCOMPARE(stripped.atoms.size(), d.atoms.size());
        QCOMPARE(zMatrix(d).size(), d.atoms.size());
        // Generous: the quadratic versions took minutes here.
        QVERIFY2(timer.elapsed() < 60000, qPrintable(QString::number(timer.elapsed())));
    }

    void atomDeletionPolicies() {
        Document ethane;
        placeTemplate(ethane, findTemplate("skeleton:ethane"), Vec::Zero());
        ethane.envelope["future_field"] = 42;
        const auto carbon = ethane.atoms.first().id;
        QCOMPARE(ethane.atom(carbon).element, QString("C"));
        const auto original = ethane.project();
        EditorSession session;
        session.reset(ethane);
        // The MCP policy removes precisely the IDs requested, leaving attached hydrogens.
        session.edit("explicit deletion", [&](Document &d) { deleteAtoms(d, {carbon}); }, true);
        QCOMPARE(session.document().project(), original);
        QCOMPARE(session.candidate()->atoms.size(), ethane.atoms.size() - 1);
        session.cancel();
        // GUI deletion includes attached hydrogens; termination of the cut bond is optional.
        session.edit(
            "uncapped deletion",
            [&](Document &d) { deleteAtoms(d, {carbon}, {.includeAttachedHydrogens = true}); }, true);
        QCOMPARE(session.candidate()->formula(), QString("CH3"));
        session.cancel();
        session.edit(
            "capped deletion",
            [&](Document &d) {
                deleteAtoms(d, {carbon}, {.includeAttachedHydrogens = true, .capCutBonds = true});
            },
            true);
        QCOMPARE(session.candidate()->formula(), QString("CH4"));
        QCOMPARE(session.candidate()->envelope, ethane.envelope);
        QVERIFY(session.confirm());
        session.navigate("undo");
        QCOMPARE(session.document().project(), original);
        session.navigate("redo");
        QCOMPARE(session.document().formula(), QString("CH4"));
        const auto hydrogen = session.document().neighbors(session.document().atoms.first().id).first();
        session.edit("remove hydrogen", [&](Document &d) {
            deleteAtoms(d, {hydrogen}, {.includeAttachedHydrogens = true, .capCutBonds = true});
        });
        QCOMPARE(session.document().formula(), QString("CH3"));

        for (const auto &id : ethane.neighbors(carbon))
            if (ethane.atom(id).element == "H") {
                ethane.atom(id).fixed = true;
                break;
            }
        session.reset(ethane);
        const auto fixed = session.document().project();
        QVERIFY_EXCEPTION_THROWN(session.edit(
                                     "fixed hydrogen",
                                     [&](Document &d) {
                                         deleteAtoms(d, {carbon},
                                                     {.includeAttachedHydrogens = true, .capCutBonds = true});
                                     },
                                     true),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(
            session.edit("unknown atom", [&](Document &d) { deleteAtoms(d, {carbon, "missing"}); }),
            std::runtime_error);
        QCOMPARE(session.document().project(), fixed);
        QVERIFY(!session.candidate());
        QCOMPARE(session.history().count(), 0);
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
        auto ca = d.residues.first().atoms["CA"];
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
        d.residues.clear();
        d.chains.clear();
        for (auto &a : d.atoms)
            a.name.clear();
        recognizeResidues(d);
        QCOMPARE(d.residues.size(), 3);
        QStringList codes;
        for (const auto &r : d.residues)
            codes << r.code;
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
            for (const auto &r : doc.residues)
                byNumber[r.number] = r.code + ":" + r.state + ":" + r.nTerminal + "/" + r.cTerminal;
            return byNumber.values();
        };
        auto [built, read] = fromXyz("AHE", "ACE", "NME", "default");
        QCOMPARE(read.formalCharge(), built.formalCharge());
        QCOMPARE(read.formalCharge(), -1);
        QCOMPARE(states(read), QStringList({"ACE:default:internal/internal", "ALA:default:ACE/internal",
                                            "HIS:HIE:internal/internal", "GLU:default:internal/NME",
                                            "NME:default:internal/internal"}));
        QCOMPARE(read.chains.first().size(), 5);
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
        const auto r = d.residues.first();
        QString rid = r.id, cb = r.atoms["CB"];
        auto ca = r.atoms["CA"];
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
        QCOMPARE(d.residues.size(), 5);
        QVERIFY2(d.validate().isEmpty(), qPrintable(d.validate().join(';')));
        duplicate(d, d.ids(), Vec(30, 0, 0));
        QCOMPARE(d.chains.size(), 2);
        for (const auto &sequence : d.chains)
            QCOMPARE(sequence.size(), 5);
        QVERIFY(d.validate().isEmpty());
    }
    void cappedResidueEdits() {
        Document d;
        buildPeptide(d, {"ALA"}, "ACE", "NME");
        QString rid;
        for (const auto &r : d.residues)
            if (r.code == "ALA")
                rid = r.id;
        mutateResidue(d, rid, "GLY", "default", "L");
        QCOMPARE(d.residues.size(), 3);
        QCOMPARE(d.formula(), QString("C5H10N2O2"));
        QVERIFY2(d.validate().isEmpty(), qPrintable(d.validate().join(';')));
        auto copy = d;
        deleteResidue(copy, rid);
        QVERIFY(copy.atoms.isEmpty());
        QVERIFY(copy.chains.isEmpty());
        setTerminals(d, rid, "charged", "charged");
        QCOMPARE(d.residues.size(), 1);
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
            for (const auto &r : p.residues)
                if (r.code == "HIS")
                    his = r.id;
            mutateResidue(p, his, code, "default", "L");
            const auto n = residue(p, his).atoms["N"];
            QString previous;
            for (const auto &neighbor : p.neighbors(n))
                if (p.atom(neighbor).element == "C" && residueForAtom(p, neighbor) != his)
                    previous = neighbor;
            QVERIFY(!previous.isEmpty());
            for (const auto &h : p.neighbors(n))
                if (p.atom(h).element == "H")
                    QVERIFY2((p.atom(h).position - p.atom(previous).position).norm() > 1.8, code);
            // Every amide N–H of the chain, the one next to the ACE cap included.
            for (const auto &r : p.residues) {
                if (!r.atoms.contains("N") || !r.atoms.contains("CA"))
                    continue;
                const auto nitrogen = r.atoms["N"];
                for (const auto &c : p.neighbors(nitrogen))
                    if (p.atom(c).element == "C" && c != r.atoms["CA"])
                        for (const auto &h : p.neighbors(nitrogen))
                            if (p.atom(h).element == "H")
                                QVERIFY2((p.atom(h).position - p.atom(c).position).norm() > 1.8,
                                         qPrintable(r.code));
            }
        }
    }
    void pdbRoundTrip() {
        Document d;
        buildPeptide(d, {"ALA", "GLY", "SER"}, "charged", "charged");
        QTemporaryDir temp;
        auto path = temp.filePath("peptide.pdb");
        exportStructure(d, path);
        auto copy = importStructure(path);
        QCOMPARE(copy.atoms.size(), 31);
        QCOMPARE(copy.residues.size(), 3);
        QVERIFY(copy.validate().isEmpty());
    }
    // An mmCIF file reads as the PDB file of the same atoms: names, residues, chains, bonds and their orders.
    void mmcifMatchesPdb() {
        Document d;
        buildPeptide(d, {"PHE", "HIS", "SER"}, "charged", "charged");
        QTemporaryDir temp;
        const auto pdb = temp.filePath("peptide.pdb"), cif = temp.filePath("peptide.cif");
        exportStructure(d, pdb);
        // Open Babel writes every bond as CONECT records, with their orders. Files from the PDB have them
        // only for hetero groups and disulfide bonds, which the distances also give, and mmCIF files none.
        QStringList atomRecords;
        for (const auto &line : QString::fromUtf8(readFile(pdb)).split('\n'))
            if (!line.startsWith("CONECT"))
                atomRecords << line;
        writeFile(pdb, atomRecords.join('\n').toUtf8());
        QString table = "data_PEPTIDE\n#\nloop_\n";
        for (const char *column :
             {"group_PDB", "id", "type_symbol", "label_atom_id", "label_alt_id", "label_comp_id",
              "label_asym_id", "label_seq_id", "pdbx_PDB_ins_code", "Cartn_x", "Cartn_y", "Cartn_z",
              "occupancy", "pdbx_formal_charge", "auth_seq_id", "auth_asym_id", "pdbx_PDB_model_num"})
            table += QString("_atom_site.%1\n").arg(column);
        for (const auto &line : atomRecords)
            if (line.startsWith("ATOM") || line.startsWith("HETATM")) {
                const auto field = [&](int from, int to) {
                    return line.mid(from - 1, to - from + 1).trimmed();
                };
                const auto charge = field(79, 80); // "1+" in PDB files, "+1" in mmCIF.
                table += QStringList{field(1, 6),
                                     field(7, 11),
                                     field(77, 78),
                                     '"' + field(13, 16) + '"',
                                     ".",
                                     field(18, 20),
                                     "A",
                                     field(23, 26),
                                     "?",
                                     field(31, 38),
                                     field(39, 46),
                                     field(47, 54),
                                     field(55, 60),
                                     charge.isEmpty() ? "?" : charge.right(1) + charge.left(1),
                                     field(23, 26),
                                     field(22, 22),
                                     "1"}
                             .join(' ') +
                         '\n';
            }
        writeFile(cif, table.toUtf8());
        const auto a = importStructure(cif), b = importStructure(pdb);
        QCOMPARE(a.atoms.size(), b.atoms.size());
        for (int i = 0; i < a.atoms.size(); ++i) {
            QCOMPARE(a.atoms[i].element, b.atoms[i].element);
            QCOMPARE(a.atoms[i].name, b.atoms[i].name);
            QCOMPARE(a.atoms[i].charge, b.atoms[i].charge);
            QCOMPARE(a.atoms[i].position, b.atoms[i].position);
        }
        const auto bonds = [](const Document &d) {
            QStringList out;
            for (const auto &bond : d.bonds)
                out << QString("%1-%2 %3")
                           .arg(std::min(d.index(bond.a), d.index(bond.b)))
                           .arg(std::max(d.index(bond.a), d.index(bond.b)))
                           .arg(bond.kind);
            out.sort();
            return out;
        };
        QCOMPARE(bonds(a), bonds(b));
        QVERIFY(
            bonds(a).join(' ').contains("aromatic")); // The phenyl ring, from the typical hydrogen counts.
        QCOMPARE(a.residues.size(), 3);
        for (int i = 0; i < a.residues.size(); ++i) {
            QCOMPARE(a.residues[i].code, b.residues[i].code);
            QCOMPARE(a.residues[i].number, b.residues[i].number);
            QCOMPARE(a.residues[i].metadata, b.residues[i].metadata);
            QCOMPARE(a.residues[i].atoms.keys(), b.residues[i].atoms.keys());
            for (const auto &name : a.residues[i].atoms.keys())
                QCOMPARE(a.index(a.residues[i].atoms[name]), b.index(b.residues[i].atoms[name]));
            QCOMPARE(a.residues[i].nTerminal, b.residues[i].nTerminal);
            QCOMPARE(a.residues[i].cTerminal, b.residues[i].cTerminal);
        }
        QCOMPARE(a.charge, b.charge);
        QCOMPARE(a.data["metadata"].toObject()["source_format"].toString(), QString("mmcif"));
    }
    // CIF syntax that other categories use, the author's names before the PDBx ones, chains longer than a
    // character, and MODEL/ALTLOC chosen as in PDB files.
    void mmcifTables() {
        const QByteArray text =
            "data_TEST\n"
            "# Other categories, whose values are not atoms.\n"
            "_struct.title 'it's a title with _tags'\n"
            "_struct_keywords.text\n"
            ";A text field\n"
            "_atom_site.id 7\n"
            ";\n"
            "loop_\n"
            "_entity.id\n"
            "_entity.type\n"
            "1 polymer\n"
            "2 \"non-polymer\"\n"
            "loop_\n"
            "_atom_site.group_PDB\n"
            "_atom_site.id\n"
            "_atom_site.type_symbol\n"
            "_atom_site.label_atom_id\n"
            "_atom_site.label_alt_id\n"
            "_atom_site.label_comp_id\n"
            "_atom_site.label_asym_id\n"
            "_atom_site.label_seq_id\n"
            "_atom_site.pdbx_PDB_ins_code\n"
            "_atom_site.Cartn_x\n"
            "_atom_site.Cartn_y\n"
            "_atom_site.Cartn_z\n"
            "_atom_site.occupancy\n"
            "_atom_site.pdbx_formal_charge\n"
            "_atom_site.auth_seq_id\n"
            "_atom_site.auth_asym_id\n"
            "_atom_site.pdbx_PDB_model_num\n"
            "ATOM   1 N  N     . GLY A 1 ? 0.000 0.000 0.000 1.00 ? 10 AA 1\n"
            "ATOM   2 C  CA    A GLY A 1 ? 1.450 0.000 0.000 0.40 ? 10 AA 1\n"
            "ATOM   3 C  CA    B GLY A 1 ? 1.460 0.100 0.000 0.60 ? 10 AA 1\n"
            "HETATM 4 ZN ZN    . ZN  B . ? 9.000 0.000 0.000 1.00 2 301 AB 1\n"
            "HETATM 5 O  \"O5'\" . XYZ C . A 0.000 9.000 0.000 1.00 ? 401 AB 1\n"
            "ATOM   6 N  N     . GLY A 1 ? 0.000 0.000 1.000 1.00 ? 10 AA 2 # The second model.\n"
            "data_SECOND\n"
            "_atom_site.id 8\n"
            "_atom_site.type_symbol C\n"
            "_atom_site.label_atom_id C\n"
            "_atom_site.Cartn_x 0\n"
            "_atom_site.Cartn_y 0\n"
            "_atom_site.Cartn_z 0\n";
        const auto atoms = readMmcif(text).atoms;
        QCOMPARE(atoms.size(), 6);
        QCOMPARE(atoms[0].chain, QString("AA"));
        QCOMPARE(atoms[0].number, 10);
        QCOMPARE(atoms[3].element, QString("Zn"));
        QCOMPARE(atoms[3].charge, 2);
        QCOMPARE(atoms[4].name, QString("O5'"));
        QCOMPARE(atoms[4].insertion, QString("A"));
        QCOMPARE(mmcifContents(readMmcif(text)).models, QStringList({"1", "2"}));
        // Automatically the location of the highest occupancy, else the one asked for.
        auto chosen = selectMmcif(atoms, {.model = 1});
        QCOMPARE(chosen.size(), 4);
        QCOMPARE(chosen[1].position.x(), 1.46);
        QVERIFY(chosen[1].altloc.isEmpty());
        QCOMPARE(selectMmcif(atoms, {.model = 1, .altloc = "A"})[1].position.x(), 1.45);
        QCOMPARE(selectMmcif(atoms, {.model = 2}).size(), 1);
        QVERIFY_EXCEPTION_THROWN(selectMmcif(atoms, {.model = 3}), std::runtime_error);

        QTemporaryDir temp;
        const auto path = temp.filePath("test.mmcif");
        writeFile(path, text);
        const auto d = importStructure(path, {.altloc = "A"});
        QCOMPARE(d.atoms.size(), 4);
        QCOMPARE(d.atoms[1].position.x(), 1.45);
        QCOMPARE(d.chains.size(), 2); // AA and AB, which Open Babel would both call A.
        QCOMPARE(d.residues.size(), 3);
        QCOMPARE(d.residues[1].metadata["pdb_chain"].toString(), QString("AB"));
        QCOMPARE(d.residues[1].chain, d.residues[2].chain);
        QVERIFY(d.residues[0].chain != d.residues[1].chain);
        QCOMPARE(d.atoms[2].charge, 2);
        QCOMPARE(d.data["metadata"].toObject()["pdb_altloc"].toString(), QString("A"));
    }
    // Chains and kinds of residues of a PDB file: names from COMPND, a modified residue of SEQRES in the
    // polymer.
    void pdbChainsAndComponents() {
        const auto record = [](const char *type, int serial, const char *name, const char *residue,
                               char chain, int number, double x, double y, double z, const char *element) {
            return QString::asprintf("%-6s%5d %-4s %-3s %c%4d    %8.3f%8.3f%8.3f  1.00  0.00          %2s\n",
                                     type, serial, name, residue, chain, number, x, y, z, element);
        };
        const auto text = QString("COMPND    MOL_ID: 1;\n"
                                  "COMPND   2 MOLECULE: SPIKE PROTEIN;\n"
                                  "COMPND   3 CHAIN: A;\n"
                                  "COMPND   4 MOL_ID: 2;\n"
                                  "COMPND   5 MOLECULE: ANTIBODY HEAVY\n"
                                  "COMPND   6 CHAIN;\n"
                                  "COMPND   7 CHAIN: B, C;\n"
                                  "SEQRES   1 A    2  GLY MSE\n"
                                  "SEQRES   1 B    1  GLY\n"
                                  "SEQRES   1 C    1  GLY\n") +
                          record("ATOM", 1, "CA", "GLY", 'A', 1, 0, 0, 0, "C") +
                          record("HETATM", 2, "CA", "MSE", 'A', 2, 3.8, 0, 0, "C") +
                          record("ATOM", 3, "CA", "GLY", 'B', 1, 0, 5, 0, "C") +
                          record("ATOM", 4, "CA", "GLY", 'C', 1, 0, 10, 0, "C") +
                          record("HETATM", 5, "ZN", "ZN", 'A', 101, 5, 5, 5, "ZN") +
                          record("HETATM", 6, "O", "HOH", 'A', 201, 9, 9, 9, "O") +
                          record("HETATM", 7, "O", "HOH", 'B', 202, 9, 12, 9, "O") +
                          "CONECT    5    6\nEND\n";
        const auto contents = pdbContents(text);
        QCOMPARE(contents.models, QStringList{"1"});
        QVERIFY(!contents.altlocs);
        QCOMPARE(contents.chains.size(), 3);
        QCOMPARE(contents.chains[0].id, QString("A"));
        QCOMPARE(contents.chains[0].description, QString("SPIKE PROTEIN"));
        QCOMPARE(contents.chains[0].residues, 2); // MSE, a HETATM record of the sequence.
        QCOMPARE(contents.chains[2].description, QString("ANTIBODY HEAVY CHAIN"));
        QCOMPARE(contents.chains[0].groups, QStringList({"ZN", "HOH"}));
        QCOMPARE(contents.chains[1].groups, QStringList{"HOH"});
        QCOMPARE(contents.polymers, 4);
        QCOMPARE(contents.ligands, 1);
        QCOMPARE(contents.waters, 2);
        const auto serials = [](const QString &records) {
            QList<int> out;
            for (const auto &line : records.split('\n'))
                if (line.startsWith("ATOM") || line.startsWith("HETATM"))
                    out << line.mid(6, 5).toInt();
            return out;
        };
        auto chosen = preparePdb(text, {.chains = {"A"}, .water = false});
        QCOMPARE(serials(chosen), QList<int>({1, 2, 5}));
        QVERIFY(!chosen.contains("CONECT")); // It names the water left out.
        chosen = preparePdb(text, {.chains = {"A"}});
        QCOMPARE(serials(chosen), QList<int>({1, 2, 5, 6}));
        QVERIFY(chosen.contains("CONECT    5    6"));

        QTemporaryDir temp;
        const auto path = temp.filePath("entry.pdb");
        writeFile(path, text.toUtf8());
        const auto d = importStructure(path, {.chains = {"A", "B"}, .ligands = false, .water = false});
        QCOMPARE(d.atoms.size(), 3);
        QCOMPARE(d.residues.size(), 3);
        const auto meta = d.data["metadata"].toObject();
        QCOMPARE(meta["pdb_chains"].toArray(), QJsonArray({"A", "B"}));
        QCOMPARE(meta["pdb_components"].toArray(), QJsonArray({"polymer"}));
        QCOMPARE(importStructure(path).atoms.size(), 7);
        // Without END, the last line break left a blank line that Open Babel took for a broken file.
        writeFile(path, record("ATOM", 1, "CA", "GLY", 'A', 1, 0, 0, 0, "C").toUtf8());
        QCOMPARE(importStructure(path).atoms.size(), 1);
        writeFile(path, text.toUtf8());
        try {
            importStructure(path, {.chains = {"X"}});
            QFAIL("read no atoms");
        } catch (const std::exception &e) {
            QVERIFY(QString::fromUtf8(e.what()).contains("鎖・成分"));
        }
    }
    // The same of an mmCIF file, from its entities; without them, from the records and the water names.
    void mmcifChainsAndComponents() {
        const QByteArray atoms =
            "loop_\n_atom_site.group_PDB\n_atom_site.id\n_atom_site.type_symbol\n"
            "_atom_site.label_atom_id\n_atom_site.label_comp_id\n_atom_site.label_asym_id\n"
            "_atom_site.label_entity_id\n_atom_site.label_seq_id\n_atom_site.Cartn_x\n"
            "_atom_site.Cartn_y\n_atom_site.Cartn_z\n_atom_site.auth_seq_id\n"
            "_atom_site.auth_asym_id\n"
            "ATOM   1 C  CA GLY A 1 1 0   0 0 1   A\n"
            "HETATM 2 C  CA MSE A 1 2 3.8 0 0 2   A\n"
            "ATOM   3 C  CA GLY B 1 1 0   5 0 1   BB\n"
            "HETATM 4 ZN ZN ZN  C 2 . 5   5 5 101 A\n"
            "HETATM 5 O  O  HOH D 3 . 9   9 9 201 A\n";
        const auto entry = readMmcif("data_TEST\nloop_\n_entity.id\n_entity.type\n_entity.pdbx_description\n"
                                     "1 polymer 'Spike protein'\n2 non-polymer 'ZINC ION'\n3 water water\n" +
                                     atoms);
        QCOMPARE(entry.atoms[1].kind, ResidueKind::Polymer); // MSE, of the polymer entity.
        QCOMPARE(entry.atoms[3].kind, ResidueKind::Ligand);
        QCOMPARE(entry.atoms[4].kind, ResidueKind::Water);
        const auto contents = mmcifContents(entry);
        QCOMPARE(contents.chains.size(), 2);
        QCOMPARE(contents.chains[1].id, QString("BB"));
        QCOMPARE(contents.chains[1].description, QString("Spike protein"));
        QCOMPARE(contents.chains[0].residues, 2);
        QCOMPARE(contents.chains[0].groups, QStringList({"ZN", "HOH"}));
        QCOMPARE(contents.polymers, 3);
        QCOMPARE(contents.ligands, 1);
        QCOMPARE(contents.waters, 1);
        QCOMPARE(selectMmcif(entry.atoms, {.chains = {"BB"}}).size(), 1);
        QCOMPARE(selectMmcif(entry.atoms, {.water = false}).size(), 4);
        QCOMPARE(selectMmcif(entry.atoms, {.ligands = false, .water = false}).size(), 3);
        QVERIFY_EXCEPTION_THROWN(selectMmcif(entry.atoms, {.chains = {"BB"}, .polymer = false}),
                                 std::runtime_error);
        // One entity as items; atoms without an entity by their record and name.
        const auto items =
            readMmcif("data_TEST\n_entity.id 1\n_entity.type polymer\n_entity.pdbx_description 'Peptide'\n" +
                      QByteArray(atoms).replace(" 1 1 0   5", " . 1 0   5").replace(" 1 2 3.8", " . 2 3.8"));
        QCOMPARE(items.descriptions.value("1"), QString("Peptide"));
        QCOMPARE(items.atoms[0].kind, ResidueKind::Polymer);
        QCOMPARE(items.atoms[1].kind, ResidueKind::Ligand); // A HETATM record without its entity.
        QCOMPARE(items.atoms[2].kind, ResidueKind::Polymer);
        QCOMPARE(items.atoms[3].kind, ResidueKind::Ligand);
        QCOMPARE(items.atoms[4].kind, ResidueKind::Water);
    }
    void mmcifErrorsNameTheProblem() {
        const auto message = [](const QByteArray &text) {
            try {
                readMmcif(text);
            } catch (const std::exception &e) {
                return QString::fromUtf8(e.what());
            }
            return QString();
        };
        const QByteArray header =
            "data_X\nloop_\n_atom_site.id\n_atom_site.type_symbol\n_atom_site.label_atom_id\n"
            "_atom_site.Cartn_x\n_atom_site.Cartn_y\n_atom_site.Cartn_z\n";
        QVERIFY(message(header + "1 C C 0 0 0\n").isEmpty());
        QVERIFY(message(header + "1 C C 0 0 0\n2 C C 0 x 0\n").contains("10行目"));
        QVERIFY(message(header + "1 C C 0 0 ?\n").contains("9行目"));
        QVERIFY(message(header + "1 C C 0 0\n").contains("2行目")); // The loop_ that a value is missing from.
        QVERIFY(message(header + "1 C 'C 0 0 0\n").contains("9行目"));
        QVERIFY(message("data_X\n_struct.title\n;open\n").contains("3行目"));
        QVERIFY(message("data_X\nloop_\n_atom_site.id\n_atom_site.Cartn_x\n1 0\n").contains("Cartn_z"));
        // A small-molecule CIF has fractional coordinates in _atom_site_fract_x, not an mmCIF table.
        QVERIFY(message("data_X\nloop_\n_atom_site_label\n_atom_site_fract_x\n_atom_site_fract_y\n"
                        "_atom_site_fract_z\nC1 0.1 0.2 0.3\n")
                    .contains("_atom_site"));
    }
    // Structure files open gzip-compressed, as the PDB hands them out, through the readers of their format.
    void compressedStructureFiles() {
        QCOMPARE(structureSuffix("/data/10MU.CIF.GZ"), QString("cif"));
        QCOMPARE(structureSuffix("pdb10mu.ent.gz"), QString("ent"));
        QCOMPARE(structureSuffix("water.xyz"), QString("xyz"));
        QTemporaryDir temp;
        const auto file = [&](const QString &name, const QByteArray &bytes) {
            const auto path = temp.filePath(name);
            writeFile(path, bytes);
            return path;
        };
        const QByteArray xyz = "3\nwater\nO 0 0 0\nH .96 0 0\nH -.24 .93 0\n";
        QCOMPARE(importStructure(file("water.xyz.gz", gzip(xyz))).atoms.size(), 3);
        Document peptide;
        buildPeptide(peptide, {"ALA", "GLY", "SER"}, "charged", "charged");
        const auto pdb = temp.filePath("peptide.pdb");
        exportStructure(peptide, pdb);
        const auto plain = importStructure(pdb);
        const auto packed = importStructure(file("peptide.ent.gz", gzip(readFile(pdb))));
        QCOMPARE(packed.atoms.size(), plain.atoms.size());
        QCOMPARE(packed.bonds.size(), plain.bonds.size());
        QCOMPARE(packed.residues.size(), 3);
        QCOMPARE(packed.data["metadata"].toObject()["source_format"].toString(), QString("pdb"));
        // Members joined with cat read as one file.
        QCOMPARE(readStructureFile(file("joined.xyz.gz", gzip(xyz.left(10)) + gzip(xyz.mid(10)))), xyz);
        const auto refusal = [](const std::function<void()> &read) {
            try {
                read();
            } catch (const std::exception &e) {
                return QString::fromUtf8(e.what());
            }
            return QString();
        };
        QVERIFY(refusal([&] {
                    readStructureFile(file("cut.xyz.gz", gzip(xyz).chopped(4)));
                }).contains("cut.xyz.gz"));
        QVERIFY(refusal([&] { readStructureFile(file("plain.xyz.gz", xyz)); }).contains("gzip"));
        // A small file that would decompress past the limit stops there.
        QVERIFY(refusal([&] {
                    readStructureFile(file("zeros.xyz.gz", gzip(QByteArray(3 << 20, '0'))), 1 << 20);
                }).contains("1 MiB"));
        const auto project = readFile(QString(FIXTURE_DIR) + "/ala_gly_ser.molforma.json");
        QVERIFY(refusal([&] {
                    importStructure(file("project.molforma.json.gz", gzip(project)));
                }).contains("圧縮"));
    }
    // A cluster model: the residues around a center, the peptide bonds so cut ending in hydrogens on fixed
    // atoms.
    void siteCutOut() {
        Document d;
        buildPeptide(d, {"ALA", "GLY", "LYS", "ALA", "GLY"}, "charged", "charged");
        d.charge = d.expectedCharge();
        QCOMPARE(d.residues[2].code, QString("LYS"));
        const auto center = d.residues[2].atoms.values();
        // A water beside the middle residue and one far away, outside residues.
        const auto addWater = [&](const Vec &at) {
            auto water = Document::fromXyz("3\n\nO 0 0 0\nH .76 0 .59\nH -.76 0 .59\n");
            for (auto &a : water.atoms)
                a.position += at;
            d.atoms += water.atoms;
            d.bonds += water.bonds;
            return water.ids();
        };
        const Vec ca = d.atom(d.residues[2].atoms["CA"]).position;
        const auto nearWater = addWater(ca + Vec(0, 0, 1.9));
        addWater(ca + Vec(40, 0, 0));
        QSet<QString> residues; // The middle residue and its neighbors, bonded to it.
        for (int r = 1; r <= 3; ++r)
            for (const auto &id : d.residues[r].atoms)
                residues.insert(id);
        const auto set = [](const Ids &ids) { return QSet<QString>(ids.begin(), ids.end()); };
        const auto site = siteAtoms(d, center, {.radius = 2});
        QCOMPARE(set(site), residues + set(nearWater));
        QCOMPARE(set(siteAtoms(d, center, {.radius = 2, .waters = false})), residues);
        // Atom by atom: the backbone atoms bonded to the center, with their hydrogens, not their residues.
        const auto atoms = set(siteAtoms(d, center, {.radius = 2, .wholeResidues = false}));
        QVERIFY(atoms.contains(d.residues[1].atoms["C"]) && !atoms.contains(d.residues[1].atoms["N"]));
        QVERIFY(atoms.contains(d.residues[3].atoms["N"]) && !atoms.contains(d.residues[3].atoms["CA"]));
        const Topology before(d);
        const auto amide = before.neighbors(d, d.residues[3].atoms["N"]);
        QVERIFY(std::any_of(amide.begin(), amide.end(),
                            [&](const QString &h) { return d.atom(h).element == "H" && atoms.contains(h); }));
        QVERIFY_EXCEPTION_THROWN(siteAtoms(d, {}), std::runtime_error);

        auto cut = d;
        const auto links = cutOutSite(cut, site);
        QCOMPARE(links.size(), 2); // GLY–ALA and ALA–GLY, at either end.
        QCOMPARE(cut.atoms.size(), site.size() + 2);
        QCOMPARE(cut.residues.size(), 3);
        QVERIFY2(cut.validate().isEmpty(), qPrintable(cut.validate().join('\n')));
        QCOMPARE(cut.charge, 1); // Lys; the charged ends are gone.
        const Topology after(cut);
        for (const auto &h : links) {
            const auto partners = after.neighbors(cut, h);
            QCOMPARE(partners.size(), 1);
            const auto &atom = after.atom(cut, partners[0]);
            QVERIFY(atom.fixed);
            // Along the bond that was cut, at the X–H length.
            QString outside;
            for (const auto &n : before.neighbors(d, atom.id))
                if (!site.contains(n))
                    outside = n;
            const Vec bond = d.atom(outside).position - atom.position,
                      link = cut.atom(h).position - atom.position;
            QVERIFY(std::abs(link.norm() - element(atom.element).radius - element("H").radius) < 1e-9);
            QVERIFY(link.normalized().dot(bond.normalized()) > 1 - 1e-9);
        }
        QVERIFY(cut.atom(d.residues[1].atoms["N"]).fixed && cut.atom(d.residues[3].atoms["C"]).fixed);
        auto loose = d;
        cutOutSite(loose, site, {.fixCuts = false});
        QVERIFY(std::none_of(loose.atoms.begin(), loose.atoms.end(), [](const Atom &a) { return a.fixed; }));
        // Structures from the PDB have no hydrogens: they are added after the cut-out, the fixed atoms
        // staying.
        auto heavy = d;
        Ids hydrogens;
        for (const auto &a : heavy.atoms)
            if (a.element == "H")
                hydrogens << a.id;
        heavy.remove(hydrogens);
        cutOutSite(heavy, siteAtoms(heavy, heavy.residues[2].atoms.values(), {.radius = 2}));
        QMap<QString, Vec> fixed;
        for (const auto &a : heavy.atoms)
            if (a.fixed)
                fixed[a.id] = a.position;
        QCOMPARE(fixed.size(), 2);
        completeHydrogens(heavy);
        QVERIFY2(heavy.validate().isEmpty(), qPrintable(heavy.validate().join('\n')));
        for (auto it = fixed.begin(); it != fixed.end(); ++it)
            QCOMPARE(heavy.atom(it.key()).position, it.value());
        QCOMPARE(heavy.formula(), cut.formula()); // The same model as cut out with its hydrogens.

        // Bonds to metals end without a hydrogen; fixed atoms stay.
        auto zinc = Document::fromXyz("3\n\nZn 0 0 0\nC 2 0 0\nC 3.5 0 0\n");
        const auto ids = zinc.ids();
        zinc.bonds = {Bond{.a = ids[0], .b = ids[1]}, Bond{.a = ids[1], .b = ids[2]}};
        auto copy = zinc;
        QCOMPARE(cutOutSite(copy, {ids[1], ids[2]}).size(), 0);
        copy = zinc;
        QCOMPARE(cutOutSite(copy, {ids[0], ids[1]}).size(), 1);
        zinc.atoms[2].fixed = true;
        QVERIFY_EXCEPTION_THROWN(cutOutSite(zinc, {ids[0], ids[1]}), std::runtime_error);
    }
    // Hydrogens in the states of pH 7 for a structure without them: the same peptide as built with its
    // charged groups, His on the nitrogen not bound to a metal, water and ions bound to a metal.
    void protonation() {
        Document built;
        buildPeptide(built, {"ALA", "ASP", "GLU", "LYS", "ARG", "HIS", "CYS", "TYR"}, "charged", "charged");
        built.charge = built.expectedCharge();
        const auto strip = [](Document d) {
            Ids hydrogens;
            for (auto &a : d.atoms) {
                a.charge = 0;
                if (a.element == "H")
                    hydrogens << a.id;
            }
            d.remove(hydrogens);
            d.charge = d.expectedCharge();
            return d;
        };
        auto d = strip(built);
        protonate(d);
        QVERIFY2(d.validate().isEmpty(), qPrintable(d.validate().join('\n')));
        QCOMPARE(d.formula(), built.formula());
        QCOMPARE(d.charge, built.charge);
        QCOMPARE(d.charge, 0); // Asp, Glu −1; Lys, Arg +1; NH3+ and COO− at the ends.
        const auto his = [](const Document &d) {
            for (const auto &r : d.residues)
                if (r.code == "HIS")
                    return r;
            return Residue();
        };
        const auto hydrogenOn = [](const Document &d, const QString &atom) {
            const auto bonded = d.neighbors(atom);
            return std::count_if(bonded.begin(), bonded.end(),
                                 [&](const QString &n) { return d.atom(n).element == "H"; });
        };
        QCOMPARE(his(d).state, QString("HIE"));
        QCOMPARE(hydrogenOn(d, his(d).atoms["NE2"]), 1);
        QCOMPARE(hydrogenOn(d, his(d).atoms["ND1"]), 0);

        // A zinc ion bound to NE2 of His and to SG of Cys, with a water bound to it.
        d = strip(built);
        const auto ne2 = d.atom(his(d).atoms["NE2"]), ring = d.atom(his(d).atoms["CE1"]);
        const Vec at = ne2.position + (ne2.position - ring.position).normalized() * 2.0;
        auto ion = Document::fromXyz(
            QString("2\n\nZn %1 %2 %3\nO %1 %2 %4\n").arg(at.x()).arg(at.y()).arg(at.z()).arg(at.z() + 2.1));
        const auto zinc = ion.atoms[0].id, water = ion.atoms[1].id;
        d.atoms += ion.atoms;
        QString sg;
        for (const auto &r : d.residues)
            if (r.code == "CYS")
                sg = r.atoms["SG"];
        d.setBond(zinc, ne2.id, "single");
        d.setBond(zinc, water, "single");
        d.setBond(zinc, sg, "single");
        registerResidue(d, {zinc}, "ZN");
        registerResidue(d, {water}, "HOH");
        d.charge = d.expectedCharge();
        protonate(d);
        QVERIFY2(d.validate().isEmpty(), qPrintable(d.validate().join('\n')));
        QCOMPARE(his(d).state, QString("HID"));
        QCOMPARE(hydrogenOn(d, his(d).atoms["NE2"]), 0);
        QCOMPARE(hydrogenOn(d, his(d).atoms["ND1"]), 1);
        QCOMPARE(hydrogenOn(d, water), 2);
        QCOMPARE(hydrogenOn(d, sg), 0);
        QCOMPARE(d.atom(sg).charge, -1);
        QCOMPARE(d.atom(zinc).charge, 2);
        QCOMPARE(d.charge, 1); // The peptide, Zn2+ and the thiolate.
        for (const auto &b : d.bonds)
            if (b.a == zinc || b.b == zinc)
                QVERIFY(b.kind == "dative" && b.b == zinc); // From the donor.
    }
    // The charged groups of the residues removed around a site stay as point charges, within their radius.
    void siteSurroundingCharges() {
        Document d;
        buildPeptide(d, {"GLY", "LYS", "GLY", "ASP", "GLY"}, "charged", "charged");
        d.charge = d.expectedCharge();
        const auto site = siteAtoms(d, d.residues[2].atoms.values(), {.radius = 0.1});
        QCOMPARE(site.size(), d.residues[2].atoms.size());
        auto cut = d;
        cutOutSite(cut, site, {.charges = true, .chargeRadius = 100});
        // NH3+ and Lys NZ +1, the carboxylates of Asp and of the C terminus −0.5 on each oxygen.
        QCOMPARE(cut.pointCharges.size(), 6);
        double sum = 0;
        for (const auto &q : cut.pointCharges) {
            sum += q.charge;
            QCOMPARE(q.source, QString("site"));
        }
        QVERIFY(std::abs(sum) < 1e-12);
        QCOMPARE(std::count_if(cut.pointCharges.begin(), cut.pointCharges.end(),
                               [](const PointCharge &q) { return q.charge == 1; }),
                 2);
        auto near = d;
        cutOutSite(near, site, {.charges = true, .chargeRadius = 2});
        QVERIFY(near.pointCharges.size() < 6);
        // With hydrogens in the states of pH 7: the site is the glycine capped with hydrogens.
        auto protonated = d;
        cutOutSite(protonated, site, {.hydrogens = true});
        QVERIFY2(protonated.validate().isEmpty(), qPrintable(protonated.validate().join('\n')));
        QCOMPARE(protonated.charge, 0);
        QVERIFY(protonated.pointCharges.isEmpty());
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
            QVERIFY2(d.charge == s.charge, s.name);
            QVERIFY2(d.multiplicity == s.multiplicity, s.name);
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
            QCOMPARE(saved.keys(), QStringList({"coordinate_units", "document", "format", "version"}));
            QCOMPARE(saved["document"].toObject()["charge"].toInt(), QString(name) == "co_en" ? 3 : 0);
            QCOMPARE(saved["document"].toObject()["multiplicity"].toInt(), 1);
            // The typed document writes back what it read.
            QCOMPARE(saved["document"].toObject(),
                     parseJson(readFile(QString(FIXTURE_DIR) + "/" + name + ".molforma.json"))["document"]
                         .toObject());
            auto copy = Document::load(file);
            QCOMPARE(copy.ids(), d.ids());
            QCOMPARE(copy.bonds.size(), d.bonds.size());
            QCOMPARE(copy.toJson()["residues"], d.toJson()["residues"]);
            QCOMPARE(copy.toJson()["centers"], d.toJson()["centers"]);
            QCOMPARE(copy.charge, d.charge);
            QCOMPARE(copy.multiplicity, d.multiplicity);
            for (int i = 0; i < d.atoms.size(); ++i)
                QVERIFY((copy.atoms[i].position - d.atoms[i].position).norm() < 1e-12);
        }
    }
    void hostileFilesStayBounded() {
        QTemporaryDir tmp;
        // Open Babel reads only the common formats: other readers are not given files from others.
        writeFile(tmp.filePath("one.bgf"),
                  "HETATM    1 C1   RES     1   0.000   0.000   0.000\nORDER 2 1 2 1\n");
        try {
            importStructure(tmp.filePath("one.bgf"));
            QFAIL("A BGF file was read.");
        } catch (const std::exception &e) {
            QVERIFY(QString::fromUtf8(e.what()).contains("対応していない形式です"));
        }
        // A chain listing a residue of another chain is refused when read, not met by an edit.
        auto project = parseJson(readFile(QString(FIXTURE_DIR) + "/ala_gly_ser.molforma.json"));
        auto doc = project["document"].toObject();
        auto residues = doc["residues"].toArray();
        auto first = residues[0].toObject();
        first["chain"] = "Y";
        residues[0] = first;
        doc["residues"] = residues;
        project["document"] = doc;
        QVERIFY_EXCEPTION_THROWN(Document::fromProject(project), std::runtime_error);
        auto twice = parseJson(readFile(QString(FIXTURE_DIR) + "/ala_gly_ser.molforma.json"));
        doc = twice["document"].toObject();
        auto chains = doc["chains"].toObject();
        const auto key = chains.keys().first();
        auto sequence = chains[key].toArray();
        sequence.append(sequence[0]);
        chains[key] = sequence;
        doc["chains"] = chains;
        twice["document"] = doc;
        QVERIFY_EXCEPTION_THROWN(Document::fromProject(twice), std::runtime_error);
        // Atoms packed far closer than in any molecule: bonds per atom are bounded and the bond orders
        // are left alone, instead of a ring search that does not finish.
        QString xyz = "64\ndense\n";
        for (int i = 0; i < 64; ++i)
            xyz += QString("C %1 %2 %3\n").arg(.3 * (i % 4)).arg(.3 * (i / 4 % 4)).arg(.3 * (i / 16));
        QElapsedTimer timer;
        timer.start();
        const auto dense = xyzStructure(xyz);
        QVERIFY(timer.elapsed() < 5000);
        QVERIFY(dense.bonds.size() <= 64 * 16 / 2);
        // Coordinates stay within 10^6 Å.
        auto far = Document::fromXyz("2\nfar\nH 0 0 0\nH 0 0 1e7\n");
        QVERIFY(!far.validate().isEmpty());
        QVERIFY(Document::fromXyz("2\nnear\nH 0 0 0\nH 0 0 .74\n").validate().isEmpty());
    }
    void samplesWriteBack() {
        // The practice files of the recipes are in the current format: the typed document writes back what
        // it read.
        for (const auto &file : QDir(RECIPES_DIR).entryInfoList({"*.molforma.json"}))
            QVERIFY2(Document::load(file.filePath()).project() == parseJson(readFile(file.filePath())),
                     qPrintable(file.fileName()));
    }
    void structureReferences() {
        // Fields MolForma does not know survive in residues, parts, centers, sites and point charges.
        auto project = parseJson(readFile(QString(FIXTURE_DIR) + "/co_en.molforma.json"));
        auto doc = project["document"].toObject();
        const auto mark = [](QJsonArray rows, const char *key) {
            auto row = rows[0].toObject();
            row["future_field"] = key;
            if (QString(key) == "centers") {
                auto sites = row["sites"].toArray();
                auto site = sites[0].toObject();
                site["future_field"] = "site";
                sites[0] = site;
                row["sites"] = sites;
            }
            rows[0] = row;
            return rows;
        };
        doc["parts"] = mark(doc["parts"].toArray(), "parts");
        doc["centers"] = mark(doc["centers"].toArray(), "centers");
        doc["point_charges"] = QJsonArray{QJsonObject{{"id", "q1"},
                                                      {"position", QJsonArray{0, 0, 9}},
                                                      {"charge", 1.0},
                                                      {"source", "manual"},
                                                      {"source_unit", "angstrom"},
                                                      {"future_field", "point_charges"}}};
        project["document"] = doc;
        auto d = Document::fromProject(project);
        QCOMPARE(d.project()["document"].toObject(), doc);
        QCOMPARE(d.parts[0].extra["future_field"].toString(), QString("parts"));
        QCOMPARE(d.centers[0].sites[0].extra["future_field"].toString(), QString("site"));

        // Renaming reaches every reference: bonds, parts and their center, sites, settings.
        const auto cobalt = d.centers[0].atom, donor = d.centers[0].sites[0].donor, part = d.parts[0].id;
        auto settings = d.data["nwchem"].toObject();
        settings["constraints"] =
            QJsonArray{QJsonObject{{"type", "bond"}, {"atoms", QJsonArray{cobalt, donor}}, {"value", 2.0}}};
        settings["scan_atoms"] = QJsonArray{donor, cobalt};
        d.data["nwchem"] = settings;
        auto renamed = d;
        renamed.renameIds({{cobalt, "co"}, {donor, "n1"}, {part, "en1"}});
        QVERIFY2(renamed.validate().isEmpty(), qPrintable(renamed.validate().join(';')));
        QCOMPARE(renamed.centers[0].atom, QString("co"));
        QCOMPARE(renamed.centers[0].sites[0].donor, QString("n1"));
        QCOMPARE(renamed.centers[0].sites[0].part, QString("en1"));
        QCOMPARE(renamed.parts[0].id, QString("en1"));
        QCOMPARE(renamed.parts[0].center(), QString("co"));
        QVERIFY(renamed.parts[0].atoms.contains("n1"));
        QVERIFY(renamed.bond("co", "n1"));
        QCOMPARE(renamed.data["nwchem"].toObject()["scan_atoms"].toArray(), (QJsonArray{"n1", "co"}));
        QCOMPARE(renamed.data["nwchem"].toObject()["constraints"].toArray()[0].toObject()["atoms"].toArray(),
                 (QJsonArray{"co", "n1"}));
        // Removing the center leaves its ligands unplaced and drops the coordinates that used it.
        auto removed = d;
        removed.remove({cobalt});
        QVERIFY(removed.centers.isEmpty());
        for (const auto &p : removed.parts)
            QVERIFY(p.center().isEmpty() && !p.metadata.contains("sites"));
        QVERIFY(removed.data["nwchem"].toObject()["constraints"].toArray().isEmpty());
        QVERIFY(removed.data["nwchem"].toObject()["scan_atoms"].toArray().isEmpty());
    }
    void mutationKeepsReferences() {
        // A mutated residue keeps its place and the IDs of the atoms it keeps by name, and with them the
        // constraints and the scan coordinate on its backbone.
        Document d;
        buildPeptide(d, parseSequence("AGA"), "charged", "charged");
        const auto gly = d.residues[1];
        auto settings = d.data["nwchem"].toObject();
        settings["constraints"] = QJsonArray{QJsonObject{
            {"type", "bond"}, {"atoms", QJsonArray{gly.atoms["N"], gly.atoms["CA"]}}, {"value", 1.46}}};
        settings["scan_atoms"] = QJsonArray{gly.atoms["C"], gly.atoms["CA"]};
        d.data["nwchem"] = settings;
        mutateResidue(d, gly.id, "SER", "default", "L");
        QVERIFY2(d.validate().isEmpty(), qPrintable(d.validate().join(';')));
        QCOMPARE(d.residues[1].id, gly.id);
        QCOMPARE(d.residues[1].code, QString("SER"));
        for (const auto *name : {"N", "CA", "C", "O"})
            QCOMPARE(d.residues[1].atoms[name], gly.atoms[name]);
        QCOMPARE(d.data["nwchem"].toObject()["constraints"].toArray().size(), 1);
        QCOMPARE(d.data["nwchem"].toObject()["scan_atoms"].toArray(),
                 (QJsonArray{gly.atoms["C"], gly.atoms["CA"]}));
        // The template parts of the old residue no longer hold its atoms.
        int holders = 0;
        for (const auto &p : d.parts)
            holders += p.atoms.contains(gly.atoms["CA"]);
        QCOMPARE(holders, 1);
    }
    void centerOfMassMovesFrame() {
        // Centering the whole structure moves the point charges with it, as orienting does.
        Document d;
        placeTemplate(d, findTemplate("skeleton:water"), Vec(3, 0, 0));
        addPointCharge(d, Vec(3, 0, 5), -0.5);
        const Vec offset = d.pointCharges[0].position - centroid(d, d.ids());
        centerMass(d, d.ids(), false);
        QVERIFY((centroid(d, d.ids()) + offset - d.pointCharges[0].position).norm() < 1e-9);
        const Vec before = d.pointCharges[0].position;
        centerMass(d, {d.atoms[0].id}, false);
        QVERIFY((d.pointCharges[0].position - before).norm() < 1e-12);
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
    void bondEditingValidatesTransaction() {
        Document d;
        placeTemplate(d, findTemplate("skeleton:ethane"), Vec::Zero());
        Ids carbons;
        for (auto &a : d.atoms)
            if (a.element == "C")
                carbons << a.id;
            else
                a.fixed = true;
        EditorSession session;
        session.reset(d);
        QVERIFY_EXCEPTION_THROWN(
            session.edit("double",
                         [&](Document &next) { changeBond(next, carbons[0], carbons[1], "double", true); }),
            std::runtime_error);
        QCOMPARE(session.document().project(), d.project());
        QCOMPARE(session.history().count(), 0);
        QVERIFY(session.edit("double",
                             [&](Document &next) { changeBond(next, carbons[0], carbons[1], "double"); }));
        QVERIFY(!session.edit("same",
                              [&](Document &next) { changeBond(next, carbons[0], carbons[1], "double"); }));
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
        QCOMPARE(d.charge, -1);
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
        // Isotopes count as other kinds, as in the point group the symmetry tool shows.
        Document hdo;
        instantiate(hdo, findTemplate("skeleton:water"));
        int hydrogens = 0;
        for (auto &a : hdo.atoms)
            if (a.element == "H" && !hydrogens++)
                a.isotope = 2;
        QCOMPARE(pointGroup(hdo).name, QString("Cs"));
        QCOMPARE(rotationalSymmetryNumber(hdo), 1);
        for (auto &a : hdo.atoms)
            if (a.element == "H")
                a.isotope = 2;
        QCOMPARE(rotationalSymmetryNumber(hdo), 2);
        // Ghosts are not part of the molecule.
        auto ghosted = hdo;
        ghosted.atoms << Atom{};
        ghosted.atoms.last().element = "He";
        ghosted.atoms.last().ghost = true;
        ghosted.atoms.last().position = Vec(0.3, 0.2, 2);
        QCOMPARE(rotationalSymmetryNumber(ghosted), 2);
        Document single;
        single.atoms << Atom{};
        QCOMPARE(rotationalSymmetryNumber(single), 0);
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
        water.multiplicity = 2;
        QVERIFY(!structureReport(water).contains("問題は見つかりませんでした"));
        // Complexes: free coordination sites, and a charge that disagrees with the oxidation number.
        Document zinc;
        addCenter(zinc, "Zn", "tetrahedral", Vec::Zero(), 2.1, 2);
        QCOMPARE(zinc.charge, 2); // The total charge followed the oxidation number.
        zinc.charge = 0;
        auto report = structureReport(zinc);
        QVERIFY2(report.contains("Zn1: 配位サイト 4 のうち 4 が空いています"), qPrintable(report));
        QVERIFY(report.contains("見込まれる全電荷は 2"));
        QVERIFY(!report.contains("形式電荷の合計と設定電荷が異なります")); // The center line says it.
        zinc.charge = 2;
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
            for (const auto &r : Document(d).residues)
                repairResidue(d, r.id);
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
    void peptide() {
        Document d;
        buildPeptide(d, parseSequence("AGS"), "charged", "charged");
        QCOMPARE(d.formula(), QString("C8H15N3O5"));
        QCOMPARE(d.atoms.size(), 31);
        QCOMPARE(d.formalCharge(), 0);
        QCOMPARE(d.residues.size(), 3);
        QVERIFY(d.validate().isEmpty());
        auto seq = d.chains.first();
        QCOMPARE(seq.size(), 3);
        setResidueTorsion(d, seq[1], "phi", -60);
        setResidueTorsion(d, seq[1], "psi", -40);
        QVERIFY(d.validate().isEmpty());
        auto next = appendResidue(d, "ALA", seq.last());
        QCOMPARE(d.residues.size(), 4);
        QVERIFY(!next.isEmpty());
        auto before = d.atoms.size();
        setTerminals(d, seq.first(), "neutral", {});
        QCOMPARE(d.atoms.size(), before - 1);
        QCOMPARE(d.formalCharge(), -1);
    }
    void peptidePrependMutateDelete() {
        Document d;
        buildPeptide(d, {"ALA"}, "charged", "charged");
        auto r = d.residues.first().id;
        auto first = prependResidue(d, "GLY", r);
        QCOMPARE(d.formula(), QString("C5H10N2O3"));
        mutateResidue(d, r, "SER", "default", "L");
        QCOMPARE(d.formula(), QString("C5H10N2O4"));
        QVERIFY2(d.validate().isEmpty(), qPrintable(d.validate().join(';')));
        deleteResidue(d, first);
        QCOMPARE(d.residues.size(), 1);
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
        QCOMPARE(d.charge, -2);
        QCOMPARE(d.expectedCharge(), -2);
        QCOMPARE(d.formalCharge(), -4);
        adjustCenter(d, zinc, "tetrahedral", 2.5, 3);
        QCOMPARE(d.charge, -1);
        d.charge = 5;
        adjustCenter(d, zinc, "tetrahedral", 2.5, 2);
        QCOMPARE(d.charge, 5);
        // trans-[Co(en)2Cl2]+ and SF6 (a main-group center).
        Document co;
        auto cobalt = addCenter(co, "Co", "octahedral", Vec::Zero(), 2.0, 3);
        placeLigand(co, cobalt, {0, 1}, findTemplate("ligand:en"));
        placeLigand(co, cobalt, {2, 3}, findTemplate("ligand:en"));
        placeLigand(co, cobalt, {4}, findTemplate("ligand:chloride"));
        placeLigand(co, cobalt, {5}, findTemplate("ligand:chloride"));
        QCOMPARE(co.formula(), QString("C4H16Cl2CoN4"));
        QCOMPARE(co.charge, 1);
        QVERIFY(validateNwchem(co).warnings.filter("見込まれる").isEmpty());
        Document sf6;
        auto sulfur = addCenter(sf6, "S", "octahedral", Vec::Zero(), 1.56, 6);
        for (int i = 0; i < 6; ++i)
            placeLigand(sf6, sulfur, {i}, findTemplate("ligand:fluoride"));
        QCOMPARE(sf6.formula(), QString("F6S"));
        QCOMPARE(sf6.charge, 0);
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
    void charges() {
        Document d;
        placeTemplate(d, findTemplate("skeleton:water"), Vec::Zero());
        auto ids = d.ids();
        std::reverse(ids.begin(), ids.end());
        waterToCharges(d, ids);
        QVERIFY(d.atoms.isEmpty());
        QCOMPARE(d.pointCharges.size(), 3);
        QCOMPARE(d.pointCharges[0].charge, -.834);
        double sum = 0;
        for (const auto &q : d.pointCharges)
            sum += q.charge;
        QVERIFY(std::abs(sum) < 1e-12);
    }
    void totalChargeFollowsStructure() {
        // One rule for the edits of residues, coordination and fragments: a total charge equal to the
        // expected charge (the formal charges, with the oxidation states of centers) follows it.
        Document d;
        buildPeptide(d, {"LYS"}, "charged", "charged");
        QCOMPARE(d.charge, 1);
        const auto lys = d.residues.first().id;
        const auto zinc = addCenter(d, "Zn", "tetrahedral", Vec(20, 0, 0), 2.3, 2);
        QCOMPARE(d.charge, 3);
        // With a center the formal charges alone (here 1) no longer equal the total charge.
        const auto asp = addTerminalResidue(d, lys, false, "ASP", "default", "L", "charged", "charged");
        QCOMPARE(d.charge, 2);
        setChainTerminals(d, lys, "neutral", "charged");
        QCOMPARE(d.charge, 1);
        mutateResidue(d, asp, "GLY", "default", "L");
        QCOMPARE(d.charge, 2);
        setOxidation(d, zinc, 3);
        QCOMPARE(d.charge, 3);
        const auto chloride = [] {
            Document f;
            Atom cl;
            cl.element = "Cl";
            cl.charge = -1;
            f.atoms << cl;
            f.charge = -1;
            return f;
        };
        insertFragment(d, chloride());
        QCOMPARE(d.charge, 2);
        QCOMPARE(d.charge, d.expectedCharge());
        // A total charge set to another value stays, also when a peptide is built beside it.
        d.charge = 7;
        setOxidation(d, zinc, 2);
        buildPeptide(d, {"GLY"}, "charged", "charged");
        insertFragment(d, chloride());
        QCOMPARE(d.charge, 7);
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
