MolForma for Windows (x64)

A molecular editor, input builder and result viewer for NWChem.
Site:   https://hnakada123.github.io/MolForma/en/
Source: https://github.com/hnakada123/MolForma

To start
  Keep this folder where you like, for example in %LOCALAPPDATA%\Programs, and start
  bin\molforma.exe. No installation is needed. To remove MolForma, delete the folder.

  MolForma is not signed, so Windows may stop it the first time ("Windows protected your PC").
  Press "More info", then "Run anyway".

What the computer needs
  64-bit Windows 11 on x64 and the latest Microsoft Visual C++ Redistributable (x64). Many PCs have
  it. If Windows says that VCRUNTIME140.dll or MSVCP140.dll is missing, or MolForma stops at once,
  install it from Microsoft: https://aka.ms/vc14/vc_redist.x64.exe
  The 3D view uses OpenGL 3.2 or later; without it MolForma draws on the CPU, more simply.

To run calculations
  NWChem is installed separately. The usual way is the Ubuntu package in WSL; in a Command Prompt:

    wsl sudo apt update
    wsl sudo apt install nwchem

  (Without WSL yet: "wsl --install" in an administrator's PowerShell, then restart.)
  Then, in MolForma, open Edit > Preferences and set these two defaults of the run settings, with the
  full path of this folder:

    NWChem executable   <this folder>\nwchem-wsl\nwchem.cmd
    MPI launcher        <this folder>\nwchem-wsl\mpirun.cmd

  Details, and a native build of NWChem without WSL:
  https://hnakada123.github.io/MolForma/en/installation/windows.html

In this folder
  bin\                  MolForma with the libraries it needs (molforma.exe starts it)
  plugins\, translations\   Parts of Qt
  nwchem-wsl\           Batch files that start NWChem in WSL
  share\                MolForma's README and manual
  LICENSE               MolForma's license, the GNU GPL version 2
  NOTICE                Notes on the libraries, NWChem and how to cite it
  SOURCES.txt           Where the source of everything in this folder is
  licenses\             The licenses of the bundled libraries

The manual is built in: Help > Guide and manual (F1).

--------------------------------------------------------------------------------

MolForma Windows版（x64）

NWChem向けの分子エディタ・計算入力作成・結果表示アプリです。
サイト: https://hnakada123.github.io/MolForma/
ソース: https://github.com/hnakada123/MolForma

起動のしかた
  このフォルダーを好きな場所（例: %LOCALAPPDATA%\Programs）に置き、bin\molforma.exe を起動します。
  インストールは不要です。削除するときは、フォルダーを削除します。

  署名をしていないため、最初はWindowsに止められることがあります（「WindowsによってPCが保護されました」）。
  「詳細情報」を押してから「実行」を押します。

必要な環境
  x64の64ビット版Windows 11と、最新の「Microsoft Visual C++ 再頒布可能パッケージ」（x64）が必要です。
  多くのPCには入っています。VCRUNTIME140.dll や MSVCP140.dll が見つからないと表示されたときや、
  起動してすぐに終了するときは、Microsoftのサイトから入れてください。
  https://aka.ms/vc14/vc_redist.x64.exe
  3D画面にはOpenGL 3.2以上を使います。使えないときはCPUで簡易に描画します。

計算するには
  NWChemは別に入れます。WSLのUbuntuのパッケージを使うのが標準の方法です。コマンドプロンプトで次を実行します。

    wsl sudo apt update
    wsl sudo apt install nwchem

  （WSLがまだないときは、管理者のPowerShellで wsl --install を実行し、再起動します。）
  続けて、MolFormaの「編集 → 環境設定」で、実行設定の既定値の次の2つを、このフォルダーの完全なパスで設定します。

    NWChem実行ファイル  <このフォルダー>\nwchem-wsl\nwchem.cmd
    MPIランチャー       <このフォルダー>\nwchem-wsl\mpirun.cmd

  詳しい手順と、WSLを使わずにNWChemをビルドする方法:
  https://hnakada123.github.io/MolForma/installation/windows.html

このフォルダーの内容
  bin\                  MolFormaと、動作に必要なライブラリ（molforma.exe で起動）
  plugins\、translations\   Qtの一部
  nwchem-wsl\           WSLのNWChemを起動するバッチファイル
  share\                MolFormaのREADMEとマニュアル
  LICENSE               MolFormaのライセンス（GNU GPLバージョン2）
  NOTICE                ライブラリとNWChemについての告知、NWChemの引用
  SOURCES.txt           このフォルダーに入っているもののソースの入手先
  licenses\             同梱ライブラリのライセンス

マニュアルは組み込まれています。「ヘルプ → 操作ガイド・マニュアル」（F1）で開きます。
