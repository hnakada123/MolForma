# SPDX-License-Identifier: GPL-2.0-or-later
"""Puts the shots into docs/ as the manual keeps its images, and lists new feature images in manual.qrc.

    publish.py features|recipes ja|en... [name...]

Copies $MOLFORMA_SHOTS_DIR/shots/<language>/<kind>/** (or only the files whose path relative to it starts
with one of the names) to docs/<kind>/images (Japanese) or docs/en/<kind>/images (English), reduced to 256
colors with ImageMagick like the existing images. Then the pages and images under docs/features and
docs/en/features are listed again in docs/manual.qrc.
"""
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from molforma_gui import REPO, WORK  # noqa: E402

FEATURE_PAGES = ["molecule", "molecule_pages", "coordinates", "calculation", "advanced", "execution",
                 "input_preview", "results", "history", "rendering", "app", "ai"]


def quantize(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["magick", str(source), "-strip", "-dither", "None", "-colors", "256", f"PNG8:{target}"],
                   check=True)


def list_feature_pages():
    """The feature pages and their images, after features.md of each language in manual.qrc."""
    path = REPO / "docs" / "manual.qrc"
    lines = [l for l in path.read_text(encoding="utf-8").split("\n") if not re.match(r"\s*<file>(en/)?features/", l)]
    out = []
    for line in lines:
        out.append(line)
        for prefix in ("", "en/"):
            if line.strip() == f"<file>{prefix}features.md</file>":
                entries = [f"{prefix}features/{page}.md" for page in FEATURE_PAGES]
                for page in FEATURE_PAGES:
                    folder = REPO / "docs" / f"{prefix}features" / "images" / page
                    if folder.is_dir():
                        entries += [f"{prefix}features/images/{page}/{f.name}" for f in sorted(folder.glob("*.png"))]
                out += [f"  <file>{e}</file>" for e in entries]
    path.write_text("\n".join(out), encoding="utf-8")


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("features", "recipes"):
        sys.exit(__doc__)
    kind = sys.argv[1]
    languages = [a for a in sys.argv[2:] if a in ("ja", "en")]
    names = [a for a in sys.argv[2:] if a not in ("ja", "en")]
    qrc = (REPO / "docs" / "manual.qrc").read_text(encoding="utf-8")
    for language in languages:
        shots = WORK / "shots" / language / kind
        docs = REPO / "docs" / ("" if language == "ja" else "en") / kind / "images"
        for source in sorted(shots.rglob("*.png")):
            relative = source.relative_to(shots).as_posix()
            if names and not any(relative.startswith(n) for n in names):
                continue
            quantize(source, docs / relative)
            print("published", (docs / relative).relative_to(REPO), flush=True)
            entry = (docs / relative).relative_to(REPO / "docs").as_posix()
            if kind == "recipes" and f"<file>{entry}</file>" not in qrc:
                print("  not in docs/manual.qrc yet:", entry, flush=True)
    if kind == "features":
        list_feature_pages()


if __name__ == "__main__":
    main()
