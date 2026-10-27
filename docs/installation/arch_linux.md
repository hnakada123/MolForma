# Arch Linuxでのインストール

MolFormaをArch Linuxでソースからビルドしてインストールし、計算に使うNWChemを用意する手順です。依存パッケージは公式リポジトリ（core・extra）から、NWChemはAURから入れます。

対象はArch Linux（x86-64）とMolForma 0.1.0です。2026-10-27に手順を確認しました（[確認した構成](#確認した構成)）。

MolFormaのパッケージは公式リポジトリにもAURにもありません（2026-10-27時点）。GitHubのソースからビルドし、`cmake --install` で `/usr/local` に配置します。pacmanでは管理しないため、更新と削除は[更新・削除](#更新削除)の手順で行います。

## 必要なパッケージ

| 区分 | パッケージ | 入手元 | 用途 |
| --- | --- | --- | --- |
| 必須 | `base-devel`、`git`、`cmake`、`ninja` | core・extra | ビルド（`base-devel` にGCC・pkgconfが含まれる） |
| 必須 | `qt6-base` | extra | 画面（Qt Widgets）、テスト（Qt Test） |
| 必須 | `eigen` | extra | 座標・幾何の計算 |
| 必須 | `openbabel` | extra | 構造ファイルの読み書き、力場による整形、SMILESからの3D構造（フォーマットプラグインと力場データを含む） |
| 任意 | `rdkit` | extra | 厳密な立体判定（R/S・E/Z）、残基認識 |
| 任意 | `vtk`、`fast_float`、`nlohmann-json`、`openmpi` | extra | 3D画面をVTK（OpenGL）で描画 |
| 任意 | `qt6-translations` | extra | 日本語表示のとき、ファイル選択などQtの標準ダイアログも日本語にする（なくても「保存」「キャンセル」などの標準ボタンは日本語） |
| 任意 | `noto-fonts-cjk` | extra | 日本語のフォント（日本語のフォントが入っていない場合） |
| 計算 | `nwchem` | AUR | 計算の実行。`openmpi` と、AURの `scalapack` も依存パッケージとして入る |

- **VTK**: `vtk` だけでは、CMakeがVTKの開発用の設定を読み込めず、MolFormaはQPainter（CPU）で描画する構成になります。VTKの設定が参照する `fast_float`・`nlohmann-json`（どちらもヘッダーのみ）と `openmpi` も入れてください。`openmpi` がないと、VTKを使う構成でCMakeがエラーで止まります。VTKで描画するには、OpenGL 3.2以上を使えるGPUドライバーも必要です。
- **RDKit**: `rdkit` はPythonなどにも依存しますが、MolFormaはC++ライブラリだけを使います。
- **NWChem**: 入力の作成・保存（「ジョブとして保存」）はNWChemがなくても使えます。

## 1. パッケージを入れる

システムを更新し、必須のパッケージを入れます。

```sh
sudo pacman -Syu --needed base-devel git cmake ninja qt6-base eigen openbabel
```

任意の機能を使う場合は、続けて次を入れます。不要なものは省けます。

```sh
sudo pacman -S --needed rdkit vtk fast_float nlohmann-json openmpi qt6-translations noto-fonts-cjk
```

## 2. NWChemを入れる（AUR）

AURの `nwchem` は、依存する `scalapack` もAURにあるため、AURの依存を解決できるヘルパー（ここではyay）で入れます。ソースからビルドするため時間がかかります。

```sh
yay -S nwchem
```

yayが入っていない場合は、先に通常のユーザーでビルドして入れます。表示されるPKGBUILDを確認してから進めてください。

```sh
git clone https://aur.archlinux.org/yay.git
cd yay
less PKGBUILD
makepkg -si
```

NWChemは `/usr/bin/nwchem` に、基底関数のライブラリは `/usr/share/nwchem/libraries/` に入ります。基底関数のライブラリはそのまま使えるため、`~/.nwchemrc` や `NWCHEM_BASIS_LIBRARY` の設定は不要です。MolFormaの「3 実行設定」の既定（実行ファイル `nwchem`、MPIで起動、MPIランチャー `mpirun`、2プロセス）のままで、このNWChemを起動できます。

## 3. ソースを取得してビルドする

ソースを取得し、Releaseでビルドします。取得したフォルダーと `build` は、更新と削除で使うので残しておきます。

```sh
git clone https://github.com/hnakada123/MolForma.git
cd MolForma
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
```

最初のコマンド（構成）の出力の `MolForma rendering:` の行で、3D描画の方式を確認できます。

| 出力 | 意味 |
| --- | --- |
| `MolForma rendering: VTK 9.7.1 (Qt6), with QPainter fallback` | VTKで描画します（OpenGLを使えない環境ではQPainter） |
| `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` | VTKを使いません。直前の `VTK development dependency unavailable:` の行に足りないものが出ます（`FastFloat::fast_float` は `fast_float`、`nlohmann_json::nlohmann_json` は `nlohmann-json`） |

足りないパッケージを入れたら、構成のコマンドからやり直します。`-j 4` は同時に動かすコンパイルの数です。メモリーが足りない場合は減らしてください。

テストを実行する場合は次のとおりです（任意）。NWChem・ネットワークは使いません。テストが不要な場合は、構成のコマンドに `-DBUILD_TESTING=OFF` を付けるとビルドが短くなります。

```sh
ctest --test-dir build --output-on-failure
```

インストールせずに `./build/molforma` で起動することもできます。

## 4. インストールする

`/usr/local` に配置し、ファイルの種類（MIME型）の情報を更新します。pacmanのフックが更新するのは `/usr/share` だけなので、`/usr/local` の分はここで更新します。

```sh
sudo cmake --install build
sudo update-mime-database /usr/local/share/mime
sudo update-desktop-database /usr/local/share/applications
```

`update-mime-database` は `shared-mime-info`、`update-desktop-database` は `desktop-file-utils` のコマンドです。次のファイルを配置します。

| 配置先 | 内容 |
| --- | --- |
| `/usr/local/bin/molforma` | 実行ファイル |
| `/usr/local/share/applications/molforma.desktop` | アプリケーションメニューの項目 |
| `/usr/local/share/icons/hicolor/` | アイコン（`scalable` と `256x256`） |
| `/usr/local/share/mime/packages/molforma.xml` | `.kgui.json`（プロジェクト）と `.nw`（NWChem入力）のファイルの種類 |
| `/usr/local/share/molforma/` | LICENSE・NOTICE・README とマニュアルのMarkdown（マニュアルは実行ファイルにも組み込み済み） |

管理者権限を使わない場合は、`cmake --install build --prefix ~/.local` でホームディレクトリに配置できます。このときは `~/.local/bin` をPATHに加え、`update-mime-database ~/.local/share/mime` を実行してください。

## 5. 起動して確認する

端末で `molforma` を実行するか、アプリケーションメニューの「MolForma」を選びます。KDE Plasmaでメニューにすぐ出ない場合は `kbuildsycoca6` を実行します。表示言語はシステムの言語に従います（「編集 → 環境設定」の「表示言語 / Language」で切り替え、再起動後に反映）。

1. 「ヘルプ → MolFormaについて」を開き、「立体判定・残基認識（RDKit）」と「3D描画」を確認します。RDKitとVTKを入れた場合は「有効」「VTK（OpenGL）」と表示されます。
2. 「3 実行設定」を開き、「NWChem実行ファイル」と「MPIランチャー」の欄の下に、PATHで見つかった `/usr/bin/nwchem`・`/usr/bin/mpirun` が表示されることを確認します。
3. 小さな分子で計算してみます。手順は[題材1 原子と結合から組む](../recipes/atoms_and_bonds.md)にあります。

## 更新・削除

### 更新する

ソースを更新してビルドし直し、インストールします。

```sh
cd MolForma
git pull --ff-only
cmake --build build -j 4
sudo cmake --install build
```

MolFormaはpacmanで管理していないため、`pacman -Syu` でQt・Open Babel・RDKit・VTKなどの共有ライブラリが新しい版になっても再ビルドされません。`molforma: error while loading shared libraries` と出て起動しなくなった場合は、`build` を削除して[3. ソースを取得してビルドする](#3-ソースを取得してビルドする)の構成からやり直し、インストールし直してください。

### 削除する

インストールしたファイルを `build` に記録された一覧から削除し、残ったフォルダー `/usr/local/share/molforma` も削除します。

```sh
cd MolForma
sudo xargs -d '\n' rm -f -- < build/install_manifest.txt
sudo rm -r /usr/local/share/molforma
sudo update-mime-database /usr/local/share/mime
sudo update-desktop-database /usr/local/share/applications
```

アプリの設定（`~/.config/MolForma/`）、自動保存（`~/.local/share/MolForma/`）、計算のジョブ（既定は `~/MolForma/jobs`）は残ります。不要なら手動で削除してください。NWChemは `sudo pacman -Rs nwchem` で削除できます。

## 困ったとき

| 症状 | 確認・対応 |
| --- | --- |
| 構成が `Qt6`・`Eigen3`・`openbabel-3` が見つからないというエラーで止まる | [1. パッケージを入れる](#1-パッケージを入れる)の必須のパッケージを入れ、構成からやり直します |
| `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` と出る | `VTK development dependency unavailable:` の行に出たパッケージ（多くは `fast_float`・`nlohmann-json`）を入れ、構成からやり直します |
| 構成が `VTK-targets.cmake` のエラーで止まる | `openmpi` を入れます。VTKを使わない場合は、構成に `-DMOLFORMA_ENABLE_VTK=OFF` を付けます |
| 3D画面の左上に「描画: QPainter（CPU）· OpenGLを使えないため簡易表示」と出る | OpenGL 3.2以上を使えません。GPUドライバーを確認します（`mesa-utils` の `glxinfo -B` で、使われているOpenGLの実装と版が分かります）。リモートデスクトップや仮想マシンでは使えない場合があります |
| VTKで描画しているのに回転が遅い | `glxinfo -B` で `llvmpipe` などと出る場合は、ソフトウェアのOpenGLで描画しています |
| 日本語が四角で表示される | `noto-fonts-cjk` などの日本語フォントを入れます |
| 日本語表示なのに、ファイル選択などQtの標準ダイアログに英語が残る | `qt6-translations` を入れます |
| 「実行前の確認」でNWChemやMPIランチャーが見つからない | 端末で `command -v nwchem mpirun` を確認します。NWChemを別の場所に入れた場合は、「3 実行設定」でパスを指定します |
| `error while loading shared libraries` で起動しない | システム更新で共有ライブラリが変わりました。[更新する](#更新する)のとおりビルドし直します |
| ビルド中にメモリーが不足する | `cmake --build build -j 2` のように並列数を減らします |

## 確認した構成

2026-10-27に、次の構成のArch Linux（x86-64）で、GitHubのソースを新たに取得してビルドし、CTestを実行しました。インストールは一時ディレクトリへの配置（`DESTDIR`）で配置先を確認しています。VTKの構成は、`fast_float`・`nlohmann-json` のパッケージの内容を参照させてビルドしました。

| 項目 | 版 |
| --- | --- |
| Qt（`qt6-base`） | 6.12.0 |
| Open Babel | 3.2.1 |
| Eigen | 5.0.1 |
| RDKit | 2026.09.1 |
| VTK | 9.7.1 |
| GCC・CMake・Ninja | 16.2.1・4.4.4・1.13.2 |
| NWChem（AURの `nwchem`） | 7.3.1 |
| Open MPI | 5.0.11 |

NWChemは `~/.nwchemrc` を置かずに `mpirun -np 2 nwchem` で起動し、水素分子のHF/6-31G*計算が完了することを確かめました。依存パッケージを入れ済みの環境での確認で、新しい環境でのpacman・yayによる依存の解決は含みません。

関連: [OS別インストール・必要パッケージ](../installation.md)、[3D描画](../features/rendering.md)、[「3 実行設定」](../features/execution.md)
