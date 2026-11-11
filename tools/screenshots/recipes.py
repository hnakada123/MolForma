# SPDX-License-Identifier: GPL-2.0-or-later
"""Screenshots of the recipes (docs/recipes/images, docs/en/recipes/images), in the views of the earlier ones.

    recipes.py ja|en [recipe...]

Covers the figures that were taken again after the selection card of the edit page changed (2026-10-27):
the steps below rebuild each state as the recipe does, and viewfit turns, zooms and pans the 3D view onto
the atoms where the earlier screenshot shows them (TARGETS, window pixels of sphere centers). Each figure
prints the largest remaining distance; a few pixels is a match. The last operation is repeated right before
a shot, so that its status bar message (shown for about 5 s) is still there.

Recipes: atoms_and_bonds, atoms_and_bonds_run (figure 9, the run panel during the calculation),
atoms_and_bonds_variations (saves, calculates, then figures 13 and 14), caffeine_smiles, my_parts,
phenoxyl_radical, stereochemistry, aromatic_substitution (default: all of these).
Shots go to $MOLFORMA_SHOTS_DIR/shots/<language>/recipes/<recipe>/<name>.png.

calculation_figures: the figures of "2 計算条件" of several recipes (2026-11-11). butane_conformers_figures and
sn2_transition_state_figures (the scan and the IRC, also in the result viewer) after butane_conformers_job and
sn2_transition_state_jobs (about an hour of NWChem together).

Figures of the result viewer and the history, which need the jobs of their recipes (named on the command
line; not part of the default): reuse_result_jobs (p-nitroaniline at B3LYP/6-31G*, optimization and
frequencies, then TDDFT at CAM-B3LYP/6-31+G*: more than an hour) before reuse_result (figures 1 and 6 and
the figure at the top), and hydrated_sodium_job (a minute) before hydrated_sodium (figure 8). The jobs are
run once, in either language, and found again through $MOLFORMA_SHOTS_DIR/recipe_jobs.json.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import viewfit  # noqa: E402
from molforma_gui import APP_HOME, APP_JOBS, HOME, SAMPLES, WORK, Gui  # noqa: E402

ETHANOL = {"O8": (562, 455), "C1": (671, 530), "C4": (789, 453), "H9": (487, 510), "H7": (874, 512),
           "H5": (794, 373), "H6": (789, 411), "H3": (670, 614), "H2": (663, 561)}
TARGETS = {
    "atoms_and_bonds": {"O8": (598, 451), "C1": (679, 533), "C4": (767, 452), "H9": (541, 506), "H6": (714, 380),
                        "H5": (834, 401), "H7": (821, 511), "H3": (618, 590), "H2": (733, 596)},
    "atoms_and_bonds/05_cleanup_candidate": ETHANOL,
    "atoms_and_bonds/06_distance": ETHANOL,
    "atoms_and_bonds/13_acetaldehyde": {"O7": (526, 490), "C1": (627, 531), "C3": (758, 458), "H2": (626, 626),
                                        "H4": (750, 368), "H5": (785, 432), "H6": (835, 524)},
    "atoms_and_bonds/14_dimethyl_ether": {"C1": (601, 546), "O9": (739, 552), "C5": (790, 420), "H3": (517, 490),
                                          "H4": (602, 641), "H6": (813, 356), "H7": (778, 363), "H8": (875, 483)},
    "caffeine_smiles/02_smiles_candidate": {
        "N2": (577, 493), "C3": (561, 419), "N4": (626, 380), "C5": (684, 431), "C6": (654, 498), "C7": (705, 561),
        "O8": (678, 623), "N9": (780, 549), "C10": (831, 611), "C11": (805, 478), "O12": (872, 467),
        "N13": (759, 417), "C14": (790, 344), "C1": (526, 553), "H18": (506, 395), "H17": (468, 536),
        "H24": (747, 300), "H21": (805, 664), "H20": (867, 607)},
    "caffeine_smiles/03_caffeine": {
        "N2": (581, 506), "C3": (567, 438), "N4": (630, 397), "C5": (685, 435), "C6": (656, 503), "C7": (705, 555),
        "O8": (679, 617), "N9": (775, 538), "C10": (824, 588), "C11": (799, 467), "O12": (862, 449),
        "N13": (757, 417), "C14": (785, 343), "C1": (537, 571), "H18": (517, 423), "H17": (481, 559),
        "H24": (745, 307), "H22": (834, 345), "H21": (798, 643), "H20": (874, 599)},
    "caffeine_smiles/04_sdf_flat": {
        "C1": (541, 637), "N2": (569, 551), "C3": (516, 480), "N4": (569, 408), "C5": (653, 436), "C6": (653, 525),
        "C7": (731, 569), "O8": (731, 659), "N9": (808, 525), "C10": (885, 570), "C11": (808, 436),
        "O12": (885, 391), "N13": (731, 391), "C14": (731, 302)},
    "caffeine_smiles/05_hydrogens": {
        "C1": (541, 637), "N2": (569, 551), "C3": (516, 480), "N4": (569, 408), "C5": (653, 436), "C6": (653, 525),
        "C7": (731, 569), "O8": (731, 659), "N9": (808, 525), "C10": (885, 570), "C11": (808, 436),
        "O12": (885, 391), "N13": (731, 391), "C14": (731, 302), "H18": (436, 481), "H16": (446, 635),
        "H22": (656, 269), "H24": (814, 265), "H21": (958, 526), "H19": (871, 658)},
    "caffeine_smiles/06_regenerate_candidate": {
        "C1": (546, 555), "N2": (566, 502), "C3": (563, 540), "N4": (636, 533), "C5": (686, 493), "C6": (644, 474),
        "C7": (685, 432), "O8": (651, 420), "N9": (759, 404), "C10": (803, 380), "C11": (799, 441),
        "O12": (870, 436), "N13": (765, 485), "C14": (807, 557), "H21": (768, 344), "H20": (853, 344),
        "H24": (868, 548), "H15": (486, 578), "H17": (584, 606), "H23": (781, 592)},
    "my_parts/02_trimethylsilane": {
        "Si1": (695, 526), "C2": (823, 457), "C3": (717, 553), "C4": (566, 438), "H5": (679, 637), "H6": (884, 432),
        "H7": (800, 380), "H8": (866, 516), "H9": (653, 612), "H10": (711, 474), "H11": (801, 590),
        "H12": (488, 490), "H13": (566, 400), "H14": (560, 369)},
    "my_parts/07_place_hint": {
        "C1": (830, 441), "C2": (809, 579), "C3": (679, 628), "C4": (570, 541), "C5": (592, 403), "C6": (722, 353),
        "H7": (933, 403), "H8": (893, 647), "H9": (661, 736), "H10": (469, 579), "H11": (508, 335),
        "H12": (740, 246)},
    "my_parts/08_tms_benzene": {
        "C1": (673, 509), "C2": (664, 579), "C3": (606, 612), "C4": (556, 580), "C5": (564, 511), "C6": (622, 476),
        "Si7": (755, 458), "H8": (704, 607), "H9": (599, 671), "H10": (509, 609), "H11": (524, 483),
        "H12": (627, 418), "C13": (827, 467), "C14": (784, 496), "C15": (731, 360), "H16": (877, 488),
        "H17": (839, 416), "H21": (807, 549), "H22": (678, 355), "H23": (731, 333), "H24": (769, 334)},
    "phenoxyl_radical/01_phenol": {
        "C1": (776, 458), "C2": (759, 558), "C3": (663, 594), "C4": (585, 530), "C5": (601, 429), "C6": (697, 392),
        "O7": (873, 419), "H8": (822, 611), "H9": (650, 675), "H10": (508, 558), "H11": (538, 377),
        "H12": (711, 312), "H13": (925, 472)},
    "stereochemistry/01_lactic": {
        "C1": (691, 465), "C2": (591, 478), "C3": (782, 481), "O4": (689, 530), "H5": (698, 387), "H6": (586, 425),
        "H7": (584, 556), "H8": (526, 465), "O10": (867, 514), "H11": (920, 518), "H12": (691, 601)},
    "aromatic_substitution": {
        "C1": (784, 460), "C2": (770, 546), "C3": (688, 578), "C4": (621, 520), "C5": (634, 434), "C6": (716, 404),
        "N7": (871, 428), "H8": (824, 591), "H9": (678, 647), "N10": (531, 554), "H11": (580, 388),
        "H12": (728, 334), "H13": (919, 471), "H14": (880, 369), "O15": (470, 500), "O16": (520, 634)},
}


class Recipes(Gui):
    def fit(self, name):
        angles, error = viewfit.fit(self, TARGETS[name])
        print(name, "angles", angles, "residual %.1f px" % error, flush=True)

    def snap(self, name):
        self.shot(WORK / "shots" / self.language / "recipes" / f"{name}.png")
        print("shot", name, flush=True)


def ethanol(g):
    """Steps 1-2: carbon, a carbon on H2, an oxygen on H3 of C1 (which gives the practice file once cleaned)."""
    g.new()
    g.activate("quickElement_C")
    g.click_empty()
    g.click_atom("H2")
    g.activate("quickElement_O")
    g.click_atom("H3")


def atoms_and_bonds(g):
    # The cover: the practice file opened, C1 and O8 chosen.
    g.open_dialog(SAMPLES + "ethanol.molforma.json")
    g.activate("quickElement_C")
    g.activate("toolAction_select")
    g.highlight("atom:C")
    g.select_atoms(g.ids("C1", "O8"))
    g.fit("atoms_and_bonds")
    g.snap("atoms_and_bonds")
    ethanol(g)
    g.refine()
    g.fit("atoms_and_bonds/05_cleanup_candidate")
    g.activate("candidateBarCancel", seconds=0.3)
    g.refine()
    g.snap("atoms_and_bonds/05_cleanup_candidate")
    g.confirm()
    g.activate("toolAction_select")
    g.select_atoms(g.ids("C1", "O8"))
    g.fit("atoms_and_bonds/06_distance")
    g.undo()
    g.refine()
    g.confirm()
    g.select_atoms(g.ids("C1", "O8"))
    g.snap("atoms_and_bonds/06_distance")


def atoms_and_bonds_run(g):
    g.open(SAMPLES + "ethanol.molforma.json")
    g.setup_execution(8)
    g.select("workspaceTabs", index=3, seconds=0.5)
    g.tool("calculation", action="start")
    time.sleep(7)  # Into the first step of the optimization.
    g.snap("atoms_and_bonds/09_run_panel")
    g.tool("calculation", action="stop")
    g.idle(60)


def atoms_and_bonds_variations(g):
    ethanol(g)
    g.refine()
    g.confirm()
    g.activate("toolAction_select")
    # Saved and calculated (steps 4-5), so that the title and the status bar show them.
    name = f"{g.language}_ethanol.molforma.json"
    os.makedirs(HOME / "MolForma" / "projects", exist_ok=True)
    if (HOME / "MolForma" / "projects" / name).exists():
        (HOME / "MolForma" / "projects" / name).unlink()
    g.activate(g.find(g.T("名前を付けて保存…")), seconds=0.8)
    g.wait(g.tool("ui_file_dialog", target=g.modal(), path=f"{APP_HOME}/MolForma/projects/{name}", accept=True), 0.8)
    g.setup_execution(8)
    g.tool("calculation", action="start")
    time.sleep(2)
    g.idle(900)
    g.activate("runPanelClose", seconds=0.3)
    g.select("workspaceTabs", index=0, seconds=0.5)
    # Acetaldehyde: delete H9 and H2 of C1, make C1=O a double bond, clean up.
    for h in ("H9", "H2"):
        g.activate("toolAction_delete")
        g.click_atom(h)
        g.confirm()
    g.select_atoms(g.ids("C1", "O7"))
    g.activate("bondOrder2Action", seconds=0.5)
    g.select_atoms([])
    g.refine()
    g.confirm()
    g.activate("toolAction_select")
    g.fit("atoms_and_bonds/13_acetaldehyde")
    g.undo()
    g.refine()
    g.confirm()
    g.snap("atoms_and_bonds/13_acetaldehyde")
    # Dimethyl ether: ethane, then an oxygen clicked onto the C–C bond.
    g.new()
    g.activate("quickElement_C")
    g.select("toolBondKind", index=0)  # Single bonds again after the double bond above.
    g.click_empty()
    g.click_atom("H2")
    g.activate("quickElement_O")
    g.click_bond("C1", "C5")
    g.fit("atoms_and_bonds/14_dimethyl_ether")
    g.undo()
    g.click_bond("C1", "C5")
    g.snap("atoms_and_bonds/14_dimethyl_ether")


def caffeine_smiles(g):
    def smiles():
        g.tool("chemistry", action="smiles", smiles="Cn1cnc2c1c(=O)n(C)c(=O)n2C")
        g.idle()
        time.sleep(0.3)

    g.new()
    g.activate("quickElement_C")
    g.activate("toolAction_select")
    g.highlight("atom:C")
    smiles()
    g.fit("caffeine_smiles/02_smiles_candidate")
    g.activate("candidateBarCancel", seconds=0.3)
    smiles()
    g.snap("caffeine_smiles/02_smiles_candidate")
    g.confirm()
    g.fit("caffeine_smiles/03_caffeine")
    g.undo()
    smiles()
    g.confirm()
    if viewfit.residual(g, TARGETS["caffeine_smiles/03_caffeine"]) > 5:
        g.fit("caffeine_smiles/03_caffeine")
    g.snap("caffeine_smiles/03_caffeine")
    g.open_dialog(SAMPLES + "caffeine_2d.sdf")
    g.highlight("atom:C")
    g.fit("caffeine_smiles/04_sdf_flat")
    g.snap("caffeine_smiles/04_sdf_flat")
    add_hydrogens = g.find(g.T("水素を補完"), "QPushButton")
    g.activate(add_hydrogens, seconds=0.6)
    g.fit("caffeine_smiles/05_hydrogens")
    g.undo()
    g.activate(add_hydrogens, seconds=0.6)
    g.snap("caffeine_smiles/05_hydrogens")
    g.activate("regenerateAction", seconds=0.5)
    g.idle()
    time.sleep(0.3)
    g.fit("caffeine_smiles/06_regenerate_candidate")  # Not repeated: regenerating again gives other coordinates.
    g.snap("caffeine_smiles/06_regenerate_candidate")


def my_parts(g):
    # Trimethylsilane: silicon from the search, three hydrogens replaced by methyl, cleaned up.
    g.new()
    g.activate("paletteCategory_atom")
    g.set("paletteSearch", "Si", seconds=0.5)
    g.tile("atom:Si")
    g.click_empty()
    g.set("paletteSearch", "", seconds=0.3)
    g.activate("paletteCategory_substituent")
    for h in ("H2", "H3", "H4"):
        g.tile("substituent:methyl")
        g.click_atom(h)
    g.refine()
    g.confirm()
    g.activate("toolAction_select")
    g.fit("my_parts/02_trimethylsilane")
    g.undo()
    g.refine()
    g.confirm()
    g.snap("my_parts/02_trimethylsilane")
    # The part, saved in the language of the window (the practice file has its Japanese name).
    part = json.load(open(SAMPLES + "trimethylsilyl.molforma-part.json", encoding="utf-8"))
    if g.language != "ja":
        part.update(id="my:Trimethylsilyl", name_en="Trimethylsilyl", name_ja="Trimethylsilyl")
    os.makedirs(HOME / "MolForma" / "parts", exist_ok=True)
    with open(HOME / "MolForma" / "parts" / "trimethylsilyl.molforma-part.json", "w", encoding="utf-8") as f:
        json.dump(part, f, ensure_ascii=False)
    g.new()
    g.activate("paletteCategory_skeleton")
    g.tile("skeleton:benzene")
    g.click_empty()
    g.activate("paletteCategory_my")
    g.activate("loadPart", seconds=0.8)
    path = f"{APP_HOME}/MolForma/parts/trimethylsilyl.molforma-part.json"
    g.wait(g.tool("ui_file_dialog", target=g.modal(), path=path, accept=True), 0.8)
    g.tile(part["id"])
    g.fit("my_parts/07_place_hint")
    # "Place part" from placing the benzene: placed again where it was, then the part chosen again.
    view = g.view_positions().values()
    x, y = sum(p[0] for p in view) / len(view), sum(p[1] for p in view) / len(view)
    g.undo()
    g.activate("paletteCategory_skeleton")
    g.tile("skeleton:benzene")
    g.click_view(x, y)
    g.activate("paletteCategory_my")
    g.tile(part["id"])
    g.snap("my_parts/07_place_hint")
    g.click_atom("H7")
    g.refine()
    g.confirm()
    g.activate("toolAction_select")
    g.fit("my_parts/08_tms_benzene")
    g.undo()
    g.refine()
    g.confirm()
    g.snap("my_parts/08_tms_benzene")


def substituted(g, skeleton, substituents, name):
    """A skeleton with substituents on the hydrogens named, cleaned up."""
    g.new()
    g.activate("paletteCategory_skeleton")
    g.tile(skeleton)
    g.click_empty()
    g.activate("paletteCategory_substituent")
    for template, hydrogen in substituents:
        g.tile(template)
        g.click_atom(hydrogen)
    g.refine()
    g.confirm()
    g.activate("toolAction_select")
    g.fit(name)
    g.undo()
    g.refine()
    g.confirm()
    g.snap(name)


def phenoxyl_radical(g):
    substituted(g, "skeleton:benzene", [("substituent:hydroxy", "H7")], "phenoxyl_radical/01_phenol")


def stereochemistry(g):
    substituted(g, "skeleton:ethane", [("substituent:carboxyl", "H3"), ("substituent:hydroxy", "H4")],
                "stereochemistry/01_lactic")


def aromatic_substitution(g):
    g.open_dialog(SAMPLES + "p_nitroaniline.molforma.json")
    g.activate("quickElement_C")
    g.activate("toolAction_select")  # As in the earlier screenshot: no palette item marked.
    g.fit("aromatic_substitution")
    g.snap("aromatic_substitution")


def calculate(g, key, hours=3, states=("finished",)):
    """Runs the calculation of the document and notes its job folder under the key."""
    g.tool("calculation", action="start")
    time.sleep(2)
    calculation = g.idle(hours * 3600)["calculation"]
    print(key, json.dumps(calculation, ensure_ascii=False), flush=True)
    if calculation["state"] not in states:
        raise RuntimeError(f"{key}: {calculation['state']}")
    path = WORK / "recipe_jobs.json"
    jobs = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    jobs[key] = os.path.basename(calculation["directory"])
    path.write_text(json.dumps(jobs, indent=1), encoding="utf-8")


def job(key):
    """The folder of a job that calculate() ran, as MolForma sees it."""
    return f"{APP_JOBS}/{json.loads((WORK / 'recipe_jobs.json').read_text(encoding='utf-8'))[key]}"


def results(g, key, tab):
    """A job in the result viewer, on a tab of its details, once the status bar shows the molecule again."""
    g.tool("results_action", action="open_directory", path=job(key))
    g.idle()
    g.tab(4)
    g.tab_text("resultsDetails", g.T(tab))
    for _ in range(60):
        if not g.tool("get_app_state")["status"]:
            break
        time.sleep(0.2)


def reuse_result_jobs(g):
    g.open(SAMPLES + "p_nitroaniline.molforma.json")
    g.setup_execution(6)
    calculate(g, "reuse_result/optimization")
    g.open(SAMPLES + "p_nitroaniline_tddft.molforma.json")
    g.setup_execution(6)
    calculate(g, "reuse_result/tddft")


def reuse_result(g):
    # Figure 1: the project of recipe 3 as opened, and its job in the history.
    g.open_dialog(SAMPLES + "p_nitroaniline.molforma.json")
    g.tab(5)
    g.idle()
    g.tool("history_action", action="select", path=job("reuse_result/optimization"))
    time.sleep(0.8)
    g.snap("reuse_result/01_history")
    # Figure 6: the same project with the settings of the TDDFT calculation, and its result.
    g.edit("set_nwchem", settings={"functional": "cam-b3lyp", "basis": "6-31+g*", "task": "energy", "tddft": True,
                                   "tddft_roots": 10})
    results(g, "reuse_result/tddft", "励起状態")
    g.snap("reuse_result/06_excited_states")
    # The figure at the top of the page: the practice file of the recipe with the same result.
    g.new()
    g.open_dialog(SAMPLES + "p_nitroaniline_tddft.molforma.json")
    results(g, "reuse_result/tddft", "励起状態")
    g.snap("reuse_result")


def hydrated_sodium_job(g):
    g.open(SAMPLES + "na_h2o4_charges.molforma.json")
    g.setup_execution(8)
    g.edit("set_nwchem", settings={"task": "energy", "prop_mulliken": True})
    calculate(g, "hydrated_sodium/charges")


def hydrated_sodium(g):
    g.open_dialog(SAMPLES + "na_h2o4_charges.molforma.json")
    g.edit("set_nwchem", settings={"task": "energy", "prop_mulliken": True})
    results(g, "hydrated_sodium/charges", "物性")
    g.snap("hydrated_sodium/08_mulliken")


def sn2_transition_state_jobs(g):
    """Steps 4-5 of the recipe: the transition state with frequencies (about 13 minutes), then IRC from it in
    both directions, which stops in the forward one with "mepgs: could not optimize" (about 25 minutes), and
    backward only (about 15 minutes); M06-2X/aug-cc-pVDZ at 8 MPI processes."""
    g.open(SAMPLES + "sn2_ts_guess.molforma.json")
    g.setup_execution(8)
    calculate(g, "sn2_transition_state/ts")
    g.tool("results_action", action="open_directory", path=job("sn2_transition_state/ts"))
    g.idle()
    g.tool("results_action", action="apply_structure")
    g.tool("history", action="confirm")
    g.edit("set_nwchem", settings={"task": "irc", "irc_points": 60, "irc_stride": 0.15})
    calculate(g, "sn2_transition_state/irc", states=("finished", "failed"))
    g.edit("set_nwchem", settings={"irc_direction": "backward"})
    calculate(g, "sn2_transition_state/irc_backward")


def butane_conformers_job(g):
    """Step 4 of the recipe: the relaxed scan of the practice file (13 points, B3LYP/6-31G*)."""
    g.open(SAMPLES + "butane_scan.molforma.json")
    g.setup_execution(6)
    calculate(g, "butane_conformers/scan", hours=4)


# Figures of "2 計算条件", taken again on 2026-11-11 when the tab gained NEB (a task and the section "IRC・スキャン・NEB").
# A recipe that builds its molecule shows "新規分子": the practice file is read over MCP into a new molecule, and a
# title the recipe does not set goes back to the default, so that the changed settings are those of the recipe.
def section(g, key):
    """Scrolls the form to a section; again once the sections opened their details, which move it."""
    rows = g.item("calculationSections")["items"]
    row = next(r["row"] for r in rows if r["cells"][0]["data"] == key)
    for _ in range(2):
        g.select("calculationSections", index=row, seconds=1.0)


def built(g, sample, **settings):
    g.new()
    g.open(SAMPLES + sample)
    g.edit("set_nwchem", settings={"title": "MolForma calculation", **settings})
    g.tab(1)
    section(g, "basic")  # The form keeps where it was scrolled to for the last molecule.


def calculation_figures(g):
    built(g, "ethanol.molforma.json")
    g.snap("atoms_and_bonds/07_calculation")
    built(g, "p_nitroaniline.molforma.json")
    g.snap("aromatic_substitution/05_calculation")
    built(g, "co_en_complex.molforma.json")
    g.snap("cobalt_complex/08_calculation")
    built(g, "benzene_water.molforma.json")
    section(g, "bsse")
    g.snap("benzene_water/05_bsse")
    built(g, "phenolate.molforma.json")
    g.snap("phenoxyl_radical/07_symmetry_check")
    built(g, "pf5.molforma.json")
    g.snap("main_group/07_symmetry_check")
    built(g, "sn2_ts_guess.molforma.json")
    g.snap("sn2_transition_state/06_ts_settings")
    # Phenol without the hydrogen of its OH: 49 electrons at multiplicity 1.
    built(g, "phenol.molforma.json")
    g.tab(0)
    atoms, bonds = g.atoms()
    element = {a["id"]: a["element"] for a in atoms}
    oxygen = next(a for a in element if element[a] == "O")
    hydrogen = next(b["a"] if b["b"] == oxygen else b["b"] for b in bonds
                    if oxygen in (b["a"], b["b"]) and element[b["a"] if b["b"] == oxygen else b["b"]] == "H")
    g.select_atoms([hydrogen])
    g.activate("deleteSelection", seconds=0.6)
    g.confirm()
    g.tab(1)
    section(g, "electronic")
    g.snap("phenoxyl_radical/02_mismatch")
    # Opened practice files keep their names.
    g.new()
    g.open_dialog(SAMPLES + "ethanol_opt.molforma.json")
    g.edit("set_nwchem", settings={"task": "frequencies"})
    g.tab(1)
    section(g, "basic")
    g.snap("isotopes/03_frequencies")
    g.new()
    g.open_dialog(SAMPLES + "na_h2o4.molforma.json")
    g.open(SAMPLES + "na_h2o4_charges.molforma.json")  # The water turned into point charges.
    g.edit("set_nwchem", settings={"title": "[Na(H2O)4]+ single point"})
    g.tab(1)
    section(g, "environment")
    g.snap("hydrated_sodium/06_environment")
    g.new()
    g.open_dialog(SAMPLES + "ace_ahe_nme.molforma.json")
    g.tab(1)
    section(g, "environment")
    g.snap("capped_peptide/08_environment")
    g.new()
    g.open_dialog(SAMPLES + "p_nitroaniline.molforma.json")
    g.edit("set_nwchem", settings={"functional": "cam-b3lyp", "basis": "6-31+g*", "task": "energy", "tddft": True,
                                   "tddft_roots": 10})
    g.tab(1)
    section(g, "excited")
    g.snap("reuse_result/04_tddft_settings")


def butane_conformers_figures(g):
    """Figures 5 (the scan in "2 計算条件") and 7 (its result, after butane_conformers_job)."""
    g.new()
    g.open_dialog(SAMPLES + "butane_anti.molforma.json")
    atoms = g.ids("C3", "C1", "C2", "C8")
    g.select_atoms(atoms)
    # As "選択中の原子を設定" and the fields would: MCP refuses a scan without its coordinate.
    g.edit("set_nwchem", settings={"task": "scan", "method": "dft", "basis": "6-31g*", "scan_atoms": atoms,
                                   "scan_start": 180, "scan_end": 0, "scan_steps": 13})
    g.tab(1)
    section(g, "path")
    g.snap("butane_conformers/05_scan_settings")
    results(g, "butane_conformers/scan", "経路・スキャン")
    g.snap("butane_conformers/07_scan_profile")


def sn2_transition_state_figures(g):
    """Figures 8-10: the IRC from the transition state (after sn2_transition_state_jobs)."""
    g.new()
    g.open_dialog(SAMPLES + "sn2_ts_guess.molforma.json")
    g.tool("results_action", action="open_directory", path=job("sn2_transition_state/ts"))
    g.idle()
    g.tool("results_action", action="apply_structure")
    g.tool("history", action="confirm")
    g.edit("set_nwchem", settings={"task": "irc", "irc_points": 60, "irc_stride": 0.15})
    g.tab(1)
    section(g, "path")
    g.snap("sn2_transition_state/08_irc_settings")
    results(g, "sn2_transition_state/irc", "経路・スキャン")
    g.snap("sn2_transition_state/09_irc_forward_stopped")
    g.edit("set_nwchem", settings={"irc_direction": "backward"})
    results(g, "sn2_transition_state/irc_backward", "経路・スキャン")
    g.snap("sn2_transition_state/10_irc_backward")


RECIPES = [atoms_and_bonds, atoms_and_bonds_run, atoms_and_bonds_variations, caffeine_smiles, my_parts, phenoxyl_radical,
           stereochemistry, aromatic_substitution]

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("ja", "en"):
        sys.exit(__doc__)
    names = sys.argv[2:] or [r.__name__ for r in RECIPES]
    for recipe in names:
        g = Recipes(sys.argv[1])
        try:
            globals()[recipe](g)
        finally:
            g.close()
