// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/mcp_gui.h"
#include "mcp/tools.h"
#include "ui/execution_panel.h"
#include "ui/history_panel.h"
#include "ui/main_window.h"
#include "ui/results_panel.h"
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QShortcut>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QTreeView>
#include <QValidator>
#include <QWheelEvent>
#include <cmath>
#include <type_traits>

namespace mf {
namespace {
QJsonObject type(const QString &name) {
    return {{"type", name}};
}
QJsonObject choice(const QStringList &values) {
    return {{"type", "string"}, {"enum", QJsonArray::fromStringList(values)}};
}
QJsonObject integer(int lo, int hi) {
    return {{"type", "integer"}, {"minimum", lo}, {"maximum", hi}};
}
QJsonObject array(const QJsonObject &item, int maximum = 10000) {
    return {{"type", "array"}, {"items", item}, {"maxItems", maximum}};
}
int index(int n, int size) {
    if (n < 0 || n >= size)
        fail("Index is outside the available range (indices are zero-based).");
    return n;
}
QString objectText(QObject *object) {
    if (auto *a = qobject_cast<QAction *>(object))
        return a->text();
    if (auto *b = qobject_cast<QAbstractButton *>(object))
        return b->text();
    if (auto *l = qobject_cast<QLabel *>(object))
        return l->text();
    if (auto *l = qobject_cast<QLineEdit *>(object))
        return l->echoMode() == QLineEdit::Normal ? l->text() : QString();
    if (auto *e = qobject_cast<QPlainTextEdit *>(object))
        return e->toPlainText();
    if (auto *e = qobject_cast<QTextEdit *>(object))
        return e->toPlainText();
    if (auto *g = qobject_cast<QGroupBox *>(object))
        return g->title();
    if (auto *w = qobject_cast<QWidget *>(object))
        return w->windowTitle();
    return {};
}
void writeOutput(const QString &path, const QByteArray &bytes, bool overwrite) {
    if (path.isEmpty())
        fail("Specify a file path.");
    if (overwrite) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            fail(file.errorString());
    } else {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            fail(file.errorString());
        if (file.write(bytes) != bytes.size() || !file.flush()) {
            const auto error = file.errorString();
            file.remove();
            fail(error);
        }
    }
}
bool belongsTo(QObject *object, QObject *root) {
    for (auto *p = object; p; p = p->parent())
        if (p == root)
            return true;
    return false;
}
QModelIndex modelParent(QAbstractItemView *view, const QJsonObject &args) {
    auto parent = view->rootIndex();
    for (const auto &row : args["parentRows"].toArray())
        parent = view->model()->index(index(row.toInt(), view->model()->rowCount(parent)), 0, parent);
    return parent;
}
Qt::KeyboardModifiers modifiers(const QJsonObject &args) {
    Qt::KeyboardModifiers result;
    for (const auto &v : args["modifiers"].toArray()) {
        const auto name = v.toString();
        if (name == "shift")
            result |= Qt::ShiftModifier;
        if (name == "ctrl")
            result |= Qt::ControlModifier;
        if (name == "alt")
            result |= Qt::AltModifier;
        if (name == "meta")
            result |= Qt::MetaModifier;
    }
    return result;
}
QString plainButtonText(const QAbstractButton *button) {
    return QString(button->text()).remove('&');
}
QJsonObject commandSchema() {
    return {
        {"type", "object"},
        {"properties", QJsonObject{{"commandId", type("string")},
                                   {"tool", type("string")},
                                   {"action", type("string")},
                                   {"state", choice({"queued", "running", "done", "failed", "cancelled"})},
                                   {"error", type("string")}}},
        {"required", QJsonArray{"commandId", "tool", "state"}}};
}
} // namespace

