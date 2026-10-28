# Ubuntuでのインストール

MolFormaをUbuntuでソースからビルドし、debパッケージを作ってaptでインストールする手順です。ビルドに使う開発用パッケージと、計算に使うNWChemは、Ubuntuの公式リポジトリから入れます。

対象はUbuntu 26.04 LTS（amd64）とMolForma 1.0.0です。2026-10-28に、Dockerの `ubuntu:26.04`（26.04.1 LTS）で手順を確認しました（[確認した構成](#確認した構成)）。

MolFormaのパッケージは、Ubuntuの公式リポジトリにもPPAにもありません（2026-10-28時点）。GitHubのソースからビルドし、CMakeに含まれるCPackでdebパッケージ（`molforma_1.0.0_amd64.deb`）を作ります。インストールしたパッケージはaptで管理するので、削除は `apt remove` で行えます。

## 必要なパッケージ

ビルドには、次のパッケージを入れます（[1. パッケージを入れる](#1-パッケージを入れる)）。

| 区分 | パッケージ | 用途 |
| --- | --- | --- |
| 必須 | `build-essential`、`cmake`、`ninja-build`、`git`、`pkgconf`、`file` | ビルドとdebパッケージの作成（`file` はCPackが依存ライブラリを調べるのに使う） |
| 必須 | `qt6-base-dev` | 画面（Qt Widgets）、テスト（Qt Test） |
| 必須 | `libeigen3-dev` | 座標・幾何の計算 |
| 必須 | `libopenbabel-dev` | 構造ファイルの読み書き、力場による整形、SMILESからの3D構造 |
| 任意 | `librdkit-dev`、`libboost-iostreams-dev`、`libboost-serialization-dev` | 厳密な立体判定（R/S・E/Z）、残基認識。Boostの2つは、RDKitのCMakeの設定が参照する |
| 任意 | `libvtk9-dev`、`libvtk9-qt-dev` | 3D画面をVTK（OpenGL）で描画。VTKのCMakeの設定が参照する `nlohmann-json3-dev` やMPIの開発用パッケージも、依存として入る |

実行時に使うパッケージは、作ったdebパッケージをインストールするときにaptが自動で入れます。Qt 6のライブラリと `qt6-qpa-plugins`、`libopenbabel7`（フォーマットプラグインと力場データを含む）、RDKit・VTKを使う構成では `librdkit1t64`・`libvtk9.5`・`libvtk9.5-qt` です。推奨パッケージの `qt6-translations-l10n`（日本語表示のとき、ファイル選択などQtの標準ダイアログも日本語にする）と `fonts-noto-cjk`（日本語のフォント）も、aptの既定の設定では一緒に入ります。

計算に使うNWChemは、公式リポジトリ（universe）の `nwchem` を入れます（[2. NWChemを入れる](#2-nwchemを入れる)）。

- **RDKitとVTK**: どちらも任意です。ビルド時にCMakeが見つけたときだけ有効になり、debパッケージの依存関係もそれに合わせて決まります。VTKで描画するには、OpenGL 3.2以上を使えるGPUドライバーも必要です。
- **NWChem**: 入力の作成・保存（「ジョブとして保存」）はNWChemがなくても使えます。

## 1. パッケージを入れる

パッケージの一覧を更新し、必須のパッケージを入れます。

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build git pkgconf file qt6-base-dev libeigen3-dev libopenbabel-dev
```

RDKitとVTKを使う場合は、続けて次を入れます。VTKの開発用パッケージは依存するもの（Java版のVTKなど）が多く、何もない状態からは上と合わせて約780個、ダウンロードは約1 GBになります。

```sh
sudo apt install librdkit-dev libboost-iostreams-dev libboost-serialization-dev libvtk9-dev libvtk9-qt-dev
```

## 2. NWChemを入れる

```sh
sudo apt install nwchem
```

Open MPI版のNWChem（`nwchem-openmpi`）と、MPIの実行環境（`openmpi-bin`）も入ります。NWChemは `/usr/bin/nwchem` に、基底関数のライブラリは `/usr/share/nwchem/` に入ります。基底関数のライブラリはそのまま使えるため、`~/.nwchemrc` や `NWCHEM_BASIS_LIBRARY` の設定は不要です。MolFormaの「3 実行設定」の既定（実行ファイル `nwchem`、MPIで起動、MPIランチャー `mpirun`、2プロセス）のままで、このNWChemを起動できます。

## 3. ソースを取得してビルドする

ソースを取得し、Releaseでビルドします。取得したフォルダーと `build` は、更新で使うので残しておきます。

```sh
git clone https://github.com/hnakada123/MolForma.git
cd MolForma
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
```

3行目の `cmake -S . -B build …`（構成。CMakeが必要なライブラリを調べてビルドの準備をする段階）を実行すると、端末に `-- MolForma rendering:` で始まる行が出ます。この行で3D描画の方式を確認できます。出力の終わり近くは、例えば次のようになります。

```
--   Found openbabel-3, version 3.1.1
-- MolForma rendering: VTK 9.5.2 (Qt6), with QPainter fallback
-- Configuring done (2.2s)
-- Generating done (0.2s)
-- Build files have been written to: …/MolForma/build
```

出力が流れて見逃した場合は、構成のコマンドをもう一度実行すると同じ行が出ます。`MolForma rendering:` の行の意味は次のとおりです。

| 出力 | 意味 |
| --- | --- |
| `MolForma rendering: VTK 9.5.2 (Qt6), with QPainter fallback` | VTKで描画します（OpenGLを使えない環境ではQPainter） |
| `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` | VTKを使いません。直前の `VTK development dependency unavailable:` の行に足りないものが出ます |

足りないパッケージを入れたら、構成のコマンドからやり直します。`-j 4` は同時に動かすコンパイルの数です。メモリーが足りない場合は減らしてください。

テストを実行する場合は次のとおりです（任意）。NWChem・ネットワークは使いません。テストが不要な場合は、構成のコマンドに `-DBUILD_TESTING=OFF` を付けるとビルドが短くなります。

```sh
ctest --test-dir build --output-on-failure
```

## 4. パッケージを作ってインストールする

`build` でdebパッケージを作り、aptでインストールします。`./` を付けると、aptはパッケージ名ではなくファイルとして扱い、足りない依存パッケージも一緒に入れます。

```sh
cd build
cpack -G DEB
sudo apt install ./molforma_1.0.0_amd64.deb
```

`cpack` は、実行ファイルが使う共有ライブラリから依存関係を割り出して（`dpkg-shlibdeps`）、パッケージに書き込みます。パッケージは次のファイルを配置します（`dpkg -L molforma` で一覧できます）。ファイルの種類やメニューの情報は、インストール時に自動で更新されます。

| 配置先 | 内容 |
| --- | --- |
| `/usr/bin/molforma` | 実行ファイル |
| `/usr/share/applications/molforma.desktop` | アプリケーションメニューの項目 |
| `/usr/share/icons/hicolor/` | アイコン（`scalable` と `256x256`） |
| `/usr/share/mime/packages/molforma.xml` | `.molforma.json`（プロジェクト）と `.nw`（NWChem入力）のファイルの種類 |
| `/usr/share/molforma/` | LICENSE・NOTICE・README とマニュアルのMarkdown（マニュアルは実行ファイルにも組み込み済み） |

## 5. 起動して確認する

端末で `molforma` を実行するか、アプリの一覧から「MolForma」を選びます。表示言語はシステムの言語に従います（「編集 → 環境設定」の「表示言語 / Language」で切り替え、再起動後に反映）。

1. 「ヘルプ → MolFormaについて」を開き、版が「MolForma 1.0.0」で、RDKit・VTKを入れた場合は「立体判定・残基認識（RDKit）: 有効」「3D描画: VTK（OpenGL）」と表示されることを確認します。OpenGLを使えない環境では「3D描画: QPainter（CPU）」になります。
2. 「3 実行設定」を開き、「NWChem実行ファイル」と「MPIランチャー」の欄の下に、PATHで見つかった `/usr/bin/nwchem`・`/usr/bin/mpirun` が表示されることを確認します。
3. 小さな分子で計算してみます。手順は[題材1 原子と結合から組む](../recipes/atoms_and_bonds.md)にあります。

## 更新・削除

### 更新する

ソースを更新してビルドし直し、パッケージを作り直してインストールします。版番号が同じ間は、aptは入れ直さないので `--reinstall` を付けます。

```sh
cd MolForma
git pull --ff-only
cmake --build build -j 4
cd build
cpack -G DEB
sudo apt install --reinstall ./molforma_1.0.0_amd64.deb
```

Ubuntuを新しいリリースに上げると、Qt・Open Babel・RDKit・VTKなどのライブラリの版が変わり、MolFormaが起動しなくなったり、依存関係が満たせず削除されたりします。その場合は、`build` を削除して[3. ソースを取得してビルドする](#3-ソースを取得してビルドする)の構成からやり直し、パッケージを作り直してインストールしてください。

### 削除する

```sh
sudo apt remove molforma
```

アプリの設定（`~/.config/MolForma/`）、自動保存（`~/.local/share/MolForma/`）、計算のジョブ（既定は `~/MolForma/jobs`）は残ります。不要なら手動で削除してください。[1. パッケージを入れる](#1-パッケージを入れる)で入れた開発用パッケージも残ります。不要なら `sudo apt remove` で指定して削除し、`sudo apt autoremove` で一緒に入ったものも削除できます。NWChemは `sudo apt remove nwchem` の後に `sudo apt autoremove` で削除できます。

## 困ったとき

| 症状 | 確認・対応 |
| --- | --- |
| 構成が `Qt6`・`Eigen3`・`openbabel-3` が見つからないというエラーで止まる | [1. パッケージを入れる](#1-パッケージを入れる)の必須のパッケージを入れ、構成からやり直します |
| 構成が `The link interface of target "RDKit::rdkit_base" contains: Boost::serialization … but the target was not found` のエラーで止まる | `librdkit-dev` はBoostの開発用パッケージを入れません。`libboost-iostreams-dev`・`libboost-serialization-dev` を入れ、構成からやり直します |
| `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` と出る | `libvtk9-dev`・`libvtk9-qt-dev` を入れたか確認します。`VTK development dependency unavailable:` の行に出たものを入れ、構成からやり直します |
| `cpack` が `CPackDeb: file utility is not available` で止まる | `sudo apt install file` で `file` を入れ、`cpack -G DEB` をやり直します |
| 更新したのに `molforma is already the newest version` と出て入れ直されない | 版番号が同じため、`sudo apt install --reinstall ./molforma_1.0.0_amd64.deb` とします |
| 3D画面の左上に「描画: QPainter（CPU）· OpenGLを使えないため簡易表示」と出る | OpenGL 3.2以上を使えません。GPUドライバーを確認します（`mesa-utils` の `glxinfo -B` で、使われているOpenGLの実装と版が分かります）。リモートデスクトップや仮想マシンでは使えない場合があります |
| VTKで描画しているのに回転が遅い | `glxinfo -B` で `llvmpipe` などと出る場合は、ソフトウェアのOpenGLで描画しています |
| 日本語が四角で表示される | `fonts-noto-cjk` などの日本語フォントを入れます |
| 日本語表示なのに、ファイル選択などQtの標準ダイアログに英語が残る | `qt6-translations-l10n` を入れます |
| 「実行前の確認」でNWChemやMPIランチャーが見つからない | 端末で `command -v nwchem mpirun` を確認します。NWChemを別の場所に入れた場合は、「3 実行設定」でパスを指定します |
| 計算のログ（標準エラー）に `osc_ucx_component.c … Error: OSC UCX component priority set inside component query failed` と出る | Open MPIの通信部品（UCX）の警告で、計算の結果には影響しません。表示しないようにするには、「3 実行設定」の追加の環境変数に、名前 `OMPI_MCA_osc`・値 `^ucx` を加えます |
| `error while loading shared libraries` で起動しない | ライブラリの版が変わりました。[更新する](#更新する)のとおり、構成からやり直してパッケージを作り直します |
| ビルド中にメモリーが不足する | `cmake --build build -j 2` のように並列数を減らします |

## 確認した構成

2026-10-28に、Dockerの `ubuntu:26.04`（Ubuntu 26.04.1 LTS、amd64）で、次のことを確かめました。

- 上のパッケージを入れたコンテナで、ソースを構成・ビルドし（RDKit・VTKとも有効）、CTestの14件すべてが通ること（並列なし・並列の両方）。XvfbとMesaのソフトウェアOpenGLで、VTKの描画経路を検査する任意のテスト（[3D描画](../features/rendering.md)）も通ること。
- `cpack -G DEB` でdebパッケージを作り、何も入れていない別のコンテナで `apt install ./molforma_1.0.0_amd64.deb` と `apt install nwchem` が依存パッケージごと入ること。一般ユーザーで `molforma` が起動し、版が1.0.0であること、ファイルの種類が登録されること。
- `~/.nwchemrc` を置かずに `mpirun -np 2 nwchem` で水素分子のHF/6-31G*計算が完了すること。MolFormaの実行設定のテスト（実際のNWChemを使う任意のテスト）で、MPI 2プロセスの水のHF/STO-3G計算が完了し、SCFエネルギーが −74.945628203255 Hartree になること。

| 項目 | 版 |
| --- | --- |
| Qt（`qt6-base-dev`） | 6.10.2 |
| Open Babel | 3.1.1 |
| Eigen | 3.4.0 |
| RDKit・Boost | 2025.03.6・1.90.0 |
| VTK | 9.5.2 |
| GCC・CMake・Ninja | 15.2.0・4.2.3・1.13.2 |
| NWChem | 7.3.1 |
| Open MPI | 5.0.10 |

実機のデスクトップ（GPUでのVTKの描画、GNOMEのアプリ一覧）と、Ubuntu 24.04 LTSなどほかの版は確認していません。

関連: [OS別インストール・必要パッケージ](../installation.md)、[3D描画](../features/rendering.md)、[「3 実行設定」](../features/execution.md)
