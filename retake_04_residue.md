# 英語マニュアルの「Residue」ページのスクリーンショットを撮り直す

以下は、Linuxの作業環境でClaude Codeに渡すプロンプトです。

---

MolFormaの英語マニュアルのスクリーンショット `docs/en/features/images/molecule_pages/04_residue.png` を撮り直してください。

## 背景

英語の表示文を `assets/translations/en.json` で変えました。

- 変更前: "Keep the charges around as point charges"
- 変更後: "Keep nearby charges as point charges"

日本語の原文は「周りの電荷を点電荷として残す」です。macOSのフォントでは、変更前の文が右側の列（幅330 px）に収まらず、「Residue」ページの右端が欠けていました。英語マニュアル `docs/en/features/molecule_pages.md` の本文は変更済みです。ただ、上のスクリーンショットには変更前の文が写ったままです（「Active-site cut-out」のカードのチェックボックス）。

## 前提

- この変更を含むコミットが、作業ツリーにあること。`grep -n "Keep nearby charges as point charges" assets/translations/en.json` で1行出ることを確かめてください。出ない場合は作業を止めて、そう報告してください。
- 撮影の手順と必要なもの（Python 3とNumPy、ImageMagickの `magick`、非特権ユーザー名前空間 `unshare -rm`）は、`tools/screenshots/README.md` にあります。最初に読んでください。このページの撮影にNWChemは要りません。

## 手順

1. MolFormaをビルドします（変更した訳文を実行ファイルに組み込むため）。

   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j 4
   ```

2. 英語の「molecule_pages」のページを撮ります。このページの6枚がすべて撮られますが、使うのは `04_residue` だけです。

   ```sh
   python3 tools/screenshots/features.py shots en molecule_pages
   ```

3. 撮れた `build/screenshots/shots/en/features/molecule_pages/04_residue.png` を開いて確かめます。
   - 「Active-site cut-out」のカードのチェックボックスが "Keep nearby charges as point charges" になっていること。
   - 右側の列の内容が右端で欠けていないこと。
   - それ以外（分子 ACE–Ala–His–Glu–NME、配列の表示、カードの並び）が、今の `docs/en/features/images/molecule_pages/04_residue.png` と同じであること。

4. この1枚だけを `docs/` に反映します。256色に減らして、ほかの画像とそろえます。

   ```sh
   python3 tools/screenshots/publish.py features en molecule_pages/04_residue
   ```

5. `git status` で確かめます。変わったのが `docs/en/features/images/molecule_pages/04_residue.png` だけであることを確認してください。ほかの画像や `docs/manual.qrc` に意図しない差分が出た場合は、元に戻してから報告してください。

6. 画像の説明文を確かめます。`docs/en/features/molecule_pages.md` の画像の代替テキスト（`![The “Residue” page …](images/molecule_pages/04_residue.png)`）が新しい画像と合っていることを確認してください。チェックボックスの文は代替テキストに含まれていないので、通常は直す必要はありません。

7. テストを実行します（埋め込まれたページの画像やリンクを `test_translations` が確かめます）。

   ```sh
   ctest --test-dir build --output-on-failure
   ```

8. 変更をコミットします。件名は短い命令形の英語にします（例: `Retake the English Residue page screenshot`）。pushはしないでください。

## 報告すること

- 新しい画像の確認結果（上の3.の各項目）
- `git status` と `git diff --stat` の結果
- テストの結果（失敗があればその出力）
