#!/usr/bin/env python3
"""Recover the hidden decryption script that TCOAAL embeds in its plugins.

The game hides base64 chunks as `function _0xNNNN_() { return "..." }` at the
ends of several plugin files. A loader in NonCombatMenu.js concatenates them in
a fixed order, base64-decodes, zlib-inflates and injects the result as a script.
"""
import base64, re, sys, zlib
from pathlib import Path

def main(www: Path) -> str:
    plugins = www / "js" / "plugins"
    chunks = {}
    for f in plugins.glob("*.js"):
        for name, body in re.findall(r'function (_0x[0-9a-f]+_)\(\)\s*\{\s*return "([^"]*)";', f.read_text(encoding="utf-8")):
            chunks[name] = body
    loader = (plugins / "NonCombatMenu.js").read_text(encoding="utf-8")
    m = re.search(r"=((?:_0x[0-9a-f]+_\(\)\+?)+),", loader)
    order = re.findall(r"(_0x[0-9a-f]+_)\(\)", m.group(1))
    blob = "".join(chunks[n] for n in order)
    return zlib.decompress(base64.b64decode(blob)).decode("utf-8")

if __name__ == "__main__":
    sys.stdout.write(main(Path(sys.argv[1])))
