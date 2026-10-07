#!/usr/bin/env python3
"""Decrypt (and re-encrypt) The Coffin of Andy and Leyley asset files.

Format (recovered from the game's hidden loader script):
  "TCOAAL" | n:u8 | payload
Only the first n payload bytes are encrypted (n == 0 means the whole payload).

Key seed comes from the on-disk file name (the hashed name, extension dropped):
  mask = 0; for ch in stem.upper(): mask = (mask << 1) ^ ord(ch)
  key  = (mask + 1) & 0xFF
Then an autokey XOR over the ciphertext:
  plain[i] = cipher[i] ^ key; key = ((key << 1) ^ cipher[i]) & 0xFF

On-disk names are sha256(<relative url>)[:16], e.g. "img/pictures/Foo.png"
-> "img/pictures/<16 hex>". Hidden-name recovery is in recover_names.py.
"""
import argparse
import hashlib
import sys
from pathlib import Path

SIG = b"TCOAAL"


def seed(name: str) -> int:
    mask = 0
    for ch in Path(name).stem.upper():
        mask = (mask << 1) ^ ord(ch)
    return (mask + 1) & 0xFF


def decrypt(data: bytes, name: str) -> bytes:
    if not data.startswith(SIG):
        raise ValueError("missing TCOAAL signature")
    n = data[len(SIG)]
    body = bytearray(data[len(SIG) + 1:])
    if n == 0:
        n = len(body)
    key = seed(name)
    for i in range(min(n, len(body))):
        c = body[i]
        body[i] = c ^ key
        key = ((key << 1) ^ c) & 0xFF
    return bytes(body)


def encrypt(data: bytes, name: str, n: int = 0) -> bytes:
    """Inverse of decrypt(). n=0 encrypts everything (as the game does for data)."""
    if n > 255:
        raise ValueError("n must fit in one byte")
    body = bytearray(data)
    key = seed(name)
    for i in range(len(body) if n == 0 else min(n, len(body))):
        c = body[i] ^ key
        body[i] = c
        key = ((key << 1) ^ c) & 0xFF
    return SIG + bytes([n]) + bytes(body)


def hash_path(url: str, length: int = 16) -> str:
    """Utils.hashPath: hash the full relative url, replace the last component."""
    parts = url.replace("\\", "/").split("/")
    last = parts[-1]
    h = hashlib.sha256("/".join(parts).encode("utf-8")).hexdigest()[:length]
    if "[BUST]" in last.upper():
        h += "[BUST]"
    if last.startswith("!"):
        h = "!" + h
    parts[-1] = h
    return "/".join(parts)


MAGIC = [
    (b"\x89PNG", ".png"),
    (b"OggS", ".ogg"),
    (b"ID3", ".mp3"),
    (b"\x00\x01\x00\x00", ".ttf"),
    (b"OTTO", ".otf"),
    (b"wOFF", ".woff"),
    (b"\x1aE\xdf\xa3", ".webm"),
    (b"LANGDATA", ".cld"),  # game language pack: "LANGDATA" + JSON
]


def guess_ext(data: bytes) -> str:
    for magic, ext in MAGIC:
        if data.startswith(magic):
            return ext
    if data[4:8] == b"ftyp":
        return ".mp4"
    if data.lstrip()[:1] in (b"{", b"["):
        return ".json"
    return ""


def decrypt_tree(src: Path, dst: Path, names: dict | None = None) -> None:
    """Decrypt every TCOAAL file under src into dst; copy everything else.

    names maps hashed relative paths (no extension) to original relative urls.
    """
    names = names or {}
    count = 0
    for f in sorted(src.rglob("*")):
        if not f.is_file():
            continue
        rel = f.relative_to(src).as_posix()
        data = f.read_bytes()
        if data.startswith(SIG):
            data = decrypt(data, f.name)
            out_rel = names.get(rel) or rel + guess_ext(data)
            count += 1
        else:
            out_rel = rel
        out = dst / out_rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
    print(f"decrypted {count} files into {dst}", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("decrypt", help="decrypt a whole www/ tree")
    d.add_argument("src", type=Path)
    d.add_argument("dst", type=Path)
    d.add_argument("--names", type=Path, help="TSV of hashed<TAB>original (from recover_names.py)")
    h = sub.add_parser("hash", help="print the on-disk name for a url, e.g. data/System.json")
    h.add_argument("url", nargs="+")
    a = ap.parse_args()

    if a.cmd == "decrypt":
        names = {}
        if a.names:
            for line in a.names.read_text(encoding="utf-8").splitlines():
                k, v = line.split("\t")
                names[k] = v
        decrypt_tree(a.src, a.dst, names)
    else:
        for u in a.url:
            print(hash_path(u))


if __name__ == "__main__":
    main()