McpGui::McpGui(MainWindow &w, McpPolicy rules) : QObject(&w), window(w), policy(std::move(rules)) {
    window.setAccessPolicy(&policy);
}
McpGui::~McpGui() {
    window.setAccessPolicy(nullptr);
}
McpEditor McpGui::editor() {
    McpEditor e;
    e.document = [this] { return window.document(); };
    e.candidate = [this]() -> std::optional<Document> { return window.candidate(); };
    e.edit = [this](const QString &name, const auto &operation, bool preview) {
        if (QApplication::activeModalWidget())
            fail("Handle the active modal dialog before editing the document.");
        auto scope = window.remoteScope();
        window.edit(name, operation, preview);
        // Like the GUI's edits: the view follows only when part of the molecule left the picture
        // (an opened project, added fragments).
        window.canvas()->keepInView();
    };
    e.history = [this](const QString &action) {
        if (QApplication::activeModalWidget())
            fail("Handle the active modal dialog before changing editing history.");
        auto scope = window.remoteScope();
        window.remoteHistory(action);
        window.canvas()->keepInView();
    };
    e.additionalTools = [this] { return tools(); };
    e.callAdditionalTool = [this](const QString &name, const QJsonObject &args) { return call(name, args); };
    e.baseDirectory = [this] { return window.executionPanel()->baseDirectory(); };
    e.taskWork = [](const QString &, const QJsonObject &result) {
        return result["state"] == "queued" ? result["commandId"].toString() : QString();
    };
    e.taskState = [this](const QString &commandId) { return task(commandId); };
    e.cancelTask = [this](const QString &commandId) { return cancel(commandId); };
    e.additionalResources = [] { return runtimeResources(); };
    e.readResource = [this](const QString &uri) {
        return runtimeResource(uri, state(), window.resultsPanel()->result(),
                               window.executionPanel()->currentRun().plan());
    };
    e.pendingPrompt = [this] { return prompt(); };
    e.answerPrompt = [this](const QString &key, const QString &choice) { answer(key, choice); };
    return e;
}
QJsonArray McpGui::tools() const {
    QJsonArray result;
    auto add = [&](const QString &name, const QString &title, const QString &description,
                   QJsonObject properties = {}, QJsonArray required = {}, bool mutation = false,
                   bool readOnly = true) {
        QJsonObject output;
        if (mutation || name == "get_ui_command")
            output = commandSchema();
        if (name == "get_app_state")
            output = QJsonObject{{"type", "object"},
                                 {"required", QJsonArray{"selection", "candidate", "chemistryRunning",
                                                         "calculation", "results", "history"}}};
        result.append(McpToolSpec{.name = name,
                                  .title = title,
                                  .description = description,
                                  .properties = properties,
                                  .required = required,
                                  .mutation = mutation,
                                  .readOnly = readOnly,
                                  .output = output}
                          .definition());
    };
    const auto target = type("string"), boolean = type("boolean"), number = type("number");
    const auto value = QJsonObject{{"type", QJsonArray{"string", "number", "boolean"}}};
    const auto rows = array(integer(0, 1000000), 10000);
    add("get_app_state", "Read application state",
        "Read GUI selection, candidate, chemistry worker, calculation, viewer and history status.");
    add("get_ui", "List GUI controls",
        "Discover native widgets/actions including dialogs. Targets are returned opaque IDs or unique "
        "objectName. Paginate with offset/limit; includeHidden exposes controls on other tabs.",
        {{"includeHidden", boolean},
         {"offset", integer(0, 100000)},
         {"limit", integer(1, 2000)},
         {"query", type("string")}});
    add("get_ui_item", "Read GUI control",
        "Read a control's text, choices or table/tree rows. offset/limit page text characters or model rows. "
        "parentRows addresses a tree parent. For the 3D views (moleculeView, resultsView) rows are atoms "
        "with their widget x/y for ui_input clicks, plus coordination-site and terminus markers. Does not "
        "change selection.",
        {{"target", target},
         {"offset", integer(0, 10000000)},
         {"limit", integer(1, 65536)},
         {"parentRows", rows}},
        {"target"});
    add("get_ui_command", "Poll GUI command",
        "Poll a GUI command: queued/running/done/failed. A running command may be waiting for a modal "
        "dialog; inspect get_ui and answer explicitly. Background workers are polled with get_app_state.",
        {{"commandId", type("string")}}, {"commandId"});
    add("ui_activate", "Activate GUI control",
        "Activate a native button, menu action or shortcut. Returns immediately with commandId, including "
        "when a "
        "dialog opens. Disabled controls are rejected.",
        {{"target", target}}, {"target"}, true);
    add("ui_set_value", "Set GUI value",
        "Set a native text, numeric, slider or checkable control. Text edits commit by default; commit=false "
        "leaves an edit pending. Uses the same GUI signals and validation.",
        {{"target", target}, {"value", value}, {"commit", boolean}}, {"target", "value"}, true);
    add("ui_select", "Select in GUI",
        "Select tabs, combo entries, list/table/tree rows (zero-based). rows supports multiple selection; "
        "parentRows addresses tree children. expand optionally expands a tree row.",
        {{"target", target},
         {"index", integer(0, 1000000)},
         {"rows", rows},
         {"column", integer(0, 10000)},
         {"parentRows", rows},
         {"expand", boolean}},
        {"target"}, true);
    add("ui_edit_cell", "Edit GUI cell",
        "Edit an editable model/table cell or its check state. Coordinates and environment tables use their "
        "normal validation and Undo.",
        {{"target", target},
         {"row", integer(0, 1000000)},
         {"column", integer(0, 10000)},
         {"value", value},
         {"parentRows", rows}},
        {"target", "row", "column", "value"}, true);
    add("ui_input", "Send GUI input",
        "Send native Qt click/double_click/context/drag/wheel/key/drop_template input inside one widget. "
        "Coordinates are local pixels. key uses PortableText such as Return or Ctrl+A; text enters text. No "
        "OS automation is used.",
        {{"target", target},
         {"action", choice({"click", "double_click", "context", "drag", "wheel", "key", "drop_template"})},
         {"x", number},
         {"y", number},
         {"endX", number},
         {"endY", number},
         {"delta", integer(-12000, 12000)},
         {"button", choice({"left", "right", "middle"})},
         {"modifiers", array(choice({"shift", "ctrl", "alt", "meta"}), 4)},
         {"key", type("string")},
         {"text", type("string")},
         {"templateId", type("string")}},
        {"target", "action"}, true);
    add("ui_file_dialog", "Answer file dialog",
        "Choose a path/filter and accept or cancel a MolForma QFileDialog. Existing-file replacement still "
        "uses the GUI confirmation dialog; inspect get_ui. GUI MCP uses non-native dialogs.",
        {{"target", target}, {"path", type("string")}, {"filter", type("string")}, {"accept", boolean}},
        {"target", "accept"}, true);
    add("capture_screenshot", "Capture screenshot",
        "Return a PNG image of a native MolForma widget/window (including VTK). Optionally save to path; "
        "overwrite is opt-in. Does not capture other applications.",
        {{"target", target},
         {"path", type("string")},
         {"overwrite", boolean},
         {"maxWidth", integer(64, 4096)}},
        {}, false, false);
    add("set_selection", "Select atoms",
        "Select editor atoms by stable IDs, without changing geometry. Empty ids clears selection.",
        {{"ids", array(type("string"))}}, {"ids"}, true);
    add("chemistry", "SMILES and refinement",
        "Start cancellable SMILES/refine/regenerate/conformer-search work, or cancel it. smiles replaces the "
        "structure; fragment adds the SMILES molecule 3 Å beside the current one. moving limits refinement "
        "to "
        "specified stable atom IDs. conformers searches rotamers with the force field and proposes the "
        "lowest-energy one. Inspect candidate after completion; confirm with history.",
        {{"action", choice({"smiles", "fragment", "refine", "regenerate", "conformers", "cancel"})},
         {"smiles", type("string")},
         {"moving", array(type("string"))}},
        {"action"}, true);
    result.append(
        calculationTool(
            "Start/stop NWChem or save an immutable input bundle using the GUI execution settings. No shell "
            "command is accepted; the executable, launcher and environment are those the user set in "
            "MolForma. "
            "Poll get_app_state; exit zero alone does not establish convergence.",
            commandSchema(), policy.allowExecutionSettings)
            .definition());
    result.append(resultsTool().definition());
    add("results_action", "Control result viewer",
        "Open/cancel/reload results; select frame/mode/orbital; play/stop vibration or trajectory; export "
        "current geometry. indices are zero-based. These change only the viewer, except apply_structure: it "
        "proposes the displayed geometry (same atoms in the same order, fixed atoms unmoved) as a candidate "
        "for the document; confirm it with history. See get_results for load errors and status.",
        {{"action",
          choice({"open_file", "open_directory", "reload", "cancel", "select_frame", "select_mode",
                  "select_orbital", "play_mode", "play_frames", "stop", "save_xyz", "apply_structure"})},
         {"path", type("string")},
         {"index", integer(0, 1000000)},
         {"amplitude", QJsonObject{{"type", "number"}, {"minimum", .01}, {"maximum", 2}}},
         {"overwrite", boolean}},
        {"action"}, true);
    add("history_action", "Manage calculation history",
        "Scan/select/reuse/open/repair/trash saved calculations, cancel a scan, or export history CSV. "
        "Repair marks a record left running on this computer, whose process has ended, as interrupted. "
        "Reuse and trash retain the normal GUI confirmation dialogs. Poll get_app_state and get_ui.",
        {{"action",
          choice({"scan", "cancel", "select", "reuse", "open_results", "repair", "trash", "save_csv"})},
         {"path", type("string")},
         {"overwrite", boolean}},
        {"action"}, true);
    return result;
}

