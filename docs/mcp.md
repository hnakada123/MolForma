# MCPサーバー

MolFormaのネイティブ実行ファイルに、分子の表示・編集とNWChem入力生成用のMCPサーバーを内蔵しています。追加のPython/Node.jsランタイムやMCP SDKのインストールは不要です。

2026-10-23に確認した最新の安定版 **2026-07-28** と、従来の **2025-11-25** に対応します。新しい仕様は初期化を省略し、リクエストごとにプロトコル情報を指定します。旧版の `initialize` に対しては `2025-11-25` を返します。それより古い版のみ対応するクライアントは更新してください。

参照した公式仕様:

- [バージョンと互換性](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning)
- [基本プロトコルとリクエストメタデータ](https://modelcontextprotocol.io/specification/2026-07-28/basic)
- [stdioトランスポート](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio)
- [ツール](https://modelcontextprotocol.io/specification/2026-07-28/server/tools)
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

画面が不要な自動処理では `"args": ["--mcp"]` を指定します。このモードは空の文書で始まり、DISPLAYやQtの画面用プラグインを必要としません。両モードは同じツールと候補確認の動作を提供します。

通信は標準入力・標準出力の改行区切りUTF-8 JSON-RPCです。標準出力にはプロトコルメッセージだけを書き、診断は標準エラーへ出します。標準入力を閉じると、GUIモードを含めてプロセスが終了します。**必要な文書は切断前に保存してください。** 接続終了時の保存ダイアログや自動保存はありません。

通常の起動ではMCPは無効です。ネットワークポートは開きません。MCPクライアントが許可した呼び出しによって、起動したMolFormaプロセスの権限でローカルファイルを読み書きします。ファイル操作のパスは各呼び出しで明示的に指定します。

## 編集の流れ

1. `list_documents` から `documentId` を取得します。
2. `get_document` にそのIDを渡し、`revision`、確定済み `project`、未確定の `candidate` を読みます。原子参照には `project.document.atoms` の安定IDを使います。
3. 編集ツールに `documentId` と `expectedRevision` を渡します。標準では候補だけを作成します（`preview: true`）。
4. 返された `candidate` を確認し、その返答の新しい `revision` を `expectedRevision` に指定して `history` の `action: "confirm"` で確定、`"cancel"` で取消します。
5. `export_document` で文字列を取得するか、`save_file` でプロジェクトを保存します。

GUIでも候補は通常の「確定」「取消」で扱えます。`preview: false` を明示すると検証後に直接確定し、GUIのUndo履歴に追加します。`history` の `undo` / `redo` も同じ履歴を使います。候補がある間の別の編集とUndo/Redoは拒否します。GUI側で整形ワーカーが動作中の場合も編集を拒否します。

`revision` は確定文書と候補の内容から生成します。GUIでの編集や候補確定により内容が変わると、古い版を指定した変更は失敗します。失敗時は再取得して差分を確認してください。文書IDはプロセスごとに異なり、切断後は無効になります。複数の呼び出し元が同じプロセスを共有する場合も、各操作にIDを渡す必要があります。

固定原子の移動、不正な参照、重複座標、不正な引数などは文書を変えずに失敗します。部分編集はプロジェクトの未知のenvelopeフィールドも保持します。`open_file` と `import_document(format: "project")` は指定したプロジェクト全体を読み込み、元のプロジェクトはUndoで復元できます。

## 公開ツール

| ツール | 用途 |
| --- | --- |
| `list_documents` / `get_document` | 文書ID、版、プロジェクト、候補の取得 |
| `list_templates` / `place_template` | 組み込み部品の検索と配置 |
| `add_atom` | 元素・Å座標による追加。任意の結合先と水素補完を指定 |
| `set_positions` / `translate` | 安定IDによる座標変更・平行移動 |
| `set_bond` / `delete_atoms` | 結合の変更・除去、原子削除と参照更新 |
| `measure` | 2/3/4原子の距離・角度・二面角 |
| `import_document` | XYZ文字列またはKAGAMI v2 JSON文字列の取込 |
| `open_file` | ローカルXYZ/MOL/SDF/PDB/KAGAMIファイルの取込 |
| `export_document` | 確定文書のXYZ、KAGAMI v2 JSON、検証済みNWChem入力文字列 |
| `save_file` | 確定文書をKAGAMI v2として保存。既存ファイルは `overwrite: true` が必要 |
| `get_nwchem` / `set_nwchem` | 計算条件・既定値・検証結果の取得と条件変更 |
| `history` | 候補の確定・取消、Undo/Redo |

引数の型と必須項目は `tools/list` のJSON Schemaを参照してください。全ツールは `content` のJSONテキストと `structuredContent` の両方を返します。引数や編集内容のエラーは `isError: true`、不正なJSON-RPCや未知のツールはプロトコルエラーを返します。

`resources/list` に文書のURI `molforma://documents/<documentId>` を公開し、`resources/read` で `get_document` と同じJSONを取得できます。リソース購読や更新通知は提供しません。

`save_file` は保存先を明示する独立した操作で、GUIの既存の保存先や「未保存」表示を変更しません。候補がある状態では保存を拒否します。`export_document` は常に確定済みの文書を返します。

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
- Streamable HTTP、OAuth、リモート接続、Prompts、Sampling、Elicitation、Tasks、購読、MCP Appsは提供せず、対応機能としても宣言しません。
- 本実装は分子編集と入力生成を対象とします。SMILES生成・力場整形などの長時間処理、NWChem実行・停止、計算履歴・結果ビューアの操作はMCPツールに公開していません。GUIから利用できます。
- 読み込みは1ファイル8 MiB、受信は1メッセージ8 MiB、文書は10,000原子までです。送信待ちデータが32 MiBを超える読み取り停止クライアントは切断します。JSON-RPCバッチには対応しません。
- PDBファイル読込はMODEL 1・ALTLOC自動選択です。モデル選択ダイアログは出しません。
- ツールは短い同期操作です。完了済みの要求に対する `notifications/cancelled` は無視します。ヘッドレスモードのUndo履歴は直近100回までです。
- ファイル保存以外の文書・候補・履歴はメモリー内にあり、プロセス終了時に失われます。

## 検証

`ctest --test-dir build --output-on-failure -R '^mcp$'` で、新旧版の通信、版の拒否、候補とUndo/Redo、固定原子・失敗時の原状保持、GUIとの競合、未知のenvelope保持、ファイル保存、NWChem入力生成を検証します。実行ファイルを子プロセスとして起動し、画面のない環境での動作、UTF-8と分割メッセージ、通知、サイズ上限、EOF終了も検証します。テストにネットワークや資格情報は不要です。
