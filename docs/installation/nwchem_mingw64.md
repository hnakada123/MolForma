# WindowsでNWChemをビルドする（64bit MinGW / GNU Fortran）

MSYS2のUCRT64環境とGNU Fortranを使い、Windows上で動くNWChemをソースからビルドします。作成するのはWindowsネイティブの64bit実行ファイルです。実行時にWSLは使いません。

2026年10月29日に、NWChem 7.3.1を全標準モジュール構成でビルドし、HF・DFT・TCEの計算とMicrosoft MPIによる2プロセス実行を確認しました。本記事はその設定と修正をまとめたものです。掲載にあたり作業場所を `C:\NWChem` に統一しています。2026年11月4日に、MolFormaからの実行と停止も確認しました（[9. MolFormaから使う](#9-molformaから使う)）。

この手順と添付のパッチは非公式のものです。NWChem・Global Arraysの開発者による確認や推奨を受けたものではありません（[ライセンス](#ライセンス)）。

MolForma本体のビルドは、既存の[Windowsでのインストール](windows.md)を参照してください。本記事は計算エンジンであるNWChemの導入手順です。MolFormaはMSVCで、NWChemはGNU Fortranでビルドして利用できます。

## 確認した構成

| 項目 | 構成・版 |
| --- | --- |
| 対象 | Windows x64 |
| NWChem | 7.3.1、タグ `v7.3.1-release` |
| MSYS2の環境 | UCRT64 |
| GCC / GNU Fortran | 16.2.0（パッケージ `16.2.0-6`） |
| GNU Make | 4.4.1 |
| OpenBLAS | 0.3.34（LP64、整数引数は32bit） |
| Global Arrays（GA） | 5.9.2 |
| Microsoft MPI | ランタイム10.1.1、MSYS2開発用パッケージ `10.1.1-26` |
| NWChemのモジュール指定 | `NWCHEM_MODULES=all` |
| ARMCIの通信方式 | `MPI-TS` |
| 固有値計算 | `USE_SERIALEIGENSOLVERS=y`、ScaLAPACKなし |

`all` はNWChemが選択する標準モジュール一式です。HF、DFT、TCE、NWPW、MDなどを含みますが、PythonやNBOなどの外部連携をすべて有効にする指定ではありません。また、全モジュールのビルド成功と、全機能の動作確認は別です。確認範囲は後述します。

Intel FortranやVisual Studioは、このNWChemのビルドには必要ありません。既存のIntel Fortran環境と共存させられます。

## 用意するものと作業場所

インターネット接続、Microsoft MPIのインストールを許可できるWindowsアカウント、ソース・ツールチェーン・中間ファイルを置ける空き容量が必要です。容量には数GB以上の余裕を持たせてください。実機では約32GBのメモリ、ビルド並列数4で作業しましたが、これは最小要件ではありません。

作業場所には、空白や日本語を含まない短いパスを使います。本記事では次の配置にします。

```text
C:\NWChem\
├─ toolchain\msys64\       MSYS2・GNU Fortran・OpenBLAS
├─ nwchem-7.3.1\           NWChemソースとビルド結果
├─ support\                本記事の添付ファイル
└─ calculations\           計算の作業フォルダー
```

以下では、**PowerShellで実行するコマンド**と、**MSYS2 UCRT64のBashで実行するコマンド**を分けています。パスの `C:\NWChem` はBashでは `/c/NWChem` と表します。

## 1. 添付ファイルを保存する

[ビルド補助ファイル一式（ZIP）](nwchem_mingw64.zip)をダウンロードし、内容を `C:\NWChem\support` に展開します。`support` の直下に `env.sh`、`prepare.sh`、`build.sh` がある配置にしてください。

| ファイル | 用途 |
| --- | --- |
| [env.sh](nwchem_mingw64/env.sh) | 今回成功したビルド設定 |
| [prepare.sh](nwchem_mingw64/prepare.sh) | ソース取得、GA標準前処理、互換性パッチ適用 |
| [build.sh](nwchem_mingw64/build.sh) | 構成、整数インターフェース変換、コンパイルとリンク |
| [mingw64-compatibility.patch](nwchem_mingw64/mingw64-compatibility.patch) | 今回追加したMinGW向けの修正 |
| [nwchem.cmd](nwchem_mingw64/nwchem.cmd) | Windowsからの1プロセス起動 |
| [nwchem-mpi.cmd](nwchem_mingw64/nwchem-mpi.cmd) | WindowsからのMPI起動 |
| [water_opt.nw](nwchem_mingw64/water_opt.nw) | 水分子の構造最適化入力 |
| [toolchain-packages.txt](nwchem_mingw64/toolchain-packages.txt) | ビルド時点のMSYS2パッケージ一覧 |

ZIPにはコンパイラ、NWChem本体、Microsoft MPIのインストーラーは含みません。それぞれ次の手順で公式配布元から取得します。`.sh` ファイルを編集するときはUTF-8・LF改行で保存してください。

## 2. MSYS2とGNU Fortranを入れる

今回のビルドでは、MSYS2のベースアーカイブを作業フォルダー内に展開しました。同じ配置を使う場合は、**PowerShell**で次を実行します。

```powershell
New-Item -ItemType Directory -Force C:\NWChem\toolchain | Out-Null
Set-Location C:\NWChem
curl.exe -fL https://github.com/msys2/msys2-installer/releases/download/nightly-x86_64/msys2-base-x86_64-latest.tar.xz -o msys2-base.tar.xz
tar.exe -xf .\msys2-base.tar.xz -C .\toolchain

$env:MSYSTEM = 'UCRT64'
& .\toolchain\msys64\usr\bin\bash.exe -l
```

続いて、起動した**Bash**で更新とパッケージ導入を行います。

```bash
pacman -Syu
```

更新の途中でMSYS2を終了するよう指示された場合は、終了してから上のPowerShellの最後の2行で開き直し、再度 `pacman -Syu` を実行します。その後、次のパッケージを入れます。

```bash
pacman -S --needed make patch diffutils git autoconf automake libtool \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-gcc-fortran \
  mingw-w64-ucrt-x86_64-msmpi \
  mingw-w64-ucrt-x86_64-openblas

echo "$MSYSTEM"
which gcc gfortran make
gfortran --version
gfortran -dumpmachine
```

`MSYSTEM` は `UCRT64`、`gfortran` は `/ucrt64/bin/gfortran`、ターゲットは `x86_64-w64-mingw32` になっていることを確認します。Makeは `/usr/bin/make` で構いません。`/mingw64` や `/usr/bin` の別のFortranコンパイラを混在させないでください。

MSYS2はローリングリリースです。`latest` アーカイブや現在のパッケージリポジトリから入る版は、表に記載した実測時の版から変わることがあります。環境の記録には次を使えます。

```bash
pacman -Q > /c/NWChem/installed-packages.txt
```

MSYS2を別の場所に導入する場合は、後述する起動スクリプトとMolFormaの実行環境にあるツールチェーンのパスも合わせて変更します。

## 3. Microsoft MPIランタイムを入れる

MSYS2の `mingw-w64-ucrt-x86_64-msmpi` はビルド用のヘッダーやライブラリを提供します。NWChemの実行には、Windows側にもMicrosoft MPIランタイムが必要です。

別の**PowerShell**で次を実行します。

```powershell
Set-Location C:\NWChem
curl.exe -fL https://github.com/microsoft/Microsoft-MPI/releases/download/v10.1.1/msmpisetup.exe -o .\toolchain\msmpisetup.exe
Get-AuthenticodeSignature .\toolchain\msmpisetup.exe
Start-Process -FilePath .\toolchain\msmpisetup.exe -Wait
```

署名の状態が `Valid` であることを確認してインストールを進めます。Windowsの管理者権限確認が出た場合は許可します。今回使ったのはランタイムの `msmpisetup.exe` で、別途MicrosoftのSDKを入れる構成ではありません。

インストール後、次を確認します。

```powershell
Test-Path 'C:\Windows\System32\msmpi.dll'
& 'C:\Program Files\Microsoft MPI\Bin\mpiexec.exe' -help
```

MPIを使ってビルドした実行ファイルは、1プロセスで直接起動する場合にもMPIランタイムを必要とします。

## 4. NWChemとGlobal Arraysを取得し、修正を適用する

**Bash**で次を実行します。

```bash
cd /c/NWChem
bash support/prepare.sh
```

このスクリプトは、以下の順序で準備します。

1. NWChemのタグ `v7.3.1-release` のソースをGitHub APIから取得し、`nwchem-7.3.1` に展開する。
2. Global Arrays 5.9.2のリリースアーカイブを `src/tools` に取得する。
3. NWChem付属の `get-tools-github` を実行し、GAの展開と、`nxtval.sh` によるNWChem標準の前処理を行う。
4. 本記事の互換性パッチを、dry-runで検査してから適用する。

`nwchem-7.3.1` が既にある場合、スクリプトは上書きせず停止します。既存の計算環境を更新するためのスクリプトではありません。

**パッチは、GAの標準前処理が終わってから適用します。** 生のGAソースに適用したり、適用済みのソースに再度適用したりしないでください。GA取得をMakeに任せたまま先にパッチを当てると、対象ファイルが存在しない、または前処理と修正が重なる問題が生じます。

今回、公式ソースに追加した主な修正は次のとおりです。

| 対象 | 修正内容 |
| --- | --- |
| GAの `bzero` / `bcopy` 呼び出し | `memset` / `memmove` で扱えるようにする |
| GA・NWChemの一部乱数関数 | MinGW用の `srand` / `rand` 経路、または既存の移植用乱数関数を使う |
| ホスト名の取得 | 該当箇所で `MPI_Get_processor_name` を使う |
| GAのディスク空き容量取得 | `GetDiskFreeSpace` の出力引数をWindows APIの `DWORD` に合わせる |
| GAの `nice()` ラッパー | MinGWでは未サポートとして `ENOSYS` を返す |
| NWChemのMDソケット用ソース | `GA_Error` に必要な宣言を読み込む |

これは今回のビルド用のローカル修正で、NWChemやGAの公式パッチとして採用されたものではありません。乱数生成経路が変わる箇所は、元の実装と同じ乱数列にはなりません。確率的手法の数値検証は今回の確認範囲に含めていません。

## 5. 全標準モジュールをビルドする

添付の `env.sh` は、次の主要な環境変数を設定します。

```bash
export NWCHEM_TOP=/c/NWChem/nwchem-7.3.1
export NWCHEM_TARGET=LINUX64
export NWCHEM_MODULES=all
export FC=gfortran CC=gcc

export USE_MPI=y USE_MPIF=y USE_MPIF4=y
export MPI_INCLUDE=/ucrt64/include
export MPI_LIB=/ucrt64/lib
export LIBMPI='-lmsmpi -lmsmpifec'
export FORCE_MPI_ENV=y
export ARMCI_NETWORK=MPI-TS

export BLASOPT=-lopenblas LAPACK_LIB=-lopenblas
export BLAS_SIZE=4 USE_64TO32=y
export USE_SERIALEIGENSOLVERS=y
```

`NWCHEM_TARGET=LINUX64` はこのMinGWビルドで使うNWChem内部のターゲット名です。生成物はLinuxバイナリーではなく、WindowsのPE x86-64実行ファイルになります。

今回のOpenBLASはLP64版なので、NWChemとの整数インターフェースを `BLAS_SIZE=4`、`USE_64TO32=y`、`make 64_to_32` で合わせます。`USE_SERIALEIGENSOLVERS=y` はScaLAPACKを使わない構成の指定で、NWChem全体のMPI並列実行を無効にするものではありません。

**Bash**でビルドします。

```bash
cd /c/NWChem
set -o pipefail
bash support/build.sh 2>&1 | tee build.log
```

`build.sh` が実行する順序は次のとおりです。

```bash
source /c/NWChem/support/env.sh
cd "$NWCHEM_TOP/src"
make nwchem_config
make -C include
make 64_to_32
make -j4
```

`make -C include` で必要なヘッダーを準備し、整数インターフェース変換の後に本体をビルドします。GAの構成・ビルドもNWChemのMakeから行われます。`tee` を使う際の `pipefail` は、ビルドの失敗がログ保存コマンドの成功に隠れないようにする指定です。

全モジュールのソース数は多く、今回の実機では整数インターフェース変換だけでも数十分かかりました。画面の出力が少ない時間帯があっても、ログとプロセスの状態を確認してください。`-j4` はコンパイルの並列数で、計算時のMPIプロセス数とは別です。

途中で止まった場合、原因を修正し、**設定を変えていなければ**次で続きからビルドできます。

```bash
source /c/NWChem/support/env.sh
cd "$NWCHEM_TOP/src"
set -o pipefail
make -j4 2>&1 | tee /c/NWChem/build-resume.log
```

コンパイラ、MPI、ARMCI、整数サイズの構成を変える場合は、別の作業フォルダーにソースを準備し直してください。異なる構成の中間ファイルを混ぜないことが重要です。

## 6. 生成物と起動環境を確認する

ビルドに成功すると、次のファイルができます。

```text
C:\NWChem\nwchem-7.3.1\bin\LINUX64\nwchem.exe
```

**Bash**で形式と依存DLLを確認できます。

```bash
objdump -f /c/NWChem/nwchem-7.3.1/bin/LINUX64/nwchem.exe
objdump -p /c/NWChem/nwchem-7.3.1/bin/LINUX64/nwchem.exe | grep 'DLL Name'
```

今回の生成物は `pei-x86-64` でした。主な依存DLLには `libgfortran-5.dll`、`libgcc_s_seh-1.dll`、`libopenblas.dll`、`libwinpthread-1.dll`、`libmsmpifec.dll`、`msmpi.dll` があります。`nwchem.exe` だけを別のPCへコピーしても動作しません。

添付の起動スクリプトは、プロセス内でDLLの検索先と次のデータライブラリを設定します。Windows全体のPATHを書き換える必要はありません。

| 変数 | 設定する場所 |
| --- | --- |
| `PATH` | `toolchain\msys64\ucrt64\bin`、Microsoft MPIの `Bin`、元のPATH |
| `NWCHEM_BASIS_LIBRARY` | `C:/NWChem/nwchem-7.3.1/src/basis/libraries/` |
| `NWCHEM_NWPW_LIBRARY` | `C:/NWChem/nwchem-7.3.1/src/nwpw/libraryps/` |
| `OPENBLAS_NUM_THREADS` | 未設定の場合のみ `1` |

ライブラリのパスは `/` 区切り・末尾 `/` 付きで渡します。今回、バックスラッシュ表記では基底ライブラリの読込に失敗し、上の表記で正常動作を確認しました。MDの力場データを使う場合は、別途、計算の作業ディレクトリーに置く `nwchemrc` などでその場所を設定します。

## 7. 水分子の構造最適化で動作を確認する

次の入力を `water_opt.nw` として保存します。同じ内容のファイルを添付しています。中性・一重項の水をRHF/6-31G*で最適化する例です。

```text
echo
start water_opt
title "Water geometry optimization: RHF/6-31G*"
memory total 512 mb
charge 0

geometry units angstrom
  O  0.000000  0.000000  0.000000
  H  0.800000  0.000000  0.600000
  H -0.800000  0.000000  0.600000
end

basis
  * library 6-31g*
end

scf
  rhf
  singlet
  thresh 1.0e-8
  maxiter 100
end

driver
  tight
  maxiter 50
  xyz water_opt
end

task scf optimize
```

**PowerShell**で、新しい計算用フォルダーを作って実行します。

```powershell
New-Item -ItemType Directory -Force C:\NWChem\calculations\water | Out-Null
Copy-Item C:\NWChem\support\water_opt.nw C:\NWChem\calculations\water\
Set-Location C:\NWChem\calculations\water
& C:\NWChem\support\nwchem.cmd water_opt.nw > water_opt.out
$LASTEXITCODE
Select-String -Path water_opt.out -Pattern 'Optimization converged','Total SCF energy','Total times'
```

終了コードが `0` で、ログに `Optimization converged` があることを確認します。今回の1プロセス計算では、初期構造をstep 0として5ステップで収束し、次の結果になりました。

| 項目 | 結果 |
| --- | ---: |
| 最終SCFエネルギー | −76.010746508443 hartree |
| O–H結合長（2本とも） | 0.94732 Å |
| H–O–H結合角 | 105.49982° |
| NWChemの表示した実時間 | 約1.6秒 |
| 最終構造 | `water_opt-005.xyz` |

計算時間や末尾の数値は環境に依存します。この例は構造最適化で、振動数解析は含みません。

2プロセス実行を試すときは、別の計算フォルダーを使います。

```powershell
New-Item -ItemType Directory -Force C:\NWChem\calculations\water-mpi2 | Out-Null
Copy-Item C:\NWChem\support\water_opt.nw C:\NWChem\calculations\water-mpi2\
Set-Location C:\NWChem\calculations\water-mpi2
& C:\NWChem\support\nwchem-mpi.cmd 2 water_opt.nw > water_opt.out
$LASTEXITCODE
```

これは読者が実行できる並列最適化の例です。今回の記録で確認した2プロセスの数値テストは、次節の公式QAのHFエネルギー・勾配計算です。上の最適化結果表は1プロセス計算の結果です。

## 8. 確認した計算と未確認の範囲

| テスト | 結果 |
| --- | --- |
| GNU FortranからのMPI呼び出し | 正常終了 |
| Global Arraysの分散配列の作成・書込・読込 | 1・2プロセスとも成功 |
| 公式QA `h2o`：HF/6-31G*エネルギー・勾配 | 1・2プロセスとも正常終了。SCFエネルギー −76.010538615916 hartreeは参照出力と一致 |
| 公式QA `dft_he_mvs15` | DFTエネルギー −2.867079060595 hartreeは参照出力と一致 |
| 公式QA `tce_h2o` | 11個の出力相関エネルギーを比較。参照値との差の最大は約5.31×10⁻¹³ hartree |
| 本記事の水のRHF/6-31G*構造最適化 | 1プロセスで収束 |

TCEで実行した手法はLCCD、LCCSD、CCD、CCSD、CCSDT、QCISD、CISD、CISDT、MBPT(2)、MBPT(3)です。MBPT(2)が2回出力されるため、比較値は11個あります。

全QAスイート、NWPW・MD・確率的手法の数値計算、多ノードMPI、ScaLAPACKは検証していません。Unixソケットを使うMD連携は、このMinGW構成では利用できません。古いソースに由来するコンパイラ警告も残っています。

掲載用スクリプトは、今回成功した設定を移設しやすい形に整理したものです。シェル構文、パッチの適用、および適用後の15ファイルが実際にビルドしたソースと一致することを検査しました。掲載用の配置で新規ダウンロードから全ビルドをもう一度行ったわけではありません。

## 9. MolFormaから使う

ここからはMolFormaの実行設定に接続するための設定例です。まず上のコマンドライン計算が成功することを確認してください。

2026年11月4日に、このネイティブ版NWChemとMolForma（Windows版、MSVCでビルド）で、MolFormaの実行設定のテスト（実際のNWChemを使う任意のテスト）を行いました。MPI 2プロセスの水のHF/STO-3G計算が完了してSCFエネルギーが −74.945628203255 hartreeになること、計算途中の停止で `mpiexec`・`smpd`・NWChemのプロセスがすべて終了することを確かめました。このテストは既定のMPIランチャー名 `mpirun` を使うため、`mpiexec.exe` を `mpirun.exe` という名前でコピーして起動しました。下の設定のように `mpiexec.exe` を直接指定する場合と、同じプログラムです。

NWChemのDLLとデータを見つけられるよう、**PowerShell**で環境を設定してMolFormaを起動します。最後の実行ファイルの場所は、MolFormaのインストール先に合わせます。

```powershell
$env:PATH = "C:\NWChem\toolchain\msys64\ucrt64\bin;C:\Program Files\Microsoft MPI\Bin;$env:PATH"
$env:NWCHEM_BASIS_LIBRARY = 'C:/NWChem/nwchem-7.3.1/src/basis/libraries/'
$env:NWCHEM_NWPW_LIBRARY = 'C:/NWChem/nwchem-7.3.1/src/nwpw/libraryps/'
& "$env:LOCALAPPDATA\Programs\MolForma\bin\molforma.exe"
```

この設定は、そのPowerShellとそこから起動したプロセスに適用されます。すでに動いているMolFormaには反映されません。

MolFormaの「3 実行設定」には、以下を設定します。

| 項目 | 設定例 |
| --- | --- |
| NWChem実行ファイル | `C:\NWChem\nwchem-7.3.1\bin\LINUX64\nwchem.exe` |
| 起動方式 | 最初は直接起動で確認し、並列計算ではMPI |
| MPIランチャー | `C:\Program Files\Microsoft MPI\Bin\mpiexec.exe` |
| MPIプロセス数 | `2` |
| OpenMP / BLASスレッド数 | まず各 `1` |

MolFormaはMPI起動時に `-np <プロセス数> <実行ファイル> input.nw` を渡します。今回のMicrosoft MPI 10.1.1では `-np` を受け付けることを確認しました。上の表ではNWChemの実体である `.exe` を指定します。本記事の `nwchem-mpi.cmd` は `プロセス数 入力ファイル` という別の引数形式なので、MolFormaのMPIランチャー欄には指定しません。既存のWSL用 `mpirun.cmd` とも組み合わせません。

「停止」では、2秒後に `mpiexec.exe` と、そこから起動された `smpd.exe`・`nwchem.exe` をまとめて終了します。

「実行前の確認」でパスを検査し、水などの小さな計算から確認します。ログや最適化後のXYZはMolFormaの結果ビューアへの読込に利用できます。設定画面の詳細は[「3 実行設定」](../features/execution.md)を参照してください。

## 困ったとき

| 症状 | 確認・対応 |
| --- | --- |
| `gfortran` がない、または別の版が使われる | `MSYSTEM=UCRT64`、`which gfortran`、`gfortran -dumpmachine` を確認します |
| パッケージ取得が繰り返し失敗する | MSYS2の接続先ミラーやネットワークを確認します。今回もミラー変更で取得できた例がありました |
| `pacman` がロックされている | ほかの更新・インストール処理が実行中でないか先に確認します。実行中の処理のロックを消さないでください |
| `sys/mman.h` などUnix向けの依存でGAが止まる | `ARMCI_NETWORK=MPI-TS` か確認します。今回試した `MPI-PR` 構成ではこの種の問題が出ました |
| パッチで `Hunk FAILED` が出る | NWChem 7.3.1、GA 5.9.2、標準前処理後・追加パッチ適用前の状態か確認します。失敗を無視して続けないでください |
| `random`、`srandom`、`bzero`、`gethostname` などでエラーになる | 指定版のソースに互換性パッチが適用されているか確認します |
| ヘッダーが見つからない | `make nwchem_config` の後に `make -C include` を実行します |
| `msmpi.dll` がない | MSYS2パッケージだけでなく、Windows用Microsoft MPIランタイムも導入します |
| `libgfortran-5.dll`、`libopenblas.dll` などがない | `ucrt64\bin` が実行プロセスのPATHにあるか確認します。まず添付の起動スクリプトを使います |
| `failed to open basis library` | `NWCHEM_BASIS_LIBRARY` の実在する場所、`/` 区切り、末尾 `/` を確認します |
| ソースアーカイブで `not a git repository` が表示される | アーカイブには `.git` がありません。今回も版情報取得で表示されましたが、最終リンクと数値テストは成功しました。別のコンパイルエラーを見落とさないでください |
| コマンドラインでは動くがMolFormaからは動かない | MolFormaが起動時に継承したPATH、データライブラリ設定、ネイティブ版の `.exe` とMicrosoft MPIの組合せを確認します |

## ライセンス

NWChemは、Pacific Northwest National Laboratory（Battelle Memorial Institute）が [Educational Community License 2.0](https://opensource.org/licenses/ECL-2.0) で公開しています。Global Arraysは、Battelle Memorial Instituteが [BSD-3-Clause](https://opensource.org/licenses/BSD-3-Clause) で公開しています。どちらも、ソースを取得して手元で修正・ビルドすることを認めています。

- この手順とスクリプトは、NWChem・Global Arrays・MSYS2・Microsoft MPIの公式配布元から各自が取得してビルドする方法を説明するもので、それらのソースや実行ファイルは含みません。
- 添付の `mingw64-compatibility.patch` には、変更した箇所の前後の数行が含まれます。パッチの先頭に、変更したファイルの著作権者とライセンス、非公式のパッチであることを記載しています。各変更は、変更先のファイルと同じライセンスで提供します。
- ビルドした `nwchem.exe` やDLLをほかの人に配布する場合は、NWChem（ECL 2.0の通知の保持と、変更したファイルの明示）、Global Arrays、OpenBLAS、GCCの実行時ライブラリ、Microsoft MPIの再頒布条件など、含めるものそれぞれのライセンスに従ってください。
- NWChemを使った計算結果を公表するときは、NWChemの文献を引用してください（[NWChemの引用](https://nwchemgit.github.io/Citation.html)）。

## 参考資料

- [NWChem公式：ソースからのビルド](https://nwchemgit.github.io/Compiling-NWChem.html)
- [NWChem 7.3.1のソース](https://github.com/nwchemgit/nwchem/tree/v7.3.1-release)
- [MSYS2公式](https://www.msys2.org/)
- [MSYS2の環境の違い](https://www.msys2.org/docs/environments/)
- [Global Arrays 5.9.2](https://github.com/GlobalArrays/ga/releases/tag/v5.9.2)
- [Microsoft MPI 10.1.1](https://github.com/microsoft/Microsoft-MPI/releases/tag/v10.1.1)

この記事の版番号・数値結果は今回のローカルビルド記録に基づきます。公式手順の一般的な説明と、今回必要になったローカル修正を区別して記載しています。
