MolForma for macOS (Apple silicon)

A molecular editor, input builder and result viewer for NWChem.
Site:   https://hnakada123.github.io/MolForma/en/
Source: https://github.com/hnakada123/MolForma

To install
  Open MolForma-*.dmg and drag MolForma to the Applications folder. Calculations need NWChem,
  which is installed separately, for example with Homebrew (brew install nwchem).

To open it the first time
  MolForma is not signed with an Apple Developer ID or notarized, so macOS refuses to open it
  at first ("Not Opened" or similar). Open it, then press "Open Anyway" under Security in System
  Settings > Privacy & Security and open it again. Or, in the Terminal:

    xattr -dr com.apple.quarantine /Applications/MolForma.app

What the computer needs
  A Mac with Apple silicon (arm64) and macOS 27 or later. Intel Macs and earlier versions of macOS
  cannot run it: the libraries come from Homebrew, built for macOS 27. Homebrew itself is needed
  only for NWChem. Downloads from the PDB use TLS 1.2, through macOS.

In this folder
  MolForma-*.dmg        MolForma with the libraries it needs
  LICENSE               MolForma's license, the GNU GPL version 2
  NOTICE                Notes on the libraries, NWChem and how to cite it
  SOURCES.txt           Where the source of everything in the disk image is
  licenses/             The licenses of the bundled libraries, also inside the app

The manual is built in: Help > Guide and manual (F1).

--------------------------------------------------------------------------------

MolForma macOS版（Apple silicon）

NWChem向けの分子エディタ・計算入力作成・結果表示アプリです。
サイト: https://hnakada123.github.io/MolForma/
ソース: https://github.com/hnakada123/MolForma

インストールのしかた
  MolForma-*.dmg を開き、MolFormaを「アプリケーション」フォルダーへドラッグします。
  計算には別途NWChemが必要です（例: Homebrewの brew install nwchem）。

初めて開くとき
  Appleの開発者IDでの署名と公証をしていないため、最初はmacOSに開くのを止められます
  （「開けません」などの表示）。一度開こうとした後、「システム設定 → プライバシーとセキュリティ」の
  「セキュリティ」にある「このまま開く」を押して、もう一度開きます。ターミナルで次を実行しても開けます。

    xattr -dr com.apple.quarantine /Applications/MolForma.app

必要な環境
  Apple siliconのMac（arm64）とmacOS 27以降が必要です。IntelのMacと、より古いmacOSでは動きません
  （ライブラリがHomebrewのmacOS 27向けのものであるため）。Homebrew自体が必要なのはNWChemのためだけです。
  PDBからのダウンロードには、macOSのTLS 1.2を使います。

このフォルダーの内容
  MolForma-*.dmg        MolFormaと、動作に必要なライブラリ
  LICENSE               MolFormaのライセンス（GNU GPLバージョン2）
  NOTICE                ライブラリとNWChemについての告知、NWChemの引用
  SOURCES.txt           ディスクイメージに入っているもののソースの入手先
  licenses/             同梱ライブラリのライセンス（アプリの中にもあります）

マニュアルは組み込まれています。「ヘルプ → 操作ガイド・マニュアル」（F1）で開きます。