QString McpGui::handle(QObject *object) {
    if (!object)
        return {};
    for (auto it = objects.begin(); it != objects.end();) {
        if (it.value().isNull())
            it = objects.erase(it);
        else {
            if (it.value() == object)
                return it.key();
            ++it;
        }
    }
    const auto id = "ui-" + QString::number(++nextHandle);
    objects[id] = object;
    return id;
}
QList<QObject *> McpGui::discover(bool hidden) {
    QList<QObject *> result;
    QSet<QObject *> seen;
    std::function<void(QObject *)> visit = [&](QObject *o) {
        if (!o || seen.contains(o))
            return;
        seen.insert(o);
        if (auto *w = qobject_cast<QWidget *>(o)) {
            if (hidden || w->isVisible())
                result.append(o);
            for (auto *a : w->actions())
                visit(a);
        } else if (qobject_cast<QAction *>(o) || qobject_cast<QShortcut *>(o))
            result.append(o);
        for (auto *c : o->children())
            visit(c);
    };
    visit(&window);
    for (auto *w : QApplication::topLevelWidgets())
        if (belongsTo(w, &window))
            visit(w);
    return result;
}
QObject *McpGui::resolve(const QString &target) {
    if (target.isEmpty() || target == "mainWindow")
        return &window;
    if (objects.contains(target)) {
        if (!objects[target])
            fail("This UI target was destroyed; refresh get_ui.");
        return objects[target];
    }
    QObject *found = nullptr;
    for (auto *o : discover(true)) {
        if (o->objectName() != target)
            continue;
        if (found)
            fail("Ambiguous objectName; use the opaque target from get_ui.");
        found = o;
    }
    if (!found)
        fail("Unknown UI target; discover it with get_ui.");
    return found;
}
QJsonObject McpGui::describe(QObject *o) {
    QJsonObject item{{"target", handle(o)},
                     {"objectName", o->objectName()},
                     {"class", o->metaObject()->className()},
                     {"parent", handle(o->parent())},
                     {"text", objectText(o).left(4096)}};
    // A file dialog itself is answered with ui_file_dialog.
    if (const auto reason = blocked(o, qobject_cast<QFileDialog *>(o) ? "ui_file_dialog" : "");
        !reason.isEmpty())
        item["blocked"] = reason;
    if (auto *w = qobject_cast<QWidget *>(o)) {
        item["visible"] = w->isVisible();
        item["enabled"] = w->isEnabled();
        item["accessibleName"] = w->accessibleName();
        item["tooltip"] = w->toolTip().left(4096);
        item["geometry"] = QJsonArray{w->x(), w->y(), w->width(), w->height()};
        item["window"] = handle(w->window());
        if (auto *p = w->parentWidget())
            if (auto *f = qobject_cast<QFormLayout *>(p->layout()))
                if (auto *label = f->labelForField(w))
                    item["label"] = objectText(label);
    }
    if (auto *a = qobject_cast<QAction *>(o)) {
        item["enabled"] = a->isEnabled();
        item["visible"] = a->isVisible();
        item["checkable"] = a->isCheckable();
        item["value"] = a->isChecked();
        item["shortcut"] = a->shortcut().toString(QKeySequence::PortableText);
    }
    if (auto *s = qobject_cast<QShortcut *>(o)) {
        item["enabled"] = s->isEnabled();
        item["shortcut"] = s->key().toString(QKeySequence::PortableText);
    }
    if (auto *b = qobject_cast<QAbstractButton *>(o)) {
        item["checkable"] = b->isCheckable();
        item["value"] = b->isChecked();
    }
    if (auto *g = qobject_cast<QGroupBox *>(o)) {
        item["checkable"] = g->isCheckable();
        item["value"] = g->isChecked();
    }
    if (auto *s = qobject_cast<QAbstractSlider *>(o)) {
        item["value"] = s->value();
        item["minimum"] = s->minimum();
        item["maximum"] = s->maximum();
    }
    if (auto *s = qobject_cast<QSpinBox *>(o)) {
        item["value"] = s->value();
        item["minimum"] = s->minimum();
        item["maximum"] = s->maximum();
    }
    if (auto *s = qobject_cast<QDoubleSpinBox *>(o)) {
        item["value"] = s->value();
        item["minimum"] = s->minimum();
        item["maximum"] = s->maximum();
    }
    if (auto *l = qobject_cast<QLineEdit *>(o))
        item["readOnly"] = l->isReadOnly();
    if (auto *e = qobject_cast<QPlainTextEdit *>(o))
        item["readOnly"] = e->isReadOnly();
    if (auto *e = qobject_cast<QTextEdit *>(o))
        item["readOnly"] = e->isReadOnly();
    if (auto *c = qobject_cast<QComboBox *>(o)) {
        item["count"] = c->count();
        item["index"] = c->currentIndex();
        item["value"] = c->currentText();
    }
    if (auto *t = qobject_cast<QTabWidget *>(o)) {
        item["index"] = t->currentIndex();
        QJsonArray tabs;
        for (int i = 0; i < t->count(); ++i)
            tabs.append(QJsonObject{{"index", i},
                                    {"text", t->tabText(i)},
                                    {"enabled", t->isTabEnabled(i)},
                                    {"target", handle(t->widget(i))}});
        item["tabs"] = tabs;
    }
    if (auto *v = qobject_cast<QAbstractItemView *>(o); v && v->model()) {
        item["rows"] = v->model()->rowCount(v->rootIndex());
        item["columns"] = v->model()->columnCount(v->rootIndex());
        item["currentRow"] = v->currentIndex().row();
        item["currentColumn"] = v->currentIndex().column();
    }
    if (auto *d = qobject_cast<QDialog *>(o))
        item["modal"] = d->isModal();
    if (auto *f = qobject_cast<QFileDialog *>(o)) {
        item["directory"] = f->directory().absolutePath();
        item["files"] = QJsonArray::fromStringList(f->selectedFiles());
        item["filters"] = QJsonArray::fromStringList(f->nameFilters());
        item["save"] = f->acceptMode() == QFileDialog::AcceptSave;
    }
    return item;
}
QByteArray McpGui::documentState() const {
    return QJsonDocument(
               QJsonArray{window.document().project(),
                          window.candidate() ? QJsonValue(window.candidate()->project()) : QJsonValue()})
        .toJson(QJsonDocument::Compact);
}
QJsonObject McpGui::state() const {
    const auto &run = window.executionPanel()->currentRun();
    const auto *r = window.resultsPanel();
    const auto viewer = r->viewerState();
    return {{"selection", idsToJson(window.selection())},
            {"candidate", window.hasCandidate()},
            {"modified", window.modified()},
            {"projectPath", window.projectPath()},
            {"tab", window.currentPanelIndex()},
            {"chemistryRunning", window.chemistryRunning()},
            {"status", window.statusBar()->currentMessage()},
            {"calculation", calculationJson(run, window.executionPanel()->savedDirectory())},
            {"results",
             QJsonObject{{"loading", r->loading()},
                         {"source", r->result().source},
                         {"pendingPath", viewer.pendingPath},
                         {"message", r->message()},
                         {"playing", viewer.animation != ResultAnimation::None},
                         {"animation", viewer.animation != ResultAnimation::None
                                           ? (viewer.animation == ResultAnimation::Frames ? "frames" : "mode")
                                           : "none"},
                         {"frame", viewer.frame},
                         {"mode", viewer.mode},
                         {"amplitude", viewer.amplitude}}},
            {"history", QJsonObject{{"scanning", window.historyPanel()->scanning()},
                                    {"root", window.historyPanel()->rootDirectory()},
                                    {"count", window.historyPanel()->entryCount()},
                                    {"selected", window.historyPanel()->selectedDirectory()},
                                    {"message", window.historyPanel()->message()}}}};
}
QJsonObject McpGui::results(const QJsonObject &args) const {
    return resultsJson(window.resultsPanel()->result(), args, state()["results"].toObject());
}

