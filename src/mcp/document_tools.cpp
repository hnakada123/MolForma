// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/atom_properties.h"
#include "editing/chemistry.h"
#include "editing/coordinates.h"
#include "editing/operations.h"
#include "editing/peptide.h"
#include "editing/site.h"
#include "editing/solvation.h"
#include "foundation/files.h"
#include "foundation/structure_file.h"
#include "io/document_storage.h"
#include "io/structure_io.h"
#include "mcp/json_schema.h"
#include "mcp/server.h"
#include "mcp/server_detail.h"
#include "mcp/tools.h"
#include "model/geometry.h"
#include "model/topology.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_execution.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

// The tools of McpServer on its document: what tools/list says of each and what a call does.
namespace mf {
using namespace mcp_detail;
namespace {
// Settings tools refuse only the errors their change brings in. Problems already there (an empty
// molecule, a multiplicity still to be chosen) are listed by get_nwchem/get_execution and keep other
// settings editable, as in the GUI.
void failOnAddedErrors(const ValidationReport &before, const ValidationReport &after) {
    QSet<QString> existing;
    for (const auto &d : before.diagnostics)
        if (d.severity == DiagnosticSeverity::Error)
            existing.insert(d.identity());
    QStringList added;
    for (const auto &d : after.diagnostics)
        if (d.severity == DiagnosticSeverity::Error && !existing.contains(d.identity()))
            added << d.message;
    if (!added.isEmpty())
        fail(added.join('\n'));
}
// Results are checked against the tools' output schemas before they are returned. Edits return this
// summary.
QJsonObject summarySchema() {
    const auto text = type("string");
    return object({{"documentId", text},
                   {"revision", text},
                   {"formula", text},
                   {"atomCount", type("integer")},
                   {"candidate", QJsonObject{{"type", QJsonArray{"object", "null"}}}}},
                  {"documentId", "revision", "formula", "atomCount", "candidate"});
}
// For documents too large to read whole: each chain with the number and the range of numbers of its polymer
// residues (amino acids, nucleotides) and its waters, every other residue (ligands, ions, sugars) on its own.
QJsonObject residueSummary(const Document &d) {
    QVector<QJsonObject> chains;
    QHash<QString, int> chainIndex;
    QJsonArray others;
    qsizetype registered = 0;
    for (const auto &r : d.residues) {
        registered += r.atoms.size();
        const auto chain = r.metadata.value("pdb_chain").toString();
        const bool polymer = polymerResidue(r), water = waterResidue(r.code);
        if (!polymer && !water) {
            others.append(QJsonObject{
                {"chain", chain}, {"number", r.number}, {"code", r.code}, {"atoms", r.atoms.size()}});
            continue;
        }
        auto it = chainIndex.find(chain);
        if (it == chainIndex.end()) {
            it = chainIndex.insert(chain, chains.size());
            chains.append({{"chain", chain}, {"polymer", 0}, {"waters", 0}});
        }
        auto &o = chains[*it];
        if (water) {
            o["waters"] = o["waters"].toInt() + 1;
            continue;
        }
        o["first"] = o["polymer"].toInt() ? std::min(o["first"].toInt(), r.number) : r.number;
        o["last"] = o["polymer"].toInt() ? std::max(o["last"].toInt(), r.number) : r.number;
        o["polymer"] = o["polymer"].toInt() + 1;
    }
    QJsonArray list;
    for (const auto &o : chains)
        list.append(o);
    return {{"chains", list}, {"others", others}, {"atomsOutsideResidues", d.atoms.size() - registered}};
}
} // namespace
void McpServer::registerTools() {
    const auto text = type("string");
    const auto add = [&](McpToolSpec spec, std::function<QJsonObject(const QJsonObject &)> call) {
        if (spec.output.isEmpty())
            spec.output = summarySchema();
        builtIn.append({spec, std::move(call)});
    };
    // Document edits: undoable, a candidate unless preview is false; they return the summary.
    const auto edit = [&](McpToolSpec spec, std::function<void(Document &, const QJsonObject &)> operation) {
        spec.properties["preview"] = QJsonObject{{"type", "boolean"}, {"default", true}};
        spec.mutation = true;
        const auto name = spec.name;
        add(spec, [this, name, operation](const QJsonObject &args) {
            this->edit(
                "MCP: " + name, [&](Document &next) { operation(next, args); }, args["preview"].toBool(true));
            return summary();
        });
    };
    const auto requireAtoms = [](const Document &d, const Ids &ids) { d.indices(ids); };

    add({.name = "list_documents",
         .title = "List documents",
         .description = "List the workspace owned by this process. Its ID expires on process exit.",
         .global = true,
         .output =
             object({{"documents", QJsonObject{{"type", "array"},
                                               {"items", object({{"documentId", text},
                                                                 {"revision", text},
                                                                 {"uri", text},
                                                                 {"gui", type("boolean")}},
                                                                {"documentId", "revision", "uri", "gui"})}}}},
                    {"documents"})},
        [this](const QJsonObject &) -> QJsonObject {
            return {{"documents", QJsonArray{QJsonObject{{"documentId", id},
                                                         {"revision", revision()},
                                                         {"uri", "molforma://documents/" + id},
                                                         {"gui", bool(editor.document)}}}}};
        });
    add({.name = "get_document",
         .title = "Read document",
         .description =
             "Read the revision with the committed project and/or separate candidate (stable atom "
             "IDs). include defaults to both; [] returns only metadata (atomCount among it). "
             "residues summarizes the residues of a document too large to read whole: each chain "
             "(author chain ID) with the count and number range of its polymer residues and its "
             "waters, and every other residue (ligands, ions, sugars) with chain, number and code, "
             "as cut_out_site takes them.",
         .properties = {{"include", QJsonObject{{"type", "array"},
                                                {"items", choice({"project", "candidate", "residues"})},
                                                {"uniqueItems", true}}}},
         .output = object({{"documentId", text},
                           {"revision", text},
                           {"formula", text},
                           {"atomCount", type("integer")},
                           {"project", type("object")},
                           {"candidate", QJsonObject{{"type", QJsonArray{"object", "null"}}}},
                           {"hasCandidate", type("boolean")},
                           {"residues", type("object")}},
                          {"documentId", "revision", "formula", "atomCount"}),
         .meta = {{"anthropic/maxResultSizeChars", McpLargeResultChars}}},
        [this](const QJsonObject &args) {
            // Only the parts asked for are made: the project of a large document is megabytes of JSON.
            const auto include =
                args.contains("include") ? args["include"].toArray() : QJsonArray{"project", "candidate"};
            const auto d = document();
            const auto c = candidate();
            QJsonObject out{{"documentId", id},
                            {"revision", revision()},
                            {"formula", d.formula()},
                            {"atomCount", d.atoms.size()}};
            if (include.contains("project"))
                out["project"] = d.project();
            if (include.contains("candidate"))
                out["candidate"] = c ? QJsonValue(c->project()) : QJsonValue();
            if (include.contains("residues"))
                out["residues"] = residueSummary(d);
            if (args.contains("include"))
                out["hasCandidate"] = c.has_value();
            return out;
        });
    add({.name = "list_templates",
         .title = "List templates",
         .description = "List built-in molecular templates and their IDs.",
         .global = true,
         .output = object(
             {{"templates", QJsonObject{{"type", "array"}, {"items", object({{"id", text}}, {"id"})}}}},
             {"templates"})},
        [](const QJsonObject &) -> QJsonObject {
            QJsonArray entries;
            for (auto v : templates()) {
                auto t = v.toObject();
                entries.append(QJsonObject{{"id", t["id"]},
                                           {"name", t["name_ja"]},
                                           {"nameEn", t["name_en"]},
                                           {"category", t["category"]}});
            }
            return {{"templates", entries}};
        });
    edit({.name = "place_template",
          .title = "Place template",
          .description = "Place a built-in template at an angstrom position.",
          .properties = {{"templateId", type("string")}, {"position", vectorSchema()}},
          .required = {"templateId", "position"}},
         [](Document &next, const QJsonObject &args) {
             placeTemplate(next, findTemplate(args["templateId"].toString()),
                           vectorFromJson(args["position"]));
         });
    edit({.name = "solvate",
          .title = "Place a solvation shell",
          .description =
              "Place count rigid solvent molecules (default 6) around the atoms ids (default: the whole "
              "molecule): first in contact with them (the solvent's O or N toward cations and polar atoms, "
              "its hydrogens toward anions), then outward, never overlapping. Places fewer when there is no "
              "room. Refine the result (xTB, force field) before a calculation.",
          .properties = {{"solvent", choice(solventTemplates())},
                         {"count", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
                         {"ids", idsSchema()}},
          .required = {"solvent"}},
         [requireAtoms](Document &next, const QJsonObject &args) {
             SolvationOptions options;
             options.count = args["count"].toInt(6);
             if (args.contains("ids")) {
                 options.around = idsFromJson(args["ids"]);
                 requireAtoms(next, options.around);
             }
             solvate(next, findTemplate(args["solvent"].toString()), options);
         });
    edit({.name = "add_atom",
          .title = "Add atom",
          .description = "Add an atom; hydrogen completion defaults to false. Optionally bond to anchorId.",
          .properties = {{"element", type("string")},
                         {"position", vectorSchema()},
                         {"anchorId", type("string")},
                         {"hydrogens", type("boolean")}},
          .required = {"element", "position"}},
         [](Document &next, const QJsonObject &args) {
             addAtom(next, args["element"].toString(), vectorFromJson(args["position"]),
                     args["anchorId"].toString(), "single", args["hydrogens"].toBool());
         });
    edit({.name = "set_positions",
          .title = "Set atom positions",
          .description = "Set atom positions by stable ID in angstrom. Fixed atoms cannot move.",
          .properties = {{"positions",
                          QJsonObject{
                              {"type", "array"},
                              {"minItems", 1},
                              {"maxItems", MaxAtoms},
                              {"items", QJsonObject{{"type", "object"},
                                                    {"properties", QJsonObject{{"id", type("string")},
                                                                               {"position", vectorSchema()}}},
                                                    {"required", QJsonArray{"id", "position"}},
                                                    {"additionalProperties", false}}}}}},
          .required = {"positions"}},
         [](Document &next, const QJsonObject &args) {
             QSet<QString> seen;
             const Topology topology(next);
             for (auto v : args["positions"].toArray()) {
                 auto o = v.toObject();
                 auto atomId = o["id"].toString();
                 if (seen.contains(atomId))
                     fail("Duplicate atom ID in positions.");
                 seen.insert(atomId);
                 auto &a = topology.atom(next, atomId);
                 auto p = vectorFromJson(o["position"]);
                 if (a.fixed && a.position != p)
                     fail("Fixed atoms cannot move.");
                 a.position = p;
             }
         });
    edit({.name = "set_bond",
          .title = "Set bond",
          .description = "Set or remove a bond between stable atom IDs.",
          .properties = {{"a", type("string")},
                         {"b", type("string")},
                         {"kind", choice({"single", "double", "triple", "aromatic", "dative", "none"})}},
          .required = {"a", "b", "kind"}},
         [requireAtoms](Document &next, const QJsonObject &args) {
             const auto a = args["a"].toString(), b = args["b"].toString();
             requireAtoms(next, {a, b});
             if (a == b)
                 fail("A bond requires two different atoms.");
             changeBond(next, a, b, args["kind"].toString());
         });
    edit({.name = "delete_atoms",
          .title = "Delete atoms",
          .description = "Delete atoms and update dependent references.",
          .properties = {{"ids", idsSchema()}},
          .required = {"ids"}},
         [requireAtoms](Document &next, const QJsonObject &args) {
             const auto ids = idsFromJson(args["ids"]);
             requireAtoms(next, ids);
             deleteAtoms(next, ids);
         });
    edit({.name = "translate",
          .title = "Translate atoms",
          .description = "Translate atoms in angstrom; reject selections containing fixed atoms.",
          .properties = {{"ids", idsSchema()}, {"offset", vectorSchema()}},
          .required = {"ids", "offset"}},
         [](Document &next, const QJsonObject &args) {
             const auto ids = idsFromJson(args["ids"]);
             const Topology topology(next);
             for (const auto &atomId : ids)
                 if (topology.atom(next, atomId).fixed)
                     fail("Fixed atoms cannot move.");
             transform(next, ids, Mat::Identity(), vectorFromJson(args["offset"]));
         });
    add({.name = "measure",
         .title = "Measure geometry",
         .description = "Measure 2/3/4 ordered atoms: distance (angstrom), angle/dihedral (degrees).",
         .properties = {{"ids", idsSchema()}},
         .required = {"ids"},
         .output = object({{"value", type("number")}, {"unit", choice({"angstrom", "degree"})}},
                          {"value", "unit"})},
        [this](const QJsonObject &args) -> QJsonObject {
            auto ids = idsFromJson(args["ids"]);
            if (ids.size() < 2 || ids.size() > 4)
                fail("measure requires 2, 3 or 4 atom IDs.");
            return {{"value", measure(document(), ids)}, {"unit", ids.size() == 2 ? "angstrom" : "degree"}};
        });
    edit({.name = "set_dimension",
          .title = "Set distance, angle or dihedral",
          .description =
              "Set the distance (angstrom), angle or dihedral (degrees) of 2/3/4 ordered atoms, as in "
              "the editor: the side of the later atom moves (for unbonded atoms of different "
              "molecules, the whole later molecule). Fixed atoms cannot move.",
          .properties = {{"ids", idsSchema()}, {"value", type("number")}},
          .required = {"ids", "value"}},
         [requireAtoms](Document &next, const QJsonObject &args) {
             const auto ids = idsFromJson(args["ids"]);
             requireAtoms(next, ids);
             setDimension(next, ids, args["value"].toDouble());
         });
    edit(
        {.name = "set_atom_properties",
         .title = "Set atom properties",
         .description =
             "Set properties of atoms by stable ID, as in the editor's atom panel. element replaces the "
             "element "
             "(hydrogens: adjust automatic hydrogens); charge is the formal charge; radical the unpaired "
             "electrons; isotope a mass number (0 clears it) or mass an exact mass in amu; fixed keeps atoms "
             "still in edits and optimizations; ghost makes basis-only centers; name the atom name.",
         .properties = {{"ids", idsSchema()},
                        {"element", type("string")},
                        {"hydrogens", type("boolean")},
                        {"charge", QJsonObject{{"type", "integer"},
                                               {"minimum", MinimumAtomCharge},
                                               {"maximum", MaximumAtomCharge}}},
                        {"radical",
                         QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", MaximumAtomRadical}}},
                        {"isotope", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 400}}},
                        {"mass", QJsonObject{{"type", "number"}, {"exclusiveMinimum", 0}}},
                        {"fixed", type("boolean")},
                        {"ghost", type("boolean")},
                        {"name", type("string")}},
         .required = {"ids"}},
        [requireAtoms](Document &next, const QJsonObject &args) {
            const auto ids = idsFromJson(args["ids"]);
            if (ids.isEmpty())
                fail("Specify atom IDs.");
            requireAtoms(next, ids);
            if (args.contains("isotope") && args.contains("mass"))
                fail("Set either isotope or mass.");
            if (args.contains("element"))
                replaceElement(next, ids, args["element"].toString(), args["hydrogens"].toBool());
            AtomProperties properties;
            if (args.contains("charge"))
                properties.charge = args["charge"].toInt();
            if (args.contains("radical"))
                properties.radical = args["radical"].toInt();
            if (args.contains("isotope"))
                properties.isotope = args["isotope"].toInt();
            if (args.contains("mass"))
                properties.mass = args["mass"].toDouble();
            if (args.contains("fixed"))
                properties.fixed = args["fixed"].toBool();
            if (args.contains("ghost"))
                properties.ghost = args["ghost"].toBool();
            if (args.contains("name"))
                properties.name = args["name"].toString();
            setAtomProperties(next, ids, properties);
        });
    edit({.name = "import_document",
          .title = "Import structure",
          .description =
              "Replace structure from XYZ text (preserving project metadata) or a complete MolForma "
              "project JSON string (.molforma.json, version 1). With format xyz and append true, the "
              "XYZ molecule is added 3 Å beside the current one instead (e.g. to build a dimer). This "
              "is undoable.",
          .properties = {{"format", choice({"xyz", "project"})},
                         {"text", type("string")},
                         {"append", type("boolean")}},
          .required = {"format", "text"}},
         [](Document &next, const QJsonObject &args) {
             if (args["append"].toBool() && args["format"] != "xyz")
                 fail("append is available for XYZ text only.");
             if (args["append"].toBool())
                 insertFragment(next, xyzStructure(args["text"].toString()));
             else if (args["format"] == "xyz")
                 replaceCoordinateStructure(next, xyzStructure(args["text"].toString()));
             else
                 next = Document::fromProject(parseJson(args["text"].toString().toUtf8()));
         });
    edit({.name = "open_file",
          .title = "Open structure file",
          .description =
              "Load a local XYZ/MOL/SDF/Mol2/CML/PDB/mmCIF/MolForma project file, NWChem input (.nw) or "
              "Gaussian input "
              "(.gjf/.com) as an undoable edit. Structure files keep the calculation settings; NWChem and "
              "Gaussian inputs also set charge and multiplicity, and NWChem inputs the calculation settings "
              "they "
              "share with MolForma. Multi-frame XYZ files need frame (0-based). PDB and mmCIF files read "
              "model (default 1), altloc (default: in each residue the location of the highest occupancy), "
              "chains (author chain IDs; default all) and the residue kinds polymer (proteins, nucleic "
              "acids), ligands (other hetero groups: ligands, ions, sugars) and water (default all). "
              "Small-molecule CIF files (fractional coordinates) read the formula unit (the molecules of the "
              "asymmetric unit, completed across the cell) or with cell the unit cell's contents; "
              "minorDisorder adds the minor disorder groups. Files "
              "may be gzip-compressed (.gz). Maximum file size: 8 MiB, also after decompressing.",
          .properties = {{"path", type("string")},
                         {"frame", QJsonObject{{"type", "integer"}, {"minimum", 0}}},
                         {"model", QJsonObject{{"type", "integer"}, {"minimum", 1}}},
                         {"altloc", type("string")},
                         {"chains", strings()},
                         {"polymer", type("boolean")},
                         {"ligands", type("boolean")},
                         {"water", type("boolean")},
                         {"cell", type("boolean")},
                         {"minorDisorder", type("boolean")}},
          .required = {"path"}},
         [this](Document &next, const QJsonObject &args) {
             auto path = args["path"].toString();
             policy.checkPath(path);
             QFileInfo info(path);
             if (!info.isFile() || info.size() > 8 * 1024 * 1024)
                 fail("Expected a regular structure file no larger than 8 MiB.");
             if (compressedFile(path))
                 readStructureFile(path, 8 * 1024 * 1024); // The limit holds for the decompressed contents.
             // A project replaces everything; a structure file only the structure, as in the GUI's
             // coordinate table, so calculation and execution settings stay. NWChem and Gaussian
             // inputs also set charge and multiplicity, and NWChem inputs the calculation settings
             // (keeping the advanced options and the additional input the user chose).
             const auto suffix = structureSuffix(path);
             const int frame = args.contains("frame") ? args["frame"].toInt() : -1;
             if (suffix == "json") {
                 next = importStructure(path);
                 return;
             }
             MacromoleculeSelection selection;
             selection.model = args.contains("model") ? args["model"].toInt() : 1;
             selection.altloc = args["altloc"].toString();
             for (const auto &chain : args["chains"].toArray())
                 selection.chains << chain.toString();
             selection.polymer = args["polymer"].toBool(true);
             selection.ligands = args["ligands"].toBool(true);
             selection.water = args["water"].toBool(true);
             CrystalSelection crystal;
             crystal.cell = args["cell"].toBool();
             crystal.minor = args["minorDisorder"].toBool();
             const auto imported = importStructure(path, selection, frame, crystal);
             replaceCoordinateStructure(next, imported);
             if (QStringList{"nw", "nwinp", "gjf", "com", "gau"}.contains(suffix)) {
                 next.charge = imported.charge;
                 next.multiplicity = imported.multiplicity;
             }
             if (suffix == "nw" || suffix == "nwinp") {
                 auto settings = imported.data["nwchem"].toObject();
                 const auto current = next.data["nwchem"].toObject();
                 for (const auto *key : {"advanced", "extra_input", "extra_tasks"})
                     if (current.contains(key))
                         settings[key] = current[key];
                 next.data["nwchem"] = settings;
             }
         });
    const auto number = [](double minimum, double maximum) {
        return QJsonObject{{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}};
    };
    edit({.name = "cut_out_site",
          .title = "Cut out site",
          .description =
              "Cut out a site, such as the active site of a protein, as a cluster model for quantum "
              "chemistry: "
              "the center and the atoms within radius (angstrom, default 5) of it, whole residues "
              "(wholeResidues, default true) and water (water, default true) included; the rest is removed "
              "and each covalent bond so cut capped with a hydrogen along the bond. The center is ids (atom "
              "IDs "
              "of the current document) or residues, each matching the author chain ID (chain), number and "
              "residue name (code) given. fixCuts (default true) fixes the capped atoms. hydrogens (default "
              "true) adds hydrogens in the states of pH 7: Asp/Glu -1, Lys/Arg +1, charged chain ends, His "
              "protonated on the nitrogen not bound to a metal, ions and sulfate by their charge, bonds to "
              "metals made dative. pointCharges (default false) keeps the charged groups removed within "
              "chargeRadius (angstrom, default 15) as point charges and includes them in the NWChem input. "
              "With "
              "path, the structure is read from that file (with model, altloc and chains as open_file) and "
              "the "
              "site replaces the current structure, the total charge set to the site's and calculation "
              "settings kept, so that structures of more than 10,000 atoms can be cut; without path the "
              "current document is cut. Maximum file size: 8 MiB, also after decompressing.",
          .properties = {{"path", type("string")},
                         {"model", QJsonObject{{"type", "integer"}, {"minimum", 1}}},
                         {"altloc", type("string")},
                         {"chains", strings()},
                         {"ids", idsSchema()},
                         {"residues", QJsonObject{{"type", "array"},
                                                  {"minItems", 1},
                                                  {"items", object({{"chain", type("string")},
                                                                    {"number", type("integer")},
                                                                    {"code", type("string")}},
                                                                   {})}}},
                         {"radius", number(0, 30)},
                         {"wholeResidues", type("boolean")},
                         {"water", type("boolean")},
                         {"fixCuts", type("boolean")},
                         {"hydrogens", type("boolean")},
                         {"pointCharges", type("boolean")},
                         {"chargeRadius", number(0, 100)}}},
         [this](Document &next, const QJsonObject &args) {
             const auto path = args["path"].toString();
             Document source = next;
             if (!path.isEmpty()) {
                 if (args.contains("ids"))
                     fail("With path, give the center as residues: the file's atoms have no IDs yet.");
                 policy.checkPath(path);
                 const QFileInfo info(path);
                 if (!info.isFile() || info.size() > 8 * 1024 * 1024)
                     fail("Expected a regular structure file no larger than 8 MiB.");
                 if (compressedFile(path))
                     readStructureFile(path,
                                       8 * 1024 * 1024); // The limit holds for the decompressed contents.
                 MacromoleculeSelection selection;
                 selection.model = args.contains("model") ? args["model"].toInt() : 1;
                 selection.altloc = args["altloc"].toString();
                 for (const auto &chain : args["chains"].toArray())
                     selection.chains << chain.toString();
                 source = importStructure(path, selection);
             }
             Ids center = args.contains("ids") ? idsFromJson(args["ids"]) : Ids();
             for (const auto &value : args["residues"].toArray()) {
                 const auto selector = value.toObject();
                 bool found = false;
                 for (const auto &r : source.residues)
                     if ((!selector.contains("chain") ||
                          r.metadata["pdb_chain"].toString() == selector["chain"].toString()) &&
                         (!selector.contains("number") || r.number == selector["number"].toInt()) &&
                         (!selector.contains("code") || r.code == selector["code"].toString())) {
                         center += r.atoms.values();
                         found = true;
                     }
                 if (selector.isEmpty() || !found)
                     fail("No residue matches " + QJsonDocument(selector).toJson(QJsonDocument::Compact) +
                          '.');
             }
             if (center.isEmpty())
                 fail("Give the center as ids or residues.");
             const SiteOptions site{args.contains("radius") ? args["radius"].toDouble() : 5,
                                    args["wholeResidues"].toBool(true), args["water"].toBool(true)};
             const SiteCutOptions cut{args["fixCuts"].toBool(true), args["hydrogens"].toBool(true),
                                      args["pointCharges"].toBool(),
                                      args.contains("chargeRadius") ? args["chargeRadius"].toDouble() : 15};
             const auto kept = siteAtoms(source, center, site);
             if (path.isEmpty())
                 cutOutNwchemSite(next, kept, cut);
             else
                 replaceWithNwchemSite(next, source, kept, cut);
         });
    add({.name = "export_document",
         .title = "Export document",
         .description =
             "Return the committed document as XYZ, MolForma project JSON, or validated NWChem input.",
         .properties = {{"format", choice({"xyz", "project", "nwchem"})}},
         .required = {"format"},
         .output =
             object({{"text", text}, {"format", text}, {"revision", text}}, {"text", "format", "revision"}),
         .meta = {{"anthropic/maxResultSizeChars", McpLargeResultChars}}},
        [this](const QJsonObject &args) -> QJsonObject {
            const auto d = document();
            const auto format = args["format"].toString();
            QString out = format == "xyz"       ? d.xyz()
                          : format == "project" ? QString::fromUtf8(QJsonDocument(d.project()).toJson())
                                                : nwchemInput(d);
            return {{"text", out}, {"format", format}, {"revision", revision()}};
        });
    add({.name = "save_file",
         .title = "Save project",
         .description =
             "Save the committed MolForma project (.molforma.json) locally. Refuse existing paths unless "
             "overwrite is true; reject pending candidates. Does not mark GUI history clean.",
         .properties = {{"path", type("string")},
                        {"overwrite", type("boolean")},
                        {"expectedRevision", type("string")}},
         .required = {"path", "expectedRevision"},
         .readOnly = false,
         .output = object({{"path", text}, {"revision", text}}, {"path", "revision"})},
        [this](const QJsonObject &args) { return saveFile(args); });
    add({.name = "get_nwchem",
         .title = "Read NWChem settings",
         .description =
             "Read NWChem settings, defaults, allowed choice values and validation errors/warnings.",
         .output = object({{"settings", type("object")},
                           {"defaults", type("object")},
                           {"choices", type("object")},
                           {"errors", strings()},
                           {"warnings", strings()}},
                          {"settings", "defaults", "choices", "errors", "warnings"})},
        [this](const QJsonObject &) -> QJsonObject {
            const auto d = document();
            auto validation = validateNwchem(d);
            QJsonObject choices; // Allowed values of each choice setting, as offered in the GUI.
            for (const auto &key : nwchemDefaults().keys()) {
                QJsonArray values;
                for (const auto &c : nwchemChoices(key))
                    values.append(QJsonObject{{"value", c.value}, {"label", c.label}});
                if (!values.isEmpty())
                    choices[key] = values;
            }
            return {{"settings", nwchemSettings(d)},
                    {"defaults", nwchemDefaults()},
                    {"choices", choices},
                    {"charge", d.charge},
                    {"multiplicity", d.multiplicity},
                    {"errors", QJsonArray::fromStringList(validation.errors)},
                    {"warnings", QJsonArray::fromStringList(validation.warnings)}};
        });
    add({.name = "get_advanced",
         .title = "Read advanced settings",
         .description = "Read all advanced/output options, their types, applicability and stored values.",
         .output =
             object({{"settings", type("object")}, {"options", type("array")}}, {"settings", "options"})},
        [this](const QJsonObject &) -> QJsonObject {
            const auto d = document();
            QJsonArray options;
            for (const auto &o : nwchemAdvancedOptions()) {
                QJsonArray choices;
                for (const auto &c : o.choices)
                    choices.append(QJsonObject{{"value", c.value}, {"label", c.label}});
                const QStringList kinds{"real", "integer",      "choice",   "flag",
                                        "path", "temperatures", "grid_axis"};
                options.append(QJsonObject{{"key", o.key},
                                           {"group", o.group},
                                           {"label", o.label},
                                           {"help", o.help},
                                           {"type", kinds[int(o.kind)]},
                                           {"minimum", o.minimum},
                                           {"maximum", o.maximum},
                                           {"choices", choices},
                                           {"default", o.defaultText},
                                           {"applicable", nwchemAdvancedApplies(o, d)}});
            }
            return {{"settings", nwchemAdvanced(d)}, {"options", options}};
        });
    edit({.name = "set_advanced",
          .title = "Set advanced settings",
          .description = "Merge typed advanced/output settings. A null value resets that option.",
          .properties = {{"settings", type("object")}},
          .required = {"settings"}},
         [this](Document &next, const QJsonObject &args) {
             const auto settings = args["settings"].toObject();
             const auto before = validateNwchem(next);
             for (auto it = settings.begin(); it != settings.end(); ++it) {
                 const auto &options = nwchemAdvancedOptions();
                 const auto o =
                     std::find_if(options.cbegin(), options.cend(),
                                  [&](const AdvancedOption &option) { return option.key == it.key(); });
                 if (o == options.cend())
                     fail("Unknown advanced setting: " + it.key());
                 if (!it.value().isNull()) {
                     const auto error = nwchemAdvancedError(*o, it.value());
                     if (!error.isEmpty())
                         fail(error);
                     if (o->kind == AdvancedKind::Path && QDir::isAbsolutePath(it.value().toString()))
                         policy.checkPath(it.value().toString());
                 }
                 setNwchemAdvanced(next, it.key(),
                                   it.value().isNull() ? QJsonValue(QJsonValue::Undefined) : it.value());
             }
             failOnAddedErrors(before, validateNwchem(next));
         });
    add({.name = "get_execution",
         .title = "Read execution settings",
         .description = "Read launch settings and local validation. Never starts a process.",
         .output = object({{"settings", type("object")},
                           {"defaults", type("object")},
                           {"errors", strings()},
                           {"warnings", strings()}},
                          {"settings", "defaults", "errors", "warnings"})},
        [this](const QJsonObject &) -> QJsonObject {
            const auto d = document();
            const auto report = validateExecution(d, baseDirectory());
            return {{"settings", executionSettings(d)},
                    {"defaults", executionDefaults()},
                    {"errors", QJsonArray::fromStringList(report.errors)},
                    {"warnings", QJsonArray::fromStringList(report.warnings)},
                    {"policy", policy.toJson()}};
        });
    edit(
        {.name = "set_execution",
         .title = "Set execution settings",
         .description =
             policy.allowExecutionSettings
                 ? "Merge launch settings, including the executable, launcher and environment overrides. "
                   "Does "
                   "not run."
                 : "Merge launch settings. The executable, launcher and environment overrides are set by the "
                   "user in MolForma and cannot be changed over MCP. Does not run.",
         .properties = {{"settings", type("object")}},
         .required = {"settings"},
         .meta = policy.allowExecutionSettings ? QJsonObject{{"anthropic/requiresUserInteraction", true}}
                                               : QJsonObject{}},
        [this](Document &next, const QJsonObject &args) {
            const auto settings = args["settings"].toObject();
            const auto current = executionSettings(next);
            const auto base = baseDirectory();
            const auto before = validateExecution(next, base, false);
            for (auto it = settings.begin(); it != settings.end(); ++it) {
                if (!executionDefaults().contains(it.key()))
                    fail("Unknown execution setting: " + it.key());
                if (!policy.allowExecutionSettings && isProtectedExecutionKey(it.key()) &&
                    it.value() != current[it.key()])
                    fail(it.key() +
                         " is chosen by the user in MolForma's execution panel and cannot be changed "
                         "over MCP. Start the server with --mcp-allow-execution-settings to permit it.");
                if ((it.key() == "job_root" || it.key() == "reference_directory") &&
                    !it.value().toString().isEmpty())
                    policy.checkPath(it.value().toString(), base);
                setExecutionSetting(next, it.key(), it.value());
            }
            failOnAddedErrors(before, validateExecution(next, base, false));
        });
    edit({.name = "set_nwchem",
          .title = "Set NWChem settings",
          .description = "Merge NWChem settings and optional charge/multiplicity. Unknown keys or invalid "
                         "calculation conditions are rejected.",
          .properties = {{"settings", type("object")},
                         {"charge", type("integer")},
                         {"multiplicity", type("integer")}},
          .required = {"settings"}},
         [this](Document &next, const QJsonObject &args) {
             const auto settings = nwchemSettings(next), defaults = nwchemDefaults();
             const auto before = validateNwchem(next);
             const auto requested = args["settings"].toObject();
             for (auto it = requested.begin(); it != requested.end(); ++it) {
                 if (!defaults.contains(it.key()) || defaults[it.key()].type() != it.value().type())
                     fail("Unknown NWChem setting or incorrect type: " + it.key());
                 // A restart names a job folder to copy from: prepared from the history, never set here.
                 if (it.key() == "restart" && !it.value().toObject().isEmpty() &&
                     it.value() != settings[it.key()])
                     fail("restart is prepared from a saved job, not set directly; {} releases it.");
                 if (!policy.allowExecutionSettings && isProtectedNwchemKey(it.key()) &&
                     it.value() != settings[it.key()])
                     fail(it.key() +
                          " is additional NWChem input that MolForma does not check; only the user can "
                          "change it in the calculation tab. Start the server with "
                          "--mcp-allow-execution-settings to permit it.");
                 setNwchemSetting(next, it.key(), it.value());
             }
             for (const auto &key : {"charge", "multiplicity"})
                 if (args.contains(key))
                     setNwchemSetting(next, key, args[key]);
             failOnAddedErrors(before, validateNwchem(next));
         });
    edit({.name = "set_neb_end",
          .title = "Set the end of an NEB path",
          .description =
              "Set the end structure of an NEB calculation (and the task to neb) from XYZ text (xyz) or a "
              "local "
              "structure file or NWChem log (path; its last structure, multi-frame XYZ: the last frame). The "
              "structure needs the document's atoms in the same order; superpose (default true) fits it onto "
              "the current structure. The end is kept by atom ID in settings.neb_end.",
          .properties = {{"xyz", type("string")}, {"path", type("string")}, {"superpose", type("boolean")}},
          .required = {}},
         [this](Document &next, const QJsonObject &args) {
             const auto before = validateNwchem(next);
             Document end;
             if (args.contains("xyz") == args.contains("path"))
                 fail("Give either xyz or path.");
             if (args.contains("xyz"))
                 end = Document::fromXyz(args["xyz"].toString());
             else {
                 const auto path = args["path"].toString();
                 policy.checkPath(path);
                 end = nebEndFromFile(path);
             }
             setNwchemSetting(next, "neb_end",
                              nwchemNebEndSetting(next, end, args["superpose"].toBool(true)));
             setNwchemSetting(next, "task", "neb");
             failOnAddedErrors(before, validateNwchem(next));
         });
    add({.name = "history",
         .title = "Confirm, cancel, undo or redo",
         .description = "Confirm/cancel a candidate, or undo/redo a committed edit.",
         .properties = {{"action", choice({"confirm", "cancel", "undo", "redo"})}},
         .required = {"action"},
         .mutation = true},
        [this](const QJsonObject &args) {
            history(args["action"].toString());
            return summary();
        });
}
QJsonObject McpServer::saveFile(const QJsonObject &args) {
    const auto d = document();
    if (candidate())
        fail("Confirm or cancel the candidate before saving.");
    auto path = args["path"].toString();
    if (path.isEmpty())
        fail("path must not be empty.");
    policy.checkPath(path);
    QFileInfo info(path);
    bool overwrite = args["overwrite"].toBool();
    if ((info.exists() || info.isSymLink()) && !overwrite) {
        if (args.contains("overwrite") || !elicitation())
            fail("File exists; explicitly set overwrite to replace it.");
        // Ask the user through the client, bound to this path and document revision.
        const QJsonObject bound{
            {"tool", "save_file"}, {"path", info.absoluteFilePath()}, {"revision", revision()}};
        const auto answer = incoming.inputResponses["overwrite"].toObject();
        if (openState() != bound || answer.isEmpty())
            throw InputRequired{
                {{"overwrite",
                  elicit(info.absoluteFilePath() + " already exists. Replace it with the current project?",
                         "overwrite",
                         {{"type", "boolean"}, {"title", "Replace the existing file"}, {"default", false}})}},
                sealState(bound)};
        if (answer["action"] != "accept" || !answer["content"].toObject()["overwrite"].toBool())
            fail("File exists and the user did not accept replacing it.");
        overwrite = true;
    }
    if (overwrite)
        saveProject(d, path);
    else {
        // NewOnly makes the no-overwrite promise atomic, including for symbolic links.
        validateDocument(d);
        QFile file(path);
        const auto bytes = QJsonDocument(d.project()).toJson();
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            fail(file.errorString());
        if (file.write(bytes) != bytes.size() || !file.flush()) {
            const auto message = file.errorString();
            file.remove();
            fail(message);
        }
    }
    return {{"path", info.absoluteFilePath()}, {"revision", revision()}};
}
} // namespace mf
