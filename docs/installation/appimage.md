# AppImageの作成（Linux共通）

MolFormaを、Linuxのディストリビューションによらず動く1つの実行ファイル（AppImage）にする手順です。Dockerの中のUbuntu 24.04でビルドし、MolFormaが使うQt・Open Babel・RDKit・VTKなどのライブラリを1つのファイルにまとめます。作ったAppImageは、インストールせずに、実行の許可を付けるだけで起動できます。

AppImageを作るPCは、Dockerが使えるx86-64のLinuxであれば、ディストリビューションを問いません。AppImageを動かすPCは、x86-64で、glibc 2.38以上とGCC 13以降の `libstdc++` を持つLinuxのデスクトップです（Ubuntu 24.04以降、Debian 13以降、Fedora 39以降、Arch Linuxなど）。対象はMolForma 1.0.0です。2026-10-28に手順を確認しました（[確認した構成](#確認した構成)）。

配布しているAppImageはまだありません（2026-10-28時点）。この手順で作ってください。

## AppImageに入るもの

| 区分 | 内容 |
| --- | --- |
| 入るもの | MolForma、Qt 6.4.2（xcb・offscreenの表示部品と、Qt自身の翻訳）、Open Babel 3.1.1（フォーマットプラグインと力場データ）、RDKit 2023.09.3、VTK 9.7.1（Qt 6向けにビルド）、Boostなどそれらが使うライブラリ、各ライブラリのライセンス（`usr/share/doc`） |
| 動かすPCのものを使う | glibc・`libstdc++`、OpenGL（GPUドライバー）、X11の基本ライブラリ（`libX11`・`libxcb` など）、fontconfig・FreeType・HarfBuzzとフォント、NWChemとMPI |

「動かすPCのものを使う」ライブラリは、デスクトップ環境には通常入っています。日本語の表示には、日本語のフォントがPCに入っている必要があります。

## 1. Dockerを用意する

Dockerの入れ方はディストリビューションごとに異なります（Arch Linuxは `sudo pacman -S docker`、Ubuntuは `sudo apt install docker.io`）。入れたら、Dockerを起動し、一般ユーザーで使えるように `docker` グループに加えます。グループの変更は、ログインし直すと有効になります。

```sh
sudo systemctl enable --now docker
sudo usermod -aG docker "$USER"
```

`docker` グループのユーザーは、Dockerを通じて管理者と同じ操作ができます。気になる場合はグループに加えず、以下の `docker` コマンドを `sudo docker` として実行してください。

## 2. ビルド環境を作る

ソースを取得し、AppImageのビルド環境（Dockerのイメージ `molforma-appimage`）を作ります。

```sh
git clone https://github.com/hnakada123/MolForma.git
cd MolForma
docker build -t molforma-appimage packaging/appimage
```

`packaging/appimage/Dockerfile` は次のことを行います。

1. Ubuntu 24.04に、ビルドに使う開発用パッケージを入れます。Ubuntu 24.04を使うのは、glibcが古めで、動かせるディストリビューションが広くなるためです。
2. VTK 9.7.1を、MolFormaが使う部品だけQt 6向けにソースからビルドします。Ubuntu 24.04のVTKはQt 5向けのため、そのままでは使えません。
3. AppImageの作成に使う道具（linuxdeploy・linuxdeploy-plugin-qt・appimagetool・AppImageの起動部分）を取得します。

ダウンロードするものは版とSHA-256を固定しています。VTKのビルドに時間がかかり、16コアのPC（同時に4つのコンパイル）で約23分かかりました。できるイメージは約2 GBです。作るのは最初の1回だけで、以後は使い回せます。

VTKのビルドで同時に動かすコンパイルの数は、既定で4です。すべてのコアを使うとデスクトップが応答しにくくなることがあるため、控えめにしています。変える場合は `docker build --build-arg JOBS=8 -t molforma-appimage packaging/appimage` のように指定します。

## 3. AppImageを作る

取得したソースのフォルダーで、次を実行します。`build-appimage/MolForma-1.0.0-x86_64.AppImage`（約100 MB）ができます。所要時間は3分ほどです。

```sh
mkdir -p build-appimage
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src:ro" -v "$PWD/build-appimage:/out" molforma-appimage
```

ソースのフォルダーは読み取り専用で渡し、ビルドはコンテナの中で行います。できたファイルは自分のユーザーの持ち物になります。同時に動かすコンパイルの数（既定は4）は、`docker run` に `-e JOBS=8` のように指定して変えられます。

`packaging/appimage/build.sh` は次のことを行います。

1. MolFormaをビルドし、AppImageの中身（AppDir）に配置します。RDKitかVTKを使えない場合は、機能の欠けたAppImageを作らずに止まります。
2. Open Babelのフォーマットプラグインと力場データをコピーし、起動時に `BABEL_LIBDIR`・`BABEL_DATADIR` をその場所に設定します。
3. linuxdeployで、MolFormaとプラグインが使うライブラリ、Qtの表示部品と翻訳を集めます。
4. 各ライブラリのライセンスを `usr/share/doc` にコピーします。
5. appimagetoolで1つのファイルにまとめます。

## 4. 起動する

実行の許可を付けて起動します。ファイルを指定して開くこともできます。

```sh
chmod +x MolForma-1.0.0-x86_64.AppImage
./MolForma-1.0.0-x86_64.AppImage
./MolForma-1.0.0-x86_64.AppImage molecule.xyz
```

- **FUSE**: AppImageは、起動時に自分自身をFUSEでマウントします。`fusermount3`（`fuse3` パッケージ）か `fusermount` が必要です。多くのデスクトップには入っています。`libfuse2` は不要です。
- **Wayland**: AppImageに入っている表示部品はxcbです。Waylandのデスクトップでは、XWaylandを通して表示します。
- **メニュー**: AppImageは自分ではアプリケーションメニューに登録しません。メニューから起動したい場合は、AppImageを管理するアプリ（Gear Lever、AppImageLauncherなど）を使ってください。
- **MCPサーバー**: MCPクライアントには、AppImageのパスに `--mcp`（GUIを表示する場合は `--mcp-gui`）を付けたものを、サーバーの起動コマンドとして登録します（[MCPサーバー](../mcp.md)）。

起動したら、「ヘルプ → MolFormaについて」で、版が「MolForma 1.0.0」で、「立体判定・残基認識（RDKit）: 有効」「3D描画: VTK（OpenGL）」と表示されることを確認します。OpenGLを使えない環境では「3D描画: QPainter（CPU）」になります。

計算に使うNWChemは、AppImageに入っていません。ディストリビューションのものを入れてください（Arch Linuxは[AURの `nwchem`](arch_linux.md#2-nwchemを入れるaur)、Ubuntuは[`apt install nwchem`](ubuntu.md#2-nwchemを入れる)）。「3 実行設定」の既定（実行ファイル `nwchem`、MPIで起動、MPIランチャー `mpirun`、2プロセス）のままで起動できます。

## 更新・削除

### 更新する

ソースを更新し、[3. AppImageを作る](#3-appimageを作る)をやり直します。ビルド環境のイメージはそのまま使えます。`packaging/appimage/` が変わった場合は、[2. ビルド環境を作る](#2-ビルド環境を作る)もやり直してください（変わっていない段は、Dockerが前回の結果を使います）。

```sh
cd MolForma
git pull --ff-only
docker build -t molforma-appimage packaging/appimage
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src:ro" -v "$PWD/build-appimage:/out" molforma-appimage
```

### 削除する

AppImageのファイルを削除します。アプリの設定（`~/.config/MolForma/`）、自動保存（`~/.local/share/MolForma/`）、計算のジョブ（既定は `~/MolForma/jobs`）は残ります。不要なら手動で削除してください。ビルド環境は `docker rmi molforma-appimage` で削除できます。

## 配布するとき

作ったAppImageをほかの人に配る場合は、同梱したソフトウェアのライセンスに従ってください。ライセンスの文面はAppImageの `usr/share/doc` にあり、`./MolForma-1.0.0-x86_64.AppImage --appimage-extract` で展開すると読めます。

- **MolForma（GPL-2.0-or-later）、Open Babel（GPL-2.0-only）、Qt**: Open Babelとリンクしたバイナリでは、QtもGPL-2.0の条件で使います（リポジトリの `NOTICE`）。バイナリを配る場合は、対応するソースも入手できるようにします。MolFormaは、AppImageを作ったコミットのソースを示してください。Open Babel・Qtなど同梱したUbuntuのパッケージのソースは、Ubuntu 24.04のソースパッケージです。VTKは、vtk.orgの9.7.1のソースです。
- **そのほか（RDKit・VTK・Boostなど）**: BSDなどのライセンスで、文面を同梱すれば配布できます。

## 困ったとき

| 症状 | 確認・対応 |
| --- | --- |
| `permission denied while trying to connect to the Docker daemon socket` と出る | `docker` グループに加えた後でログインし直したか確認します。または `sudo docker …` で実行します |
| `docker build` が `sha256sum` の不一致で止まる | ダウンロードしたファイルが、固定した版と違います。ネットワークの途中で書き換えられた可能性もあるため、そのまま進めずに原因を確かめてください |
| ビルド中にデスクトップが応答しにくくなる、メモリーが不足する | `--build-arg JOBS=2`（`docker build`）や `-e JOBS=2`（`docker run`）で、同時に動かすコンパイルの数を減らします |
| `CMake cannot use VTK` または `CMake cannot use RDKit` で止まる | ビルド環境のイメージが古いか、作成が途中で失敗しています。[2. ビルド環境を作る](#2-ビルド環境を作る)をやり直します |
| `version 'GLIBC_2.38' not found` や `GLIBCXX_3.4.32' not found` で起動しない | 動かすPCのglibcや `libstdc++` が古すぎます。Ubuntu 24.04以降など、新しいディストリビューションで使ってください |
| FUSEでマウントできず起動しない | `fuse3` パッケージ（`fusermount3`）を入れます。入れられない場合は、`./MolForma-1.0.0-x86_64.AppImage --appimage-extract-and-run` で、展開してから起動できます |
| 3D画面の左上に「描画: QPainter（CPU）· OpenGLを使えないため簡易表示」と出る | OpenGL 3.2以上を使えません。GPUドライバーを確認します。リモートデスクトップや仮想マシンでは使えない場合があります |
| 日本語が四角で表示される | 日本語のフォントをPCに入れます（Arch Linuxは `noto-fonts-cjk`、Ubuntu・Debianは `fonts-noto-cjk`、Fedoraは `google-noto-sans-cjk-fonts` など） |
| 「実行前の確認」でNWChemやMPIランチャーが見つからない | 端末で `command -v nwchem mpirun` を確認します。NWChemを別の場所に入れた場合は、「3 実行設定」でパスを指定します |

## 確認した構成

2026-10-28に、この手順でAppImageを作り、次のことを確かめました。

- **ビルド環境**: Ubuntu 24.04、GCC 13.3.0、CMake 3.28.3、Qt 6.4.2、Open Babel 3.1.1、RDKit 2023.09.3、VTK 9.7.1。道具はlinuxdeploy 1-alpha-20251107-1、linuxdeploy-plugin-qt 1-alpha-20250213-1、appimagetool 1.9.1、AppImageの起動部分（type2-runtime）20251108です。同じ環境で、MolFormaのCTest（14件）もすべて通りました。
- **Arch Linux（作業したPC）**: AppImageを展開せずに（FUSEで）起動しました。SMILESから3D構造を作り（Open Babelとワーカー）、NWChem 7.3.1でMPI 2プロセスの水のHF計算が完了しました。
- **Dockerのコンテナ**: Ubuntu 24.04・Debian 13・Fedora 39・Fedora 40のコンテナで、展開して起動する形（`--appimage-extract-and-run`）で確かめました。コンテナには、デスクトップ相当のライブラリと、XvfbとMesaのソフトウェアOpenGLを入れています。版が1.0.0であること、SMILESから3D構造を作れること、xcbで画面を表示して画面写真を書き出せることを確かめました（Ubuntu 24.04とFedora 40では、3D画面がVTKで描画されていることを画像でも確認）。
- **Waylandのセッション**: Waylandのセッションに見える環境（`QT_QPA_PLATFORM` が未設定、`wayland;xcb`、`wayland`）でも、xcbで起動しました。
- **Ubuntu 26.04のコンテナ（`nwchem` パッケージ入り）**: NWChemでの計算が完了しました。

実機のGPUでのVTKの描画と、Waylandのデスクトップでの実際の表示は、AppImageでは確認していません。

関連: [OS別インストール・必要パッケージ](../installation.md)、[Arch Linuxでのインストール](arch_linux.md)、[Ubuntuでのインストール](ubuntu.md)
