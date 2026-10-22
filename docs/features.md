# 「1 分子」「2 座標」「3 計算条件」「4 詳細設定」

MolFormaはNWChem向けの分子作成・座標編集・計算入力作成用デスクトップアプリです。KAGAMI GUIの「1 分子」「2 座標」「3 計算条件」「4 詳細設定」をQt6 WidgetsとC++20で実装し、計算条件・詳細設定はNWChem用に構成しています。分子座標は倍精度のÅ、原子参照は永続IDで管理します。実行・計算履歴・結果ビューアのタブは今後の実装対象です。

## 「2 座標」

- 「1 分子」と分子データ・選択・候補・Undo/Redoを共有します。表の行と3D画面の原子を相互に選択でき、距離・角度・二面角も表示します。従来の形式電荷・不対電子・原子名の一覧編集は「編集 → 座標と原子属性の一覧」に残しています。
- 元素、X/Y/Z、質量数またはamu、ゴースト、固定をセル単位で編集します。元素はH〜RnとD/T、`@C`などの入力に対応します。元素を変えたときは旧元素の質量指定を解除します。NaN・無限大・未知元素・原子の重複を拒否し、エラーを表示して元のセルへ戻します。失敗した編集は履歴にも分子にも反映しません。
- 表示単位はÅ/Bohrです（1 Bohr = 0.529177210903 Å）。切替やカメラ操作で物理座標を変更しません。表示は有効数字14桁で、編集したセルだけを読み取るため他の座標の精度は維持されます。
- 原子追加ダイアログでは表と同じ単位で1原子を入力します。結合・水素を自動補完しません。選択削除・全削除・構造ファイル読込・XYZ貼付は候補として表示し、画面下部で確定・取消します。固定原子の座標変更・削除・構造置換は拒否します。
- 「選択原子を原点へ」は1原子を選択して分子全体を平行移動します。固定原子が動く場合は拒否します。点電荷の位置は変更しません。
- XYZ / MOL / SDF / PDB / ENTを構造として読み込めます。PDBではMODEL/ALTLOCを選択します。読込・貼付による置換でもプロジェクトの実行・結果情報や計算設定を保持し、旧原子を参照する設定は保留にします。結合は入力構造から引き継ぎ、XYZでは距離から推定します。XYZの入出力は表の表示にかかわらずÅです。`@元素`と第5列の質量はKAGAMI互換の拡張で、一般的なXYZ読込ソフトでは未対応の場合があります。固定・結合・残基・点電荷等を保存する場合はプロジェクト形式を使います。

### NWChemへの出力

座標表の右に座標ブロックを表示し、コピー・`.nw`保存ができます。出力単位は表の表示単位と独立で、`document.nwchem.geometry_units` に保存され、Undo/Redoにも対応します。既存プロジェクトのKAGAMI設定 `job.bohr` はNWChemの単位として流用しません。

- `geometry units angstrom` または `geometry units au` を出力します。Å出力では `angstrom_to_au` を明示し、Bohr出力との換算を揃えます。
- `nocenter noautosym noautoz` により、指定した原子順・座標系でのCartesian入力を作成します。自動対称化、Z-matrix、周期境界条件の編集は未対応です。
- 同位体質量数はOpen Babel C++の同位体データからamuへ変換し、NWChemの `mass` として出力します。質量数をそのまま実質量として出力しません。任意質量は小数で指定でき、データベースにない同位体は明示質量が必要です。
- ゴーストは `bqC` 等のゼロ核電荷の中心として出力します。`basis` 内に `bqC library C <basis-name>` 等の明示的な基底割当が必要で、プレビュー末尾に案内を付けます。原子の形式電荷をNWChemの核電荷指定へ転用しません。
- 固定フラグは `constraints` の `fix atom` へ出力時の原子番号で変換します。先頭の `clear` は以前の固定条件を除去するため、既存入力へ組み込む場合は必要な他の拘束も同じブロックへ追加してください。

