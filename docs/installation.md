# OS別インストール・必要パッケージ

MolFormaを動かすのに必要なソフトウェアと、OSごとのインストール手順です。MolFormaはGitHubで公開しているソースからビルドします（Windowsではフォルダーにインストール）。計算には、別にインストールしたNWChemを使います。使用するOSのページを選んでください。

## OSごとの手順

| OS | 手順 | 主な内容 |
| --- | --- | --- |
| Arch Linux | [Arch Linuxでのインストール](installation/arch_linux.md) | PKGBUILDからパッケージを作ってpacmanでインストール（依存パッケージも自動で導入）、AURからNWChem。VTK描画の確認、更新・削除、困ったとき |
| Ubuntu 26.04 LTS | [Ubuntuでのインストール](installation/ubuntu.md) | aptで開発用パッケージとNWChemを入れ、ソースからビルドしてdebパッケージを作り、aptでインストール。VTK描画の確認、更新・削除、困ったとき |
| Windows 11 | [Windowsでのインストール](installation/windows.md) | Visual Studio（MSVC）とvcpkgでビルドし、Open Babel・RDKit・VTKもソースからビルドして、Qtなどを含めたフォルダーにインストール。NWChemはWSLのUbuntuに入れ、付属のバッチファイルから起動。更新・削除、困ったとき |
| Linux（ディストリビューション共通） | [AppImageの作成](installation/appimage.md) | Dockerの中でビルドし、Qt・Open Babel・RDKit・VTKを1つのファイルにまとめたAppImageを作る。glibc 2.38以上のLinux（Ubuntu 24.04以降、Debian 13以降、Fedora 39以降など）でインストールせずに起動。配布するときのライセンス、困ったとき |

ほかのOSの手順は、用意でき次第このページに追加します。

## 必要なソフトウェア

OSによらず、次のソフトウェアを使います。実行時にPythonは必要ありません。

| 区分 | ソフトウェア | 用途 |
| --- | --- | --- |
| 必須 | C++20に対応したコンパイラ、CMake 3.21以上、pkg-config（MSVCでは不要） | ビルド |
| 必須 | Qt6（6.4以上。Widgets、テストにはTest） | 画面 |
| 必須 | Eigen3 | 座標・幾何の計算 |
| 必須 | Open Babel 3（C++ライブラリと、フォーマットプラグイン・力場データ） | 構造ファイルの読み書き、力場による整形、SMILESからの3D構造 |
| 任意 | RDKit C++ | 厳密な立体判定（R/S・E/Z）、残基認識 |
| 任意 | Qt6に対応したVTK 9.1以上（開発用パッケージ） | 3D画面をVTK（OpenGL）で描画。ないときはQPainter（CPU）で描画 |
| 計算 | NWChemと、そのビルドに合うMPI（MPI版の場合。WindowsではWSLのもの） | 計算の実行。入力の作成・保存だけならなくても使えます |

任意のものはビルド時にCMakeが見つけたときだけ有効になります。有効になった機能は「ヘルプ → MolFormaについて」で確認できます。3D描画の方式は[3D描画](features/rendering.md)、NWChemの起動条件は[「3 実行設定」](features/execution.md)を参照してください。
