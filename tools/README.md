# TCOAAL asset tools

Tools for working with a legally owned copy of *The Coffin of Andy and Leyley*
(Steam build 3.0.11). No game content is included.

| Tool | Purpose |
|---|---|
| `tcoaal_crypt.py decrypt <www> <out> [--names names.tsv]` | Decrypt every `TCOAAL` asset (data, img, audio, language pack) and add extensions |
| `tcoaal_crypt.py hash <url>…` | Show the on-disk hashed name for a url, e.g. `data/System.json` |
| `recover_names.py <www> [--extra deob.js]` | Recover original names where possible (all `data/*.json`; most assets are referenced by hash in the game data, so their real names aren't in the build) |
| `extract_loader.py <www>` | Rebuild the hidden game-logic script hidden in the plugin files |
| `deobfuscate.js <hidden.js>` | Inline its obfuscator.io string table to make it readable |

## Format

* **Filenames:** `sha256("<dir>/<Name>.<ext>").hex()[:16]`. A leading `!` and a `[BUST]` marker are kept.
  At runtime, `App.redirect()` tries the plain path first and falls back to the hashed path.
* **Asset container:** `"TCOAAL"` + `n:u8` + payload. Only the first `n` bytes are encrypted, and `n == 0` means the whole payload.
  Data files are fully encrypted; images and audio only encrypt their headers.
* **Key:** `m = 0; for c in UPPER(stem of on-disk name): m = (m << 1) ^ ord(c)`, then `k = (m + 1) & 0xFF`.
* **Cipher:** autokey XOR over the ciphertext: `p[i] = c[i] ^ k; k = ((k << 1) ^ c[i]) & 0xFF`.
* **Language pack** (`data/9c7050ae76645487`): `"LANGDATA"` + JSON.
* **Hidden script:** 7 `function _0x…_(){return "<base64>"}` chunks across YEP_CoreEngine, YEP_SaveEventLocations,
  GALV_RollCredits and NonCombatMenu. The loader at the end of NonCombatMenu.js joins them, base64-decodes,
  zlib-inflates and injects the result as a `<script>`. That script holds `Crypto.dekit`, the hashed-path redirects,
  the language system, save handling and the DRM check. The DRM check is a djb2 of
  `www/Copyrights - Coffin of Andy and Leyley.txt` against `0x6ee0ff1a`.
