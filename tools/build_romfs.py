#!/usr/bin/env python3
"""Turn a legally owned TCOAAL install into a romfs tree for the QuickJS runtime.

  build_romfs.py <game dir or its www/> <out romfs dir> [--jsbc path/to/jsbc]

What it does (all on the user's own copy; nothing from the game is shipped):
  * copies www/ with every TCOAAL-encrypted asset decrypted in place
    (hashed file names are kept, the game resolves them itself)
  * extracts the game's hidden core script into js/plugins/_tcoaal_core.js and
    patches BUILD_FLAGS so the game neither decrypts again nor requires Steam
  * makes the hidden-script loader in NonCombatMenu.js load that file instead
  * replaces the asm.js Vorbis decoder plugins with the runtime's native one
  * precompiles every .js file to QuickJS bytecode (.jsbc) when --jsbc is given
"""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from extract_loader import main as extract_core  # noqa: E402
from tcoaal_crypt import SIG, decrypt  # noqa: E402

RUNTIME_JS = Path(__file__).resolve().parent.parent / "runtime" / "js"
CORE_NAME = "_tcoaal_core.js"
# Files the port does not need on the console.
SKIP_DIRS = {"greenworks", "languages/tool"}

# BUILD_FLAGS: 0x1 = Steam required, 0x80 = encrypted assets, 0x100 = hashed names.
CORE_PATCHES = [
    (r"BUILD_FLAGS=0x181\b", "BUILD_FLAGS=0x100"),
]


def patch(text: str, pattern: str, repl: str, what: str) -> str:
    new, n = re.subn(pattern, repl, text)
    if n != 1:
        raise SystemExit(f"patch '{what}' matched {n} times (expected 1); unsupported game version?")
    return new


def copy_www(src: Path, dst: Path) -> None:
    n = 0
    for f in sorted(src.rglob("*")):
        rel = f.relative_to(src)
        if any(rel.as_posix() == d or rel.as_posix().startswith(d + "/") for d in SKIP_DIRS):
            continue
        out = dst / rel
        if f.is_dir():
            out.mkdir(parents=True, exist_ok=True)
            continue
        data = f.read_bytes()
        if data.startswith(SIG):
            data = decrypt(data, f.name)
            n += 1
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
    print(f"copied www, decrypted {n} assets", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--jsbc", type=Path, help="host jsbc compiler from the runtime build")
    a = ap.parse_args()

    www = a.game / "www" if (a.game / "www").is_dir() else a.game
    if not (www / "js" / "plugins" / "NonCombatMenu.js").exists():
        raise SystemExit(f"{www} does not look like the game's www/ folder")

    if a.out.exists():
        shutil.rmtree(a.out)
    game_www = a.out / "www"
    copy_www(www, game_www)

    plugins = game_www / "js" / "plugins"
    core = extract_core(www)
    for pat, repl in CORE_PATCHES:
        core = patch(core, pat, repl, pat)
    (plugins / CORE_NAME).write_text(core, encoding="utf-8")

    ncm = plugins / "NonCombatMenu.js"
    text = ncm.read_text(encoding="utf-8")
    text = patch(text, r"function _\(\)\{", "function _(){return __loadScript('js/plugins/%s');" % CORE_NAME,
                 "hidden script loader")
    ncm.write_text(text, encoding="utf-8")

    # The runtime provides `stbvorbis` natively; keep the file names the plugin loads.
    for name in ("stbvorbis_stream.js", "stbvorbis_stream_asm.js"):
        (plugins / name).write_text("// replaced by the native decoder in the port runtime\n", encoding="utf-8")

    rt_out = a.out / "runtime"
    shutil.copytree(RUNTIME_JS, rt_out)

    if a.jsbc:
        scripts = sorted(p for p in a.out.rglob("*.js"))
        # One compiler process for all files keeps this fast.
        subprocess.run([str(a.jsbc), "--root", str(a.out), *map(str, scripts)], check=True)
        print(f"compiled {len(scripts)} scripts to bytecode", file=sys.stderr)


if __name__ == "__main__":
    main()