QJsonObject McpGui::enqueue(const QString &name, const QJsonObject &args) {
    int active = 0;
    for (const auto &c : commands)
        active += c["state"] == "queued" || c["state"] == "running";
    if (active >= 16)
        fail("Too many pending UI commands. Finish dialogs or poll existing commands.");
    QJsonObject captured = args;
    if (args.contains("target")) {
        auto *object = resolve(args["target"].toString());
        if (const auto reason = blocked(object, name); !reason.isEmpty())
            fail(reason);
        captured["target"] = handle(object);
    }
    const auto id = "command-" + QString::number(++nextCommand);
    const auto base = documentState();
    commands[id] = QJsonObject{{"commandId", id}, {"tool", name}, {"state", "queued"}};
    if (args.contains("action"))
        commands[id]["action"] = args["action"];
    commandOrder.append(id);
    // Never evict an in-flight command while a nested modal event loop is running.
    while (commandOrder.size() > 128) {
        auto it = std::find_if(commandOrder.begin(), commandOrder.end(), [&](const QString &key) {
            const auto state = commands[key]["state"];
            return state == "done" || state == "failed" || state == "cancelled";
        });
        if (it == commandOrder.end())
            break;
        commands.remove(*it);
        commandOrder.erase(it);
    }
    QTimer::singleShot(0, this, [this, name, captured, id, base] {
        // tasks/cancel may have withdrawn the command before it ran.
        if (!commands.contains(id) || commands[id]["state"] != "queued")
            return;
        commands[id]["state"] = "running";
        try {
            if (base != documentState())
                fail("Document changed before the queued command ran; refresh and retry.");
            auto scope = window.remoteScope();
            commands[id]["result"] = execute(name, captured);
            commands[id]["state"] = "done";
        } catch (const std::exception &e) {
            commands[id]["state"] = "failed";
            commands[id]["error"] = QString::fromUtf8(e.what());
        }
    });
    return commands[id];
}
// A task follows its command and then the background work it starts, so a task for
// "calculation start" completes when NWChem stops, not when the button returns.
std::optional<QJsonObject> McpGui::task(const QString &commandId) const {
    if (!commands.contains(commandId))
        return std::nullopt;
    const auto command = commands[commandId];
    const auto state = command["state"].toString(), tool = command["tool"].toString(),
               action = command["action"].toString();
    if (state == "queued" || state == "running")
        return QJsonObject{{"state", state}, {"message", tool + " is " + state + " in the GUI."}};
    if (state == "cancelled")
        return QJsonObject{{"state", "cancelled"}, {"message", tool + " was cancelled before it ran."}};
    if (state == "failed")
        return QJsonObject{{"state", "failed"}, {"error", command["error"]}};
    const auto app = this->state();
    QString section;
    bool busy = false;
    if (tool == "chemistry" && action != "cancel") {
        section = "chemistryRunning";
        busy = app["chemistryRunning"].toBool();
    } else if (tool == "calculation" && action == "start") {
        section = "calculation";
        busy = app["calculation"].toObject()["active"].toBool();
    } else if (tool == "results_action" &&
               (action == "open_file" || action == "open_directory" || action == "reload")) {
        section = "results";
        busy = app["results"].toObject()["loading"].toBool();
    } else if (tool == "history_action" && action == "scan") {
        section = "history";
        busy = app["history"].toObject()["scanning"].toBool();
    }
    if (busy)
        return QJsonObject{{"state", "running"},
                           {"message", tool + " " + action + " is running in the background."}};
    auto result = command;
    if (!section.isEmpty())
        result["app"] = QJsonObject{{section, app[section]}, {"status", app["status"]}};
    return QJsonObject{{"state", "done"}, {"result", result}};
}
bool McpGui::cancel(const QString &commandId) {
    if (!commands.contains(commandId))
        return false;
    auto &command = commands[commandId];
    const auto state = command["state"].toString(), tool = command["tool"].toString(),
               action = command["action"].toString();
    if (state == "queued") {
        command["state"] = "cancelled";
        return true;
    }
    // A GUI call that is still running, for example inside a dialog, cannot be interrupted.
    if (state != "done")
        return false;
    auto scope = window.remoteScope();
    if (tool == "chemistry" && action != "cancel" && window.chemistryRunning()) {
        window.cancelWorker();
        return true;
    }
    if (tool == "calculation" && action == "start" && window.executionPanel()->running()) {
        window.executionPanel()->stopCalculation();
        return true;
    }
    if (tool == "results_action" &&
        (action == "open_file" || action == "open_directory" || action == "reload") &&
        window.resultsPanel()->loading()) {
        window.resultsPanel()->cancelLoad();
        return true;
    }
    if (tool == "history_action" && action == "scan" && window.historyPanel()->scanning()) {
        window.historyPanel()->cancelScan();
        return true;
    }
    return false;
}
QString McpGui::blocked(const QObject *object, const QString &tool) const {
    for (auto *o = object; o; o = o->parent()) {
        if (o->property("mcpProtected").toBool())
            return "This dialog must be answered by the user in MolForma; MCP cannot operate it.";
        if (!policy.allowExecutionSettings && o->property("mcpExecutionSetting").toBool())
            return "The NWChem executable, launcher and environment are set by the user in MolForma and "
                   "cannot be changed over MCP (start the server with --mcp-allow-execution-settings to "
                   "permit it).";
        if (policy.restricted()) {
            if (o->property("mcpPath").toBool())
                return "With --mcp-allow-dir, choose this location with history_action scan and a path.";
            if (tool != "ui_file_dialog" && qobject_cast<const QFileDialog *>(o))
                return "With --mcp-allow-dir, answer file dialogs with ui_file_dialog so the path is "
                       "checked.";
        }
    }
    return {};
}
void McpGui::checkPaths(const QString &name, const QJsonObject &args) const {
    if (policy.restricted() && args.contains("path") &&
        (name == "ui_file_dialog" || name == "capture_screenshot" || name == "results_action" ||
         name == "history_action"))
        policy.checkPath(args["path"].toString());
}
QJsonObject McpGui::prompt() {
    auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (!box || !belongsTo(box, &window) || !blocked(box, {}).isEmpty())
        return {};
    QJsonArray choices;
    for (auto *button : box->buttons())
        choices.append(plainButtonText(button));
    QString message = box->windowTitle();
    for (const auto &text : {box->text(), box->informativeText()})
        if (!text.isEmpty())
            message +=
                (message.isEmpty() ? "" : "\n") +
                (Qt::mightBeRichText(text) ? QTextDocumentFragment::fromHtml(text).toPlainText() : text);
    // Handles are never reused, so each dialog gets a key that is unique for the task's lifetime.
    return {{"key", "dialog-" + handle(box)}, {"message", message}, {"choices", choices}};
}
void McpGui::answer(const QString &key, const QString &choice) {
    QPointer box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (!box || prompt()["key"] != key)
        return;
    // Click after this RPC returns, so the dialog's nested event loop does not run inside it.
    QTimer::singleShot(0, box, [this, box, choice] {
        if (!box || !box->isVisible())
            return;
        auto scope = window.remoteScope();
        for (auto *button : box->buttons())
            if (!choice.isEmpty() && plainButtonText(button) == choice)
                return button->click();
        if (choice.isEmpty() && box->escapeButton())
            return box->escapeButton()->click();
        if (choice.isEmpty())
            box->reject();
    });
}
QJsonObject McpGui::call(const QString &name, const QJsonObject &args) {
    checkPaths(name, args);
    window.refreshStalePanels(); // Hidden tabs must show the current document before they are read.
    if (name == "get_app_state")
        return state();
    if (name == "get_results")
        return results(args);
    if (name == "get_ui_command") {
        const auto id = args["commandId"].toString();
        if (!commands.contains(id))
            fail("Unknown or expired commandId.");
        return commands[id];
    }
    if (name == "get_ui") {
        QJsonArray items;
        int total = 0;
        qsizetype bytes = 0;
        bool full = false;
        const auto query = args["query"].toString();
        const int offset = args["offset"].toInt(), limit = args["limit"].toInt(200);
        for (auto *o : discover(args["includeHidden"].toBool())) {
            if (!query.isEmpty() &&
                !(o->objectName() + ' ' + o->metaObject()->className() + ' ' + objectText(o))
                     .contains(query, Qt::CaseInsensitive))
                continue;
            if (!full && total >= offset && items.size() < limit)
                full = !appendPage(items, describe(o), bytes);
            ++total;
        }
        return {{"items", items},
                {"total", total},
                {"offset", offset},
                {"nextOffset", offset + items.size()},
                {"activeModal", belongsTo(QApplication::activeModalWidget(), &window)
                                    ? handle(QApplication::activeModalWidget())
                                    : QString()},
                {"activePopup", belongsTo(QApplication::activePopupWidget(), &window)
                                    ? handle(QApplication::activePopupWidget())
                                    : QString()}};
    }
    if (name == "get_ui_item") {
        auto *o = resolve(args["target"].toString());
        auto out = describe(o);
        const int offset = args["offset"].toInt(), limit = args["limit"].toInt(100);
        const auto text = objectText(o);
        out["text"] = text.mid(offset, args["limit"].toInt(4096));
        out["textLength"] = text.size();
        QJsonArray items;
        qsizetype bytes = 0;
        if (auto *c = qobject_cast<QComboBox *>(o)) {
            for (int i = offset; i < std::min(c->count(), offset + std::min(limit, 1000)); ++i)
                if (!appendPage(items,
                                QJsonObject{{"index", i},
                                            {"text", c->itemText(i)},
                                            {"data", QJsonValue::fromVariant(c->itemData(i))}},
                                bytes))
                    break;
        }
        if (auto *v = qobject_cast<QAbstractItemView *>(o); v && v->model()) {
            const auto parent = modelParent(v, args);
            auto *m = v->model();
            out["rows"] = m->rowCount(parent);
            for (int row = offset; row < std::min(m->rowCount(parent), offset + std::min(limit, 200));
                 ++row) {
                QJsonArray cells;
                for (int col = 0; col < std::min(m->columnCount(parent), 64); ++col) {
                    auto i = m->index(row, col, parent);
                    const auto check = i.data(Qt::CheckStateRole);
                    cells.append(QJsonObject{
                        {"column", col},
                        {"text", i.data().toString().left(4096)},
                        {"data", QJsonValue::fromVariant(i.data(Qt::UserRole))},
                        {"editable", bool(i.flags() & Qt::ItemIsEditable)},
                        {"enabled", bool(i.flags() & Qt::ItemIsEnabled)},
                        {"checkState", check.isValid() ? QJsonValue(check.toInt()) : QJsonValue()},
                        {"selected", v->selectionModel()->isSelected(i)},
                        {"children", m->rowCount(i)},
                        {"geometry", QJsonArray{v->visualRect(i).x(), v->visualRect(i).y(),
                                                v->visualRect(i).width(), v->visualRect(i).height()}}});
                }
                if (!appendPage(items, QJsonObject{{"row", row}, {"cells", cells}}, bytes))
                    break;
            }
            out["viewport"] = handle(v->viewport());
        }
        // The 3D views list their atoms with the widget positions ui_input clicks on, plus the
        // coordination-site and terminus markers.
        if (auto *view = qobject_cast<MoleculeView *>(o)) {
            view->renderNow(); // The markers come from painting.
            const auto &atoms = view->documentRef().atoms;
            out["rows"] = int(atoms.size());
            for (int i = offset; i < std::min(int(atoms.size()), offset + std::min(limit, 1000)); ++i) {
                const auto p = view->atomScreenPosition(atoms[i].id);
                if (!appendPage(items,
                                QJsonObject{{"row", i},
                                            {"id", atoms[i].id},
                                            {"label", atoms[i].element + QString::number(i + 1)},
                                            {"x", std::round(p.x() * 10) / 10},
                                            {"y", std::round(p.y() * 10) / 10},
                                            {"visible", view->rect().contains(p.toPoint()) &&
                                                            !view->hiddenAtoms().contains(atoms[i].id)}},
                                bytes))
                    break;
            }
            QJsonArray markers;
            for (const auto &m : view->markerList())
                markers.append(
                    QJsonObject{{"x", std::round(m.point.x() * 10) / 10},
                                {"y", std::round(m.point.y() * 10) / 10},
                                {m.site >= 0 ? "center" : "residue", m.owner},
                                {m.site >= 0 ? "site" : "terminus",
                                 m.site >= 0 ? QJsonValue(m.site + 1) : QJsonValue(m.n ? "N" : "C")}});
            out["markers"] = markers;
        }
        out["items"] = items;
        out["offset"] = offset;
        out["nextOffset"] = offset + items.size();
        return out;
    }
    if (name == "capture_screenshot") {
        auto *w = qobject_cast<QWidget *>(resolve(args["target"].toString("mainWindow")));
        if (!w || !w->isVisible())
            fail("Screenshot target must be a visible MolForma widget.");
        auto pixmap = w->grab();
        const int max = args["maxWidth"].toInt(1600);
        if (pixmap.width() > max || pixmap.height() > 4096)
            pixmap = pixmap.scaled(max, 4096, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!pixmap.save(&buffer, "PNG") || bytes.size() > 8 * 1024 * 1024)
            fail("Screenshot is empty or exceeds 8 MiB.");
        if (args.contains("path"))
            writeOutput(args["path"].toString(), bytes, args["overwrite"].toBool());
        return {{"width", pixmap.width()},
                {"height", pixmap.height()},
                {"path", args["path"]},
                {"_mcpImage", QJsonObject{{"type", "image"},
                                          {"mimeType", "image/png"},
                                          {"data", QString::fromLatin1(bytes.toBase64())}}}};
    }
    return enqueue(name, args);
}

