# MolForma

NWChem向けの分子作成・座標編集・計算入力作成を行う、Qt6 / C++20 のデスクトップアプリです。KAGAMI GUIを基に「1 分子」から「6 入力プレビュー」と「結果ビューア」「履歴」を実装しています。日本語の部品パレット、3D編集画面、座標表、NWChem入力の検査・出力を備えます。実行時にKAGAMIのソースツリーやPythonは必要ありません。NWChemのローカル実行・停止・ログ表示、結果構造・振動・CUBE等値面の表示に対応します。保存した計算の履歴一覧、入力・ログの参照、分子と条件の再利用に対応します。

## ビルドと起動

必要な開発パッケージは Qt6 Widgets / Test、Eigen3、Open Babel 3、CMake 3.21以上、C++20コンパイラです。Ninjaを使う例:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
./build/molforma
```

構造ファイルを指定して起動できます。

```sh
./build/molforma tests/fixtures/ala_gly_ser.kgui.json
./build/molforma molecule.xyz
```

Qt Testが不要な配布用ビルドでは `-DBUILD_TESTING=OFF` を指定します。Open Babelの共有ライブラリに加え、フォーマットプラグインと力場データも必要です。Pythonバインディングは使用しません。RDKit C++開発パッケージをCMakeが検出すると、厳密な立体判定・記録と残基認識も有効になります。

```sh
cmake --install build --prefix /tmp/molforma-install
/tmp/molforma-install/bin/molforma
```

## 操作

- 左のパレットで原子・骨格などを選び、中央をクリックして配置します。部品のドラッグ＆ドロップにも対応します。
- 原子を順にクリックして選択・解除します。2 / 3 / 4原子で距離 / 角度 / 二面角を測定し、「寸法を適用」で変更します。
- 左ドラッグで回転、右ドラッグで表示位置の移動、ホイールで拡大縮小。Shiftドラッグで範囲選択、Ctrlドラッグで選択を追加します。
- 配位子・配列構築・削除・整形などの候補は、画面下部の「変更を確定」で反映します。Escは候補の取消、Ctrl+Zは確定済み操作のUndoです。
- `.kgui.json` は結合・残基・配位中心・断片割当・固定・同位体・点電荷・設定を保持します。XYZは座標の交換用です。
- 「ファイル → SMILESから作成」で水素付きの3D分子を生成します。整形処理中は「整形を中止」で中断できます。
- 「2 座標」で元素・X/Y/Z・同位体質量・ゴースト・固定を編集します。表のÅ/Bohr切替は表示だけを変えます。原子追加、削除候補、全体の原点移動、XYZの貼付・コピー・保存にも対応します。
- 同タブ右側でNWChemの `geometry` / `constraints` ブロックを確認し、コピーまたは `.nw` に保存できます。出力単位は表の表示単位と独立です。このファイルは座標部分の入力断片であり、計算には電荷・基底・手法・`task` を追加する必要があります。
- 「3 計算条件」でHF・DFT・MP2・CCSD系の手法、基底、全電荷・多重度、収束、最適化・振動数、COSMO等を設定します。電子数と組合せの検査後、座標・基底/ECP・計算条件・`task` を含むNWChem入力をコピー・保存できます。組込み・ユーザープリセット、Undo/Redo、プロジェクト保存にも対応します。詳しい対応範囲は下記を参照してください。
- 「4 詳細設定」でNWChem用の7カテゴリ・42項目を調整します。HF/DFTの収束、MP2/CCSD、最適化、振動数・熱化学、COSMO、軌道ファイル・保存先を検索・絞り込みできます。空欄は既定値を使用し、明示した値だけを基本条件へ上書きします。項目・カテゴリ単位のリセット、Undo/Redo、入力検査・プレビュー・保存に対応します。

- 「5 実行設定」でNWChem実行ファイル、MPI起動数、OpenMP/BLASスレッド数、保存先、追加の環境変数を設定します。メモリは「3 計算条件」と共有します。入力一式の保存、非同期の実行・停止、時間制限、標準出力／標準エラーの確認に対応します。計算には別途NWChemと、MPI版では対応するMPIランチャーが必要です。
- 「6 入力プレビュー」で完成したNWChem入力を読み取り専用で確認します。行番号・色分け、検索、ブロック移動、検証結果から設定タブへの移動、全文コピー・`.nw`保存に対応します。通常の書出し入力と、入力movecsのパスを解決した実行時入力を切り替えられます。プレビューと保存にNWChem本体は不要です。
- 「結果ビューア」でNWChemログ、XYZ構造列、CUBE、MOLDENを開けます。構造表示・エネルギー一覧、振動アニメーション、CUBEの正負の等値面、MOLDENの軌道情報を確認し、基準構造をXYZ・画面をPNGで保存できます。「5 実行設定」から直近のジョブを開くこともできます。結果の閲覧は編集中の分子を変更しません。

機能の対応範囲、元GUIとの差異、検証例は [docs/features.md](docs/features.md) を参照してください。

- 「履歴」で保存済みの計算を検索・並べ替え、入力・ログ・結果を確認します。「分子・条件を再利用」は新しい未保存プロジェクトを作ります。表示中の一覧のCSV保存、このウィンドウで実行中の計算の停止、終了したジョブのごみ箱への移動にも対応します。

## 開発

ソースは `src/`、Qt Testは `tests/`、説明は `docs/`、組み込みデータは `assets/` にあります。データはQt Resourceに埋め込み、実行時の作業ディレクトリに依存しません。

```sh
clang-format -i src/*.h src/*.cpp tests/*.cpp
QT_QPA_PLATFORM=offscreen ./build/molforma \
  tests/fixtures/ala_gly_ser.kgui.json --screenshot /tmp/molforma.png
```

テストはネットワーク、私有データ、KAGAMIのインストールを必要としません。画面設定とユーザープリセットは `QSettings("MolForma", "MolForma")` に、分子・計算条件・実行設定はプロジェクトに保存します。整形は同じ実行ファイルのワーカーモードを `QProcess` で起動し、一時ディレクトリのJSONで受け渡します。

## 出典

参照先は KAGAMI の `docs/00_gui/molecule_tab.html` と `tools/kagami_gui/`（2026-10-22時点のローカルツリー）です。101件の部品データと元素データを同梱しています。由来とライセンス表示は [NOTICE](NOTICE)、[assets/KAGAMI-LICENSE](assets/KAGAMI-LICENSE)、[assets/KAGAMI-NOTICE](assets/KAGAMI-NOTICE) を参照してください。