このタブの出力は**座標と固定原子の入力断片**です。完成した計算入力は「3 計算条件」で作成します。未確定候補はプレビューのみで、確定・取消まで出力ボタンを無効化します。NWChem入力の読込・計算実行・出力結果の解析は未実装です。NWChem本体のインストールはこのタブの使用には不要です。

入力仕様の参照: [Cartesian coordinate input](https://nwchemgit.github.io/Cartesian-coordinate-input.html)、[Geometry keywords](https://nwchemgit.github.io/Keywords-for-the-GEOMETRY-directive.html)、[Constraints](https://nwchemgit.github.io/Constraints.html)、[Basis sets](https://nwchemgit.github.io/Basis.html)。

## 「3 計算条件」

左側で条件を設定し、右側で電子数・参照波動関数・エラー・確認事項と完成したNWChem入力を確認します。条件の変更は共有のUndo/Redoへ記録します。矛盾した条件も修正途中の設定として保持しますが、エラーが残る間は入力の生成・コピー・保存を禁止し、以前の有効なプレビューは消去します。候補表示中・整形中は条件の変更と出力を無効にします。

### 対応する計算

| 項目 | 対応範囲 |
| --- | --- |
| 手法 | HF、DFT、MP2、CCSD、CCSD(T) |
| 計算目的 | 一点エネルギー、勾配、構造最適化、振動数、最適化→振動数。CCSD系は閉殻RHFの一点エネルギーのみ |
| 参照波動関数 | 自動・RHF/RKS・UHF/UKS・ROHF。自動は一重項を制限、開殻HFをROHF、開殻DFT/MP2を非制限にします。DFT/MP2のROHF参照は未対応 |
| DFT | B3LYP、PBE、PBE0、BLYP、BP86。分散補正なし・D3・D3(BJ)、fine/xfineグリッド。PBE0＋D3(BJ)は未対応 |
| 基底 | STO-3G、6-31G、6-31G*、6-31G**、6-311G**、cc-pVDZ、cc-pVTZ、aug-cc-pVDZ、def2-SVP、def2-TZVP。球面/Cartesian切替 |
| 収束 | SCF最大反復・3段階の収束条件、最適化最大反復・標準/厳密/緩い、CC最大反復、相関計算の内殻凍結 |
| 環境 | HF/DFTのCOSMO（誘電率指定）、保存された点電荷の任意の有効化 |
| 出力 | 計算名・タイトル・MPIプロセスごとのメモリ、Å/Bohr、完成した`.nw`のコピー・保存 |

基底の元素範囲はNWChemの組込みライブラリに基づく固定リストで検査します。基底ファイル自体は同梱しません。STO-3GはH〜I、6-31G/6-31G**はH〜Zn、6-31G*はH〜Kr、6-311G**はH〜Ca・Ga〜Kr・Iです。cc-pVDZ/cc-pVTZはH〜Kr（Kを除く）、aug-cc-pVDZはH〜Kr（K・Caを除く）、def2系はH〜La・Hf〜Rnに対応します。実行先に選択したライブラリが必要です。任意基底の入力、原子別基底変更、任意ECPは未対応です。

def2系ではRb〜Xeに28電子、Cs〜Laに46電子、Hf〜Rnに60電子を除去する `def2-ecp` を自動指定します。実原子だけにECPを付け、電子数を補正してα/β電子数・多重度の整合性を検査します。ゴーストの核電荷は0で、`bqC library C ...` のように基底を明示します。相関計算の `freeze atomic` は既定でオフです。有効にした場合はNWChemの原子別規則で凍結する軌道数を表示し、相関電子が不足する設定を拒否します。ECP中心とゴーストは内殻凍結の対象外です。

振動数はHF/DFTの解析Hessianを基本とし、MP2・ROHF・ECP・COSMOでは数値Hessianを選びます。手動で数値Hessianを選択することもできます。固定・ゴースト・外部点電荷を含む振動数解析と、ゴーストを含むCOSMOは未対応です。最適化に続けて振動数を求める場合は、二つの `task` を順に出力します。SCF収束は標準1e-5・厳密1e-7・さらに厳密1e-9で、HFは `thresh`、DFTはenergy/density/gradientに適用します。CCの収束閾値は1e-7です。「4 詳細設定」で個別の閾値を指定すると、対応する値を上書きします。

### 保存・プリセット・互換性

- NWChem固有の条件は `document.nwchem` に保存します。`geometry_units` は「2 座標」と共有します。全電荷・多重度は既存の `job.charge` / `job.multiplicity` を共有し、「1 分子」での変更も反映します。原子の形式電荷と計算の全電荷は区別し、異なるときは確認事項を表示します。
- KAGAMIの手法・基底・詳細セクションからNWChemの条件を推測しません。NWChem設定がないプロジェクトではHF/STO-3Gの初期条件を表示します。KAGAMIの条件、未知のフィールド、実行・結果参照は保存時に保持します。KAGAMIの拘束・スキャン・FMO/CP/SAPT・相対論・占有数等は変換せず、既存の詳細設定がある場合は画面で案内します。原子の固定フラグはNWChemへ出力します。
- 点電荷は初期状態では出力しません。このタブで有効にすると `bq units angstrom` へ出力します。KAGAMIのQM/MM有効フラグとは独立です。点電荷の座標は分子の出力単位にかかわらずÅを明示します。
- HF/STO-3G、B3LYP-D3(BJ)/def2-SVP最適化＋振動数、内殻凍結MP2/cc-pVDZの組込みプリセットがあります。適用はUndo一回で戻せます。適用前の変更点は説明欄のツールチップに表示します。プリセットは分子・電荷・多重度・計算名・タイトル・メモリ・単位・未知フィールドを変更しません。ユーザープリセットの登録・削除はローカルのQSettingsに保存します。
- タイトルと計算名は入力確定時に反映します。NWChemの別命令が混入しないよう、計算名の文字種・長さ、タイトルの改行・引用符等を検査します。入力プレビューは読取専用です。

計算実行、MPIの起動設定、出力の解析、既存`.nw`の読込、TCE・励起状態・遷移状態・IRC・MD・周期系・任意キーワードの編集は今後の対象です。NWChem本体がなくても条件編集と入力生成はできます。入力検査は収束や物理モデルの妥当性まで保証するものではありません。

仕様の参照: [HF](https://nwchemgit.github.io/Hartree-Fock-Theory-for-Molecules.html)、[DFT](https://nwchemgit.github.io/Density-Functional-Theory-for-Molecules.html)、[MP2](https://nwchemgit.github.io/MP2.html)、[CCSD](https://nwchemgit.github.io/CCSD.html)、[ECP](https://nwchemgit.github.io/ECP.html)、[Optimization](https://nwchemgit.github.io/Geometry-Optimization.html)、[Frequencies](https://nwchemgit.github.io/Hessians-and-Vibrational-Frequencies.html)、[COSMO](https://nwchemgit.github.io/COSMO-Solvation-Model.html)、[TASK](https://nwchemgit.github.io/TASK.html)。

### 計算条件タブの検証

`calculation`のQt Testは入力生成、手法・参照・タスクの組合せ、電子数、基底の元素範囲、ECP、ゴースト、固定番号、同位体、単位、点電荷、入力の不正値、保存互換性、プリセット、Undo/Redo、タブ間同期、候補の操作制御を検査します。通常のCTestではNWChem・ネットワークを使用しません。

任意の実機検証用入力と画面画像を生成できます。

```sh
QT_QPA_PLATFORM=offscreen MOLFORMA_CALCULATION_ARTIFACTS=/tmp/molforma-calculation \
  ./build/test_calculation engineArtifacts
```

2026-10-22にNWChem 7.3.1（MPI 2プロセス、OMP/OpenBLAS各1スレッド）で23種類の生成入力の正常終了を確認しました。HF、5種のDFT汎関数と分散補正、MP2、CCSD、CCSD(T)、開殻HF/DFT、def2-ECP、内殻凍結、ゴースト・同位体・固定、Å/Bohr、点電荷、COSMO、解析/数値振動数を含みます。水のB3LYP-D3(BJ)/def2-SVPプリセットは最適化と振動数解析が完了し、振動数は1638.93 / 3791.78 / 3886.86 cm⁻¹でした。AddressSanitizerでも計算条件タブのテストを実行しています。

CCSD(T)の検証には水/6-31Gを使用しています。この環境では最小基底のH₂/STO-3GのCCSD(T)がNWChemの三重励起処理中にMPI通信エラーとなりました。入力検査ではこのような実行環境・エンジン固有の問題は検出しません。

## 「4 詳細設定」

KAGAMI固有の詳細キーワードは流用せず、NWChemの分子計算に合わせた7カテゴリ・42項目を用意しています。左側の項目名・キーワード・説明で検索し、「現在の計算で使用する項目」「変更した項目のみ」「全項目」を切り替えます。右側には「3 計算条件」と同じ検査結果・完成した入力を表示し、コピー・`.nw`保存ができます。

| カテゴリ | 設定項目 |
| --- | --- |
| HF / 参照SCF（5） | 波動関数の収束 `thresh`、積分の打切り `tol2e`、`direct`、閉殻RHF専用の `diis` と `diisbas`。MP2/CCSDの参照SCFにも適用 |
| DFT（9） | energy/density/gradientの個別収束、ダンピング率・初期反復数、レベルシフト、DIISのFock行列数、`direct`、`grid nodisk` |
| MP2 / CCSD（7） | 明示的な凍結占有軌道数、MP2の `tight` とプロセスごとの `scratchdisk`、CCの `thresh`・`tol2e`・`diisbas`・`nodisk` |
| DRIVER 最適化（7） | `gmax`・`grms`・`xmax`・`xrms`、`trust`、`eprec`、各構造のXYZ出力 |
| 振動数・熱化学（4） | 1〜20個の温度、数値Hessianの `fd_delta`、解析Hessianの `hessian thresh`、振動モードのXYZ出力 |
| COSMO（4） | 表面分割 `minbem`、初期多面体 `ificos`、誘電遮蔽 `screen`、表面電荷幅 `zeta` |
| 軌道・ファイル（6） | 原子密度／一電子Hamiltonian／既存movecsによる初期軌道、入力・出力movecs、`scratch_dir`・`permanent_dir`、`echo` |

数値は `1e-8` / `1D-8` 等の指数表記に対応します。空欄・「既定」は明示指定を解除し、「3 計算条件」またはNWChemの既定値を使います。値の確定時に型・有限性・範囲・手法との整合を検査し、有効な設定だけで入力を生成します。修正途中の不正値もプロジェクトへ保存できますが、使用中の項目にエラーがある間は出力できません。各項目の「戻す」、カテゴリ単位・全体のリセット、共有Undo/Redoに対応します。Ctrl+Sでも入力中の値を確定してから保存します。

### 適用範囲と優先順位

- 明示したSCF/DFT/CCの閾値、凍結軌道数、最適化閾値は基本条件より優先します。DRIVERでは標準・厳密等のプリセット指示の後に個別閾値を出力します。凍結軌道数0は内殻凍結を解除し、相関電子が不足する指定は拒否します。
- DFTの値を保持したままHFへ変更した場合など、現在の計算に適用できない値は「保留」とし、入力へ出力しません。対応する手法・計算目的へ戻すと再適用します。数値Hessianの差分幅と解析Hessianの精度も、この規則で切り替えます。
- 最適化の変位・trust radius・数値差分幅はBohr、勾配はHartree/Bohr、エネルギーはHartreeです。座標のÅ/Bohr切替では変更しません。温度はKで指定します。
- 最適化構造は `<計算名>_opt`、振動モードは `<計算名>_vib` をファイル名の接頭辞にします。XYZの生成は外部でNWChemを実行したときに行われます。
- 入力movecsは実行先のファイルを指定し、MolFormaでは読みません。原子順・基底との互換性は実行時の確認が必要です。既存movecsを使う場合は、NWChemが入力ファイルを出力先として再利用する既定動作を避けるため、別の出力movecsを指定します。パスの別名・シンボリックリンクによる同一性までは検査しません。
- movecsのパスは引用符で囲み、空白を含むパスにも対応します。NWChemの起動時ディレクトリ指定は別の構文なので、`scratch_dir` / `permanent_dir` は引用符なしで出力し、空白・コロン・引用符を含むパスを拒否します。各ディレクトリは実行先に存在する必要があります。環境変数展開、ホスト／MPIランク別の保存先指定、複数ディレクトリの指定は未対応です。
- 候補表示中・整形中は編集と出力を無効化します。詳細設定の変更は分子座標や固定原子を動かしません。

### 保存と互換性

明示指定だけを `document.nwchem.advanced` に保存します。タブを表示しただけでは既定値をプロジェクトへ追加しません。未知の詳細フィールドは保持し、未対応であることを表示して出力から除外します。KAGAMIの `job.sections` やプロジェクトの実行・結果参照も保持します。

ユーザープリセットには「軌道・ファイル」以外の詳細設定を含めます。組込みプリセットや旧形式のプリセットに詳細指定がない場合は、これらの上書きを解除してプリセットの基本条件を有効にします。軌道・保存先・echoと未知フィールドは保持し、プリセット適用全体をUndo一回で戻せます。

任意のキーワード入力、SCF半直接計算の細かなキャッシュ調整、RODFT/CDFT・部分占有、任意の軌道占有、TCE、相対論、距離・角度拘束、スキャン、遷移状態、IRC、MD、周期系などは未対応です。既存KAGAMIの設定をこれらのNWChem指示へ自動変換しません。NWChem本体やPythonは、詳細設定の編集・保存・入力生成には不要です。

仕様の参照: [HF](https://nwchemgit.github.io/Hartree-Fock-Theory-for-Molecules.html)、[DFT](https://nwchemgit.github.io/Density-Functional-Theory-for-Molecules.html)、[MP2](https://nwchemgit.github.io/MP2.html)、[CCSD](https://nwchemgit.github.io/CCSD.html)、[DRIVER](https://nwchemgit.github.io/Geometry-Optimization.html)、[Hessian・振動数](https://nwchemgit.github.io/Hessians-and-Vibrational-Frequencies.html)、[COSMO](https://nwchemgit.github.io/COSMO-Solvation-Model.html)、[Scratch directory](https://nwchemgit.github.io/Scratch_Dir.html)。

### 詳細設定タブの検証

`advanced`のQt Testは、項目の型・値域、適用と保留、閾値の優先順位、HF/DFTの違い、凍結軌道、解析/数値Hessian、温度、パス、未知フィールドの保持、プリセット、検索・絞り込み、リセット、Undo/Redo、候補、Ctrl+S、フォーカス中の終了を検査します。通常のCTestはNWChemもネットワークも不要です。

任意の実機検証用入力と画面画像を生成できます。出力先には空白・コロンを含まないパスを指定してください。

```sh
QT_QPA_PLATFORM=offscreen MOLFORMA_ADVANCED_ARTIFACTS=/tmp/molforma-advanced \
  ./build/test_advanced engineArtifacts
```

軌道の読込試験 `vectors_details.nw` は、軌道を保存する `files_details.nw` の実行後に使用します。

2026-10-22にNWChem 7.3.1（MPI 2プロセス、OMP/OpenBLAS各1スレッド）で9種類の詳細設定入力の正常終了を確認しました。HFのDIIS・積分精度、DFTの個別収束・ダンピング・グリッド、MP2/CCSD、最適化→解析振動数、MP2数値振動数、COSMO、保存先、movecsの書出し・再読込を含みます。温度100/298.15/350 K、数値差分幅0.003 Bohrの反映とXYZ生成も確認しています。初期軌道の保存・再読込は同じSCFエネルギー −74.945628203255 Hartreeを得ました。詳細設定・計算条件のQt TestはAddressSanitizerでも検証しています。

## 実装済み

| 分類 | 操作 |
| --- | --- |
| ファイル | KAGAMI GUI v2プロジェクト、XYZ / MOL / SDF / PDBの読込・保存、XYZコピー・貼付、SMILES → 3D |
| パレット | H〜Rn、骨格19種、置換基32種、配位子21種、アミノ酸28状態、配位中心、分子サムネイル付き一覧（日本語名・接続点表示・HiDPI対応）、検索、マイ部品の保存・読込・登録解除 |
| 3D画面 | 回転・平行移動・ズーム、+X/+Y/+Z、3軸回転スライダー、番号、水素、非表示Hのマーカー、配位矢印、固定・同位体表示、配位サイト、末端追加ハンドル、PNG |
| 選択 | 原子・結合・部品・残基・連結分子・全体、番号と範囲指定、矩形、右クリックメニュー |
| 編集 | 原子追加、結合途中への挿入、元素変更、置換基付加、部品全体の置換（1接続点）、単・二重・三重・芳香族・配位結合、座標表、座標適用、平行移動、複製、削除、水素補完 |
| 寸法・立体 | 距離・角度・IUPAC二面角の測定と設定、結合片側の選択、回転、鏡映、不斉中心の置換基交換、E/Z幾何の切替、重心・主慣性軸、原子順の向き合わせ、R/S・E/Z判定と記録 |
| 原子属性 | 形式電荷、不対電子、同位体・質量、ゴースト、固定、元素順・残基順・選択優先の並べ替え |
| 配位 | 12形状、中心配置、単座・二座配位子、サイト指定、配位子交換・移動、形状・距離・酸化数、個別サイト方向、単座配位子のサイト交換、供与H方向の調整 |
| ペプチド | 1/3文字配列、L/D、側鎖状態、荷電/中性末端、ACE/NME、N/C末端追加、連続追加、残基置換・削除、鎖の分割・再連結、phi/psi/omega/chi、配列リンク、手動登録、結合グラフからの残基認識、欠損側鎖補完 |
| 断片 | FMO / CP / SAPT0、連結成分・1/2残基相当、Cα–C(=O)切断、手動割当、金属と近接する断片の統合、電荷・多重度・BDA:BAA、色分け、対応表CSV |
| 点電荷 | 追加・更新・削除、TIP3P水の変換、QM/MM有効・無効、KAGAMIの `x y z q` 出力 |
| 計算準備 | 全電荷・多重度、最適化設定、凍結・拘束・スキャン参照、削除時の参照失効、保存時の原子番号再割当 |
| 編集履歴 | 候補の確定・取消、Undo/Redo、失敗した編集のロールバック、未保存表示、保存前の候補処理 |
| 整形 | Open Babel MMFF94 / UFF、全体・局所整形、3D再生成、固定の保持、処理中止 |

## 元GUIとの差異・未移植部分

全機能の完全互換移植ではありません。特に以下は今後の移植対象です。

- 元GUIのPDB専用ウィザードと画面構成は異なります。「PDBの診断・修復 → FMO」で残基診断・欠損補完・再認識・分割候補を作り、主画面で確認・確定します。補完は主鎖N/CA/Cと標準アミノ酸テンプレートが揃う残基を対象とし、未知の残基や欠損主鎖は自動修復しません。MODEL/ALTLOC選択と原子名・鎖情報を保持します。
- KAGAMIエンジンと同じ基底/ECP・距離判定を用いる断片検証。ここでは原子の網羅・番号・切断結合・断片数などを検査します。既存の断片設定をNWChem用の計算条件へ変換する機能は未対応です。
- KAGAMI入力ファイル自体の生成、v1プロジェクト移行、英語UI、VTK/GPU描画、結果ファイル読込。

R/S・E/Zの判定・記録と結合グラフからの残基認識には、オプションのRDKit C++ライブラリを使います。この環境では有効です。見つからない環境では該当ボタンを無効化します。

3D表示はQPainterで奥行き順に描画します。整形・SMILES生成をRDKitからOpen Babelへ変更したため、元GUIとは座標や収束結果が異なります。金属・配位結合・ゴーストの力場整形は拒否します。マイ部品の付加は1接続点、配位子は1/2供与原子に対応します。複数フレームのXYZは拒否します。

既存プロジェクトの実行設定・結果参照も保持します。拘束・スキャンはKAGAMIの `section/key/text/ids/span` 形式で保存し、並べ替え後の番号へ変換します。削除で無効になった設定は保留として保存します。断片割当と点電荷は `document` と `job.sections` の両方へ反映します。

### 座標タブの検証

`ctest --test-dir build --output-on-failure` で従来のcore/editorとcoordinatesの回帰テストを実行します。coordinatesは単位変換と精度、セル単位のロールバック、同位体・ゴースト・固定、選択同期、原子追加、候補・Undo/Redo、構造置換時の参照失効・envelope保持、出力単位の保存、並べ替え後の固定番号を検査します。

任意の目視・実機検証用に、次のコマンドでスクリーンショットとNWChemへの試験入力（STO-3G、水＋炭素ゴースト、重水素、酸素固定）を生成できます。通常のCTestはNWChemを必要としません。

```sh
QT_QPA_PLATFORM=offscreen MOLFORMA_COORDINATE_ARTIFACTS=/tmp/molforma-coordinates \
  ./build/test_coordinates nwchemExportAndSettings
```

2026-10-22の検証では、上記のÅ/Bohr入力をNWChem 7.3.1で実行し、固定原子4とゼロ核電荷の `bqC` が認識され、SCFエネルギーがそれぞれ −74.945716644654 / −74.945716644653 Hartreeとなることを確認しています。

## 操作例

### Ala–Gly–Ser

1. 新規分子を開き、「残基」の配列に `AGS` を入力します。
2. N末端をプロトン化、C末端をCOO−にして「配列から構築」を押します。
3. 候補の **C8H15N3O5、31原子、形式電荷0** を確認し、確定します。
4. 「割当」でFMO・2残基ずつを選び、分割案を生成します。
5. 2断片、電荷 `1,-1`、Cα–C(=O)切断を確認して適用します。

### 四面体の亜鉛錯体

1. 配位中心パレットからZnを選びます。
2. 「配位」の形状を四面体、距離を2.3 Å、酸化数を2にします。
3. 空白をクリックして中心を置きます。
4. 配位子パレットから塩化物を選び、サイト1〜4へ順に配置・確定します。
5. 「割当」で計算の全電荷を−2、多重度を1にします。

### 水二量体のCP割当

1. 水の骨格を配置し、選択単位「全体」で選択します。
2. 「編集 → 寸法・立体」の複製間隔を `0,0,4` Åにして複製します。
3. 「割当」でCounterpoise・連結成分を選びます。
4. 3原子ずつの2断片・電荷 `0,0` を確認して適用します。

## テスト

`ctest --test-dir build --output-on-failure` を使用します。

- 全101テンプレートの生成・再読込、元KAGAMIの4プロジェクト往復。
- 不正XYZ、同位体、水素補完、付加、数値寸法、固定・環内結合の拒否。
- ペプチドの組成・末端・置換・キャップ、配位距離・二座サイト条件。
- FMO境界、CP、拘束・スキャン番号再割当、TIP3P。
- SMILES、固定を含む整形、SDF/PDB往復。
- GUIクリック配置、Delete/Esc、配列構築、候補確定・取消、Undo/Redo、失敗時の原状保持。
- 骨格サムネイルの表示・検索後の配置、残基状態名の区別、マイ部品のサムネイル更新とHiDPI表示。
- キャップ付き残基の置換・削除・末端切替、部品置換、座標・結合を変えない金属近接断片の統合。

この環境では、C++で新規作成したペプチド、拘束・スキャン付き水分子、亜鉛錯体、キャップ付きペプチドの4例を元KAGAMIで読み込み、設定参照の検査と計算入力生成も確認しました（計算実行は対象外）。この相互運用確認だけに元KAGAMIのPythonコードを使用し、アプリと通常テストには依存させていません。

ReleaseビルドのQt Testはコア26件・分子GUI8件・座標11件（初期化・終了処理を含む）が成功しています。座標テストはAddressSanitizerでも確認しています。別ディレクトリへのインストール後のX11起動・画面保存と、ワーカーのSMILES生成・固定原子を保持する整形・不正SMILESのエラー応答も確認しています。

`tests/fixtures/` はKAGAMI作例の4ファイルです。スクリーンショットは `--screenshot` で再生成できます。
