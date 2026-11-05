MolForma for Linux (x86-64)

A molecular editor, input builder and result viewer for NWChem.
Site:   https://hnakada123.github.io/MolForma/en/
Source: https://github.com/hnakada123/MolForma

To start
  Keep the AppImage where you like, allow it to run and start it:

    chmod +x MolForma-*.AppImage
    ./MolForma-*.AppImage

  No installation is needed. If it cannot mount with FUSE, install fuse3 or start it with
  --appimage-extract-and-run. Calculations need NWChem, which is installed separately.

What the computer needs
  x86-64 Linux with a desktop, glibc 2.38 or later and GCC 13's libstdc++ or later: Ubuntu 24.04 or later,
  Debian 13 or later, Fedora 39 or later, Arch Linux and others. OpenSSL 3, OpenGL, X11 libraries and
  fonts come from the computer (Japanese fonts for the Japanese interface).

In this folder
  MolForma-*.AppImage   MolForma with the libraries it needs
  LICENSE               MolForma's license, the GNU GPL version 2
  NOTICE                Notes on the libraries, NWChem and how to cite it
  SOURCES.txt           Where the source of everything in the AppImage is
  licenses/             The licenses of the bundled libraries, also inside the AppImage

The manual is built in: Help > Guide and manual (F1).

--------------------------------------------------------------------------------

MolForma Linux版（x86-64）

NWChem向けの分子エディタ・計算入力作成・結果表示アプリです。
サイト: https://hnakada123.github.io/MolForma/
ソース: https://github.com/hnakada123/MolForma

起動のしかた
  AppImageを好きな場所に置き、実行の許可を付けて起動します。

    chmod +x MolForma-*.AppImage
    ./MolForma-*.AppImage

  インストールは不要です。FUSEでマウントできない場合は、fuse3を入れるか、
  --appimage-extract-and-run を付けて起動してください。計算には別途NWChemが必要です。

必要な環境
  x86-64のLinuxのデスクトップで、glibc 2.38以上とGCC 13以降のlibstdc++が必要です
  （Ubuntu 24.04以降、Debian 13以降、Fedora 39以降、Arch Linuxなど）。OpenSSL 3、OpenGL、
  X11のライブラリとフォントはPCのものを使います（日本語の表示には日本語のフォントが必要です）。

このフォルダーの内容
  MolForma-*.AppImage   MolFormaと、動作に必要なライブラリ
  LICENSE               MolFormaのライセンス（GNU GPLバージョン2）
  NOTICE                ライブラリとNWChemについての告知、NWChemの引用
  SOURCES.txt           AppImageに入っているもののソースの入手先
  licenses/             同梱ライブラリのライセンス（AppImageの中にもあります）

マニュアルは組み込まれています。「ヘルプ → 操作ガイド・マニュアル」（F1）で開きます。
