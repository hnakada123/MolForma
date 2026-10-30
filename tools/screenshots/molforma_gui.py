# SPDX-License-Identifier: GPL-2.0-or-later
"""Drives `molforma --mcp-gui` over MCP (JSON-RPC on stdio) to take the screenshots of the manual."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WORK = Path(os.environ.get("MOLFORMA_SHOTS_DIR", REPO / "build" / "screenshots"))
BINARY = os.environ.get("MOLFORMA_BINARY", str(REPO / "build" / "molforma"))
SAMPLES = f"{REPO}/docs/recipes/samples/"
CATALOG = json.load(open(REPO / "assets" / "translations" / "en.json", encoding="utf-8"))
# MolForma runs in a user namespace whose /home holds a home of "user" (and the real home, for the
# repository), so that the paths in the screenshots read ~/MolForma/jobs. MOLFORMA_SHOTS_NAMESPACE=0
# runs it directly instead, with the scratch home under WORK.
NAMESPACE = os.environ.get("MOLFORMA_SHOTS_NAMESPACE", "1") != "0"
HOME = WORK / "home"  # The home MolForma sees, as seen by these scripts.
JOBS = HOME / "MolForma" / "jobs"
APP_HOME = "/home/user" if NAMESPACE else str(HOME)
APP_JOBS = f"{APP_HOME}/MolForma/jobs"  # The same folder, as MolForma sees it.
DISCARD = ("Discard", "Don't Save", "Close without Saving", "変更を破棄", "破棄", "保存しない")


def namespace_script(stage, command):
    """Shell commands that put /home/user (and the real home) in place inside `unshare -rm`."""
    real = Path.home()
    steps = []
    if real.parent == Path("/home") and real.name != "user":
        steps.append(f"mount --bind {real} {stage}/real")
    steps += [f"mount --bind {HOME / 'MolForma'} {stage}/molforma", "mount -t tmpfs tmpfs /home",
              "mkdir -p /home/user/MolForma", f"mount --bind {stage}/molforma /home/user/MolForma"]
    if real.parent == Path("/home") and real.name != "user":
        steps += [f"mkdir -p {real}", f"mount --bind {stage}/real {real}"]
    return " && ".join(steps + ["cd /home/user", "exec " + command])


class Gui:
    def __init__(self, language="ja"):
        self.language = language
        self.scratch = tempfile.mkdtemp(prefix="molforma-shots-")
        os.makedirs(HOME / "MolForma" / "jobs", exist_ok=True)
        os.makedirs(f"{self.scratch}/runtime", mode=0o700)
        env = dict(os.environ)
        env.update(
            QT_QPA_PLATFORM="offscreen",
            MOLFORMA_RENDERER="painter",
            MOLFORMA_LANGUAGE=language,
            XDG_CONFIG_HOME=f"{self.scratch}/config",  # Fresh settings, never the user's.
            XDG_DATA_HOME=f"{self.scratch}/data",
            XDG_RUNTIME_DIR=f"{self.scratch}/runtime",
            HOME=APP_HOME,
            # Inside the namespace MolForma (and mpirun) run as root.
            OMPI_ALLOW_RUN_AS_ROOT="1",
            OMPI_ALLOW_RUN_AS_ROOT_CONFIRM="1",
        )
        command = f"{BINARY} --mcp-gui --mcp-allow-execution-settings"
        if NAMESPACE:
            os.makedirs(f"{self.scratch}/real")
            os.makedirs(f"{self.scratch}/molforma")
            argv = ["unshare", "-rm", "sh", "-c", namespace_script(self.scratch, command)]
        else:
            argv = command.split()
        self.process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env,
                                        text=True, bufsize=1, cwd=APP_HOME if not NAMESPACE else None)
        self.counter = 0
        self.doc = None
        self.call("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                 "clientInfo": {"name": "molforma-screenshots", "version": "1"}})
        self.process.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        self.process.stdin.flush()
        self.schemas = {t["name"]: t["inputSchema"].get("properties", {})
                        for t in self.call("tools/list", {})["tools"]}
        self.doc = self.tool("list_documents")["documents"][0]["documentId"]

    # Protocol
    def call(self, method, params):
        self.counter += 1
        self.process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.counter, "method": method,
                                             "params": params}) + "\n")
        self.process.stdin.flush()
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError("molforma exited")
            message = json.loads(line)
            if message.get("id") == self.counter:
                if "error" in message:
                    raise RuntimeError(message["error"])
                return message["result"]

    def tool(self, name, check=True, **args):
        """Calls a tool, filling in the document ID and its current revision where the tool takes them."""
        if os.environ.get("TRACE"):
            shown = {k: v for k, v in args.items() if k not in ("documentId", "expectedRevision")}
            print("call", name, shown, file=sys.stderr, flush=True)
        properties = self.schemas.get(name, {})
        if "documentId" in properties and "documentId" not in args and self.doc:
            args["documentId"] = self.doc
        if "expectedRevision" in properties and "expectedRevision" not in args and self.doc:
            args["expectedRevision"] = self.tool("get_document", include=[])["revision"]
        result = self.call("tools/call", {"name": name, "arguments": args})
        data = result.get("structuredContent")
        if data is None:
            text = result["content"][0].get("text", "") if result.get("content") else ""
            try:
                data = json.loads(text)
            except ValueError:
                data = {"text": text}
        if check and result.get("isError"):
            raise RuntimeError(f"{name} {args}: {data}")
        return data

    def close(self):
        try:
            self.process.stdin.close()
            self.process.wait(10)
        except Exception:
            self.process.kill()
        shutil.rmtree(self.scratch, ignore_errors=True)

    # GUI commands
    def T(self, japanese):
        """A text of the interface in the session's language."""
        return japanese if self.language == "ja" else CATALOG[japanese]

    def wait(self, command=None, seconds=0.3):
        if command and "commandId" in command:
            for _ in range(200):
                state = self.tool("get_ui_command", commandId=command["commandId"]).get("state")
                if state in ("done", "failed", "cancelled"):
                    break
                # A dialog the command opened keeps it running: the caller answers the dialog.
                if state == "running" and self.modal():
                    break
                time.sleep(0.05)
        time.sleep(seconds)

    def activate(self, target, seconds=0.3, **kw):
        command = self.tool("ui_activate", target=target, **kw)
        self.wait(command, seconds)
        return command

    def select(self, target, seconds=0.3, **kw):
        command = self.tool("ui_select", target=target, **kw)
        self.wait(command, seconds)
        return command

    def set(self, target, value, seconds=0.3, **kw):
        command = self.tool("ui_set_value", target=target, value=value, **kw)
        self.wait(command, seconds)
        return command

    def key(self, target, key):
        self.wait(self.tool("ui_input", target=target, action="key", key=key), 0.4)

    def ui(self, **kw):
        return self.tool("get_ui", **kw)

    def item(self, target, **kw):
        return self.tool("get_ui_item", target=target, **kw)

    def find(self, text, cls=None):
        """The widget or action that shows exactly this text."""
        offset = 0
        while True:
            r = self.ui(query=text, includeHidden=True, offset=offset, limit=200)
            for it in r["items"]:
                if str(it.get("text", "")).replace("&", "") == text and (not cls or it["class"] == cls):
                    return it["target"]
            if not r.get("nextOffset") or not r["items"]:
                raise KeyError(text)
            offset = r["nextOffset"]

    def modal(self):
        return self.ui(limit=1).get("activeModal")

    def answer(self, *texts):
        """Presses the button of the open message box that shows one of the texts, if there is one."""
        modal = self.modal()
        if not modal:
            return False
        offset, buttons = 0, []
        while True:
            r = self.ui(offset=offset, limit=300)
            buttons += [it for it in r["items"] if it.get("window") == modal and it["class"] == "QPushButton"]
            if not r.get("nextOffset") or not r["items"]:
                break
            offset = r["nextOffset"]
        button = next((b for b in buttons if b["text"].replace("&", "") in texts), None)
        if button is None:  # Another dialog, such as a file dialog.
            return False
        self.activate(button["target"], seconds=0.5)
        return True

    def idle(self, timeout=120):
        """Waits for calculations, cleanup, result loading and history scans to finish."""
        state = {}
        for _ in range(int(timeout / 0.2)):
            state = self.tool("get_app_state")
            if not (state["calculation"]["active"] or state["chemistryRunning"] or state["results"]["loading"]
                    or state["history"]["scanning"]):
                break
            time.sleep(0.2)
        return state

    # Documents
    def edit(self, name, **args):
        return self.tool(name, preview=False, **args)

    def new(self):
        self.activate("newAction", seconds=0.5)
        self.answer(*DISCARD)

    def open(self, path):
        """Opens a file over MCP (undoable, the window keeps its title)."""
        self.tool("open_file", path=path, preview=False)
        time.sleep(0.5)

    def open_dialog(self, path):
        """Opens a file as a person would, through File → Open."""
        self.activate("openAction", seconds=0.8)
        self.answer(*DISCARD)
        self.wait(self.tool("ui_file_dialog", target=self.modal(), path=path, accept=True), 1.0)
        self.answer(*DISCARD)  # Unsaved changes are asked about after the file is chosen.
        self.answer("OK")  # What to read of a PDB or mmCIF file: all of it, as the dialog proposes.
        time.sleep(0.5)
        self.idle()

    def setup_execution(self, ranks=4):
        self.edit("set_execution", settings={"executable": "nwchem", "launcher": "mpirun", "mode": "mpi",
                                             "mpi_ranks": ranks, "job_root": APP_JOBS})

    def fresh(self, sample=None, ranks=4):
        """A new molecule (or a practice file), shown on the first tab and page."""
        self.new()
        if sample:
            self.open(sample if sample.startswith("/") else SAMPLES + sample)
        self.setup_execution(ranks)
        self.select("workspaceTabs", index=0, seconds=0.2)
        self.select("propertyTabs", index=0, seconds=0.2)

    def atoms(self, candidate=False):
        d = self.tool("get_document", include=["project", "candidate"])
        doc = (d["candidate"] if candidate and d.get("candidate") else d["project"])["document"]
        return doc["atoms"], doc["bonds"]

    def ids(self, *labels):
        """Atom IDs by the labels the 3D view shows (C1, O8, …)."""
        atoms, _ = self.atoms()
        by = {f"{a['element']}{i + 1}": a["id"] for i, a in enumerate(atoms)}
        return [by[name] for name in labels]

    def select_atoms(self, ids):
        self.tool("set_selection", ids=ids)
        time.sleep(0.3)

    def tab(self, index):
        self.select("workspaceTabs", index=index, seconds=0.6)

    def tab_text(self, widget, text):
        tabs = self.item(widget)["tabs"]
        self.select(widget, index=next(t["index"] for t in tabs if t["text"].startswith(text)), seconds=0.6)

    # The 3D view and the palette
    def view_positions(self):
        return {i["label"]: (i["x"], i["y"]) for i in self.item("moleculeView", limit=500)["items"]}

    def click_view(self, x, y):
        self.wait(self.tool("ui_input", target="moleculeView", action="click", x=x, y=y), 0.5)

    def click_atom(self, label):
        self.click_view(*self.view_positions()[label])

    def click_bond(self, a, b):
        p = self.view_positions()
        self.click_view((p[a][0] + p[b][0]) / 2, (p[a][1] + p[b][1]) / 2)

    def click_empty(self, x=200, y=300):
        self.click_view(x, y)

    def palette_row(self, template):
        palette = self.item("templatePalette", limit=200)
        return palette, next(r for r in palette["items"] if r["cells"][0]["data"] == template)

    def tile(self, template):
        """Clicks a part of the palette, as a person would (the "Place" tool takes it)."""
        _, row = self.palette_row(template)
        self.select("templatePalette", index=row["row"], seconds=0.3)  # Scrolls it into view.
        palette, row = self.palette_row(template)
        x, y, w, h = row["cells"][0]["geometry"]
        self.wait(self.tool("ui_input", target=palette["viewport"], action="click", x=x + w / 2, y=y + h / 2), 0.4)

    def highlight(self, template):
        """Marks a palette item without switching the tool."""
        _, row = self.palette_row(template)
        self.select("templatePalette", index=row["row"], seconds=0.3)

    def undo(self):
        self.activate("undoAction", seconds=0.4)

    def confirm(self):
        self.activate("candidateBarConfirm", seconds=0.6)

    def refine(self):
        """"Clean up all", waiting for its candidate."""
        self.activate("refineButton", seconds=0.5)
        self.idle()
        time.sleep(0.3)

    def shot(self, path, target="mainWindow"):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        time.sleep(0.4)
        return self.tool("capture_screenshot", target=target, path=str(path), overwrite=True)
