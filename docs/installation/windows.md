# Windowsでのインストール

MolFormaをWindowsでMicrosoft Visual C++（MSVC）を使ってソースからビルドし、フォルダーにインストールする手順です。ビルドに使うライブラリはvcpkgとソースから（RDKitとVTKを含む）、計算に使うNWChemはWSL（Windows Subsystem for Linux）のUbuntuに入れます。

対象はWindows 11（x64）とMolForma 1.0.0です。2026-10-28に、Windows 11 HomeとVisual Studio Community 2026で手順を確認しました（[確認した構成](#確認した構成)）。

MolFormaのWindows用のインストーラーや配布ファイルはありません。GitHubのソースからビルドし、`cmake --install` で、実行に必要なQt・Open BabelのファイルをまとめたMolFormaのフォルダーを作ります。NWChemはWindows版が公式には配布されていないため、WSLのUbuntuのパッケージを使い、MolFormaに付属する2つのバッチファイル（`nwchem.cmd`・`mpirun.cmd`）から起動します。

## 必要なソフトウェア

| 区分 | ソフトウェア | 用途 |
| --- | --- | --- |
| 必須 | Visual Studio 2022以降（ワークロード「C++によるデスクトップ開発」） | MSVCのコンパイラ、Windows SDK、CMake、Ninja |
| 必須 | Git for Windows | ソースの取得。付属のPerlをOpen Babelのビルドに使う |
| 必須 | Qt 6（6.4以上）の「MSVC 2022 64-bit」 | 画面（Qt Widgets）、テスト（Qt Test）、`windeployqt` |
| 必須 | vcpkgの `eigen3`・`zlib`・`libxml2` | 座標・幾何の計算、gzipで圧縮した構造ファイルの読込、Open Babelのビルド |
| 必須 | Open Babel 3.2.1（ソースからビルド） | 構造ファイルの読み書き、力場による整形、SMILESからの3D構造 |
| 任意 | RDKit Release_2026_09_1（ソースからビルド）と、vcpkgのBoost | 厳密な立体判定（R/S・E/Z）、残基認識 |
| 任意 | VTK 9.7.1（Qt 6に合わせてソースからビルド） | 3D画面をVTK（OpenGL）で描画。ないときはQPainter（CPU）で描画 |
| 計算 | WSL 2のUbuntuと、そのパッケージ `nwchem` | 計算の実行。入力の作成・保存だけならなくても使えます |

- **Open Babel**: vcpkgにはないため、ソースからMSVCでビルドします（[3. Open Babelをビルドする](#3-open-babelをビルドする)）。
- **RDKitとVTK**: どちらも任意で、ビルド時にCMakeが見つけたときだけ有効になります（[4. RDKitとVTKをビルドする（任意）](#4-rdkitとvtkをビルドする任意)）。VTKがないときは、3D画面をQPainter（CPU）で描画します（[3D描画](../features/rendering.md)）。
- **Python**: ビルドにも実行にも使いません。

以下のコマンドは、スタートメニューの「x64 Native Tools Command Prompt for VS」（MSVCの環境を設定したコマンドプロンプト）で実行します。作業用のフォルダーは `C:\dev` とします。

## 1. Visual Studio・Git・Qtを入れる

1. [Visual Studio](https://visualstudio.microsoft.com/ja/)（Communityで可）をインストールし、ワークロード「C++によるデスクトップ開発」を選びます。MSVC・Windows SDK・「Windows用C++ CMakeツール」（CMakeとNinja）が入ります。
2. [Git for Windows](https://gitforwindows.org/) をインストールします。Open Babelのビルドに、付属のPerl（`C:\Program Files\Git\usr\bin\perl.exe`）を使います。
3. [Qt Online Installer](https://www.qt.io/download-qt-installer) でQtをインストールし、Qt 6の版の下の「MSVC 2022 64-bit」を選びます（Qtアカウントが必要です）。以下では `C:\Qt\6.11.2\msvc2022_64` に入れたとします。MSVC 2022用のQtは、Visual Studio 2026のMSVCでもそのまま使えます。

## 2. vcpkgでライブラリを入れる

[vcpkg](https://vcpkg.io/) を取得し、Eigen・zlib・libxml2を入れます。

```bat
cd /d C:\dev
git clone https://github.com/microsoft/vcpkg.git
vcpkg\bootstrap-vcpkg.bat -disableMetrics
vcpkg\vcpkg install --triplet x64-windows eigen3 zlib libxml2[core]
```

ヘッダー・ライブラリ・DLLは `C:\dev\vcpkg\installed\x64-windows` に入ります。

## 3. Open Babelをビルドする

Open Babel 3.2.1のソースを取得し、ビルドして `C:\dev\openbabel-install` にインストールします。

```bat
cd /d C:\dev
git clone -c core.autocrlf=false -c core.eol=lf --depth 1 --branch openbabel-3-2-1 https://github.com/openbabel/openbabel.git
cd openbabel
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=C:/dev/openbabel-install ^
  -DCMAKE_PREFIX_PATH=C:/dev/vcpkg/installed/x64-windows -DOB_USE_PREBUILT_BINARIES=OFF ^
  -DWITH_INCHI=OFF -DWITH_MAEPARSER=OFF -DWITH_COORDGEN=OFF -DBUILD_GUI=OFF -DENABLE_TESTS=OFF ^
  "-DPERL_EXECUTABLE=C:/Program Files/Git/usr/bin/perl.exe"
cmake --build build -j 4
cmake --install build
```

- `-c core.autocrlf=false -c core.eol=lf`: データファイルの改行をLFのまま取得します。Git for Windowsの既定ではCRLFに変換され、そのデータでは環状の分子（シクロヘキサンなど）の3D構造の生成に失敗します。
- `-DWITH_INCHI=OFF`: Open Babel 3.2.1に含まれるInChIライブラリは、MSVCではリンクできません。MolFormaはInChIを使いません。
- `-DOB_USE_PREBUILT_BINARIES=OFF` など: Open Babelのソースに含まれない、または構成時にダウンロードする部品（GUI、Maestro形式、Coordgen）を使いません。

`C:\dev\openbabel-install\bin` に `openbabel-3.dll` とフォーマットのプラグイン（`*.obf`）が、`bin\data` に力場などのデータが入ります。Open BabelのCMakeの設定（`lib\cmake\openbabel3`）もここに入り、MolFormaはこれで見つけます（MSVCのビルドではpkg-configの設定は作られません）。

## 4. RDKitとVTKをビルドする（任意）

RDKit（厳密な立体判定・残基認識）とVTK（3D画面のOpenGL描画）は任意です。RDKitはvcpkgになく、vcpkgのVTKはvcpkg自身のQtを使うため、どちらもソースからビルドします。使わない場合はこの章を飛ばし、[5. MolFormaをビルドする](#5-molformaをビルドする)の `CMAKE_PREFIX_PATH` から `C:/dev/rdkit-install` と `C:/dev/vtk-install` を除きます。4コアのPCで、BoostとRDKitで約40分、VTKで約45分かかりました（一部を並行して実行）。

### RDKit

RDKitが使うBoostのライブラリをvcpkgで入れます。

```bat
cd /d C:\dev
vcpkg\vcpkg install --triplet x64-windows boost-algorithm boost-any boost-assign boost-container boost-crc boost-dynamic-bitset ^
  boost-flyweight boost-format boost-functional boost-graph boost-integer boost-iostreams[core] boost-json ^
  boost-lexical-cast boost-multi-array boost-multiprecision boost-numeric-conversion boost-optional ^
  boost-parameter boost-predef boost-property-map boost-property-tree boost-random boost-range ^
  boost-rational boost-smart-ptr boost-tokenizer boost-unordered boost-variant2
```

RDKit Release_2026_09_1のソースを取得し、C++のライブラリだけをビルドして `C:\dev\rdkit-install` にインストールします。構成の途中で、RDKitのCMakeがCatch2・better-enums・RingDecomposerLibをGitHubから取得します。

```bat
cd /d C:\dev
git clone -c core.autocrlf=false -c core.eol=lf --depth 1 --branch Release_2026_09_1 https://github.com/rdkit/rdkit.git
cd rdkit
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=C:/dev/rdkit-install ^
  -DCMAKE_PREFIX_PATH=C:/dev/vcpkg/installed/x64-windows -DRDK_INSTALL_INTREE=OFF ^
  -DRDK_BUILD_PYTHON_WRAPPERS=OFF -DRDK_BUILD_CPP_TESTS=OFF -DRDK_TEST_MMFF_COMPLIANCE=OFF ^
  -DRDK_USE_BOOST_SERIALIZATION=OFF -DRDK_USE_BOOST_IOSTREAMS=OFF -DRDK_USE_BOOST_STACKTRACE=OFF ^
  -DRDK_BUILD_COORDGEN_SUPPORT=OFF -DRDK_BUILD_MAEPARSER_SUPPORT=OFF -DRDK_BUILD_CHEMDRAW_SUPPORT=OFF ^
  -DRDK_BUILD_PUBCHEMSHAPE_SUPPORT=OFF -DRDK_BUILD_DESCRIPTORS3D=OFF -DRDK_BUILD_FREETYPE_SUPPORT=OFF
cmake --build build -j 4
cmake --install build
```

- MSVCでは静的ライブラリ（`*.lib`）を作ります。MolFormaの実行ファイルに組み込まれるので、インストールしたフォルダーにRDKitのDLLは増えません。
- Pythonのラッパー、図の描画のフォント（FreeType）、Coordgen・Maestro形式・ChemDraw形式・PubChemの形状比較・3D記述子は、MolFormaが使わないのでビルドしません。

### VTK

VTK 9.7.1のソースを取得し、MolFormaが使うモジュールだけを、Qtに合わせてビルドします（AppImageと同じ構成です）。`certutil` が表示するSHA-256が `cae04fd355004cb916a409db79d53a208f1221e975aeacc7540ee67b148ee91a` であることを確認してから展開します。

```bat
cd /d C:\dev
curl -L -o VTK-9.7.1.tar.gz https://vtk.org/files/release/9.7/VTK-9.7.1.tar.gz
certutil -hashfile VTK-9.7.1.tar.gz SHA256
tar -xzf VTK-9.7.1.tar.gz
cd VTK-9.7.1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=C:/dev/vtk-install ^
  -DCMAKE_PREFIX_PATH=C:/Qt/6.11.2/msvc2022_64 -DBUILD_SHARED_LIBS=ON -DVTK_QT_VERSION=6 ^
  -DVTK_BUILD_TESTING=OFF -DVTK_BUILD_EXAMPLES=OFF -DVTK_WRAP_PYTHON=OFF -DVTK_WRAP_JAVA=OFF -DVTK_USE_MPI=OFF ^
  -DVTK_GROUP_ENABLE_Imaging=DONT_WANT -DVTK_GROUP_ENABLE_MPI=DONT_WANT -DVTK_GROUP_ENABLE_Qt=DONT_WANT ^
  -DVTK_GROUP_ENABLE_Rendering=DONT_WANT -DVTK_GROUP_ENABLE_StandAlone=DONT_WANT ^
  -DVTK_GROUP_ENABLE_Views=DONT_WANT -DVTK_GROUP_ENABLE_Web=DONT_WANT ^
  -DVTK_MODULE_ENABLE_VTK_CommonCore=YES -DVTK_MODULE_ENABLE_VTK_CommonDataModel=YES ^
  -DVTK_MODULE_ENABLE_VTK_FiltersSources=YES -DVTK_MODULE_ENABLE_VTK_RenderingCore=YES ^
  -DVTK_MODULE_ENABLE_VTK_RenderingOpenGL2=YES -DVTK_MODULE_ENABLE_VTK_GUISupportQt=YES ^
  -DVTK_MODULE_ENABLE_VTK_InteractionStyle=YES
cmake --build build -j 4
cmake --install build
```

`C:\dev\vtk-install\bin` にVTKのDLLが入ります。VTKで描画するには、OpenGL 3.2以上を使えるGPUドライバーも必要です。

## 5. MolFormaをビルドする

ソースを取得し、Releaseでビルドします。取得したフォルダーと `build` は、更新で使うので残しておきます。

```bat
cd /d C:\dev
git clone https://github.com/hnakada123/MolForma.git
cd MolForma
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  "-DCMAKE_PREFIX_PATH=C:/Qt/6.11.2/msvc2022_64;C:/dev/openbabel-install;C:/dev/rdkit-install;C:/dev/vtk-install;C:/dev/vcpkg/installed/x64-windows"
cmake --build build -j 4
```

`cmake -S . -B build …`（構成）の出力の終わり近くに、見つけたOpen Babelと3D描画の方式が出ます。VTKを入れていない場合は `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` になります。RDKitは、CMakeのキャッシュ（`build\CMakeCache.txt`）の `rdkit_DIR` が `C:/dev/rdkit-install/lib/cmake/rdkit` なら見つかっています。

```
-- Found OpenBabel3, version 3.2.1, in C:/dev/openbabel-install/bin
-- MolForma rendering: VTK 9.7.1 (Qt6), with QPainter fallback
```

ビルドすると、`build` の実行ファイルの横に、それが使うDLL（Qt・Open Babel・VTK・vcpkgのもの）とOpen Babelのプラグイン、Qtの場所を示す `qt.conf` がコピーされます。このため `build\molforma.exe` は、PATHを設定しなくても起動できます。

テストを実行する場合は次のとおりです（任意）。NWChem・ネットワークは使いません。

```bat
ctest --test-dir build --output-on-failure
```

## 6. インストールする

`build` からインストールします。ここではユーザーごとのアプリの場所（`%LOCALAPPDATA%\Programs\MolForma`）に入れます。管理者の権限は要りません。

```bat
cmake --install build --prefix "%LOCALAPPDATA%\Programs\MolForma"
```

`windeployqt` がQtのDLL・プラグイン・翻訳を、CMakeがOpen Babel・VTK・vcpkgの必要なDLLを集め、このフォルダーだけで起動できるようにします（RDKitとVTKを含めて約190 MB、どちらもなしで約110 MB）。

| 配置先 | 内容 |
| --- | --- |
| `bin\molforma.exe` | 実行ファイル（RDKitを組み込み済み）。同じフォルダーにQt・Open Babel・VTK・zlib・libxml2のDLL |
| `bin\*.obf`、`bin\data\` | Open Babelのフォーマットのプラグインと、力場などのデータ |
| `plugins\`、`translations\` | Qtのプラグイン（画面・画像形式など）と、Qtの標準ダイアログの翻訳 |
| `nwchem-wsl\` | WSLのNWChemを起動する `nwchem.cmd`・`mpirun.cmd`（[7. NWChemを入れる（WSL）](#7-nwchemを入れるwsl)） |
| `share\molforma\` | LICENSE・NOTICE・README とマニュアルのMarkdown（マニュアルは実行ファイルにも組み込み済み） |

スタートメニューから起動できるように、ショートカットを作ります（任意）。PowerShellで次を実行します。

```powershell
$s = (New-Object -ComObject WScript.Shell).CreateShortcut("$env:APPDATA\Microsoft\Windows\Start Menu\Programs\MolForma.lnk")
$s.TargetPath = "$env:LOCALAPPDATA\Programs\MolForma\bin\molforma.exe"
$s.Save()
```

MSVCの実行時ライブラリ（`MSVCP140.dll` など）はフォルダーに含めません。Visual Studioを入れたPCにはあります。フォルダーをほかのPCにコピーして使う場合は、そのPCに「Microsoft Visual C++ 再頒布可能パッケージ」（x64）を入れてください。

## 7. NWChemを入れる（WSL）

WSLのUbuntuにNWChemを入れます。WSLを使っていない場合は、管理者のPowerShellで `wsl --install` を実行し、再起動後にUbuntuのユーザー名とパスワードを設定します。続けて、コマンドプロンプトで次を実行します。

```bat
wsl sudo apt update
wsl sudo apt install nwchem
```

Ubuntu 24.04では、NWChem 7.2.2とOpen MPI版（`nwchem-openmpi`）・MPIの実行環境（`openmpi-bin`）が入ります。基底関数のライブラリはそのまま使えるため、`~/.nwchemrc` の設定は不要です。

MolFormaからは、インストールしたフォルダーの `nwchem-wsl` にある2つのバッチファイルを使います。どちらも既定のWSLディストリビューションで、ジョブフォルダー（WSLからは `/mnt/c/…` に見える）を作業場所にしてNWChemを実行します。

| ファイル | MolFormaから呼ばれる形 | WSLで実行するもの |
| --- | --- | --- |
| `nwchem.cmd` | `nwchem.cmd input.nw`（起動方式「直接実行」） | `nwchem input.nw` |
| `mpirun.cmd` | `mpirun.cmd -np 2 <NWChem実行ファイル> input.nw`（起動方式「MPI」） | `mpirun -np 2 nwchem input.nw` |

MolFormaの「編集 → 環境設定」で、実行設定の既定値を次のようにして保存します。起動方式「MPI」と起動数2は既定のままです。`<ユーザー名>` はWindowsのユーザー名です（`%LOCALAPPDATA%` は書けないので、展開したパスを入力します）。

| 項目 | 値 |
| --- | --- |
| NWChem実行ファイル | `C:\Users\<ユーザー名>\AppData\Local\Programs\MolForma\nwchem-wsl\nwchem.cmd` |
| MPIランチャー | `C:\Users\<ユーザー名>\AppData\Local\Programs\MolForma\nwchem-wsl\mpirun.cmd` |

既定値は、新規作成と構造ファイルを開いたときに使います。既に作ったプロジェクトは「3 実行設定」で同じように変えます。`nwchem-wsl` フォルダーをWindowsのPATHに加えれば、名前の `nwchem`・`mpirun`（実行設定の既定）のまま使うこともできます。

- スレッド数（`OMP_NUM_THREADS`・`OPENBLAS_NUM_THREADS`・`OMP_STACKSIZE`）は、バッチファイルが `WSLENV` でWSLに渡します。「追加の環境変数」はWSLには渡りません。必要ならバッチファイルの `WSLENV` の行に名前を加えます。
- 「停止」では、2秒後に `cmd.exe`・`wsl.exe` とWSLの `mpirun`・NWChemをまとめて終了します（[「3 実行設定」](../features/execution.md)）。
- ジョブフォルダー（既定は `C:\Users\<ユーザー名>\MolForma\jobs`）はWSLからはネットワーク経由のように見えるため、大きな一時ファイルを書く計算は、Linuxで直接動かすより遅くなります。

## 8. 起動して確認する

スタートメニューの「MolForma」か、`%LOCALAPPDATA%\Programs\MolForma\bin\molforma.exe` から起動します。表示言語はWindowsの言語に従います（「編集 → 環境設定」の「表示言語 / Language」で切り替え、再起動後に反映）。

1. 「ヘルプ → MolFormaについて」を開き、版が「MolForma 1.0.0」で、RDKit・VTKを入れた場合は「立体判定・残基認識（RDKit）: 有効」「3D描画: VTK（OpenGL）」と表示されることを確認します。OpenGLを使えない環境（一部のリモートデスクトップや仮想マシン）では「3D描画: QPainter（CPU）」になります。
2. 部品パレットの検索欄に `CC1CCCCC1`（メチルシクロヘキサン）と入力し、「SMILES「CC1CCCCC1」から3Dで作成」で3D構造の候補ができることを確認します（Open Babelとそのデータを使います）。
3. 「3 実行設定」を開き、「実行前の確認」でNWChem実行ファイルとMPIランチャーが見つかっていることを確認します。
4. 小さな分子で計算してみます。手順は[題材1 原子と結合から組む](../recipes/atoms_and_bonds.md)にあります。

設定は `%APPDATA%\MolForma\MolForma.ini` に、未保存の作業の自動保存は `%LOCALAPPDATA%\MolForma\autosave` に置きます（Windowsのレジストリは使いません）。

## 更新・削除

### 更新する

ソースを更新してビルドし直し、同じ場所にインストールし直します。Open Babel・RDKit・VTK・Qt・vcpkgのライブラリを更新しない間は、[5. MolFormaをビルドする](#5-molformaをビルドする)の構成からやり直す必要はありません。

```bat
cd /d C:\dev\MolForma
git pull --ff-only
cmake --build build -j 4
cmake --install build --prefix "%LOCALAPPDATA%\Programs\MolForma"
```

Qtの版を変えた場合は、VTKもそのQtでビルドし直し、MolFormaの `build` を削除して、新しいQtのフォルダーを `CMAKE_PREFIX_PATH` に指定して構成からやり直します。インストール先も一度削除してから入れ直すと、古いQtのファイルが残りません。

### 削除する

インストールしたフォルダー `%LOCALAPPDATA%\Programs\MolForma` と、作った場合はスタートメニューのショートカット（`%APPDATA%\Microsoft\Windows\Start Menu\Programs\MolForma.lnk`）を削除します。設定（`%APPDATA%\MolForma`）、自動保存（`%LOCALAPPDATA%\MolForma`）、計算のジョブ（既定は `%USERPROFILE%\MolForma\jobs`）は残ります。不要なら手動で削除してください。ビルドに使った `C:\dev` の下のフォルダー（MolForma・Open Babel・RDKit・VTK・vcpkgと、それぞれの `*-install`）も、不要なら削除できます。NWChemは `wsl sudo apt remove nwchem` の後に `wsl sudo apt autoremove` で削除できます。

## 困ったとき

| 症状 | 確認・対応 |
| --- | --- |
| `cmake` や `cl` が見つからない | 「x64 Native Tools Command Prompt for VS」で実行しているか確認します。普通のコマンドプロンプトにはMSVCとCMakeの設定がありません |
| MolFormaの構成が `Could not find a package configuration file provided by "OpenBabel3"` や `Qt6`・`Eigen3` が見つからないというエラーで止まる | `CMAKE_PREFIX_PATH` に、QtのMSVC版のフォルダー・`C:/dev/openbabel-install`・`C:/dev/vcpkg/installed/x64-windows` の3つを `;` で区切って指定したか確認します |
| RDKitのビルドが `boost/iostreams/tee.hpp` が見つからないというエラーで止まる | vcpkgで `boost-iostreams[core]` を入れます。RDKitは `-DRDK_USE_BOOST_IOSTREAMS=OFF` でもこのヘッダーを使います |
| RDKitの構成が `Could NOT find Freetype` で止まる | 構成に `-DRDK_BUILD_FREETYPE_SUPPORT=OFF` を付けてやり直します |
| `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` と出る | `CMAKE_PREFIX_PATH` に `C:/dev/vtk-install` を入れたか、VTKをMolFormaと同じQtでビルドしたか確認します。直前の `VTK development dependency unavailable:` の行に足りないものが出ます |
| 3D画面の左上に「描画: QPainter（CPU）· OpenGLを使えないため簡易表示」と出る | OpenGL 3.2以上を使えません。GPUドライバーを更新します。リモートデスクトップや仮想マシンでは使えない場合があります |
| Open Babelの構成が `Could NOT find Perl` で止まる | `-DPERL_EXECUTABLE` にGit for Windowsの `perl.exe` を指定します |
| Open Babelのビルドが `inchi.dll` のリンクエラー（`Get_inchi_Input_FromAuxInfo` など）で止まる | 構成に `-DWITH_INCHI=OFF` を付けてやり直します |
| SMILESや部品からの3D生成で「2つの原子が同じ位置に重なっています」と出る（特に環のある分子） | Open BabelのデータがCRLFで取得されています。[3. Open Babelをビルドする](#3-open-babelをビルドする)の `git clone` の `-c` の指定でソースを取り直し、`build` と `C:\dev\openbabel-install` を削除してビルドし直します |
| 整形で「この構造に使える力場パラメータがありません」と出る | Open Babelのデータ（`bin\data`）が見つかっていません。インストールしたフォルダーなら `bin\data` があるか、ビルドしたフォルダーの `molforma.exe` なら `C:\dev\openbabel-install\bin\data` があるか確認します。環境変数 `BABEL_DATADIR` を設定している場合は、その値が優先されます |
| `build\molforma.exe` やテストが `Qt6Core.dll` などが見つからないというエラーで起動しない | DLLは実行ファイルをリンクしたときに `build` にコピーします。Qt・Open Babel・RDKit・VTKを入れ直した後や、`build` のDLLを消した後は、`build` の `*.exe` を削除して `cmake --build build` で作り直します |
| 「実行前の確認」でNWChem実行ファイルやMPIランチャーが見つからない | 「3 実行設定」のパスが `nwchem-wsl` の2つのバッチファイルを指しているか確認します |
| 計算がすぐに失敗し、ログ（標準エラー）に `wsl` のエラーが出る | コマンドプロンプトで `wsl nwchem` と `wsl mpirun --version` が動くか確認します。既定でないWSLディストリビューションを使う場合は、バッチファイルの `wsl.exe` の後に `-d <ディストリビューション名>` を加えます |
| 標準エラーに `your 131072x1 screen size is bogus. expect trouble` と出る | WSLの `ps` などが出す警告で、計算には影響しません |

## 確認した構成

2026-10-28に、次の構成で確かめました。

- 上の手順でOpen Babel・RDKit・VTKとMolFormaを構成・ビルドし、CTestの14件すべてが通ること（RDKitの検査もスキップされずに実行）。RDKit・VTKのない構成でも14件すべてが通ること。
- 実際のデスクトップ（GPUのOpenGL）で、VTKの描画経路を検査する任意のテスト（`MOLFORMA_REQUIRE_VTK=1` で `test_rendering` を実行。[3D描画](../features/rendering.md)）が通ること。
- `cmake --install` で作ったフォルダーから、Qt・Open Babel・vcpkgのフォルダーをPATHに含めずに起動でき、シクロヘキサン・メチルシクロヘキサンのSMILESから3D構造を生成できること。
- WSLのUbuntu 24.04にaptで入れたNWChemを `mpirun.cmd`・`nwchem.cmd` で起動し、MolFormaの実行設定のテスト（実際のNWChemを使う任意のテスト）で、MPI 2プロセスの水のHF/STO-3G計算が完了し、SCFエネルギーが −74.945628203255 Hartree になること。計算途中の停止で、WSLの `mpirun` とNWChemも終了すること。

| 項目 | 版 |
| --- | --- |
| Windows | Windows 11 Home（x64） |
| Visual Studio・MSVC | Community 2026 18.9.2・14.51.36231 |
| CMake・Ninja（Visual Studio付属） | 4.3.1・1.13.2 |
| Git for Windows | 2.50.1 |
| Qt（MSVC 2022 64-bit） | 6.11.2 |
| Open Babel | 3.2.1 |
| RDKit・Boost（vcpkg） | 2026.09.1・1.91.0 |
| VTK | 9.7.1 |
| Eigen・zlib・libxml2（vcpkg） | 5.0.1・1.3.2・2.15.3 |
| WSLのUbuntu | 24.04.4 LTS |
| NWChem・Open MPI（WSL） | 7.2.2・4.1.6 |

vcpkgは、Visual Studioに付属する `vcpkg.exe` と、それに合う版のポート（タグ `2026.06.24`）を使いました。Windows 10とARM64のWindowsは確認していません。

関連: [OS別インストール・必要パッケージ](../installation.md)、[3D描画](../features/rendering.md)、[「3 実行設定」](../features/execution.md)
