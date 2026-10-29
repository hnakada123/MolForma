# SPDX-License-Identifier: GPL-2.0-or-later
"""Screenshots of the feature pages (docs/features/images, docs/en/features/images).

    features.py jobs                    NWChem jobs for the result viewer and history (ethanol, dimethyl ether)
    features.py run ja|en               the run panel during a calculation (ja: stopped, en: finished)
    features.py shots ja|en [page...]   the other figures, of every page or of the pages named

Run "jobs" and "run ja" / "run en" first: the result viewer opens the ethanol job, and history lists all
of them. Shots go to $MOLFORMA_SHOTS_DIR/shots/<language>/features/<page>/<name>.png.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from molforma_gui import APP_JOBS, JOBS, SAMPLES, WORK, Gui  # noqa: E402


def neighbors(bonds, atom):
    return [b["b"] if b["a"] == atom else b["a"] for b in bonds if atom in (b["a"], b["b"])]


def elements(atoms):
    return {a["id"]: a["element"] for a in atoms}


def jobs():
    """Ethanol (with MOLDEN output) and dimethyl ether at the same level, so that history can compare ΔE."""
    g = Gui("ja")
    try:
        g.open(SAMPLES + "ethanol.molforma.json")
        g.setup_execution(8)
        g.edit("set_advanced", settings={"molden_output": True})
        g.edit("set_nwchem", settings={"start": "ethanol"})
        g.tool("calculation", action="start")
        time.sleep(2)
        print(json.dumps(g.idle(900)["calculation"], ensure_ascii=False), flush=True)
        g.tool("chemistry", action="smiles", smiles="COC")
        g.idle()
        g.tool("history", action="confirm")
        g.edit("set_nwchem", settings={"start": "dimethyl_ether"})
        g.tool("calculation", action="start")
        time.sleep(2)
        print(json.dumps(g.idle(900)["calculation"], ensure_ascii=False), flush=True)
    finally:
        g.close()


class Shots(Gui):
    def __init__(self, language, pages):
        super().__init__(language)
        self.pages = set(pages)

    def want(self, page):
        return not self.pages or page in self.pages

    def snap(self, name, target="mainWindow"):
        if self.want(name.split("/")[0]):
            self.shot(WORK / "shots" / self.language / "features" / f"{name}.png", target)
            print("shot", name, flush=True)


def run(language):
    """A calculation captured while it optimizes: stopped in Japanese, finished in English."""
    g = Shots(language, [])
    try:
        if language == "ja":
            g.open(SAMPLES + "p_nitroaniline.molforma.json")
            g.setup_execution(4)
            g.edit("set_nwchem", settings={"start": "p_nitroaniline"})
        else:
            g.open(SAMPLES + "ethanol.molforma.json")
            g.setup_execution(4)
            g.tool("chemistry", action="smiles", smiles="CC=O")
            g.idle()
            g.tool("history", action="confirm")
            g.edit("set_nwchem", settings={"start": "acetaldehyde"})
        g.tool("calculation", action="start")
        g.tab(2)
        time.sleep(80 if language == "ja" else 45)  # Into the optimization steps.
        g.snap("execution/02_run_panel")
        if language == "ja":
            g.tool("calculation", action="stop")
        print(json.dumps(g.idle(900)["calculation"], ensure_ascii=False), flush=True)
    finally:
        g.close()


def shots(language, pages):
    g = Shots(language, pages)
    T = g.T
    try:
        if g.want("molecule"):
            g.fresh("p_nitroaniline.molforma.json")
            g.activate("quickElement_N")
            g.snap("molecule/01_window")
            g.activate("toolAction_select")
            atoms, bonds = g.atoms()
            el = elements(atoms)
            nitro = next(a for a in el if el[a] == "N" and [el[n] for n in neighbors(bonds, a)].count("O") == 2)
            oxygens = [n for n in neighbors(bonds, nitro) if el[n] == "O"]
            ring = next(n for n in neighbors(bonds, nitro) if el[n] == "C")
            ortho = next(n for n in neighbors(bonds, ring) if el[n] == "C")
            g.select_atoms([oxygens[0], nitro, ring, ortho])
            g.snap("molecule/02_measure")
            g.select_atoms([nitro] + oxygens)
            g.activate("deleteSelection", seconds=0.8)
            g.snap("molecule/03_candidate")
            g.activate("candidateBarCancel")
            g.select_atoms([])
            g.activate("openPeriodicTable", seconds=0.8)
            modal = g.modal()
            g.snap("molecule/04_periodic_table", modal)
            g.key(modal, "Escape")

        if g.want("molecule_pages"):
            g.fresh("ethanol.molforma.json")
            atoms, bonds = g.atoms()
            el = elements(atoms)
            g.select_atoms([next(a for a in el if el[a] == "O")])
            g.snap("molecule_pages/01_edit")
            g.fresh("lactic_R.molforma.json")
            atoms, bonds = g.atoms()
            el = elements(atoms)
            g.select("propertyTabs", index=1)
            g.activate(g.find(T("立体を記録（R/S・E/Z）")), seconds=0.6)
            center = next(a for a in el if el[a] == "C" and sum(el[n] == "O" for n in neighbors(bonds, a)) == 1
                          and len([n for n in neighbors(bonds, a) if el[n] != "H"]) == 3)
            g.select_atoms([center])
            g.snap("molecule_pages/02_transform")
            g.fresh("co_en_complex.molforma.json")
            atoms, _ = g.atoms()
            g.select("propertyTabs", index=3)
            g.select_atoms([a["id"] for a in atoms if a["element"] == "Co"])
            g.snap("molecule_pages/03_coordination")
            g.fresh("ace_ahe_nme.molforma.json")
            g.select("propertyTabs", index=4)
            g.snap("molecule_pages/04_residue")
            g.fresh("na_h2o4_charges.molforma.json")
            g.select("propertyTabs", index=5)
            g.snap("molecule_pages/05_charge")
            g.fresh("sf6.molforma.json")
            g.activate("symmetryAction", seconds=1.0)
            modal = g.modal()
            g.snap("molecule_pages/06_symmetry", modal)
            g.key(modal, "Escape")

        if g.want("coordinates"):
            g.fresh("ethanol_od.molforma.json")
            atoms, bonds = g.atoms()
            el = elements(atoms)
            oxygen = next(a for a in el if el[a] == "O")
            carbon = next(n for n in neighbors(bonds, oxygen) if el[n] == "C")
            g.activate("moleculeLayout_both", seconds=0.6)
            g.select_atoms([oxygen] + [n for n in neighbors(bonds, oxygen) if el[n] == "H"])
            g.snap("coordinates/01_view_and_table")
            g.activate("moleculeLayout_table", seconds=0.6)
            g.select_atoms([carbon, oxygen])
            g.snap("coordinates/02_table")
            g.activate("moleculeLayout_view", seconds=0.4)

        if g.want("calculation"):
            g.fresh("ethanol.molforma.json")
            g.tab(1)
            g.snap("calculation/01_overview")
            g.fresh("phenoxyl.molforma.json")
            g.tab(1)
            g.set("nwchem_multiplicity", 1, seconds=0.6)  # 49 electrons: an error to fix.
            g.snap("calculation/02_diagnostics")
            g.fresh("p_nitroaniline_tddft.molforma.json")
            g.tab(1)
            g.activate("calculationChangeFilter_changed", seconds=0.6)
            g.snap("calculation/03_changed")
            g.activate("calculationChangeFilter_all")

        if g.want("advanced"):
            def section(key):
                rows = g.item("calculationSections")["items"]
                g.select("calculationSections", index=next(r["row"] for r in rows if r["cells"][0]["data"] == key),
                         seconds=0.6)

            g.fresh("ethanol.molforma.json")
            g.tab(1)
            g.activate("calculationDetails_convergence", seconds=0.6)
            g.set("advanced_dft_energy", "1e-8", seconds=0.6)
            section("convergence")
            g.wait(g.tool("ui_input", target="advanced_dft_energy", action="click", x=20, y=10), 0.6)
            g.snap("advanced/01_advanced")
            g.activate("calculationDetails_convergence", seconds=0.4)
            section("orbitals")
            g.set("advanced_molden_output", True)
            g.set("advanced_cube_output", True, seconds=0.6)
            g.snap("advanced/02_output")

        if g.want("execution"):
            g.fresh("ethanol.molforma.json")
            g.tab(2)
            g.activate("executionCheck", seconds=0.8)
            g.snap("execution/01_settings")

        if g.want("input_preview"):
            g.fresh("ethanol.molforma.json")
            g.edit("set_advanced", settings={"molden_output": True, "dft_energy": 1e-8})
            g.tab(3)
            line = g.item("inputPreviewText")["text"].split("\n").index("dft") + 1
            starts = sorted(int(it["objectName"][len("inputPreviewBlock"):])
                            for it in g.ui(query="inputPreviewBlock", includeHidden=True, limit=200)["items"]
                            if it.get("objectName", "").startswith("inputPreviewBlock"))
            g.select("inputPreviewOutline", index=starts.index(line), seconds=0.6)
            g.snap("input_preview/01_preview")

        if g.want("results"):
            job = next(d for d in sorted(os.listdir(JOBS)) if d.startswith("ethanol-"))
            folder = f"{APP_JOBS}/{job}"
            g.fresh("ethanol.molforma.json")
            g.tool("results_action", action="open_directory", path=folder)
            g.idle()
            g.tab(4)
            time.sleep(5.5)  # The status bar message of set_execution lasts about five seconds.
            g.tab_text("resultsDetails", T("概要"))
            g.snap("results/01_summary")
            g.tab_text("resultsDetails", T("最適化"))
            g.snap("results/02_optimization")
            g.tab_text("resultsDetails", T("振動"))
            modes = g.tool("get_results", section="modes")["items"]
            stretch = max((m for m in modes if not m.get("rigid")), key=lambda m: m["frequency"])
            g.tool("results_action", action="select_mode", index=stretch["index"])
            time.sleep(0.4)
            g.set(g.find(T("矢印")), True, seconds=0.6)
            g.snap("results/03_vibration")
            molden = next(f for f in os.listdir(JOBS / job) if f.endswith(".molden"))
            g.tool("results_action", action="open_file", path=f"{folder}/{molden}")
            g.idle()
            time.sleep(2.0)
            g.idle()
            g.snap("results/04_orbital")

        if g.want("history"):
            memos = {"ethanol": ("題材1の練習データ", "Practice file of recipe 1"),
                     "dimethyl_ether": ("エタノールの異性体", "Isomer of ethanol"),
                     "acetaldehyde": ("SMILES CC=O から作成", "Built from SMILES CC=O")}
            for d in os.listdir(JOBS):
                name = d.rsplit("-", 3)[0]
                if name in memos:
                    (JOBS / d / "memo.txt").write_text(memos[name][0 if language == "ja" else 1], encoding="utf-8")
            ethanol = next(f"{APP_JOBS}/{d}" for d in sorted(os.listdir(JOBS)) if d.startswith("ethanol-"))
            g.fresh()
            g.tab(5)
            g.idle()
            g.tool("history_action", action="select", path=ethanol)
            time.sleep(0.8)
            g.snap("history/01_list")
            g.activate("historyReference", seconds=0.8)
            g.snap("history/02_delta_e")
            g.activate("historyCompareOff")

        if g.want("rendering"):
            g.fresh("ace_ae_nme.molforma.json")
            atoms, bonds = g.atoms()
            el = elements(atoms)
            position = {a["id"]: a["position"] for a in atoms}
            g.activate(g.find(T("水素結合を表示")), seconds=0.4)
            # A measurement beside the hydrogen bond: the terminal methyl carbons of the caps.
            methyls = [a for a in el if el[a] == "C" and sum(el[n] == "H" for n in neighbors(bonds, a)) == 3]
            far = max(((a, b) for a in methyls for b in methyls if a < b),
                      key=lambda p: sum((position[p[0]][i] - position[p[1]][i]) ** 2 for i in range(3)))
            g.select_atoms(list(far))
            g.activate(g.find(T("選択中の計測を画面に表示")), seconds=0.4)
            g.select_atoms([])
            g.snap("rendering/01_hydrogen_bonds")
            g.activate(g.find(T("水素結合を表示")), seconds=0.2)
            g.fresh("caffeine.molforma.json")
            g.activate(g.find(T("空間充填（CPK）")), seconds=0.6)
            g.activate("labelMode_none", seconds=0.4)
            g.snap("rendering/02_space_filling")
            g.activate(g.find(T("ボール＆スティック")), seconds=0.2)
            g.activate("labelMode_all", seconds=0.2)

        if g.want("app"):
            # The second figure of the page is the window in the other language (molecule/01_window):
            # Preferences hold run settings, which MCP may not operate.
            g.fresh("ethanol.molforma.json")
            g.activate("manualAction", seconds=1.5)
            modal = g.modal()
            g.snap("app/01_manual", modal)
            g.key(modal, "Escape")

        if g.want("ai"):
            g.fresh()
            g.open(SAMPLES + "caffeine.molforma.json")
            atoms, _ = g.atoms()
            g.select_atoms([a["id"] for a in atoms if a["element"] == "N"])
            g.snap("ai/01_mcp_gui")
    finally:
        g.close()


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("jobs", "run", "shots"):
        sys.exit(__doc__)
    if sys.argv[1] == "jobs":
        jobs()
    elif sys.argv[1] == "run":
        run(sys.argv[2])
    else:
        shots(sys.argv[2], sys.argv[3:])
