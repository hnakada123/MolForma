# Screenshots of the manual

These scripts take the screenshots of `docs/features` and `docs/recipes` (and their English versions) by
driving a real MolForma window over MCP: `molforma --mcp-gui` runs offscreen with the QPainter renderer
and a window of 1380×900, and the scripts click, type and open files through the `ui_*` tools as a person
would. They are development tools; MolForma itself does not depend on Python.

Requirements: a built `build/molforma`, Python 3 with NumPy, ImageMagick (`magick`), and for the NWChem
figures an `nwchem` and `mpirun` in `PATH`. By default MolForma runs in a user namespace (`unshare -rm`,
util-linux) whose `/home` holds a home of `user`, so that paths in the screenshots read
`/home/user/MolForma/jobs` and `~/MolForma/jobs`; this needs unprivileged user namespaces.

| Script | What it does |
| --- | --- |
| `features.py jobs` | Runs ethanol (with MOLDEN output) and dimethyl ether at B3LYP/6-31G*, optimization → frequencies, for the result viewer and history |
| `features.py run ja\|en` | The run panel during a calculation: p-nitroaniline, stopped (ja); acetaldehyde, finished (en) |
| `features.py shots ja\|en [page…]` | The other figures of the feature pages, of every page or the pages named (`molecule`, `molecule_pages`, `coordinates`, `calculation`, `advanced`, `execution`, `input_preview`, `results`, `history`, `rendering`, `app`, `ai`) |
| `recipes.py ja\|en [recipe…]` | The recipe figures retaken on 2026-10-27, in the views of the earlier screenshots (see below) |
| `publish.py features\|recipes ja\|en… [name…]` | Copies the shots into `docs/`, reduced to 256 colors like the other images, and lists the feature pages and images in `docs/manual.qrc` again |

A full run of the feature pages: `jobs`, `run ja`, `run en`, `shots ja`, `shots en`, then
`publish.py features ja en`; check the images before committing them. The figures of `app` show the guide
and manual, so build MolForma again after changing the pages. Preferences cannot be captured: they hold run
settings, which MCP may not operate.

Environment variables: `MOLFORMA_SHOTS_DIR` (work folder, default `build/screenshots`: shots in
`shots/<language>/<features|recipes>/`, the scratch home with the jobs in `home/`), `MOLFORMA_BINARY`
(default `build/molforma`), `MOLFORMA_SHOTS_NAMESPACE=0` (run MolForma directly; paths then show the work
folder), `TRACE=1` (print every MCP call to standard error).

## Views of the recipe figures

`recipes.py` rebuilds each state as the recipe does (the hydrogens a part replaces decide the atom labels
and the shape), then `viewfit.py` turns the 3D view so that the atoms land where the earlier screenshot shows
them: from the window pixels of a few atoms (`TARGETS`, read off the earlier screenshot) it fits the
orientation (the sliders are yaw, pitch and roll of Rz·Rx·Ry; a flat molecule is solved in its plane),
then matches the zoom (the wheel scales by 1.0015 per step) and the pan (right drag). Each figure prints the
largest remaining distance; a few pixels is a match. Most figures of the recipes used yaw 45 / pitch 20,
yaw 30 / pitch 20 or the front view.

Status bar messages last about five seconds, longer than fitting the view, so the last operation is undone
and done again (or its candidate made again) right before a shot. "Regenerate 3D" gives other coordinates
each time and is not repeated.

## Things to know

- `get_ui_command` reports completion in `state`; `ui_file_dialog` needs the dialog as `target`; a slider
  set to the value it already shows does not signal (after opening a file, the view is turned while the
  sliders still show 0).
- Unsaved changes are asked about after the file of File → Open is chosen; `Gui.open_dialog` answers both.
- Changing the bond order of two atoms also sets the bond type of the "Place" tool (`toolBondKind`).
