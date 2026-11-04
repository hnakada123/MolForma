# macOSでのインストール

MolFormaをmacOSでソースからビルドし、ホームフォルダーの下にインストールする手順です。ビルドに使うライブラリと、計算に使うNWChemは、パッケージマネージャーの[Homebrew](https://brew.sh/)で入れます。

対象はApple siliconのMac（arm64）のmacOS 27とMolForma 1.0.0です。2026-11-04に、macOS 27.0.1とHomebrew 7.0.10で手順を確認しました（[確認した構成](#確認した構成)）。

MolFormaのパッケージは、Homebrewにはありません（2026-11-04時点）。GitHubのソースからビルドし、`cmake --install` で `~/.local` に入れます。macOSのアプリケーションバンドル（`.app`）は作らないため、MolFormaは「ターミナル」から起動します（[macOSでの制限](#macosでの制限)）。

## 必要なパッケージ

ビルドには、次のものを入れます（[1. Command Line ToolsとHomebrewを入れる](#1-command-line-toolsとhomebrewを入れる)、[2. パッケージを入れる](#2-パッケージを入れる)）。

| 区分 | パッケージ | 用途 |
| --- | --- | --- |
| 必須 | Xcode Command Line Tools | C++コンパイラ（Apple clang）、`git`、macOSのSDK（zlibを含む） |
| 必須 | Homebrewの `cmake`、`ninja`、`pkgconf` | ビルド |
| 必須 | `qtbase` | 画面（Qt Widgets）、テスト（Qt Test） |
| 必須 | `eigen` | 座標・幾何の計算 |
| 必須 | `open-babel` | 構造ファイルの読み書き、力場による整形、SMILESからの3D構造（フォーマットプラグインと力場データを含む） |
| 任意 | `rdkit` | 厳密な立体判定（R/S・E/Z）、残基認識 |
| 任意 | `vtk` | 3D画面をVTK（OpenGL）で描画。HomebrewのVTKはQt 6に対応している |
| 任意 | `qttranslations` | 日本語表示のとき、ファイル選択などQtの標準ダイアログも日本語にする |

Qt全体の `qt` は不要です（MolFormaが使うのは `qtbase` だけで、`qt` はQt WebEngineなど多くのモジュールを入れます）。ビルドしたMolFormaは、Homebrewのライブラリ（`/opt/homebrew` の下）をそのまま使います。

計算に使うNWChemは、Homebrewの `nwchem` を入れます（[3. NWChemを入れる](#3-nwchemを入れる)）。

- **RDKitとVTK**: どちらも任意です。ビルド時にCMakeが見つけたときだけ有効になります。
- **NWChem**: 入力の作成・保存（「ジョブとして保存」）はNWChemがなくても使えます。

## 1. Command Line ToolsとHomebrewを入れる

C++コンパイラなどを含むXcode Command Line Toolsを入れます。Xcodeを入れてある場合は不要です。

```sh
xcode-select --install
```

Homebrewを入れていない場合は、[Homebrewのページ](https://brew.sh/)の手順で入れます。インストールの最後に表示される、`brew` をPATHに加える設定（`~/.zprofile` への追記）も行ってください。Apple siliconのMacでは、Homebrewは `/opt/homebrew` に入ります。

## 2. パッケージを入れる

必須のパッケージを入れます。

```sh
brew install cmake ninja pkgconf qtbase eigen open-babel
```

RDKitとVTK、Qtの日本語訳を使う場合は、続けて次を入れます。VTKは依存するもの（Qt Quickの `qtdeclarative`、HDF5、NetCDFなど）が多く、VTKだけで約300 MBになります。

```sh
brew install rdkit vtk qttranslations
```

## 3. NWChemを入れる

```sh
brew install nwchem
```

Open MPI（`open-mpi`）、ScaLAPACK・OpenBLAS、GNU Fortran（`gcc`）なども依存として入ります。NWChemは約1 GBです。NWChemは `/opt/homebrew/bin/nwchem` に、MPIの実行環境 `mpirun` は `/opt/homebrew/bin/mpirun` に入ります。HomebrewのNWChemは、基底関数などのライブラリの場所を書いた `/opt/homebrew/etc/nwchemrc` を読むため、`~/.nwchemrc` や `NWCHEM_BASIS_LIBRARY` の設定は不要です。MolFormaの「3 実行設定」の既定（実行ファイル `nwchem`、MPIで起動、MPIランチャー `mpirun`、2プロセス）のままで、このNWChemを起動できます。

NWChemだけで動くか確かめる場合は、空のフォルダーで次の入力を `h2.nw` として保存し、実行します（任意）。

```
start h2
geometry
  H 0 0 0
  H 0 0 0.74
end
basis
  * library 6-31g*
end
task scf energy
```

```sh
mpirun -np 2 nwchem h2.nw > h2.out
grep "Total SCF energy" h2.out
```

`Total SCF energy =     -1.126755317965` と出れば正常です。

## 4. ソースを取得してビルドする

ソースを取得し、Releaseでビルドします。取得したフォルダーと `build` は、更新で使うので残しておきます。

```sh
git clone https://github.com/hnakada123/MolForma.git
cd MolForma
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
```

3行目の `cmake -S . -B build …`（構成。CMakeが必要なライブラリを調べてビルドの準備をする段階）を実行すると、端末に `-- MolForma rendering:` で始まる行が出ます。この行で3D描画の方式を確認できます。出力の終わり近くは、例えば次のようになります。

```
--   Found openbabel-3, version 3.2.1
-- MolForma rendering: VTK 9.7.1 (Qt6), with QPainter fallback
-- Configuring done (2.1s)
-- Generating done (1.1s)
-- Build files have been written to: …/MolForma/build
```

| 出力 | 意味 |
| --- | --- |
| `MolForma rendering: VTK 9.7.1 (Qt6), with QPainter fallback` | VTKで描画します（OpenGLを使えない環境ではQPainter） |
| `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` | VTKを使いません。`vtk` を入れたか確認します |

足りないパッケージを入れたら、構成のコマンドからやり直します。`-j 4` は同時に動かすコンパイルの数です。メモリーが足りない場合は減らしてください。

テストを実行する場合は次のとおりです（任意）。NWChem・ネットワークは使わず、アプリの設定と自動保存は一時フォルダーに置くため、自分の設定は変わりません。テストが不要な場合は、構成のコマンドに `-DBUILD_TESTING=OFF` を付けるとビルドが短くなります。

```sh
ctest --test-dir build --output-on-failure
```

## 5. インストールする

`~/.local` にインストールし、`~/.local/bin` をPATHに加えます。PATHの設定は最初の1回だけで、新しく開いたターミナルから有効になります。

```sh
cmake --install build --prefix ~/.local
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zprofile
```

次のファイルを配置します。

| 配置先 | 内容 |
| --- | --- |
| `~/.local/bin/molforma` | 実行ファイル |
| `~/.local/share/molforma/` | LICENSE・NOTICE・README とマニュアルのMarkdown（マニュアルは実行ファイルにも組み込み済み） |
| `~/.local/share/applications/`、`~/.local/share/icons/`、`~/.local/share/mime/` | Linuxのデスクトップ用のメニュー項目・アイコン・ファイルの種類。macOSでは使いません |

インストールせずに、ビルドしたフォルダーの `build/molforma` をそのまま起動しても使えます。

## 6. 起動して確認する

ターミナルで `molforma` を実行します。ファイルを指定して開くこともできます（例: `molforma water.molforma.json`）。表示言語はシステムの言語に従います（環境設定（⌘,）の「表示言語 / Language」で切り替え、再起動後に反映）。macOSでは、「環境設定…」「MolFormaについて」の項目が、表示言語によってメニューバーの「MolForma」メニューに移ることがあります。

1. 「MolFormaについて」（「ヘルプ」メニュー、または「MolForma」メニュー）を開き、版が「MolForma 1.0.0」で、RDKit・VTKを入れた場合は「立体判定・残基認識（RDKit）: 有効」「3D描画: VTK（OpenGL）」と表示されることを確認します。
2. 「3 実行設定」を開き、「NWChem実行ファイル」と「MPIランチャー」の欄の下に、PATHで見つかった `/opt/homebrew/bin/nwchem`・`/opt/homebrew/bin/mpirun` が表示されることを確認します。表示されない場合は、[困ったとき](#困ったとき)を参照してください。
3. 小さな分子で計算してみます。手順は[題材1 原子と結合から組む](../recipes/atoms_and_bonds.md)にあります。

## macOSでの制限

- **アプリケーションバンドルがない**: `.app` を作らないため、Finder・Launchpad・Dockからは起動できず、`.molforma.json` をダブルクリックしても開きません。ターミナルから `molforma` で起動してください。
- **Homebrewのライブラリに依存する**: ビルドしたMolFormaは `/opt/homebrew` のQt・Open Babel・RDKit・VTKを使います。ほかのMacへコピーしても動きません。

## 更新・削除

### 更新する

ソースを更新してビルドし直し、インストールし直します。

```sh
cd MolForma
git pull --ff-only
cmake --build build -j 4
cmake --install build --prefix ~/.local
```

`brew upgrade` でQt・Open Babel・RDKit・VTKが新しい版になると、ライブラリのファイル名が変わり、MolFormaが起動しなくなることがあります（`dyld[…]: Library not loaded` と出ます）。その場合は、`build` を削除して[4. ソースを取得してビルドする](#4-ソースを取得してビルドする)の構成からやり直し、インストールし直してください。

### 削除する

インストールしたファイルを削除します。

```sh
rm ~/.local/bin/molforma
rm -r ~/.local/share/molforma
rm ~/.local/share/applications/molforma.desktop ~/.local/share/icons/hicolor/scalable/apps/molforma.svg
rm ~/.local/share/icons/hicolor/256x256/apps/molforma.png ~/.local/share/mime/packages/molforma.xml
```

アプリの設定（`~/Library/Preferences/MolForma/MolForma.ini`）、自動保存（`~/Library/Application Support/MolForma/`）、計算のジョブ（既定は `~/MolForma/jobs`）は残ります。不要なら手動で削除してください。Homebrewで入れたパッケージは、`brew uninstall` で指定して削除し、`brew autoremove` で一緒に入ったものも削除できます。NWChemは `brew uninstall nwchem` の後に `brew autoremove` で削除できます。

## 困ったとき

| 症状 | 確認・対応 |
| --- | --- |
| 構成が `No CMAKE_CXX_COMPILER could be found` などで止まる | Command Line Toolsを入れます（`xcode-select --install`）。`xcode-select -p` で場所が出れば入っています |
| 構成が `Qt6`・`Eigen3`・`openbabel-3` が見つからないというエラーで止まる | [2. パッケージを入れる](#2-パッケージを入れる)の必須のパッケージを入れ、構成からやり直します |
| `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` と出る | `brew install vtk` で入れ、構成からやり直します |
| `molforma: command not found` と出る | `~/.local/bin` がPATHにありません。[5. インストールする](#5-インストールする)のPATHの設定を行い、ターミナルを開き直します |
| 起動時に `dyld[…]: Library not loaded` と出る | Homebrewのライブラリの版が変わりました。[更新する](#更新する)のとおり、構成からやり直してインストールし直します |
| 「実行前の確認」でNWChemやMPIランチャーが見つからない | ターミナルで `command -v nwchem mpirun` を確認します。`brew` をPATHに加える設定（`~/.zprofile`）を行ってから、MolFormaを起動したターミナルを開き直します。それでも見つからない場合は、「3 実行設定」で `/opt/homebrew/bin/nwchem`・`/opt/homebrew/bin/mpirun` を指定します |
| NWChemの計算が、基底関数のライブラリが見つからないというエラーで止まる | `/opt/homebrew/etc/nwchemrc` があるか確認します。ない場合は `brew reinstall nwchem` で入れ直します |
| 日本語表示なのに、ファイル選択などQtの標準ダイアログに英語が残る | `brew install qttranslations` で入れます |
| ビルド中にメモリーが不足する | `cmake --build build -j 2` のように並列数を減らします |

## 確認した構成

2026-11-04に、Apple siliconのMac（macOS 27.0.1、arm64、10コア・メモリー32 GB）で、次のことを確かめました。

- 上のパッケージを入れて、ソースを構成・ビルドできること（RDKit・VTKとも有効）。ビルドは `-j 8` で約1分で、警告は出ませんでした。
- `cmake --install` で入れた `molforma` が起動し、プロジェクトファイルを開いた画面の写真（`--screenshot`）を撮れること。設定が `~/Library/Preferences/MolForma/MolForma.ini` に保存されること。
- `~/.nwchemrc` を置かずに `mpirun -np 2 nwchem` で水素分子のHF/6-31G*計算が完了し、SCFエネルギーが −1.126755317965 Hartree になること。MolFormaの実行設定のテスト（実際のNWChemを使う任意のテスト、[「3 実行設定」](../features/execution.md)）で、MPI 2プロセスの水のHF/STO-3G計算が完了し、SCFエネルギーが −74.945628203255 Hartree になること。
- CTestの16件すべてが通ること（並列）。テストの間、`~/Library/Preferences`・`~/Library/Application Support` のMolFormaの設定と自動保存が変わらないこと。

| 項目 | 版 |
| --- | --- |
| Qt（`qtbase`） | 6.12.0 |
| Open Babel | 3.2.1 |
| Eigen | 5.0.1 |
| RDKit・Boost | 2026.09.1・1.92.0 |
| VTK | 9.7.1 |
| Apple clang・CMake・Ninja | 21.0.0・4.4.4・1.13.2 |
| NWChem | 7.3.1 |
| Open MPI | 5.0.11 |

IntelのMac（Homebrewは `/usr/local` に入ります）、macOS 26以前、画面を操作しての長時間の利用は確認していません。

関連: [OS別インストール・必要パッケージ](../installation.md)、[3D描画](../features/rendering.md)、[「3 実行設定」](../features/execution.md)