QJsonObject McpGui::execute(const QString &name, const QJsonObject &args) {
    window.refreshStalePanels();
    auto *modal = QApplication::activeModalWidget();
    QObject *object = args.contains("target") ? resolve(args["target"].toString()) : nullptr;
    auto *widget = qobject_cast<QWidget *>(object);
    if (modal && (!object || !belongsTo(object, modal)))
        fail("A modal dialog is open. Handle it with get_ui/ui_* before changing the main window.");
    if (const auto reason = object ? blocked(object, name) : QString(); !reason.isEmpty())
        fail(reason);
    if (widget && !widget->isEnabled())
        fail("This control is disabled.");
    if (name == "ui_activate") {
        if (auto *b = qobject_cast<QAbstractButton *>(object))
            b->click();
        else if (auto *a = qobject_cast<QAction *>(object)) {
            if (!a->isEnabled() || a->isSeparator())
                fail("This action is disabled.");
            QMenu *visibleMenu = nullptr;
            for (auto *owner : a->associatedObjects())
                if (auto *menu = qobject_cast<QMenu *>(owner);
                    menu && menu->isVisible() && belongsTo(menu, &window))
                    visibleMenu = menu;
            if (visibleMenu) {
                // Native activation closes a popup/exec() loop as well as triggering its action.
                visibleMenu->setActiveAction(a);
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QApplication::sendEvent(visibleMenu, &enter);
            } else if (a->menu())
                a->menu()->popup(window.mapToGlobal(QPoint(20, 60)));
            else
                a->trigger();
        } else if (auto *s = qobject_cast<QShortcut *>(object)) {
            auto *parent = qobject_cast<QWidget *>(s->parent());
            if (!s->isEnabled() || (parent && !parent->isEnabled()))
                fail("This shortcut is disabled.");
            QMetaObject::invokeMethod(s, "activated", Qt::DirectConnection);
        } else
            fail("Target is not a button or action.");
    } else if (name == "ui_set_value") {
        QPointer<QObject> guard(object);
        const auto value = args["value"];
        const auto numeric = [&](double lo, double hi, bool integral = false) {
            if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < lo ||
                value.toDouble() > hi || (integral && std::floor(value.toDouble()) != value.toDouble()))
                fail("Numeric value is outside the control's range/type.");
            return value.toDouble();
        };
        if (auto *s = qobject_cast<QDoubleSpinBox *>(object))
            s->setValue(numeric(s->minimum(), s->maximum()));
        else if (auto *s = qobject_cast<QSpinBox *>(object))
            s->setValue(int(numeric(s->minimum(), s->maximum(), true)));
        else if (auto *s = qobject_cast<QAbstractSlider *>(object))
            s->setValue(int(numeric(s->minimum(), s->maximum(), true)));
        else if (auto *l = qobject_cast<QLineEdit *>(object)) {
            if (l->isReadOnly() || !value.isString())
                fail("Target requires editable text.");
            if (value.toString().size() > l->maxLength())
                fail("Text exceeds the control's maximum length.");
            if (l->validator()) {
                auto text = value.toString();
                int pos = text.size();
                if (l->validator()->validate(text, pos) != QValidator::Acceptable)
                    fail("Text rejected by the control's validator.");
            }
            l->setText(value.toString());
            l->setModified(true); // As typed: fields that apply only changed text see the edit.
            if (args["commit"].toBool(true))
                QMetaObject::invokeMethod(l, "editingFinished", Qt::DirectConnection);
        } else if (auto *e = qobject_cast<QPlainTextEdit *>(object)) {
            if (e->isReadOnly() || !value.isString())
                fail("Target requires editable text.");
            e->setPlainText(value.toString());
        } else if (auto *e = qobject_cast<QTextEdit *>(object)) {
            if (e->isReadOnly() || !value.isString())
                fail("Target requires editable text.");
            e->setPlainText(value.toString());
        } else if (auto *b = qobject_cast<QAbstractButton *>(object)) {
            if (!b->isCheckable() || !value.isBool())
                fail("Target requires a checkable boolean.");
            if (b->isChecked() != value.toBool())
                b->click();
        } else if (auto *g = qobject_cast<QGroupBox *>(object)) {
            if (!g->isCheckable() || !value.isBool())
                fail("Target requires a checkable boolean.");
            g->setChecked(value.toBool());
        } else if (auto *a = qobject_cast<QAction *>(object)) {
            if (!a->isEnabled() || !a->isCheckable() || !value.isBool())
                fail("Target requires an enabled checkable action.");
            if (a->isChecked() != value.toBool())
                a->trigger();
        } else
            fail("Unsupported value control; use ui_select or ui_edit_cell.");
        if (guard && qobject_cast<QAbstractSpinBox *>(guard.data()) && args["commit"].toBool(true))
            QMetaObject::invokeMethod(guard, "editingFinished", Qt::DirectConnection);
    } else if (name == "ui_select") {
        int n = args["index"].toInt(-1);
        if (auto *c = qobject_cast<QComboBox *>(object)) {
            index(n, c->count());
            if (!(c->model()->flags(c->model()->index(n, c->modelColumn(), c->rootModelIndex())) &
                  Qt::ItemIsEnabled))
                fail("This choice is disabled.");
            c->setCurrentIndex(n);
            QMetaObject::invokeMethod(c, "activated", Qt::DirectConnection, Q_ARG(int, n));
        } else if (auto *t = qobject_cast<QTabWidget *>(object)) {
            index(n, t->count());
            if (!t->isTabEnabled(n))
                fail("This tab is disabled.");
            t->setCurrentIndex(n);
        } else if (auto *v = qobject_cast<QAbstractItemView *>(object); v && v->model()) {
            const auto parent = modelParent(v, args);
            auto rows = args["rows"].toArray();
            if (rows.isEmpty() && n >= 0)
                rows.append(n);
            const int col = index(args["column"].toInt(), v->model()->columnCount(parent));
            QModelIndexList indices;
            for (const auto &row : rows) {
                auto i = v->model()->index(index(row.toInt(), v->model()->rowCount(parent)), col, parent);
                if (!(i.flags() & Qt::ItemIsEnabled) || !(i.flags() & Qt::ItemIsSelectable))
                    fail("This item cannot be selected.");
                indices.append(i);
            }
            if (indices.size() > 1 && v->selectionMode() == QAbstractItemView::SingleSelection)
                fail("This view only supports one selection.");
            v->selectionModel()->clearSelection();
            for (const auto &i : indices) {
                QItemSelectionModel::SelectionFlags flags = QItemSelectionModel::Select;
                if (v->selectionBehavior() == QAbstractItemView::SelectRows)
                    flags |= QItemSelectionModel::Rows;
                v->selectionModel()->select(i, flags);
                v->selectionModel()->setCurrentIndex(i, QItemSelectionModel::NoUpdate);
                v->scrollTo(i);
                if (auto *tree = qobject_cast<QTreeView *>(v); tree && args.contains("expand"))
                    tree->setExpanded(i, args["expand"].toBool());
            }
        } else
            fail("Target is not a selectable control.");
    } else if (name == "ui_edit_cell") {
        auto *v = qobject_cast<QAbstractItemView *>(object);
        if (!v || !v->model())
            fail("Target has no item model.");
        const auto parent = modelParent(v, args);
        auto *m = v->model();
        auto i = m->index(index(args["row"].toInt(), m->rowCount(parent)),
                          index(args["column"].toInt(), m->columnCount(parent)), parent);
        const auto value = args["value"];
        int role = value.isBool() ? Qt::CheckStateRole : Qt::EditRole;
        if (!(i.flags() & Qt::ItemIsEnabled) ||
            !(i.flags() & (value.isBool() ? Qt::ItemIsUserCheckable : Qt::ItemIsEditable)))
            fail("This cell is not editable.");
        if (!m->setData(i,
                        value.isBool() ? QVariant(value.toBool() ? Qt::Checked : Qt::Unchecked)
                                       : value.toVariant(),
                        role))
            fail("Cell edit was rejected.");
    } else if (name == "ui_file_dialog") {
        auto *d = qobject_cast<QFileDialog *>(object);
        if (!d)
            fail("Target is not a file dialog.");
        if (!args["accept"].toBool())
            d->reject();
        else {
            if (args.contains("filter")) {
                if (!d->nameFilters().contains(args["filter"].toString()))
                    fail("Unknown file filter.");
                d->selectNameFilter(args["filter"].toString());
            }
            if (args.contains("path")) {
                const QFileInfo file(args["path"].toString());
                d->setDirectory(file.absolutePath());
                d->selectFile(file.absoluteFilePath());
                // The visible QFileSystemModel may still be fetching the new directory.
                // Fill the native Qt filename field too, just as a typed absolute path.
                if (auto *filename = d->findChild<QLineEdit *>("fileNameEdit"))
                    filename->setText(file.absoluteFilePath());
            }
            if (policy.restricted()) {
                auto files = d->selectedFiles();
                if (files.isEmpty())
                    files << d->directory().absolutePath();
                for (const auto &file : files)
                    policy.checkPath(file);
            }
            QMetaObject::invokeMethod(d, "accept", Qt::DirectConnection);
        }
    } else if (name == "ui_input") {
        if (!widget || !widget->isVisible())
            fail("Input target must be a visible widget.");
        const auto action = args["action"].toString();
        auto mods = modifiers(args);
        QPointer<QWidget> receiver(widget);
        QPointF pos(args["x"].toDouble(widget->width() / 2.), args["y"].toDouble(widget->height() / 2.));
        if (!widget->rect().contains(pos.toPoint()))
            fail("Input position is outside this widget.");
        auto button = args["button"] == "right"    ? Qt::RightButton
                      : args["button"] == "middle" ? Qt::MiddleButton
                                                   : Qt::LeftButton;
        const auto mouse = [&](QEvent::Type kind, QPointF point, Qt::MouseButtons buttons) {
            if (!receiver)
                return;
            QMouseEvent event(kind, point, receiver->mapToGlobal(point.toPoint()),
                              kind == QEvent::MouseMove ? Qt::NoButton : button, buttons, mods);
            QApplication::sendEvent(receiver, &event);
        };
        if (action == "key") {
            widget->setFocus();
            int key = 0;
            if (args.contains("key")) {
                const auto seq = QKeySequence::fromString(args["key"].toString(), QKeySequence::PortableText);
                if (seq.count() != 1 || seq[0].key() == Qt::Key_unknown)
                    fail("Expected one portable key combination.");
                key = seq[0].key();
                mods |= seq[0].keyboardModifiers();
            }
            if (!key && args["text"].toString().isEmpty())
                fail("Specify key or text.");
            QKeyEvent press(QEvent::KeyPress, key, mods, args["text"].toString()),
                release(QEvent::KeyRelease, key, mods, args["text"].toString());
            QApplication::sendEvent(widget, &press);
            if (receiver)
                QApplication::sendEvent(receiver, &release);
        } else if (action == "wheel") {
            QWheelEvent event(pos, widget->mapToGlobal(pos.toPoint()), {}, QPoint(0, args["delta"].toInt()),
                              Qt::NoButton, mods, Qt::NoScrollPhase, false);
            QApplication::sendEvent(widget, &event);
        } else if (action == "context") {
            QContextMenuEvent event(QContextMenuEvent::Mouse, pos.toPoint(),
                                    widget->mapToGlobal(pos.toPoint()), mods);
            QApplication::sendEvent(widget, &event);
        } else if (action == "drop_template") {
            if (args["templateId"].toString().isEmpty())
                fail("Specify templateId.");
            QMimeData mime;
            mime.setData("application/x-molforma-template", args["templateId"].toString().toUtf8());
            // Older Qt6 versions only accept integer positions for drag-enter events.
            using DragPosition = std::conditional_t<
                std::is_constructible_v<QDragEnterEvent, QPointF, Qt::DropActions, const QMimeData *,
                                        Qt::MouseButtons, Qt::KeyboardModifiers>,
                QPointF, QPoint>;
            QDragEnterEvent enter(DragPosition(pos.toPoint()), Qt::CopyAction, &mime, button, mods);
            QApplication::sendEvent(widget, &enter);
            QDropEvent drop(pos, Qt::CopyAction, &mime, button, mods);
            if (receiver)
                QApplication::sendEvent(receiver, &drop);
        } else {
            QPointF end(args["endX"].toDouble(pos.x()), args["endY"].toDouble(pos.y()));
            if (action == "drag" && !widget->rect().contains(end.toPoint()))
                fail("Drag endpoint is outside this widget.");
            mouse(QEvent::MouseButtonPress, pos, button);
            if (action == "drag") {
                for (int i = 1; i <= 8; ++i)
                    mouse(QEvent::MouseMove, pos + (end - pos) * (i / 8.), button);
                pos = end;
            }
            mouse(QEvent::MouseButtonRelease, pos, Qt::NoButton);
            if (action == "double_click") {
                mouse(QEvent::MouseButtonDblClick, pos, button);
                mouse(QEvent::MouseButtonRelease, pos, Qt::NoButton);
            }
        }
    } else if (name == "set_selection") {
        const auto ids = idsFromJson(args["ids"]);
        for (const auto &id : ids)
            window.displayDocument().atom(id);
        window.setSelection(ids);
    } else if (name == "chemistry") {
        const auto action = args["action"].toString();
        if (action == "cancel")
            window.cancelWorker();
        else {
            const auto moving = idsFromJson(args["moving"]);
            for (const auto &id : moving)
                window.document().atom(id);
            if ((action == "smiles" || action == "fragment") && args["smiles"].toString().trimmed().isEmpty())
                fail("Specify SMILES.");
            window.runWorker(action, args["smiles"].toString(), moving);
        }
    } else if (name == "calculation") {
        const auto action = args["action"].toString();
        if (action == "start")
            window.executionPanel()->startCalculation();
        else if (action == "stop")
            window.executionPanel()->stopCalculation();
        else
            window.executionPanel()->saveBundle();
    } else if (name == "results_action") {
        auto *p = window.resultsPanel();
        const auto action = args["action"].toString();
        if (action == "open_file" || action == "open_directory") {
            if (args["path"].toString().isEmpty())
                fail("Specify path.");
            window.showPanel(p);
            if (action == "open_file")
                p->openFile(args["path"].toString());
            else
                p->openDirectory(args["path"].toString());
        } else if (action == "cancel")
            p->cancelLoad();
        else if (action == "stop")
            p->stopAnimation();
        else {
            if (p->loading())
                fail("Wait for the current result load or cancel it.");
            if (action == "reload")
                p->reloadResult();
            else if (action == "select_frame")
                p->selectFrame(args["index"].toInt(-1));
            else if (action == "select_mode" || action == "play_mode") {
                std::optional<double> amplitude;
                if (args.contains("amplitude"))
                    amplitude = args["amplitude"].toDouble();
                p->selectMode(args["index"].toInt(p->viewerState().mode), amplitude, action == "play_mode");
            } else if (action == "select_orbital")
                p->selectOrbital(args["index"].toInt(-1));
            else if (action == "play_frames")
                p->playTrajectory();
            else if (action == "apply_structure")
                p->applyStructure();
            else if (action == "save_xyz")
                writeOutput(args["path"].toString(), p->displayedGeometry().xyz().toUtf8(),
                            args["overwrite"].toBool());
        }
    } else if (name == "history_action") {
        auto *p = window.historyPanel();
        const auto action = args["action"].toString();
        if (action == "scan") {
            if (args["path"].toString().isEmpty())
                fail("Specify history root path.");
            p->setRoot(args["path"].toString());
        } else if (action == "cancel")
            p->cancelScan();
        else if (action == "select") {
            const auto path = QDir(args["path"].toString()).absolutePath();
            if (p->scanning())
                fail("Wait for the history scan.");
            if (!p->containsDirectory(path))
                fail("Directory is not in the current history.");
            p->selectDirectory(path);
        } else if (action == "save_csv") {
            writeOutput(args["path"].toString(), historyCsv(p->visibleEntries()).toUtf8(),
                        args["overwrite"].toBool());
        } else {
            if (p->selectedDirectory().isEmpty())
                fail("Select a history entry first.");
            policy.checkPath(p->selectedDirectory());
            if (action == "reuse")
                p->reuseSelected();
            else if (action == "open_results")
                p->openResult();
            else if (action == "repair")
                p->repairSelected();
            else if (action == "trash")
                p->trashSelected();
        }
    } else
        fail("Unknown GUI operation.");
    return state();
}
} // namespace mf
