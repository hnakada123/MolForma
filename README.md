# MolForma

KAGAMI GUI の「1 分子」タブを基にした、Qt6 / C++20 の独立した分子エディタです。日本語の部品パレット、3D編集画面、編集・配位・残基・割当・電荷パネルを備えます。実行時に KAGAMI のソースツリーや Python は必要ありません。

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

機能の対応範囲、元GUIとの差異、検証例は [docs/features.md](docs/features.md) を参照してください。

## 開発

ソースは `src/`、Qt Testは `tests/`、説明は `docs/`、組み込みデータは `assets/` にあります。データはQt Resourceに埋め込み、実行時の作業ディレクトリに依存しません。

```sh
clang-format -i src/*.h src/*.cpp tests/*.cpp
QT_QPA_PLATFORM=offscreen ./build/molforma \
  tests/fixtures/ala_gly_ser.kgui.json --screenshot /tmp/molforma.png
```

テストはネットワーク、私有データ、KAGAMIのインストールを必要としません。設定は `QSettings("MolForma", "MolForma")` に保存します。整形は同じ実行ファイルのワーカーモードを `QProcess` で起動し、一時ディレクトリのJSONで受け渡します。

## 出典

参照先は KAGAMI の `docs/00_gui/molecule_tab.html` と `tools/kagami_gui/`（2026-10-22時点のローカルツリー）です。101件の部品データと元素データを同梱しています。由来とライセンス表示は [NOTICE](NOTICE)、[assets/KAGAMI-LICENSE](assets/KAGAMI-LICENSE)、[assets/KAGAMI-NOTICE](assets/KAGAMI-NOTICE) を参照してください。
