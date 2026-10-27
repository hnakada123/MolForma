# Arch Linuxでのインストール

MolFormaをArch Linuxのパッケージとして作り、pacmanでインストールする手順です。リポジトリの `packaging/arch/PKGBUILD` から `makepkg` でパッケージ `molforma-git` を作ります。依存パッケージは公式リポジトリ（core・extra）から自動で入り、計算に使うNWChemはAURから入れます。

対象はArch Linux（x86-64）とMolForma 1.0.0です。2026-10-28に手順を確認しました（[確認した構成](#確認した構成)）。

MolFormaのパッケージは公式リポジトリにもAURにもありません（2026-10-28時点）。PKGBUILDはGitHubのmainブランチのソースを取得してビルドします。インストールしたパッケージはpacmanで管理するので、削除は `pacman -R` で行えます。

## 必要なパッケージ

次のパッケージは、`makepkg -s` が公式リポジトリから自動で入れます。

| 区分 | パッケージ | 用途 |
| --- | --- | --- |
| 実行時 | `qt6-base` | 画面（Qt Widgets） |
| 実行時 | `openbabel` | 構造ファイルの読み書き、力場による整形、SMILESからの3D構造（フォーマットプラグインと力場データを含む） |
| 実行時 | `rdkit` | 厳密な立体判定（R/S・E/Z）、残基認識 |
| 実行時 | `vtk` | 3D画面をVTK（OpenGL）で描画 |
| ビルド時 | `git`、`cmake`、`ninja`、`eigen` | ソースの取得とビルド |
| ビルド時 | `boost`、`fast_float`、`nlohmann-json`、`openmpi` | RDKit・VTKのCMakeの設定が参照するもの |

次のパッケージは、必要に応じて自分で入れます。

| 区分 | パッケージ | 入手元 | 用途 |
| --- | --- | --- | --- |
| 必須 | `base-devel` | core | `makepkg` でのパッケージ作成（GCCを含む） |
| 計算 | `nwchem` | AUR | 計算の実行。`openmpi` と、AURの `scalapack` も依存パッケージとして入る |
| 任意 | `qt6-translations` | extra | 日本語表示のとき、ファイル選択などQtの標準ダイアログも日本語にする（なくても「保存」「キャンセル」などの標準ボタンは日本語） |
| 任意 | `noto-fonts-cjk` | extra | 日本語のフォント（日本語のフォントが入っていない場合） |

- **RDKitとVTK**: PKGBUILDはRDKitとVTKを使う構成でビルドし、CMakeがどちらかを使えないときはビルドを止めます。VTKで描画するには、OpenGL 3.2以上を使えるGPUドライバーも必要です。`rdkit` はPythonなどにも依存しますが、MolFormaはC++ライブラリだけを使います。
- **NWChem**: 入力の作成・保存（「ジョブとして保存」）はNWChemがなくても使えます。

## 1. 準備する

システムを更新し、パッケージの作成に使う `base-devel` と `git` を入れます。任意のパッケージを使う場合は、続けて入れます。

```sh
sudo pacman -Syu --needed base-devel git
sudo pacman -S --needed qt6-translations noto-fonts-cjk
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

## 3. パッケージを作ってインストールする

作業用のフォルダーにPKGBUILDを取得し、内容を確認してから `makepkg` を実行します。作業用のフォルダーは、更新のときにも使います。

```sh
mkdir -p ~/.cache/molforma-arch
cd ~/.cache/molforma-arch
curl -LO https://raw.githubusercontent.com/hnakada123/MolForma/main/packaging/arch/PKGBUILD
less PKGBUILD
makepkg -si
```

`makepkg -si` は次のことを順に行います。

1. 足りない依存パッケージをpacmanで入れます（`-s`）。管理者のパスワードを聞かれます。
2. GitHubのmainブランチのソースを取得してビルドします。途中で `-- MolForma rendering: VTK 9.7.1 (Qt6), with QPainter fallback` の行が出れば、VTKで描画する構成です。CMakeがVTKやRDKitを使えない場合は、`==> ERROR: CMake cannot use VTK` などと出てここで止まります（[困ったとき](#困ったとき)）。
3. テストを実行します。NWChem・ネットワークは使いません。省く場合は `makepkg -si --nocheck` とします。
4. パッケージ（`molforma-git-1.0.0.r<コミット数>.g<コミットID>-1-x86_64.pkg.tar.zst`）を作り、pacmanでインストールします（`-i`）。

ビルドはCPUのコアをすべて使います。メモリーが足りない場合は、`CMAKE_BUILD_PARALLEL_LEVEL=2 makepkg -si` のように同時に動かすコンパイルの数を減らしてください。

パッケージを作らずに開発用にビルドする手順は、[README](https://github.com/hnakada123/MolForma#readme)にあります。

## 4. 起動して確認する

パッケージは次のファイルを配置します（`pacman -Ql molforma-git` で一覧できます）。ファイルの種類やメニューの情報は、pacmanのフックが自動で更新します。

| 配置先 | 内容 |
| --- | --- |
| `/usr/bin/molforma` | 実行ファイル |
| `/usr/share/applications/molforma.desktop` | アプリケーションメニューの項目 |
| `/usr/share/icons/hicolor/` | アイコン（`scalable` と `256x256`） |
| `/usr/share/mime/packages/molforma.xml` | `.kgui.json`（プロジェクト）と `.nw`（NWChem入力）のファイルの種類 |
| `/usr/share/molforma/` | LICENSE・NOTICE・README とマニュアルのMarkdown（マニュアルは実行ファイルにも組み込み済み） |

端末で `molforma` を実行するか、アプリケーションメニューの「MolForma」を選びます。KDE Plasmaでメニューにすぐ出ない場合は `kbuildsycoca6` を実行します。表示言語はシステムの言語に従います（「編集 → 環境設定」の「表示言語 / Language」で切り替え、再起動後に反映）。

1. 「ヘルプ → MolFormaについて」を開き、版が「MolForma 1.0.0」で、「立体判定・残基認識（RDKit）: 有効」「3D描画: VTK（OpenGL）」と表示されることを確認します。OpenGLを使えない環境では「3D描画: QPainter（CPU）」になります。
2. 「3 実行設定」を開き、「NWChem実行ファイル」と「MPIランチャー」の欄の下に、PATHで見つかった `/usr/bin/nwchem`・`/usr/bin/mpirun` が表示されることを確認します。
3. 小さな分子で計算してみます。手順は[題材1 原子と結合から組む](../recipes/atoms_and_bonds.md)にあります。

## 更新・削除

### 更新する

PKGBUILDを取得し直してから、パッケージを作り直します。`makepkg` はmainブランチの最新のソースを取得し、新しい版のパッケージを作ってインストールします。

```sh
cd ~/.cache/molforma-arch
curl -LO https://raw.githubusercontent.com/hnakada123/MolForma/main/packaging/arch/PKGBUILD
makepkg -si
```

AURに公開していないため、`pacman -Syu` や `yay -Syu` ではMolFormaは更新されません。`pacman -Syu` でQt・Open Babel・RDKit・VTKなどの共有ライブラリが新しい版になり、`molforma: error while loading shared libraries` と出て起動しなくなった場合は、ソースに変更がなくても `makepkg -sif` で作り直してください（`-f` は、作成済みのパッケージがあっても作り直す指定です）。

### 削除する

```sh
sudo pacman -R molforma-git
```

アプリの設定（`~/.config/MolForma/`）、自動保存（`~/.local/share/MolForma/`）、計算のジョブ（既定は `~/MolForma/jobs`）と作業用のフォルダー（`~/.cache/molforma-arch`）は残ります。不要なら手動で削除してください。ビルドのために入った依存パッケージは、`pacman -Qdtq` でほかから使われていないものを確認できます。NWChemは `sudo pacman -Rs nwchem` で削除できます。

## 困ったとき

| 症状 | 確認・対応 |
| --- | --- |
| `==> ERROR: CMake cannot use VTK` で止まる | その前の `VTK development dependency unavailable:` の行に足りないものが出ます（`FastFloat::fast_float` は `fast_float`、`nlohmann_json::nlohmann_json` は `nlohmann-json`）。そのパッケージを入れて `makepkg -si` をやり直します |
| `==> ERROR: CMake cannot use RDKit` で止まる | `boost` が入っているか確認し、`makepkg -si` をやり直します |
| テストが失敗してパッケージができない | 失敗したテストの出力を添えて、[GitHubのIssues](https://github.com/hnakada123/MolForma/issues)で知らせてください。テストを省いて作るには `makepkg -si --nocheck` とします |
| ビルド中にメモリーが不足する | `CMAKE_BUILD_PARALLEL_LEVEL=2 makepkg -si` のように並列数を減らします |
| 3D画面の左上に「描画: QPainter（CPU）· OpenGLを使えないため簡易表示」と出る | OpenGL 3.2以上を使えません。GPUドライバーを確認します（`mesa-utils` の `glxinfo -B` で、使われているOpenGLの実装と版が分かります）。リモートデスクトップや仮想マシンでは使えない場合があります |
| VTKで描画しているのに回転が遅い | `glxinfo -B` で `llvmpipe` などと出る場合は、ソフトウェアのOpenGLで描画しています |
| 日本語が四角で表示される | `noto-fonts-cjk` などの日本語フォントを入れます |
| 日本語表示なのに、ファイル選択などQtの標準ダイアログに英語が残る | `qt6-translations` を入れます |
| 「実行前の確認」でNWChemやMPIランチャーが見つからない | 端末で `command -v nwchem mpirun` を確認します。NWChemを別の場所に入れた場合は、「3 実行設定」でパスを指定します |
| `error while loading shared libraries` で起動しない | システム更新で共有ライブラリが変わりました。[更新する](#更新する)のとおり `makepkg -sif` で作り直します |

## 確認した構成

2026-10-28に、次の構成のArch Linux（x86-64）で、このPKGBUILDから `makepkg` でパッケージを作り、テスト（PKGBUILDの `check()`）が通ることと、パッケージの中身・依存関係を確かめました。依存パッケージはpacmanで入れてあり、`makepkg` の実行時・ビルド時の依存の確認も通っています。

| 項目 | 版 |
| --- | --- |
| Qt（`qt6-base`） | 6.12.0 |
| Open Babel | 3.2.1 |
| Eigen | 5.0.1 |
| RDKit・Boost | 2026.09.1・1.92.0 |
| VTK | 9.7.1 |
| fast_float・nlohmann-json | 8.3.2・3.12.0 |
| GCC・CMake・Ninja | 16.2.1・4.4.4・1.13.2 |
| NWChem（AURの `nwchem`） | 7.3.1 |
| Open MPI | 5.0.11 |

NWChemは `~/.nwchemrc` を置かずに `mpirun -np 2 nwchem` で起動し、水素分子のHF/6-31G*計算が完了することを確かめました。依存パッケージを入れ済みの環境での確認で、作ったパッケージのpacmanでのインストール（`-i`）と、新しい環境での `makepkg -s`・yayによる依存の自動インストールは含みません。パッケージの実行ファイルは、展開して起動できることと版が1.0.0であることを確かめています。

関連: [OS別インストール・必要パッケージ](../installation.md)、[3D描画](../features/rendering.md)、[「3 実行設定」](../features/execution.md)
