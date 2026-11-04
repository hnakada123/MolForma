# macOSでのインストール

MolFormaをmacOSでソースからビルドし、アプリケーション（`MolForma.app`）のディスクイメージ（DMG）を作って、「アプリケーション」フォルダーにインストールする手順です。ビルドに使うライブラリと、計算に使うNWChemは、パッケージマネージャーの[Homebrew](https://brew.sh/)で入れます。

対象はApple siliconのMac（arm64）のmacOS 27とMolForma 1.0.0です。2026-11-04に、macOS 27.0.1とHomebrew 7.0.10で手順を確認しました（[確認した構成](#確認した構成)）。

MolFormaのパッケージは、Homebrewにはありません（2026-11-04時点）。GitHubのソースからビルドし、CMakeに含まれるCPackでDMG（`MolForma-1.0.0-arm64.dmg`）を作ります。`MolForma.app` は、使うライブラリ（Qt・Open Babel・RDKit・VTKなど）をすべて含み、Homebrewのフォルダーのライブラリを読み込みません。インストールした後は、Homebrewのライブラリを更新・削除しても動きます。NWChemは含まないので、別に入れます。

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

Qt全体の `qt` は不要です（MolFormaが使うのは `qtbase` だけで、`qt` はQt WebEngineなど多くのモジュールを入れます）。これらのライブラリはビルドとDMGの作成に使い、DMGを作るときに `MolForma.app` の中へコピーされます。

計算に使うNWChemは、Homebrewの `nwchem` を入れます（[3. NWChemを入れる](#3-nwchemを入れる)）。

- **RDKitとVTK**: どちらも任意です。ビルド時にCMakeが見つけたときだけ有効になり、DMGにも入ります。`qttranslations` を入れた場合は、Qtの日本語訳もDMGに入ります。
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

Open MPI（`open-mpi`）、ScaLAPACK・OpenBLAS、GNU Fortran（`gcc`）なども依存として入ります。NWChemは約1 GBです。NWChemは `/opt/homebrew/bin/nwchem` に、MPIの実行環境 `mpirun` は `/opt/homebrew/bin/mpirun` に入ります。HomebrewのNWChemは、基底関数などのライブラリの場所を書いた `/opt/homebrew/etc/nwchemrc` を読むため、`~/.nwchemrc` や `NWCHEM_BASIS_LIBRARY` の設定は不要です。MolFormaの「3 実行設定」の既定（実行ファイル `nwchem`、MPIで起動、MPIランチャー `mpirun`、2プロセス）のままで、このNWChemを起動できます。FinderやDockから起動したアプリには、ターミナルのPATHの設定が渡りませんが、`MolForma.app` は `/opt/homebrew/bin`（IntelのMacでは `/usr/local/bin`）をPATHに加えてNWChemとMPIランチャーを探します。

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

## 5. DMGを作る

`build` で `cpack` を実行し、DMGを作ります。

```sh
cd build
cpack -G DragNDrop
```

1〜2分で、`build` に `MolForma-1.0.0-arm64.dmg`（約85 MB）ができます。CPackは、次のことを行ってから、`MolForma.app` と「アプリケーション」フォルダーへのリンクをDMGに入れます。

1. `MolForma.app` を組み立てます。実行ファイル、アイコン、ファイルの種類などを書いた `Info.plist`、Open Babelのフォーマットプラグインと力場データ、Qtの日本語訳を入れます。
2. Qtの `macdeployqt` で、Qtのフレームワークとプラグインをコピーします。
3. `packaging/macos/bundle.cmake` で、残りのライブラリ（Open Babel・RDKit・Boost・VTKなど）をコピーし、すべての参照をアプリの中のコピーに向けます。Homebrewのフォルダーを参照するものは残しません。
4. 含めたライブラリのライセンスの文面と、パッケージ名・版・ライセンスの一覧を `Contents/Resources/licenses` に入れます（[ほかのMacで使う・配布するとき](#ほかのmacで使う配布するとき)）。
5. アドホック署名（開発者IDのない署名）をします。Apple siliconのMacは、署名のないプログラムを実行しないためです。

DMGを作らずに `MolForma.app` だけを作る場合は、`cmake --install build --prefix <フォルダー>` を実行します。`<フォルダー>/MolForma.app` ができます。

## 6. インストールする

DMGを開き、`MolForma` を「アプリケーション」フォルダーへドラッグします。DMGはターミナルからも開けます。

```sh
open MolForma-1.0.0-arm64.dmg
```

コピーが終わったら、Finderのサイドバーの「MolForma」の取り出しボタンでDMGを取り出します。DMGは、ほかのMacで使う場合のほかは削除してもかまいません。

ターミナルやMCPクライアントから起動する場合は、`/Applications/MolForma.app/Contents/MacOS/molforma` を指定します（例: `/Applications/MolForma.app/Contents/MacOS/molforma water.molforma.json`。MCPは[MCP](../mcp.md)）。`molforma` の名前で起動したい場合は、PATHの通ったフォルダーにリンクを作ります。

```sh
mkdir -p ~/.local/bin
ln -sf /Applications/MolForma.app/Contents/MacOS/molforma ~/.local/bin/molforma
```

`~/.local/bin` がPATHにない場合は、`echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zprofile` で加え、ターミナルを開き直します。

## 7. 起動して確認する

Launchpad、または「アプリケーション」フォルダーの「MolForma」から起動します。表示言語はシステムの言語に従います（環境設定（⌘,）の「表示言語 / Language」で切り替え、再起動後に反映）。macOSでは、「環境設定…」「MolFormaについて」の項目が、表示言語によってメニューバーの「MolForma」メニューに移ることがあります。

1. 「MolFormaについて」（「ヘルプ」メニュー、または「MolForma」メニュー）を開き、版が「MolForma 1.0.0」で、RDKit・VTKを入れた場合は「立体判定・残基認識（RDKit）: 有効」「3D描画: VTK（OpenGL）」と表示されることを確認します。
2. 「3 実行設定」を開き、「NWChem実行ファイル」と「MPIランチャー」の欄の下に、PATHで見つかった `/opt/homebrew/bin/nwchem`・`/opt/homebrew/bin/mpirun` が表示されることを確認します。表示されない場合は、[困ったとき](#困ったとき)を参照してください。
3. 小さな分子で計算してみます。手順は[題材1 原子と結合から組む](../recipes/atoms_and_bonds.md)にあります。

Finderからは、ファイルを右クリックして「このアプリケーションで開く → MolForma」を選ぶか、DockのMolFormaのアイコンにドラッグして開けます。NWChem入力（`.nw`）は、ほかに開くアプリがなければダブルクリックで開きます。構造ファイル（XYZ・MOL・SDF・Mol2・CML・PDB・mmCIF、Gaussian入力）とプロジェクト（`.molforma.json`）は、「このアプリケーションで開く」の候補に出ます。プロジェクトは拡張子が `.json` のため、ダブルクリックするとJSONの既定のアプリで開きます。

## ほかのMacで使う・配布するとき

DMGは、作ったMacと同じ種類（Apple silicon）で、同じかより新しいmacOSのMacで使えます。HomebrewのライブラリはそのMacのmacOS向けにビルドされているため、`Info.plist` に書く最小のmacOSの版も、DMGを作ったMacのmacOSの版（ここではmacOS 27）にしています。使うMacには、計算のためにNWChemを入れます（[3. NWChemを入れる](#3-nwchemを入れる)）。

- **初めて開くとき**: 開発者IDで署名・公証していないため、インターネットやAirDropで受け取ったDMGのアプリは、初めて開くときに「“MolForma”を開けません」などと表示されます。「システム設定 → プライバシーとセキュリティ」の下に出る「このまま開く」を押してから、もう一度開きます。ターミナルで、インストールした後に `xattr -dr com.apple.quarantine /Applications/MolForma.app` を実行しても開けます。自分のMacで作ったDMGは、この確認なしで開きます。
- **ライセンス**: DMGを人に配る場合は、含めたソフトウェアのライセンスに従ってください。`MolForma.app/Contents/Resources/licenses` に、各パッケージのライセンスの文面（Homebrewのパッケージにある場合）と、パッケージ名・版・ライセンスの一覧 `packages.txt` があります。MolForma（GPL-2.0-or-later）、Open Babel（GPL-2.0-only）、Open BabelとリンクしたためGPL-2.0の条件で使うQtは、バイナリを配るときに対応するソースも入手できるようにします（リポジトリの `NOTICE`）。MolFormaはDMGを作ったコミットのソースを示し、ほかのパッケージは `packages.txt` の版のソースを示してください。

## 更新・削除

### 更新する

ソースを更新してビルドし直し、DMGを作り直します。

```sh
cd MolForma
git pull --ff-only
cmake --build build -j 4
cd build
cpack -G DragNDrop
```

MolFormaを終了してから、新しいDMGの `MolForma` を「アプリケーション」フォルダーへドラッグし、「置き換える」を選びます。

インストールした `MolForma.app` は、`brew upgrade` でHomebrewのライブラリが新しい版になっても、そのまま動きます。ビルドしたフォルダーの `build` は、Qt・Open Babel・RDKit・VTKの版が変わると、ビルドやDMGの作成に失敗することがあります（`dyld[…]: Library not loaded` などと出ます）。その場合は、`build` を削除して[4. ソースを取得してビルドする](#4-ソースを取得してビルドする)の構成からやり直してください。

### 削除する

「アプリケーション」フォルダーの `MolForma` をゴミ箱に入れます。[6. インストールする](#6-インストールする)でリンクを作った場合は、`rm ~/.local/bin/molforma` で削除します。

アプリの設定（`~/Library/Preferences/MolForma/MolForma.ini`）、自動保存（`~/Library/Application Support/MolForma/`）、計算のジョブ（既定は `~/MolForma/jobs`）は残ります。不要なら手動で削除してください。Homebrewで入れたパッケージは、`brew uninstall` で指定して削除し、`brew autoremove` で一緒に入ったものも削除できます。DMGを作った後は、NWChemを除いてHomebrewのパッケージを削除しても `MolForma.app` は動きます。NWChemは `brew uninstall nwchem` の後に `brew autoremove` で削除できます。

## 困ったとき

| 症状 | 確認・対応 |
| --- | --- |
| 構成が `No CMAKE_CXX_COMPILER could be found` などで止まる | Command Line Toolsを入れます（`xcode-select --install`）。`xcode-select -p` で場所が出れば入っています |
| 構成が `Qt6`・`Eigen3`・`openbabel-3` が見つからないというエラーで止まる | [2. パッケージを入れる](#2-パッケージを入れる)の必須のパッケージを入れ、構成からやり直します |
| `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` と出る | `brew install vtk` で入れ、構成からやり直します |
| `cpack` が `… not found in …` で止まる | アプリに入れるライブラリが、Homebrewのフォルダーに見つかりません。`brew upgrade` の後なら、`build` を削除して構成からやり直します |
| 開くと「“MolForma”を開けません」「開発元を検証できません」などと表示される | 受け取ったDMGのアプリです。[ほかのMacで使う・配布するとき](#ほかのmacで使う配布するとき)のとおり、「プライバシーとセキュリティ」の「このまま開く」を押すか、`xattr -dr com.apple.quarantine /Applications/MolForma.app` を実行します |
| 「“MolForma”は壊れているため開けません」と表示される | 署名が壊れています。DMGを作り直し、インストールし直します。アプリの中のファイルを書き換えた場合も、この表示になります |
| `molforma: command not found` と出る | [6. インストールする](#6-インストールする)のリンクとPATHの設定を行い、ターミナルを開き直します |
| 「実行前の確認」でNWChemやMPIランチャーが見つからない | ターミナルで `command -v nwchem mpirun` を確認します。`/opt/homebrew/bin`・`/usr/local/bin` 以外にNWChemを入れた場合は、「3 実行設定」でパスを指定します |
| NWChemの計算が、基底関数のライブラリが見つからないというエラーで止まる | `/opt/homebrew/etc/nwchemrc` があるか確認します。ない場合は `brew reinstall nwchem` で入れ直します |
| 日本語表示なのに、ファイル選択などQtの標準ダイアログに英語が残る | `brew install qttranslations` で入れ、DMGを作り直します |
| ビルド中にメモリーが不足する | `cmake --build build -j 2` のように並列数を減らします |

## 確認した構成

2026-11-04〜05に、Apple siliconのMac（macOS 27.0.1、arm64、10コア・メモリー32 GB）で、次のことを確かめました。

- 上のパッケージを入れて、ソースを構成・ビルドできること（RDKit・VTKとも有効）。ビルドは `-j 8` で約1分で、警告は出ませんでした。CTestの16件すべてが通り、テストの間、`~/Library/Preferences`・`~/Library/Application Support` のMolFormaの設定と自動保存が変わらないこと。
- `cpack -G DragNDrop` が約1分半で `MolForma-1.0.0-arm64.dmg`（84 MB）を作ること。DMGに `MolForma.app`（205 MB、38のHomebrewパッケージのライブラリを含む）と「アプリケーション」フォルダーへのリンクがあり、`codesign --verify --deep --strict` が通ること。
- DMGからコピーした `MolForma.app` で、次のことができること。読み込まれるライブラリ（`DYLD_PRINT_LIBRARIES`）に、Homebrewのフォルダーのものが1つもないこと。プロジェクトファイルを開いた画面の写真（`--screenshot`）を撮れること。SMILESからの3D構造で、アプリの中のOpen Babelのプラグインが使われること。Finderから開いたファイル（`open -a`）が読み込まれること。
- FinderやDockから起動したときと同じ最小のPATH（`/usr/bin:/bin:/usr/sbin:/sbin`）で起動した `MolForma.app` が、既定の実行設定のまま `/opt/homebrew/bin` のNWChemと `mpirun` を見つけ、MPI 2プロセスの水素分子の計算を完了すること。
- `~/.nwchemrc` を置かずに `mpirun -np 2 nwchem` で水素分子のHF/6-31G*計算が完了し、SCFエネルギーが −1.126755317965 Hartree になること。MolFormaの実行設定のテスト（実際のNWChemを使う任意のテスト、[「3 実行設定」](../features/execution.md)）で、MPI 2プロセスの水のHF/STO-3G計算が完了し、SCFエネルギーが −74.945628203255 Hartree になること。

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

IntelのMac（Homebrewは `/usr/local` に入ります）、macOS 26以前、Homebrewを入れていないMacでの起動、受け取ったDMGを開くときの確認の表示、画面を操作しての長時間の利用は確認していません。

関連: [OS別インストール・必要パッケージ](../installation.md)、[3D描画](../features/rendering.md)、[「3 実行設定」](../features/execution.md)、[MCP](../mcp.md)
