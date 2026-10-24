# MCPサーバー

MolFormaのネイティブ実行ファイルに、分子編集、NWChemの入力生成・実行、計算履歴、結果表示とGUI操作用のMCPサーバーを内蔵しています。追加のPython/Node.jsランタイムやMCP SDKのインストールは不要です。

2026-10-24に確認した最新の安定版 **2026-07-28** と、従来の **2025-11-25** に対応します。新しい仕様は初期化を省略し、リクエストごとにプロトコル情報を指定します。新しい仕様の `server/discover`、`tools/list`、`resources/list`、`resources/templates/list`、`resources/read` の結果には、必須のキャッシュ情報 `ttlMs: 0` と `cacheScope: "private"` を付けます。文書はGUI操作でも変わるため、クライアントにキャッシュさせません。旧版の `initialize` に対しては `2025-11-25` を返します。それより古い版のみ対応するクライアントは更新してください。

ツールとリソースに加えて、次の機能を提供します。

- **Prompts**: 定型の作業手順を `prompts/list` / `prompts/get` で提供します（[Prompts](#prompts)）。
- **Elicitation（MRTR）**: 既存ファイルへの `save_file` で、上書きしてよいかを利用者に確認します（新しい仕様のみ）。
- **Tasks拡張**（`io.modelcontextprotocol/tasks`）: GUI操作と、それが開始した計算・整形・読込・履歴走査を、標準のタスクとして追跡し、`tasks/cancel` で止めます。画面なしモードでは計算と結果の読込が対象です（[Tasks拡張](#tasks拡張)）。
- **リソース**: 文書（JSON・XYZ・NWChem入力）、アプリの状態、計算出力、結果の要約とログを公開します。Claude Codeでは `@` で会話に添付できます（[リソース](#リソース)）。
- **画面なしモードの計算と結果**: `--mcp` でもNWChemの実行と結果の読込ができます。
- **安全のための制限**: MCPからはNWChemの実行ファイル・ランチャー・環境変数を変更できません。読み書きする場所も起動引数で制限できます（[安全のための制限](#安全のための制限)）。
- すべてのツールに表示名 `title` と出力の `outputSchema` を付けます。返す `structuredContent` は、返す前にサーバー側でこのスキーマに照合します。

2026-07-28で廃止された `ping` は、新しい仕様のリクエストでは `-32601` を返します。旧版の接続では引き続き使えます。

参照した公式仕様:

- [バージョンと互換性](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning)
- [基本プロトコルとリクエストメタデータ](https://modelcontextprotocol.io/specification/2026-07-28/basic)
- [stdioトランスポート](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio)
- [ツール](https://modelcontextprotocol.io/specification/2026-07-28/server/tools)
- [キャッシュ](https://modelcontextprotocol.io/specification/2026-07-28/server/utilities/caching)
- [プロンプト](https://modelcontextprotocol.io/specification/2026-07-28/server/prompts)
- [Multi Round-Trip Requests](https://modelcontextprotocol.io/specification/2026-07-28/basic/patterns/mrtr)
- [Elicitation](https://modelcontextprotocol.io/specification/2026-07-28/client/elicitation)
- [Tasks拡張 2026-07-28](https://github.com/modelcontextprotocol/ext-tasks/blob/main/specification/2026-07-28/tasks.md)
- [2026-07-28の公式スキーマ](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2026-07-28/schema.ts)

## 接続設定

MCPクライアントのサーバー設定で、実行ファイルの絶対パスと引数を指定します。`mcpServers` 形式を使うクライアントでは次のように設定します。

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

`--mcp-gui` はクライアントが新しいMolFormaウィンドウを起動し、そのウィンドウの文書を操作します。既に起動している別のウィンドウには接続しません。GUIの表示には通常のデスクトップ環境が必要です。起動時のファイルも指定できます。

```json
"args": ["--mcp-gui", "/absolute/path/to/molecule.kgui.json"]
```

画面が不要な自動処理では `"args": ["--mcp"]` を指定します。このモードは空の文書で始まり、DISPLAYやQtの画面用プラグインを必要としません。両モードは23個の文書・設定ツールを共有します。`--mcp` はさらに計算・結果用の4ツールを提供し、合計27個です。`--mcp-gui` は同名の4ツールを含む17個のGUI・実行・結果ツールを提供し、合計40個です。GUI機能を表示なしで自動検証する場合は `--mcp-gui` に `QT_QPA_PLATFORM=offscreen`、`MOLFORMA_RENDERER=painter` を設定できます。

通信は標準入力・標準出力の改行区切りUTF-8 JSON-RPCです。標準出力にはプロトコルメッセージだけを書き、診断は標準エラーへ出します。標準入力を閉じると、GUIモードを含めてプロセスが終了します。**必要な文書は切断前に保存してください。** 接続終了時の保存ダイアログや自動保存はありません。

通常の起動ではMCPは無効です。ネットワークポートは開きません。MCPクライアントが許可した呼び出しによって、起動したMolFormaプロセスの権限でローカルファイルを読み書きします。ファイル操作のパスは各呼び出しで明示的に指定します。読み書きする場所を限る方法と、実行するプログラムの保護は[安全のための制限](#安全のための制限)を参照してください。

### Claude Codeでの登録

Claude Code 2.1.310で、新しい仕様（2026-07-28）による接続とツールの呼び出しを確認しています。Claude Codeは `server/discover` で接続するため、互換モードは使いません。

自分の環境だけで使う場合は、`claude mcp add` で登録します。`--` より後ろがMolFormaの起動コマンドです。

```sh
claude mcp add molforma -- /absolute/path/to/molforma --mcp-gui
```

既定の登録範囲は `local` で、現在のプロジェクトだけで有効です。すべてのプロジェクトで使う場合は `--scope user` を付けます。どちらも設定は `~/.claude.json` に保存されます。

チームで設定を共有する場合は `--scope project` を付けます。設定はリポジトリ直下の `.mcp.json` に保存されます。利用者ごとに実行ファイルの場所が違う場合は、環境変数の展開を使い、絶対パスを書かないようにします。

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

`${MOLFORMA_BIN:-molforma}` は、環境変数 `MOLFORMA_BIN` が設定されていればその値、なければ `PATH` 上の `molforma` を使います。プロジェクトの `.mcp.json` を対話セッションで初めて使うときは、Claude Codeが利用の承認を求めます。承認をやり直す場合は `claude mcp reset-project-choices` を実行します。

Claude Codeは起動時の環境変数をそのまま子プロセスへ渡すため、Linuxでも `DISPLAY` などの追加設定は通常不要です。画面のない環境で起動したClaude Codeから `--mcp-gui` を使うと、表示先がないためMolFormaが起動直後に終了します。その場合は `--mcp` を指定してください。

接続の確認と問題の調査には次の方法を使います。

- Claude Code内で `/mcp` を開き、`molforma` が接続済みであることとツール数（`--mcp-gui` は40、`--mcp` は27）を確認します。
- 端末で `claude mcp list` または `claude mcp get molforma` を実行します。接続に失敗した場合は、`Issue:` 行に原因が表示されます。
- GUIの起動に時間がかかって接続待ちが時間切れになる場合は、`MCP_TIMEOUT=30000 claude` のように起動の待ち時間（ミリ秒）を延ばします。
- MolFormaを更新した後は、`/mcp` から再接続するか、Claude Codeを再起動します。

Claude Code 2.1.310の動作は次のとおりです（2026-10-24確認）。

- MolFormaのプロンプトを `/molforma:review_candidate (MCP)` のようなコマンドとして提示します。`/mcp__molforma__geometry_optimization dft 6-31g* yes` のように、引数を空白で区切って定義順に渡します。空白を含む引数は渡せません。
- `@molforma:molforma://app/state` のように、[リソース](#リソース)を会話に添付できます。
- `tools/call` でElicitationを宣言するため、`save_file` の上書き確認が利用者に表示されます。確認を表示できない `claude -p` では取消として扱われ、ファイルは上書きされません。
- 対話セッションでは `tools/call` でTasks拡張を宣言します（v2.1.304以降）。GUI操作と計算・結果の読込はタスクになり、Claude Codeが完了まで追跡します。利用者が取り消すと `tasks/cancel` が送られ、MolFormaは処理を止めます。一度だけ実行する `claude -p "<指示>"` ではTasksを使わないため、`commandId` と `get_app_state` で完了を確認します。stream-json入力で `CLAUDE_AUTO_BACKGROUND_TASKS=1` を指定した `claude -p` はTasksを使います。
- `--mcp-allow-execution-settings` を指定した場合、`set_execution` と `calculation` に `_meta["anthropic/requiresUserInteraction"]` を付けます。Claude Codeは auto モードや許可設定があっても呼び出しのたびに利用者に確認し、確認できない `claude -p` では実行しません。指定しない場合は、実行するプログラムをMolFormaが利用者の設定に限るため、この印は付けません。

Claude Codeは1回のツール結果が既定で25,000トークン（環境変数 `MAX_MCP_OUTPUT_TOKENS` で変更可能）、または50,000文字を超えると、結果を会話に直接入れずファイルへ保存します。MolFormaは編集結果を変更の要約にとどめ、GUI一覧・表・結果を約40 KiBで区切るため、通常はこの上限内に収まります。`get_document`、`export_document`、`get_results` には `_meta["anthropic/maxResultSizeChars"]` を付け、10万文字までは会話に直接入れます。それより大きな文書では `get_document` の `include` で必要な部分だけを取得してください。詳細は[Claude Code公式のMCP設定](https://code.claude.com/docs/en/mcp)を参照してください。

### Codexでの登録

LinuxでGUIを表示する場合、Codexが子プロセスへ渡す環境変数も指定します。`codex mcp add molforma -- /absolute/path/to/molforma --mcp-gui` だけでは、デスクトップの接続情報が引き継がれず、初期化前に終了する場合があります。

`~/.codex/config.toml` の該当サーバーを次のように設定してください。既存の `[mcp_servers.molforma]` があれば、そのセクションを編集します。

```toml
[mcp_servers.molforma]
command = "/absolute/path/to/molforma"
args = ["--mcp-gui"]
env_vars = ["DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS"]
```

`env_vars` は変数の値を固定せず、Codexの起動環境から引き継ぐ設定です。Codex自体も同じ利用者のデスクトップ環境から起動してください。画面のない環境では `args = ["--mcp"]` を使い、`env_vars` は不要です。通常は互換モードのMCP 2025-11-25で接続します。

設定を反映するためCodexのMCP設定を再読み込みするかCodexを再起動し、`/mcp` で `molforma: connected (38 tools)`（GUIモード）を確認します。実行ファイルを更新した場合も再接続が必要です。アカウントへの再ログインやMolForma用の認証は不要です。[Codex公式のMCP設定](https://learn.chatgpt.com/docs/extend/mcp?surface=cli) も参照してください。

`failed (0 tools)` と初期化時の `connection closed` が表示される場合は、上記の環境変数を最初に確認してください。Linuxで表示先が未指定のときは、MolFormaも標準エラーに `requires a desktop display` と設定方法を出します。無効な表示先や画面のアクセス権の問題も、GUIの起動前に解決する必要があります。

## 安全のための制限

MCPクライアント（とその背後のLLM）は、読み込んだファイルや計算結果の内容に影響されることがあります。MolFormaは次の制限をサーバー側で適用します。どちらも起動引数でだけ変更でき、MCPのリクエストからは変更できません。現在の制限は `get_execution` の `policy` で確認できます。

### 実行ファイル・ランチャー・環境変数・追加入力の保護

NWChem実行ファイル（`executable`）、MPIランチャー（`launcher`）、追加の環境変数（`environment`）を変えると、任意のプログラムを起動したり、`LD_PRELOAD` などで任意のコードを読み込ませたりできます。計算条件の追加入力（`set_nwchem` の `extra_input` / `extra_tasks`）はMolFormaが検査せずにNWChem入力へ加えるため、同じ扱いにしています（`task shell` と `python` ブロックは常に拒否します）。既定では、これらをMCPから変更できません。

- `set_execution` で実行ファイル・ランチャー・環境変数を、`set_nwchem` で追加入力を現在と異なる値にすると失敗します。MPI起動数・スレッド数・ジョブ保存先・時間制限など、その他の実行設定と計算条件は変更できます。
- GUIモードでは、「5 実行設定」タブの実行ファイル・ランチャーの欄と参照ボタン、環境変数の表と追加・削除ボタン、「3 計算条件」の追加入力の欄と「追加入力を適用」ボタンをMCPから操作できません。`get_ui` の `blocked` に理由を示します。
- プロジェクトの読込や履歴の再利用でも値は変わるため、計算の開始時にも確認します。MolFormaは、MCP以外の操作（利用者のGUI操作や、起動時に指定したファイル）で最後に設定された値を「信頼済み」として記録します。MCPから開始する計算は、信頼済みの値と一致する場合だけ実行します。MCPが作成した候補を利用者が「変更を確定」した場合や、MCPによる編集を利用者がUndo/Redoした場合も、その値は信頼済みになりません。
- 一致しない設定で利用者が「実行」を押すと、設定内容を示す確認ダイアログを表示します。このダイアログはMCPから取得・操作できず、Elicitationにもなりません。「はい」を選ぶと、その値が信頼済みになります。
- 画面なしの `--mcp` では、利用者がGUIの「編集 → 環境設定」で保存した値（未設定なら `PATH` 上の `nwchem` と `mpirun`、追加の環境変数なし）だけを信頼し、その値で文書を開始します。環境設定のダイアログとメニューはMCPから操作できません。
- MCPから実行ファイルなどを指定させる場合は、起動引数に `--mcp-allow-execution-settings` を加えます。
- `--mcp-allow-execution-settings` を指定した場合は、`set_execution` と `calculation` に、Claude Codeが呼び出しのたびに利用者に確認する印を付けます（[Claude Codeでの登録](#claude-codeでの登録)）。

### ファイルを扱う場所の制限

起動引数に `--mcp-allow-dir <ディレクトリ>` を指定すると、MCPが読み書きする場所をそのディレクトリ以下に限ります。複数回指定できます。存在しないディレクトリを指定すると起動しません。指定しない場合は制限しません。

```json
"args": ["--mcp-gui", "--mcp-allow-dir", "/home/user/molforma-work"]
```

- 対象は次のとおりです。
  - 文書ツール: `open_file`、`save_file`、`set_execution` の `job_root` / `reference_directory`、`set_advanced` の絶対パス
  - GUIツール: `results_action` / `history_action` / `capture_screenshot` / `ui_file_dialog` のパス、履歴の再利用・結果表示・ごみ箱の対象
  - `ui_*` で操作したGUI: 「プロジェクトを保存」（Ctrl+S）の保存先、履歴の「分子・条件を再利用」「結果ビューアで開く」「保存先を開く」「ごみ箱へ」の対象、履歴タブを表示したときの一覧の読込、行を選んだときの入力・ログの読込
  - MCPから開始する計算と入力一式の保存: ジョブ保存先、参照ディレクトリ、入力movecs、作業・永続ディレクトリ、出力movecs
- パスはシンボリックリンクをたどって判定します。リンク先のないリンクは拒否します。`~` と `~/` は実行時と同じくホームディレクトリとして判定します。
- GUIモードでは、ファイル選択ダイアログを `ui_file_dialog` でだけ操作でき、確定時に選択されたパスを確認します。ダイアログ内の部品や履歴の保存先欄を `ui_*` で操作することはできません。
- 判定後の読み書きまでの間にファイルを置き換えられる場合や、NWChem自体が入力以外に参照するファイル（基底ライブラリなど）は対象外です。

## 編集の流れ

1. `list_documents` から `documentId` を取得します。
2. `get_document` にそのIDを渡し、`revision`、確定済み `project`、未確定の `candidate` を読みます。原子参照には `project.document.atoms` の安定IDを使います。`include` に `["project"]` / `["candidate"]` を指定するとその部分だけを、`[]` を指定すると版・組成式・原子数と `hasCandidate` だけを返します。省略時は両方を返します。
3. 編集ツールに `documentId` と `expectedRevision` を渡します。標準では候補だけを作成します（`preview: true`）。
4. 編集ツールと `history` は、プロジェクト全体ではなく要約を返します。要約は新しい `revision`、確定文書の `formula` / `atomCount`、候補の `formula` / `atomCount` と、追加・削除・変更された原子ID（`addedIds` / `removedIds` / `changedIds`、各100件まで。超過時は `idsTruncated: true`）です。候補がなければ `candidate` は `null` です。候補の内容は `get_document`（`include: ["candidate"]`）で確認します。その後、新しい `revision` を `expectedRevision` に指定して `history` の `action: "confirm"` で確定、`"cancel"` で取消します。
5. `export_document` で文字列を取得するか、`save_file` でプロジェクトを保存します。

GUIでも候補は通常の「確定」「取消」で扱えます。`preview: false` を明示すると検証後に直接確定し、GUIのUndo履歴に追加します。`history` の `undo` / `redo` も同じ履歴を使います。候補がある間の別の編集とUndo/Redoは拒否します。GUI側で整形ワーカーが動作中の場合も編集を拒否します。

`revision` は確定文書と候補の内容から生成します。GUIでの編集や候補確定により内容が変わると、古い版を指定した変更は失敗します。失敗時は再取得して差分を確認してください。文書IDはプロセスごとに異なり、切断後は無効になります。複数の呼び出し元が同じプロセスを共有する場合も、各操作にIDを渡す必要があります。

固定原子の移動、不正な参照、重複座標、不正な引数などは文書を変えずに失敗します。部分編集はプロジェクトの未知のenvelopeフィールドも保持します。`open_file`（プロジェクト）と `import_document(format: "project")` は指定したプロジェクト全体を読み込み、元のプロジェクトはUndoで復元できます。`open_file` でXYZ/MOL/SDF/Mol2/CML/PDBを開いた場合は、「2 座標」での読込と同じく構造だけを置き換え、計算条件・実行設定を保持します。NWChem入力（`.nw`）とGaussian入力（`.gjf` / `.com`）は電荷・多重度も設定し、NWChem入力はMolFormaと共通の計算条件も取り込みます（詳細設定と追加入力は保持）。複数フレームのXYZは `frame`（0始まり）が必要です。

## 公開ツール

| ツール | 用途 |
| --- | --- |
| `list_documents` / `get_document` | 文書ID、版、プロジェクト、候補の取得。`include` で取得範囲を指定 |
| `list_templates` / `place_template` | 組み込み部品の検索と配置 |
| `add_atom` | 元素・Å座標による追加。任意の結合先と水素補完を指定 |
| `set_positions` / `translate` | 安定IDによる座標変更・平行移動 |
| `set_bond` / `delete_atoms` | 結合の変更・除去、原子削除と参照更新 |
| `measure` | 2/3/4原子の距離・角度・二面角 |
| `set_dimension` | 2/3/4原子の距離（Å）・角度・二面角（度）の設定。GUIと同じく後の原子の側（結合していない別々の分子では後の原子の分子全体）を動かす |
| `set_atom_properties` | 安定IDの原子の元素（`hydrogens` で水素の調整）、形式電荷、不対電子、同位体（質量数、0で解除）または質量（amu）、固定、ゴースト、原子名の変更 |
| `import_document` | XYZ文字列またはMolFormaプロジェクト（`.kgui.json`、version 2）のJSON文字列の取込。XYZは `append: true` で今の分子の横に追加 |
| `open_file` | ローカルXYZ/MOL/SDF/Mol2/CML/PDB/MolFormaプロジェクト、NWChem・Gaussian入力の取込。複数フレームのXYZは `frame` で選択 |
| `export_document` | 確定文書のXYZ、MolFormaプロジェクトのJSON、検証済みNWChem入力文字列 |
| `save_file` | 確定文書をMolFormaプロジェクト（`.kgui.json`）として保存。既存ファイルは `overwrite: true` か、利用者の上書き承認が必要 |
| `get_nwchem` / `set_nwchem` | 計算条件・既定値・選択肢（`choices`）・検証結果の取得と条件変更。溶媒（SMD・溶媒名）、相対論、RI-J、物性、遷移状態探索・IRC・拘束（`constraints`、原子は安定ID）・スキャン（`scan_atoms` 等）、BSSE、後続の一点計算を含む。追加入力（`extra_input` / `extra_tasks`）は変更不可 |
| `get_advanced` / `set_advanced` | 詳細・可視化出力設定の型、選択肢、適用条件、値を取得・変更。`null` で個別リセット |
| `get_execution` / `set_execution` | 実行ファイル、MPI、スレッド数、作業場所、環境変数、時間制限の取得・変更 |
| `history` | 候補の確定・取消、Undo/Redo |

次の4ツールは `--mcp` で追加されます。`--mcp-gui` にも同名のツールがあり、GUIの画面と同じ計算・結果を扱います。

| ツール | 用途 |
| --- | --- |
| `get_app_state` | 候補の有無、計算・結果読込の状態 |
| `calculation` | NWChem実行・停止、入力一式の保存 |
| `get_results` | 読込済み結果の要約（計算のエネルギー `final_energy`（最後のtask・振動数の参照構造・BSSE補正後）・熱化学・双極子・最適化状態・スキャンとIRCの各点・BSSE・溶媒和・原子ごとのMulliken/ESP/RESP電荷・NMR遮蔽・分極率を含む `properties`）、構造列・エネルギー・軌道（`number` はスピンごとの軌道番号で、ログに一部の軌道しかなくてもMOLDENと同じ番号）・振動数（並進・回転は `rigid`）・励起状態・最適化ステップ・変位・ログ |
| `results_action` | 結果ファイル・計算フォルダーの読込、再読込、読込の取消 |

画面なしモードの計算と読込はバックグラウンドで進み、`calculation` / `results_action` はすぐに戻ります。完了はタスクか `get_app_state` で確認します。計算はMolFormaの終了（標準入力を閉じる）とともに停止します。結果の表示・再生・画像取得はGUIモードだけです。

次のツールは `--mcp-gui` で追加されます。

| ツール | 用途 |
| --- | --- |
| `get_app_state` | 選択、候補、整形、計算、結果読込・再生、履歴走査の状態 |
| `get_ui` / `get_ui_item` | ウィジェット・メニュー・ショートカット・ダイアログの一覧、入力値、選択肢、表・ツリー・ログの取得 |
| `get_ui_command` | GUI操作の進行状態と失敗理由の取得 |
| `ui_activate` | ボタン、メニュー項目、ショートカットの実行 |
| `ui_set_value` | テキスト・数値・チェック・スライダーの変更。テキストは入力した場合と同じく変更済みとして確定するため、変更時だけ反映する欄（ジョブ保存先など）にも効きます。`commit: false` は入力確定を省略 |
| `ui_select` / `ui_edit_cell` | タブ・選択肢・表・ツリーの選択、編集可能セルの変更 |
| `ui_input` | MolForma内のクリック・ドラッグ・ホイール・キー・部品ドロップ（`templateId` は `list_templates` のID、`atom:元素記号`、`center:元素記号`。不明なIDは何も置かず、ほかの拒否された編集と同じ警告ダイアログを開きます） |
| `ui_file_dialog` | ファイル選択・保存ダイアログのパスとフィルター指定、確定・取消 |
| `capture_screenshot` | MolFormaのウィンドウまたは指定部品のPNG取得・保存 |
| `set_selection` | 安定IDによる原子選択。空配列で解除 |
| `chemistry` | SMILES生成（置換）、SMILESからの断片の追加（`fragment`）、全体・局所整形、3D再生成、配座探索（`conformers`、最安定の配座を候補にする。今の構造と同じなら候補を作らない）、中止。結果は候補として確認 |
| `calculation` | 入力一式の保存、NWChem実行・停止 |
| `get_results` / `results_action` | 結果読込・取消、構造列・エネルギー・軌道・振動数・励起状態・最適化ステップ・変位・ログ取得、選択・再生・停止、XYZ保存、表示中の構造を文書の候補にする `apply_structure` |
| `history_action` | 履歴の走査・取消、ジョブ選択・再利用・結果表示、記録の修復（`repair`: このPCで実行中と記録されたままプロセスが終了したジョブを「中断」に直す）、ごみ箱への移動、絞り込まれた一覧のCSV保存 |

引数の型と必須項目は `tools/list` のJSON Schemaを参照してください。全ツールは `content` のJSONテキストと `structuredContent` の両方を返します。画面取得は追加でMCPの `image` コンテンツを返します。引数や編集内容のエラーは `isError: true`、不正なJSON-RPCや未知のツールはプロトコルエラーを返します。

## リソース

`resources/list` は次のリソースを返します。`resources/read` は読んだ時点の内容を返します。リソースの購読や更新通知は提供しません。

| URI | 形式 | 内容 |
| --- | --- | --- |
| `molforma://documents/<documentId>` | JSON | `get_document` と同じ確定文書・候補・版 |
| `molforma://documents/<documentId>/xyz` | XYZ | 確定文書の構造（Å） |
| `molforma://documents/<documentId>/nwchem` | テキスト | 確定文書から生成したNWChem入力。生成できない場合は `-32603` と理由を返す |
| `molforma://app/state` | JSON | `get_app_state` と同じ状態 |
| `molforma://calculation/output` | テキスト | 現在または最後の計算の `output.out` の末尾 |
| `molforma://calculation/stderr` | テキスト | 現在または最後の計算の `stderr.err`（MPI・ランチャーのメッセージ）の末尾 |
| `molforma://results/summary` | JSON | `get_results` の要約 |
| `molforma://results/log` | テキスト | 読込済み結果のログの末尾 |

テキストのリソースは末尾の10万文字までです。それより前を省いた場合は、先頭に注記を付けます。

`save_file` は保存先を明示する独立した操作で、GUIの既存の保存先や「未保存」表示を変更しません。候補がある状態では保存を拒否します。`export_document` は常に確定済みの文書を返します。

保存先に既にファイルがある場合の動作は次のとおりです。

- `overwrite: true` を指定すると上書きします。`overwrite: false` を指定すると失敗します。
- `overwrite` を省略し、クライアントがElicitation（form）に対応している場合は、`resultType: "input_required"` を返して利用者に上書きの可否を尋ねます。クライアントは、利用者の回答（`inputResponses.overwrite`）と返された `requestState` を付けて同じ呼び出しを再送します。承認された場合だけ上書きし、拒否・取消の場合は `isError: true` で失敗します。
- `requestState` は署名付きで、10分で期限が切れます。保存先のパスと文書の版に結び付いているため、改ざんされた場合や、文書が変わった後に再送された場合は、もう一度確認を求めます。
- Elicitationに対応しないクライアントと旧版の接続では、従来どおり `overwrite: true` が必要です。

## Prompts

`prompts/list` は次のプロンプトを返します。`prompts/get` は、引数を埋め込んだ作業手順を1件のユーザーメッセージとして返します。引数はすべて文字列です。不明なプロンプト、必須引数の不足、文字列以外の値は `-32602` になります。Claude Codeでは `/mcp__molforma__<名前>` のスラッシュコマンドとして使えます。

| 名前 | モード | 引数 | 内容 |
| --- | --- | --- | --- |
| `review_candidate` | 両方 | なし | 未確定の候補を要約し、利用者の判断を待ってから確定・取消する |
| `geometry_optimization` | 両方 | `method`、`basis`、`frequencies`（`yes` で振動数計算も実行） | 計算条件を設定して構造最適化を行う。`--mcp` では入力ファイルの出力まで |
| `build_from_smiles` | `--mcp-gui` | `smiles`（必須） | SMILESから3D候補を作成し、確認してから確定する |
| `frequency_analysis` | `--mcp-gui` | `path`（出力ファイルまたは実行フォルダー） | 結果を開き、虚振動を報告し、モードを再生して画像を取得する |

プロンプトはLLMへの指示文です。MolFormaの状態は変更しません。

## GUIの操作と完了確認

`get_ui` の `query` で表示文言・クラス名・`objectName` を検索できます。他のタブの部品を取得する場合は `includeHidden: true` を使います。`target` には返された `ui-N` または一意な `objectName` を指定します。ダイアログを閉じるとその部品のIDは失効します。名前が重複する場合は `ui-N` を使ってください。

GUI変更は `documentId` と `expectedRevision` を必要とし、`queued` の `commandId` を直ちに返します。**GUI変更には `preview` 引数はありません。** 対応する画面操作と同じタイミングで確定または候補化します。実行直前にも文書と候補の変更を検査します。

1. `get_ui_command(commandId)` を呼び、`queued` / `running` / `done` / `failed` を確認します。
2. `running` が続く場合は `get_ui` の `activeModal` を調べます。確認ダイアログの文言を読み、該当ボタンを別の `ui_activate` で明示的に操作します。
3. ファイル選択は `ui_file_dialog` で操作できます。GUI MCPではQtのファイルダイアログを使うため、OSのファイル選択画面を別途自動操作する必要はありません。既存ファイルの上書き確認は通常どおり表示します。
4. `done` はGUI呼出しが戻ったことを示します。画面側の検証メッセージは `get_ui_item` で確認してください。整形・計算・読込・履歴走査の完了は `get_app_state` で確認します。計算の終了コード0だけでは収束を保証しません。

モーダル表示中は、そのダイアログ内の操作を受け付けます。背景の文書編集は拒否します。保留操作は最大16件、完了記録は保留分と合わせて直近128件までです。他のアプリケーションの画面は操作しません。シェルコマンドは受け付けず、NWChemの実行ファイル・ランチャー・環境変数は[安全のための制限](#安全のための制限)のとおり利用者が決めます。

一覧・表・結果は `offset` / `limit` で分割取得します。`nextOffset` がある場合は次回の `offset` に使い、件数が0になったら終了します。GUI一覧・表行・結果行は1ページ約40 KiBで区切ります。1件だけで40 KiBを超える項目（大きな構造の構造列など）は、そのページに単独で返します。1件が2 MiBを超える場合は失敗します。表の `parentRows` はツリーの親を指定する0始まりの行番号列です。`get_ui_item` は表の `viewport` とセルの画面座標も返すため、クリックで動くリスト項目にも `ui_input` を使えます。タブ・表・結果の番号は0始まり、原子の指定は安定IDです。ログの `offset` / `limit` は文字数で、最大65,536文字を取得できます。

既存GUIの各操作は次の経路で利用できます。

| GUIの範囲 | MCPの経路 |
| --- | --- |
| 分子編集、部品・ペプチド・配位・立体化学、断片・残基・点電荷 | 文書ツールと `get_ui` → `ui_*`。ダイアログも同じ手順 |
| PDBのMODEL/ALTLOC選択、診断・修復、カスタム部品、設定・プリセット | 対応するメニュー・ボタンを `ui_activate`、内容を `ui_set_value` / `ui_select` |
| 座標表、固定・ゴースト・質量、計算条件・詳細設定 | `ui_edit_cell` / `ui_set_value`、または設定ツール |
| 入力プレビュー、検索・コピー・保存 | タブと各ボタンを `ui_*` で操作、本文は `get_ui_item` |
| NWChem実行と履歴 | `calculation` / `history_action` と `get_app_state`。再利用・ごみ箱の確認もMCPで処理 |
| 振動・虚振動、構造列、軌道・CUBE、表示方向・透明度・等値・PNG保存 | `get_results` / `results_action`、表示設定は `ui_*`、画像は `capture_screenshot` |
| メインウィンドウの保存先、未保存表示、終了時の確認 | ファイルメニューを `ui_activate`、必要なダイアログを `ui_*` |

これはMolFormaに実装済みの機能の公開です。未実装の計算・表示機能が新しく追加されるわけではありません。RDKit等の依存機能が無効なビルドでは、対応するGUI部品も無効です。

### Tasks拡張

クライアントが `tools/call` の `_meta` の `clientCapabilities.extensions` に `io.modelcontextprotocol/tasks` を宣言した場合、GUI変更ツールは `commandId` の代わりに `resultType: "task"` のタスクを返します。宣言しない場合は、上記の `commandId` の方式のままです。画面なしモードでは、`calculation`（`start`）と `results_action`（読込）がタスクを返します。サーバーは `server/discover` の `capabilities.extensions` でこの拡張への対応を示します。

- `tasks/get` は、GUI操作が実行待ち・実行中の間は `working` を返します。操作が戻ると `completed` になり、`result` に `get_ui_command` と同じ操作記録を含むツール結果を入れます。
- 次の操作では、操作が開始した処理が終わるまで `working` のままです。終了時の `result.structuredContent.app` に、該当する `get_app_state` の項目を入れます。
  - `chemistry`（`cancel` 以外）: 整形ワーカーの終了まで
  - `calculation`（`start`）: NWChemの終了・停止まで
  - `results_action`（`open_file` / `open_directory` / `reload`）: 結果の読込完了まで
  - `history_action`（`scan`）: 履歴の走査完了まで
- GUIで失敗した操作は、`isError: true` のツール結果を持つ `completed` になります。`failed` はJSON-RPCエラーにだけ使う状態のため、使いません。
- 実行中に確認ダイアログ（`QMessageBox`）が開き、クライアントがElicitation（form）を宣言している場合は、`input_required` と `inputRequests` を返します。各リクエストは、ダイアログの文言とボタン名の選択肢（`choice`）を持つElicitationです。`tasks/update` で `{"action": "accept", "content": {"choice": "<ボタン名>"}}` を送ると、そのボタンを押します。拒否・取消の場合は、ダイアログの取消ボタン（Esc相当）を押します。
- Elicitationを宣言しないクライアントには `working` を返し、`statusMessage` で `get_ui` / `ui_activate` による応答を促します。
- `pollIntervalMs` は500です。`ttlMs` は `null`（期限なし）ですが、タスクは直近128件まで保持します。元の操作記録が消えた後の `tasks/get` は `-32602` になります。
- `tasks/cancel` は、まだ止められる処理を止めます。実行待ちのGUI操作は実行せずに取り消します。開始済みの整形・計算・結果読込・履歴走査は、各ツールの中止操作と同じ方法で止めます。止めた後の `tasks/get` は `cancelled` を返します。実行中のGUI呼出し（ダイアログの表示中など）と完了済みの処理は止めず、本来の結果を返します。
- タスクの状態通知（`subscriptions/listen` による `notifications/tasks`）は提供しないため、`tasks/get` でポーリングしてください。
- 拡張を宣言しないリクエストからの `tasks/*` は `-32021` になります。

### 振動アニメーションの例

以下は `tools/call` のツール名と `arguments` の例です。各変更の前に取得した文書IDと現在の版を `DOC` / `REV` に置き換え、返された操作IDの完了を確認して次へ進みます。

```json
{"name":"results_action","arguments":{"documentId":"DOC","expectedRevision":"REV","action":"open_file","path":"/absolute/path/to/output.out"}}
{"name":"get_results","arguments":{"documentId":"DOC","section":"modes","offset":0,"limit":100}}
{"name":"results_action","arguments":{"documentId":"DOC","expectedRevision":"REV","action":"play_mode","index":0,"amplitude":0.3}}
{"name":"capture_screenshot","arguments":{"documentId":"DOC","target":"mainWindow","path":"/absolute/path/to/mode.png"}}
{"name":"results_action","arguments":{"documentId":"DOC","expectedRevision":"REV","action":"stop"}}
```

読込完了後、`frequency < 0` の行が虚振動です。変位データのない行は表示・選択できますが再生できません。正振動も同じ `play_mode` で再生できます。`mode_displacements` は基準構造と変位を返します。結果操作やカメラ操作は編集文書を変更しません。

計算から行う場合は、先に `set_nwchem` / `set_advanced` / `set_execution` で設定を確定し、`calculation(action: "start")` を呼びます。NWChemの実行ファイルなどは利用者がMolFormaで設定します。`get_app_state.calculation.directory` が保存された実行フォルダーです。終了後に `results_action(action: "open_directory", path: ...)` で結果を開けます。

## 新仕様の通信例

初期化ハンドシェイクなしで次の1行を標準入力に送信できます。

```json
{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}
```

ツール呼び出しにも同じ必須メタデータを指定します。

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}},"name":"list_documents","arguments":{}}}
```

正常な結果には `resultType: "complete"` を返します。未対応バージョンは `-32022` と対応版一覧、必須メタデータ不足は `-32602` で応答します。旧方式では `initialize`、`notifications/initialized` の後に通常のリクエストを送ってください。旧版の応答に `resultType` は追加しません。

## 範囲と制限

- stdioはPOSIX環境（Linux/macOS）向けです。Windowsの標準ストリーム転送は未実装です。
- Streamable HTTP、OAuth、リモート接続、Sampling、Roots、Logging、補完（`completion/complete`）、購読（`subscriptions/listen`）、MCP Appsは提供せず、対応機能としても宣言しません。
- Elicitationはform形式だけを使います。対象は `save_file` の上書き確認と、Tasks実行中の `QMessageBox` だけです。ファイル選択などその他のダイアログは `get_ui` → `ui_*` で操作します。
- Tasks拡張の対象は、GUI変更ツールと、画面なしモードの計算開始・結果読込です。文書ツールは同期で結果を返します。
- GUI操作、結果の表示・再生、画面取得、SMILES生成・整形、計算履歴は `--mcp-gui` だけです。画面を作らない `--mcp` は文書・設定用の23ツールと計算・結果用の4ツールを提供します。
- 読み込みは1ファイル8 MiB、受信は1メッセージ8 MiB、文書は10,000原子までです。送信待ちデータが32 MiBを超える読み取り停止クライアントは切断します。JSON-RPCバッチには対応しません。
- `open_file` によるPDB読込はMODEL 1・ALTLOC自動選択です。選択が必要な場合はGUIのファイルメニューから読み込み、ダイアログをMCPで操作します。
- 文書ツールは同期、GUI変更ツールは操作IDを返します。長時間処理の取消は `chemistry` / `calculation` / `results_action` / `history_action` の中止操作を使います。完了済みRPCへの `notifications/cancelled` は無視します。ヘッドレスモードのUndo履歴は直近100回までです。
- 結果読込は既存ビューアと同じ上限（1ファイル128 MiB、2,000原子・2,000構造、構造列は合計20万原子）です。画面取得は横幅最大4,096ピクセル・PNG 8 MiBまでです。
- ファイル保存以外の文書・候補・履歴はメモリー内にあり、プロセス終了時に失われます。

## 検証

`ctest --test-dir build --output-on-failure -R '^mcp$'` で、新旧版の通信、版の拒否、候補とUndo/Redo、固定原子・失敗時の原状保持、GUIとの競合、未知のenvelope保持、ファイル保存、NWChem入力生成を検証します。実行ファイルを子プロセスとして起動し、ダイアログ表示中の通信、ファイル選択、合成MOLDENの虚振動・正振動の再生と停止、PNG取得、模擬NWChemの実行と取消、履歴走査・選択・CSV出力、SMILES候補作成までMCPのみで検証します。数値・座標表・選択操作、古い版と破棄済み部品の拒否、ページ上限、UTF-8と分割メッセージ、通知、EOF終了も対象です。全ツールの `title` / `outputSchema`、Prompts、`save_file` の上書き確認（承認・拒否・改ざん・版の変更）、Tasksによるダイアログへの回答と、模擬NWChemの終了・停止までの追跡も検証します。実行設定の保護（MCPからの変更・プロジェクト読込後の実行の拒否、利用者の確認ダイアログ）、許可ディレクトリ（シンボリックリンク、ファイル選択ダイアログ）、`tasks/cancel` による取消、リソース、画面なしモードでの模擬NWChemの実行と結果読込も検証します。テストにネットワークや資格情報は不要です。
