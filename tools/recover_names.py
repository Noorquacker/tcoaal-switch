#!/usr/bin/env python3
"""Recover original asset names for a decrypted TCOAAL tree.

On-disk names are sha256("<dir>/<Name>.<ext>")[:16]. We gather every string
that appears in the decrypted data/plugins/scripts, try it as a file name in
every asset folder, and keep the hashes that match a real file.

Output: TSV "hashed_rel_path<TAB>original_rel_path", consumable by
tcoaal_crypt.py decrypt --names.
"""
import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from tcoaal_crypt import SIG, decrypt, hash_path  # noqa: E402

FOLDER_EXT = {"img": ".png", "audio": ".ogg", "data": ".json"}
RPG_DATA = ["Actors", "Classes", "Skills", "Items", "Weapons", "Armors", "Enemies", "Troops",
            "States", "Animations", "Tilesets", "CommonEvents", "System", "MapInfos"]


def strings_in(obj, out: set):
    if isinstance(obj, str):
        out.add(obj)
    elif isinstance(obj, list):
        for v in obj:
            strings_in(v, out)
    elif isinstance(obj, dict):
        for k, v in obj.items():
            out.add(k)
            strings_in(v, out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("www", type=Path, help="original (encrypted) www/ folder")
    ap.add_argument("--extra", type=Path, nargs="*", default=[], help="extra text files to mine for names (e.g. deobfuscated script)")
    a = ap.parse_args()

    # Hashed files that need a name, keyed by relative path without extension.
    targets = {}
    for f in a.www.rglob("*"):
        if f.is_file() and f.read_bytes()[:len(SIG)] == SIG:
            targets[f.relative_to(a.www).as_posix()] = f
    folders = sorted({t.rsplit("/", 1)[0] for t in targets})

    words = set(RPG_DATA)
    words.update("Map%03d" % i for i in range(1000))
    texts = [p.read_text(encoding="utf-8", errors="replace") for p in a.extra]
    texts += [p.read_text(encoding="utf-8", errors="replace") for p in (a.www / "js").rglob("*.js")]
    for t in texts:
        words.update(re.findall(r"[\"']([^\"'\n]{1,120})[\"']", t))
    for rel, f in targets.items():
        data = decrypt(f.read_bytes(), f.name)
        if rel.startswith("data/"):
            try:
                strings_in(json.loads(data[8:] if data.startswith(b"LANGDATA") else data), words)
            except ValueError:
                pass

    # Strings may be paths ("pictures/Foo") or carry extensions; add basenames too.
    cands = set()
    for w in words:
        w = w.strip()
        if not w or len(w) > 120:
            continue
        for v in (w, w.rsplit("/", 1)[-1]):
            cands.add(re.sub(r"\.(png|ogg|m4a|json)$", "", v, flags=re.I))

    found = {}
    for folder in folders:
        ext = FOLDER_EXT[folder.split("/")[0]] if folder.split("/")[0] in FOLDER_EXT else None
        exts = [ext] if ext else [".png", ".ogg", ".json", ".ttf"]
        for c in cands:
            for e in exts:
                url = f"{folder}/{c}{e}"
                h = hash_path(url)
                if h in targets and h not in found:
                    found[h] = url

    for h in sorted(found):
        print(f"{h}\t{found[h]}")
    print(f"recovered {len(found)}/{len(targets)} names", file=sys.stderr)
    missing = sorted(set(targets) - set(found))
    if missing:
        print("unresolved (first 20): " + ", ".join(missing[:20]), file=sys.stderr)


if __name__ == "__main__":
    main()
