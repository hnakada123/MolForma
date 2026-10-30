# MCP server

The native MolForma executable has a built-in MCP server for molecule editing, NWChem input generation and runs, calculation history, result display and GUI operation. No additional Python/Node.js runtime or MCP SDK installation is needed.

It supports the latest stable version **2026-07-28**, checked on 2026-10-24, and the earlier **2025-11-25**. The new specification skips initialization and specifies protocol information in each request. In the new specification, the results of `server/discover`, `tools/list`, `resources/list`, `resources/templates/list` and `resources/read` carry the required cache information `ttlMs: 0` and `cacheScope: "private"`. Because the document also changes through GUI operations, clients are not allowed to cache it. For the legacy `initialize`, the server returns `2025-11-25`. Update clients that support only older versions.

Besides tools and resources, the server provides the following features.

- **Prompts**: routine task procedures through `prompts/list` / `prompts/get` ([Prompts](#prompts)).
- **Elicitation (MRTR)**: `save_file` to an existing file asks the user whether to overwrite it (new specification only).
- **Tasks extension** (`io.modelcontextprotocol/tasks`): tracks GUI operations, and the calculations, cleanups, loads and history scans they start, as standard tasks and stops them with `tasks/cancel`. In headless mode, calculations and result loading are covered ([Tasks extension](#tasks-extension)).
- **Resources**: exposes the document (JSON, XYZ, NWChem input), the app state, calculation output, and the result summary and log. In Claude Code, you can attach them to the conversation with `@` ([Resources](#resources)).
- **Calculations and results in headless mode**: `--mcp` can also run NWChem and load results.
- **Safety restrictions**: MCP cannot change the NWChem executable, launcher or environment variables. Launch arguments can also restrict where files are read and written ([Safety restrictions](#safety-restrictions)).
- Every tool has a display name `title` and an output schema `outputSchema`. The server validates the returned `structuredContent` against this schema before returning it.

`ping`, removed in 2026-07-28, returns `-32601` for requests of the new specification. Legacy connections can still use it.

Official specifications referenced:

- [Versioning and compatibility](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning)
- [Base protocol and request metadata](https://modelcontextprotocol.io/specification/2026-07-28/basic)
- [stdio transport](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio)
- [Tools](https://modelcontextprotocol.io/specification/2026-07-28/server/tools)
- [Caching](https://modelcontextprotocol.io/specification/2026-07-28/server/utilities/caching)
- [Prompts](https://modelcontextprotocol.io/specification/2026-07-28/server/prompts)
- [Multi Round-Trip Requests](https://modelcontextprotocol.io/specification/2026-07-28/basic/patterns/mrtr)
- [Elicitation](https://modelcontextprotocol.io/specification/2026-07-28/client/elicitation)
- [Tasks extension 2026-07-28](https://github.com/modelcontextprotocol/ext-tasks/blob/main/specification/2026-07-28/tasks.md)
- [Official 2026-07-28 schema](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2026-07-28/schema.ts)

## Connection setup

In the server configuration of your MCP client, specify the absolute path of the executable and its arguments. For clients that use the `mcpServers` format, configure it as follows.

```json
{
  "mcpServers": {
    "molforma": {
      "command": "/absolute/path/to/molforma",
      "args": ["--mcp-gui"]
    }
  }
}
```

With `--mcp-gui`, the client starts a new MolForma window and operates on that window's document. It does not connect to another window that is already running. Showing the GUI needs a normal desktop environment. You can also specify a file to open at startup.

```json
"args": ["--mcp-gui", "/absolute/path/to/molecule.molforma.json"]
```

For automated processing that needs no window, specify `"args": ["--mcp"]`. This mode starts with an empty document and needs neither DISPLAY nor Qt's display plugins. Both modes share 23 document and settings tools. `--mcp` adds 4 calculation and result tools, 27 in total. `--mcp-gui` adds 17 GUI, run and result tools, including the 4 tools of the same names, 40 in total. To test GUI features automatically without a display, you can set `QT_QPA_PLATFORM=offscreen` and `MOLFORMA_RENDERER=painter` for `--mcp-gui`.

Tool names, descriptions and schemas are in English. Messages in results (errors, warnings, check results, etc.) and the GUI follow MolForma's interface language (“Language” in Preferences; if not set, the system language). Setting `MOLFORMA_LANGUAGE` (`ja` / `en`) in the environment variables of the server configuration fixes that language.

Communication is newline-delimited UTF-8 JSON-RPC over standard input and standard output. Only protocol messages are written to standard output; diagnostics go to standard error. Closing standard input ends the process, including in GUI mode. **Save the documents you need before disconnecting.** There is no save dialog or autosave when the connection ends.

MCP is disabled in a normal launch. No network port is opened. Calls that the MCP client permits read and write local files with the permissions of the MolForma process it started. File operation paths are given explicitly in each call. For how to limit where files are read and written, and how the programs that are run are protected, see [Safety restrictions](#safety-restrictions).

### Registering in Claude Code

Connection and tool calls with the new specification (2026-07-28) are confirmed with Claude Code 2.1.310. Claude Code connects with `server/discover`, so it does not use the compatibility mode.

For use only in your own environment, register with `claude mcp add`. Everything after `--` is the MolForma launch command.

```sh
claude mcp add molforma -- /absolute/path/to/molforma --mcp-gui
```

The default registration scope is `local`, which applies only to the current project. To use it in all projects, add `--scope user`. Both store the configuration in `~/.claude.json`.

To share the configuration with a team, add `--scope project`. The configuration is stored in `.mcp.json` at the repository root. If the executable's location differs between users, use environment variable expansion instead of writing an absolute path.

```json
{
  "mcpServers": {
    "molforma": {
      "command": "${MOLFORMA_BIN:-molforma}",
      "args": ["--mcp-gui"]
    }
  }
}
```

`${MOLFORMA_BIN:-molforma}` uses the value of the environment variable `MOLFORMA_BIN` if it is set, otherwise `molforma` on `PATH`. The first time a project's `.mcp.json` is used in an interactive session, Claude Code asks you to approve it. To redo the approval, run `claude mcp reset-project-choices`.

Claude Code passes its startup environment variables unchanged to child processes, so additional settings such as `DISPLAY` are normally not needed, even on Linux. If you use `--mcp-gui` from Claude Code started in an environment without a display, MolForma exits right after starting because it has no display to show on. In that case, specify `--mcp`.

Use the following to check the connection and investigate problems.

- In Claude Code, open `/mcp` and check that `molforma` is connected and the number of tools (40 for `--mcp-gui`, 27 for `--mcp`).
- In a terminal, run `claude mcp list` or `claude mcp get molforma`. If the connection fails, the `Issue:` line shows the cause.
- If the GUI takes long to start and waiting for the connection times out, extend the startup timeout (milliseconds), as in `MCP_TIMEOUT=30000 claude`.
- After updating MolForma, reconnect from `/mcp` or restart Claude Code.

Claude Code 2.1.310 behaves as follows (checked 2026-10-24).

- It presents MolForma's prompts as commands such as `/molforma:review_candidate (MCP)`. Arguments are separated by spaces and passed in definition order, as in `/mcp__molforma__geometry_optimization dft 6-31g* yes`. Arguments containing spaces cannot be passed.
- You can attach [resources](#resources) to the conversation, as in `@molforma:molforma://app/state`.
- It declares Elicitation in `tools/call`, so the overwrite confirmation of `save_file` is shown to the user. `claude -p`, which cannot show the confirmation, treats it as a cancellation, and the file is not overwritten.
- In interactive sessions, it declares the Tasks extension in `tools/call` (v2.1.304 and later). GUI operations, calculations and result loading become tasks, which Claude Code tracks until they complete. When the user cancels, `tasks/cancel` is sent and MolForma stops the work. The one-shot `claude -p "<instructions>"` does not use Tasks, so check completion with `commandId` and `get_app_state`. `claude -p` with stream-json input and `CLAUDE_AUTO_BACKGROUND_TASKS=1` uses Tasks.
- With `--mcp-allow-execution-settings`, `set_execution` and `calculation` carry `_meta["anthropic/requiresUserInteraction"]`. Claude Code then asks the user on every call, even in auto mode or with permission settings, and `claude -p`, which cannot ask, does not run them. Without this option, MolForma limits the programs it runs to the user's settings, so this mark is not added.

Claude Code saves a tool result to a file instead of putting it directly into the conversation when a single result exceeds 25,000 tokens by default (changeable with the environment variable `MAX_MCP_OUTPUT_TOKENS`) or 50,000 characters. MolForma keeps edit results to a summary of the changes and splits GUI lists, tables and results into pages of about 40 KiB, so results normally stay within this limit. `get_document`, `export_document` and `get_results` carry `_meta["anthropic/maxResultSizeChars"]`, so results up to 100,000 characters go directly into the conversation. For larger documents, fetch only the parts you need with `include` of `get_document`. For details, see [Claude Code's official MCP documentation](https://code.claude.com/docs/en/mcp).

### Registering in Codex

To show the GUI on Linux, also specify the environment variables that Codex passes to the child process. With only `codex mcp add molforma -- /absolute/path/to/molforma --mcp-gui`, the desktop connection information is not inherited and MolForma may exit before initialization.

Configure the server in `~/.codex/config.toml` as follows. If a `[mcp_servers.molforma]` section already exists, edit that section.

```toml
[mcp_servers.molforma]
command = "/absolute/path/to/molforma"
args = ["--mcp-gui"]
env_vars = ["DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS"]
```

`env_vars` does not fix the variables' values; it inherits them from the environment Codex was started in. Start Codex itself from the same user's desktop environment as well. In an environment without a display, use `args = ["--mcp"]`; `env_vars` is not needed. Codex normally connects with MCP 2025-11-25 in compatibility mode.

To apply the settings, reload Codex's MCP configuration or restart Codex, and check for `molforma: connected (38 tools)` (GUI mode) in `/mcp`. You also need to reconnect after updating the executable. You do not need to log in to your account again or set up authentication for MolForma. See also [Codex's official MCP documentation](https://learn.chatgpt.com/docs/extend/mcp?surface=cli).

If `failed (0 tools)` and `connection closed` during initialization appear, check the environment variables above first. On Linux, when no display is specified, MolForma also writes `requires a desktop display` and how to configure it to standard error. An invalid display or display access permission problems must also be resolved before the GUI can start.

## Safety restrictions

MCP clients (and the LLMs behind them) can be influenced by the content of files they read and of calculation results. MolForma applies the following restrictions on the server side. Both can be changed only with launch arguments, not by MCP requests. `policy` of `get_execution` shows the current restrictions.

### Protecting the executable, launcher, environment variables and additional input

Changing the NWChem executable (`executable`), the MPI launcher (`launcher`) or the additional environment variables (`environment`) can start arbitrary programs or load arbitrary code, for example through `LD_PRELOAD`. The additional input of the calculation settings (`extra_input` / `extra_tasks` of `set_nwchem`) is added to the NWChem input without checks by MolForma, so it is treated the same way (`task shell` and `python` blocks are always refused). By default, MCP cannot change these.

- `set_execution` fails if it sets the executable, launcher or environment variables, and `set_nwchem` fails if it sets the additional input, to values different from the current ones. Other run settings and calculation settings, such as the number of MPI processes, threads, the job folder and the time limit, can be changed.
- In GUI mode, MCP cannot operate the executable and launcher fields and their browse buttons in the “3 Run settings” tab, the environment variable table and its add and remove buttons, or the additional input fields and the “Apply additional input” button in “2 Calculation”. `blocked` of `get_ui` gives the reason.
- Loading a project, a history entry or an autosave copy also changes these values, so they are checked again when a calculation starts. For each setting, MolForma treats as “trusted” the value saved in “Edit → Preferences” and the values the user entered in the window or accepted in the confirmation dialog before a run. Values read from a file (including a file given at startup or a project MCP saved), the history or an autosave copy are not trusted until accepted. Calculations started over MCP run only if all values are trusted. The values do not become trusted when the user applies “Confirm changes” to a candidate created by MCP, or undoes or redoes an edit. Changing one setting in the window does not make the other settings trusted.
- If the user presses “Run calculation” with settings that are not trusted, a confirmation dialog showing the settings appears, whether or not MCP is connected ([“3 Run settings”](features/execution.md)). MCP cannot read or operate this dialog, and it does not become an Elicitation. Choosing “Yes” makes those values trusted.
- In headless `--mcp`, only the values the user saved in the GUI under “Edit → Preferences…” are trusted (if not set, `nwchem` and `mpirun` on `PATH` and no additional environment variables), and the document starts with those values. MCP cannot operate the Preferences dialog or menu.
- To let MCP specify the executable and the other settings, add `--mcp-allow-execution-settings` to the launch arguments.
- With `--mcp-allow-execution-settings`, `set_execution` and `calculation` carry the mark that makes Claude Code ask the user on every call ([Registering in Claude Code](#registering-in-claude-code)).

### Restricting file locations

With `--mcp-allow-dir <directory>` in the launch arguments, MCP reads and writes only under that directory. The option can be given more than once. MolForma does not start if the directory does not exist. Without it, there is no restriction.

```json
"args": ["--mcp-gui", "--mcp-allow-dir", "/home/user/molforma-work"]
```

- The following are covered.
  - Document tools: `open_file`, `save_file`, `job_root` / `reference_directory` of `set_execution`, absolute paths in `set_advanced`
  - GUI tools: paths of `results_action` / `history_action` / `capture_screenshot` / `ui_file_dialog`, and the targets of reusing history entries, showing results and moving to the trash
  - The GUI operated with `ui_*`: the destination of “Save project” (Ctrl+S); the targets of “Reuse molecule and settings”, “Open in result viewer”, “Open folder” and “Move to trash” in the history; loading the list when the History tab is shown; loading the input and log when a row is selected
  - Calculations and input sets saved over MCP: job folder, reference directory, input movecs, scratch and permanent directories, output movecs
- Paths are checked after following symbolic links. Dangling links are refused. `~` and `~/` are treated as the home directory, as at run time. Paths containing `..` are refused (after a symbolic link, `..` leads to the parent of the link's target, not of the folder holding the link).
- When a result folder is opened, the file actually read (a symbolic link in the folder, or a visualization file recorded in `run.json`) is checked as well as the folder.
- In GUI mode, file dialogs can be operated only with `ui_file_dialog`, and the selected path is checked when the dialog is accepted. The widgets inside the dialog and the “Job folder” field of the history cannot be operated with `ui_*`.
- Not covered: files that are replaced between the check and the read or write, and files that NWChem itself reads besides the input (basis set libraries, etc.).

## Editing workflow

1. Get a `documentId` from `list_documents`.
2. Pass that ID to `get_document` and read the `revision`, the confirmed `project` and the unconfirmed `candidate`. Use the stable IDs in `project.document.atoms` for atom references. `include` set to `["project"]` / `["candidate"]` returns only that part; `[]` returns only the revision, formula, atom count and `hasCandidate`. If omitted, both are returned.
3. Pass `documentId` and `expectedRevision` to an edit tool. By default it only creates a candidate (`preview: true`).
4. Edit tools and `history` return a summary rather than the whole project. The summary consists of the new `revision`, the `formula` / `atomCount` of the confirmed document, the `formula` / `atomCount` of the candidate, and the added, removed and changed atom IDs (`addedIds` / `removedIds` / `changedIds`, up to 100 each; `idsTruncated: true` when there are more). Without a candidate, `candidate` is `null`. Check the candidate's content with `get_document` (`include: ["candidate"]`). Then pass the new `revision` as `expectedRevision` and confirm with `action: "confirm"` of `history`, or discard with `"cancel"`.
5. Get a string with `export_document`, or save the project with `save_file`.

In the GUI, candidates can also be handled with the usual “Confirm changes” and “Discard candidate”. With an explicit `preview: false`, the edit is confirmed directly after validation and added to the GUI's undo history. `undo` / `redo` of `history` use the same history. While a candidate exists, other edits and undo/redo are refused. Edits are also refused while a cleanup worker is running in the GUI.

`revision` is a version number that changes whenever the confirmed document or the candidate changes (edits, undo/redo, and candidates created, confirmed or cancelled; returning to earlier contents does not bring back an earlier revision). When GUI edits or the like change the revision, changes that specify an old revision fail. When that happens, fetch the document again and check the differences. Document IDs differ per process and become invalid after disconnecting. Even when several callers share the same process, each operation needs the ID.

Moving fixed atoms, invalid references, duplicate coordinates, invalid arguments and similar errors fail without changing the document. Partial edits also keep unknown envelope fields of the project. `open_file` (project) and `import_document(format: "project")` load the whole given project, and undo restores the previous project. Opening XYZ/MOL/SDF/Mol2/CML/PDB/mmCIF with `open_file` replaces only the structure, like “Import structure file…” in the coordinate table, and keeps the calculation and run settings. NWChem input (`.nw`) and Gaussian input (`.gjf` / `.com`) also set the charge and multiplicity, and NWChem input also imports the calculation settings it shares with MolForma (advanced settings and additional input are kept). Multi-frame XYZ needs `frame` (0-based).

## Available tools

| Tool | Purpose |
| --- | --- |
| `list_documents` / `get_document` | Get document IDs, revision, project and candidate. `include` sets what to return |
| `list_templates` / `place_template` | Search and place built-in parts |
| `add_atom` | Add an atom by element and coordinates in Å, with an optional bond partner and hydrogen completion |
| `set_positions` / `translate` | Set coordinates or translate by stable ID |
| `set_bond` / `delete_atoms` | Change or remove bonds; delete atoms and update references (the total charge follows the sum of formal charges and oxidation numbers when they matched) |
| `measure` | Distance, angle or dihedral of 2/3/4 atoms |
| `set_dimension` | Set the distance (Å), angle or dihedral (degrees) of 2/3/4 atoms. As in the GUI, moves the side of the later atom (for separate, unbonded molecules, the whole molecule of the later atom) |
| `set_atom_properties` | Change the element (adjust hydrogens with `hydrogens`), formal charge, unpaired electrons, isotope (mass number, 0 to clear) or mass (amu), fixed, ghost and atom name of atoms by stable ID |
| `import_document` | Import an XYZ string or the JSON string of a MolForma project (`.molforma.json`, version 1). With `append: true`, XYZ is added beside the current molecule |
| `open_file` | Import local XYZ/MOL/SDF/Mol2/CML/PDB/mmCIF/MolForma project files and NWChem or Gaussian input. `frame` selects the frame of multi-frame XYZ |
| `export_document` | XYZ of the confirmed document, MolForma project JSON, validated NWChem input string |
| `save_file` | Save the confirmed document as a MolForma project (`.molforma.json`). An existing file needs `overwrite: true` or the user's approval to overwrite |
| `get_nwchem` / `set_nwchem` | Get calculation settings, defaults, options (`choices`) and validation results, and change settings. Includes solvent (SMD, solvent name), relativistic effects, RI-J, properties, transition state search, IRC, constraints (`constraints`, atoms by stable ID), scans (`scan_atoms` etc.), BSSE and the follow-up single point. The additional input (`extra_input` / `extra_tasks`) cannot be changed. Only errors the change brings in refuse it; errors already there (no atoms yet, a multiplicity still to be set) do not block other settings |
| `get_advanced` / `set_advanced` | Get and change the types, options, applicability conditions and values of the advanced and visualization output settings. `null` resets an individual setting |
| `get_execution` / `set_execution` | Get and change the executable, MPI, number of threads, working locations, environment variables and time limit (errors in the calculation settings do not block execution settings; `set_advanced` likewise refuses only errors its change brings in) |
| `history` | Confirm or discard the candidate, undo/redo |

`set_atom_properties` uses the same validation as the coordinate table and atom properties panel. Formal charge is −99 to 20, unpaired electrons 0 to 8, and atom names at most 16 characters without control characters. Unknown isotope mass numbers change neither the candidate nor the committed molecule. Use `mass` for a custom mass.

The following 4 tools are added with `--mcp`. `--mcp-gui` has tools of the same names, which handle the same calculations and results as the GUI window.

| Tool | Purpose |
| --- | --- |
| `get_app_state` | Whether there is a candidate; calculation and result loading state |
| `calculation` | Run or stop NWChem, save the input set |
| `get_results` | Summary of the loaded results (`properties` including the calculation's energy `final_energy` (last task, reference structure of frequencies, BSSE-corrected), thermochemistry, dipole, optimization state, each point of scans and IRC, BSSE, solvation, per-atom Mulliken/ESP/RESP charges, NMR shielding and polarizability), structures, energies, orbitals (`number` is the per-spin orbital number, the same as in MOLDEN even if the log lists only some orbitals), frequencies (translations and rotations are `rigid`), excited states, optimization steps, displacements and log |
| `results_action` | Load, reload or cancel loading a result file or calculation folder |

Calculations and loading in headless mode run in the background, and `calculation` / `results_action` return immediately. Check completion with a task or `get_app_state`. Calculations stop when MolForma exits (standard input closed). Showing, playing and capturing results is available only in GUI mode.

The following tools are added with `--mcp-gui`.

| Tool | Purpose |
| --- | --- |
| `get_app_state` | State of the selection, candidate, cleanup, calculation, result loading and playback, and history scan |
| `get_ui` / `get_ui_item` | List widgets, menus, shortcuts and dialogs; get input values, options, tables, trees and logs. For the 3D views (`moleculeView`, `resultsView`) it returns each atom's stable ID, label (`C1` and so on), the position inside the widget for `ui_input` clicks and whether it is in view (`visible`), and the positions of coordination sites (`center`, `site`) and terminus handles (`residue`, `terminus`) |
| `get_ui_command` | Get the progress and failure reason of GUI operations |
| `ui_activate` | Trigger buttons, menu items and shortcuts |
| `ui_set_value` | Change text, numbers, checkboxes and sliders. Text is committed as edited, as if typed, so it also works for fields that apply only when changed (such as the job folder). `commit: false` skips committing the input |
| `ui_select` / `ui_edit_cell` | Select tabs, options, table and tree items; change editable cells |
| `ui_input` | Clicks, drags, wheel, keys and part drops inside MolForma (`templateId` is an ID from `list_templates`, `atom:<element symbol>` or `center:<element symbol>`. An unknown ID places nothing and opens the same warning dialog as other refused edits) |
| `ui_file_dialog` | Set the path and filter of file open and save dialogs; accept or cancel |
| `capture_screenshot` | Get or save a PNG of the MolForma window or a given widget |
| `set_selection` | Select atoms by stable ID. An empty array clears the selection |
| `chemistry` | Build from SMILES (replaces the structure), add a fragment from SMILES (`fragment`), clean up all or locally, regenerate 3D, conformer search (`conformers`; makes the most stable conformer the candidate, and makes no candidate if it equals the current structure), cancel. Results are reviewed as candidates |
| `calculation` | Save the input set, run or stop NWChem |
| `get_results` / `results_action` | Load results or cancel loading; get structures, energies, orbitals, frequencies, excited states, optimization steps, displacements and log; select, play, stop; save XYZ; `apply_structure`, which makes the shown structure a candidate for the document |
| `history_action` | Scan the history or cancel the scan; select, reuse and show the results of jobs; repair records (`repair`: marks jobs recorded as running on this PC whose process has ended as “Interrupted”); move to the trash; save the filtered list as CSV |

When a vibration animation is selected in the GUI file list, `get_results.source` is the path of an original step XYZ file. `results_action(action: "reload")` rereads the original XYZ files of that mode, including added or removed steps.

See the JSON Schema of `tools/list` for argument types and required fields. All tools return both JSON text in `content` and `structuredContent`. Screen captures also return MCP `image` content. Errors in arguments or edits return `isError: true`; invalid JSON-RPC and unknown tools return protocol errors.

## Resources

`resources/list` returns the following resources. `resources/read` returns the content at the time of reading. Resource subscriptions and update notifications are not provided.

| URI | Format | Content |
| --- | --- | --- |
| `molforma://documents/<documentId>` | JSON | The same confirmed document, candidate and revision as `get_document` |
| `molforma://documents/<documentId>/xyz` | XYZ | Structure of the confirmed document (Å) |
| `molforma://documents/<documentId>/nwchem` | Text | NWChem input generated from the confirmed document. If it cannot be generated, returns `-32603` with the reason |
| `molforma://app/state` | JSON | The same state as `get_app_state` |
| `molforma://calculation/output` | Text | End of `output.out` of the current or last calculation |
| `molforma://calculation/stderr` | Text | End of `stderr.err` (MPI and launcher messages) of the current or last calculation |
| `molforma://results/summary` | JSON | Summary of `get_results` |
| `molforma://results/log` | Text | End of the log of the loaded results |

Text resources contain at most the last 100,000 characters. If earlier text is omitted, a note is added at the start.

`save_file` is a separate operation with an explicit destination; it does not change the GUI's current save location or its unsaved indicator. It refuses to save while there is a candidate. `export_document` always returns the confirmed document.

When a file already exists at the destination:

- `overwrite: true` overwrites it. `overwrite: false` fails.
- If `overwrite` is omitted and the client supports Elicitation (form), the server returns `resultType: "input_required"` and asks the user whether to overwrite. The client resends the same call with the user's answer (`inputResponses.overwrite`) and the returned `requestState`. The file is overwritten only if the user approves; on decline or cancel, the call fails with `isError: true`.
- `requestState` is signed and expires after 10 minutes. It is bound to the destination path and the document revision, so if it has been tampered with, or is resent after the document changed, the server asks again.
- Clients without Elicitation and legacy connections still need `overwrite: true`, as before.

## Prompts

`prompts/list` returns the following prompts. `prompts/get` returns the task procedure, with the arguments filled in, as a single user message. All arguments are strings. An unknown prompt, a missing required argument or a non-string value gives `-32602`. In Claude Code, they are available as the slash commands `/mcp__molforma__<name>`.

| Name | Mode | Arguments | Content |
| --- | --- | --- | --- |
| `review_candidate` | Both | None | Summarizes the unconfirmed candidate and confirms or discards it after waiting for the user's decision |
| `geometry_optimization` | Both | `method`, `basis`, `frequencies` (`yes` also runs a frequency calculation) | Sets the calculation settings and runs a geometry optimization. With `--mcp`, it goes only as far as exporting the input file |
| `build_from_smiles` | `--mcp-gui` | `smiles` (required) | Creates a 3D candidate from SMILES, reviews it and then confirms it |
| `frequency_analysis` | `--mcp-gui` | `path` (output file or run folder) | Opens the results, reports imaginary frequencies, plays a mode and captures an image |

Prompts are instructions to the LLM. They do not change MolForma's state.

## GUI operation and completion checks

`query` of `get_ui` searches displayed text, class names and `objectName`. To get widgets on other tabs, use `includeHidden: true`. Hidden widgets, such as the coordinate table (`coordinateTable`) of “1 Molecule” or the fields of closed advanced settings (`advanced_<key>`), can also be fetched with `includeHidden: true` and operated with `ui_edit_cell` / `ui_set_value`. Switch the coordinate table display with `ui_select` on `moleculeLayout` (`view` / `both` / `table`). To open or fold the parts palette, `ui_activate` `paletteToggle` in the tool rail. For `target`, give a returned `ui-N` or a unique `objectName`. Closing a dialog invalidates the IDs of its widgets. If names are duplicated, use `ui-N`.

GUI changes require `documentId` and `expectedRevision` and immediately return a `commandId` in the `queued` state. **GUI changes have no `preview` argument.** They confirm or create a candidate at the same point as the corresponding GUI operation. Changes to the document and candidate are also checked right before execution. Clicking the next open site with the “Place” tool and a ligand confirms the previous ligand's candidate and makes the next one, as in the GUI (a person's click never confirms a candidate MCP made). After edits made with the document tools, the view fits the whole molecule again when it left the picture, as after GUI edits.

1. Call `get_ui_command(commandId)` and check for `queued` / `running` / `done` / `failed`.
2. If it stays `running`, inspect `activeModal` of `get_ui`. Read the text of the confirmation dialog and press the appropriate button explicitly with a separate `ui_activate`.
3. File dialogs can be operated with `ui_file_dialog`. GUI MCP uses Qt's file dialogs, so you do not need to automate the OS file picker separately. The confirmation for overwriting an existing file is shown as usual.
4. `done` means that the GUI call has returned. Check validation messages in the window with `get_ui_item`. Check the completion of cleanups, calculations, loading and history scans with `get_app_state`. An exit code of 0 alone does not guarantee convergence.

While a modal dialog is shown, operations within that dialog are accepted. Document edits in the background are refused. Up to 16 operations can be pending, and completion records are kept for the latest 128, including pending ones. Windows of other applications are not operated. Shell commands are not accepted, and the user decides the NWChem executable, launcher and environment variables, as described in [Safety restrictions](#safety-restrictions).

Lists, tables and results are fetched in pages with `offset` / `limit`. If `nextOffset` is present, use it as the next `offset`; stop when the count becomes 0. GUI lists, table rows and result rows are split into pages of about 40 KiB. An item larger than 40 KiB on its own (such as the structures of a large molecule) is returned alone on its page. An item larger than 2 MiB fails. `parentRows` of a table is a list of 0-based row numbers giving the tree parent. `get_ui_item` also returns the table's `viewport` and the screen coordinates of cells, so `ui_input` also works for list items that react to clicks. Tab, table and result indices are 0-based; atoms are specified by stable ID. `offset` / `limit` of logs count characters, and up to 65,536 characters can be fetched.

Each existing GUI operation is available through the following routes.

| GUI area | MCP route |
| --- | --- |
| Molecule editing, parts, peptides, coordination, stereochemistry, fragments, residues, point charges | Document tools and `get_ui` → `ui_*`. Dialogs follow the same steps |
| PDB MODEL/ALTLOC selection, diagnosis and repair, My parts, settings and presets | `ui_activate` the corresponding menu or button; contents with `ui_set_value` / `ui_select` |
| Coordinate table, fixed, ghost, mass, calculation settings and advanced settings | `ui_edit_cell` / `ui_set_value`, or the settings tools |
| Input preview, search, copy, save | Operate the tab and its buttons with `ui_*`; get the text with `get_ui_item` |
| NWChem runs and history | `calculation` / `history_action` and `get_app_state`. Confirmations for reuse and the trash are also handled over MCP |
| Vibrations and imaginary frequencies, structures, orbitals and CUBE, view direction, opacity, isovalue, PNG export | `get_results` / `results_action`; display settings with `ui_*`; images with `capture_screenshot` |
| Save location of the main window, unsaved indicator, confirmation on quit | `ui_activate` the File menu; required dialogs with `ui_*` |

This exposes features already implemented in MolForma; it does not add new calculation or display features. In builds where dependencies such as RDKit are disabled, the corresponding GUI widgets are disabled too.

### Tasks extension

If the client declares `io.modelcontextprotocol/tasks` in `clientCapabilities.extensions` of the `_meta` of `tools/call`, GUI change tools return a task with `resultType: "task"` instead of a `commandId`. Without the declaration, the `commandId` scheme above is used. In headless mode, `calculation` (`start`) and `results_action` (loading) return tasks. The server indicates support for this extension in `capabilities.extensions` of `server/discover`.

- `tasks/get` returns `working` while the GUI operation is waiting or running. When the operation returns, the task becomes `completed`, and `result` holds a tool result containing the same operation record as `get_ui_command`.
- For the following operations, the task stays `working` until the work that the operation started ends. At the end, `result.structuredContent.app` holds the corresponding `get_app_state` items.
  - `chemistry` (except `cancel`): until the cleanup worker ends
  - `calculation` (`start`): until NWChem ends or is stopped
  - `results_action` (`open_file` / `open_directory` / `reload`): until loading the results completes
  - `history_action` (`scan`): until the history scan completes
- An operation that failed in the GUI becomes `completed` with a tool result with `isError: true`. `failed` is not used, because it is a state only for JSON-RPC errors.
- If a confirmation dialog (`QMessageBox`) opens during execution and the client has declared Elicitation (form), the task returns `input_required` and `inputRequests`. Each request is an Elicitation with the dialog's text and the button names as options (`choice`). Sending `{"action": "accept", "content": {"choice": "<button name>"}}` with `tasks/update` presses that button. On decline or cancel, the dialog's cancel button (equivalent to Esc) is pressed.
- For clients that do not declare Elicitation, the task returns `working`, and `statusMessage` asks for a response through `get_ui` / `ui_activate`.
- `pollIntervalMs` is 500. `ttlMs` is `null` (no expiry), but only the latest 128 tasks are kept. `tasks/get` after the original operation record is gone gives `-32602`.
- `tasks/cancel` stops work that can still be stopped. A pending GUI operation is canceled without running. Cleanups, calculations, result loading and history scans that have started are stopped in the same way as by each tool's cancel operation. After stopping, `tasks/get` returns `cancelled`. A GUI call in progress (such as while a dialog is shown) and completed work are not stopped and return their actual results.
- Task status notifications (`notifications/tasks` via `subscriptions/listen`) are not provided, so poll with `tasks/get`.
- `tasks/*` from requests that do not declare the extension gives `-32021`.

### Example: vibration animation

The following are examples of tool names and `arguments` for `tools/call`. Before each change, replace `DOC` / `REV` with the document ID and current revision you fetched, and check that the returned operation ID has completed before moving on.

```json
{"name":"results_action","arguments":{"documentId":"DOC","expectedRevision":"REV","action":"open_file","path":"/absolute/path/to/output.out"}}
{"name":"get_results","arguments":{"documentId":"DOC","section":"modes","offset":0,"limit":100}}
{"name":"results_action","arguments":{"documentId":"DOC","expectedRevision":"REV","action":"play_mode","index":0,"amplitude":0.3}}
{"name":"capture_screenshot","arguments":{"documentId":"DOC","target":"mainWindow","path":"/absolute/path/to/mode.png"}}
{"name":"results_action","arguments":{"documentId":"DOC","expectedRevision":"REV","action":"stop"}}
```

After loading completes, rows with `frequency < 0` are imaginary frequencies (`imaginary: true`). Those below −10 cm⁻¹ have `significant: true`; smaller negative values are often numerical noise. `imaginaryModes` / `significantImaginaryModes` in the `counts` of the `summary` give their numbers. Rows without displacement data can be shown and selected but not played. Real (positive) frequencies can be played with the same `play_mode`. `mode_displacements` returns the reference structure and the displacements. Result and camera operations do not change the edited document.

To start from a calculation, first set the settings with `set_nwchem` / `set_advanced` / `set_execution`, then call `calculation(action: "start")`. The user sets the NWChem executable and similar settings in MolForma. `get_app_state.calculation.directory` is the saved run folder. After the run ends, open the results with `results_action(action: "open_directory", path: ...)`.

In both GUI and headless mode, `open_directory` prefers `output.out`; if it is absent, it opens the first supported result file in the listing. A directory containing only XYZ files is supported. Failed or cancelled loads keep the previous result.

## Message examples for the new specification

Without an initialization handshake, you can send the following single line to standard input.

```json
{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}
```

Tool calls also specify the same required metadata.

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}},"name":"list_documents","arguments":{}}}
```

Successful results return `resultType: "complete"`. An unsupported version gets `-32022` with the list of supported versions, and missing required metadata gets `-32602`. With the legacy method, send `initialize` and `notifications/initialized` before normal requests. Legacy responses do not get `resultType`.

## Scope and limitations

- stdio works on Linux, macOS and Windows. On Windows it reads and writes in separate threads. Give the path of `molforma.exe` as the server command (for example `claude mcp add molforma -- C:\Users\<user>\AppData\Local\Programs\MolForma\bin\molforma.exe --mcp-gui`).
- Streamable HTTP, OAuth, remote connections, Sampling, Roots, Logging, completion (`completion/complete`), subscriptions (`subscriptions/listen`) and MCP Apps are not provided and are not declared as capabilities.
- Elicitation uses only the form mode. It covers only the `save_file` overwrite confirmation and `QMessageBox` during Tasks. Other dialogs, such as file dialogs, are operated with `get_ui` → `ui_*`.
- The Tasks extension covers GUI change tools and, in headless mode, starting calculations and loading results. Document tools return their results synchronously.
- GUI operations, result display and playback, screen capture, building from SMILES and cleanup, and the calculation history are available only with `--mcp-gui`. The windowless `--mcp` provides 23 document and settings tools and 4 calculation and result tools.
- Reading is limited to 8 MiB per file (for gzip-compressed files, also after decompressing), receiving to 8 MiB per message, and documents to 10,000 atoms. A client that stops reading while more than 32 MiB of data waits to be sent is disconnected. JSON-RPC batches are not supported.
- PDB and mmCIF loading with `open_file` uses MODEL 1 and selects ALTLOC automatically. If you need to choose, load the file from the GUI's File menu and operate the dialog over MCP.
- Document tools are synchronous; GUI change tools return an operation ID. To cancel long operations, use the cancel operations of `chemistry` / `calculation` / `results_action` / `history_action`. `notifications/cancelled` for completed RPCs is ignored. The undo history in headless mode keeps the latest 100 steps.
- Result loading has the same limits as the existing viewer (128 MiB per file, 2,000 atoms and 2,000 structures, 200,000 atoms in total for a structure series). Screen captures are limited to 4,096 pixels in width and 8 MiB of PNG.
- Except for saved files, documents, candidates and history are kept in memory and are lost when the process ends.

## Tests

`ctest --test-dir build --output-on-failure -R '^mcp$'` tests communication with the new and legacy versions, rejection of revisions, candidates and undo/redo, fixed atoms and keeping the original state on failure, conflicts with the GUI, preservation of unknown envelope fields, file saving and NWChem input generation. It launches the executable as a child process and tests, over MCP only, communication while a dialog is shown, file dialogs, playing and stopping imaginary and real frequencies from a synthetic MOLDEN file, PNG capture, running and canceling a mock NWChem, history scanning, selection and CSV export, and creating candidates from SMILES. Numeric values, the coordinate table and selection operations, rejection of old revisions and destroyed widgets, page limits, UTF-8 and split messages, notifications and exit on EOF are also covered. It also tests `title` / `outputSchema` of all tools, Prompts, the `save_file` overwrite confirmation (approval, decline, tampering, revision change), answering dialogs through Tasks, and tracking until the mock NWChem ends or is stopped. Protection of the run settings (refusing changes over MCP and runs after loading a project, the user's confirmation dialog), allowed directories (symbolic links, file dialogs), cancellation with `tasks/cancel`, resources, and running a mock NWChem and loading its results in headless mode are tested too. The tests need no network or credentials.
