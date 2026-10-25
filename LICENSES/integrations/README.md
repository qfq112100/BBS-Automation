# Pinned integration licenses and integrity evidence

This directory contains unmodified license/notice copies and static provenance records, not full upstream repositories, runtime executables or complete legal clearance. The native BBS UI remains a statically supported Win32 application; optional tool payloads are independent Python/.NET/Electron applications extracted from the single downloadable EXE before an explicit launch. Windows10/11 x64 and the documented tool-specific prerequisites still apply.

## Rose 1.4.9

- [Official release](https://github.com/Alban1911/Rose/releases/tag/1.4.9); source commit 146f7986b9606ffb7f05bcb78452c8e739c63ad1.
- [Original official update ZIP](https://github.com/Alban1911/Rose/releases/download/1.4.9/update_package_1.4.9.zip): 30,421,755 bytes, verified SHA256 1df1cc91219017c425d05c423566031a899a6865130ebd14b2adbe704af8bbc9.
- [Rose MIT notice](<rose/licenses/Rose-MIT-LICENSE.txt>) from [tagged source](https://raw.githubusercontent.com/Alban1911/Rose/1.4.9/LICENSE); copyright2026 Alban and Florent.
- [Pengu MIT notice](<rose/licenses/PenguLoader-MIT-LICENSE.txt>) from [vendored tagged source](https://raw.githubusercontent.com/Alban1911/Rose/1.4.9/vendor/PenguLoader-1.1.6/LICENSE); copyright2024 Pengu Loader.
- [CPython3.13.14 notice](<rose/licenses/CPython-3.13.14-LICENSE.txt>) from [official matching tag](https://raw.githubusercontent.com/python/cpython/v3.13.14/LICENSE). Python version was read from bundled DLL metadata, not executed.
- [Collected full notice bundle](<rose/THIRD_PARTY_NOTICES.txt>) and [available original payload notices](<rose/licenses/payload/>), including Tcl/Tk, websockets and cryptography.
- [Upstream archive/per-file provenance](<rose/payload-provenance.json>) and [static import record](<rose/static-pe-imports.json>). These records describe the original 1,161-file Rose release **before** the final BBS packaging overlay adds LTK and notices. Their missing-patcher fields are historical upstream facts, not a declaration that the final BBS overlay lacks the pair.

The tagged Rose README says patcher redistribution is excluded from Rose. This does not itself grant patcher rights. This integration instead relies on the separately reviewed exact-tag LTK custom binary license below; retain both sets of notices. Python packaging does not remove .NET Framework4.7.2 requirements for the Pengu loader.

## League Akari 1.5.1

- [Official release](https://github.com/LeagueAkari/LeagueAkari/releases/tag/v1.5.1); [official archive](https://github.com/LeagueAkari/LeagueAkari/releases/download/v1.5.1/league-akari-win-x64.7z).
- Archive88,579,402 bytes; verified SHA256 9b7819d22a50afe13f6a0b382d45ec1da60d3979d25fa7e36f99b8e93221224a, matching the publisher digest. [Preserved publisher asset listing](<akari/published-assets.html>).
- [Akari MIT notice](<akari/licenses/LeagueAkari-LICENSE.txt>) from [official tagged source](https://raw.githubusercontent.com/LeagueAkari/LeagueAkari/v1.5.1/LICENSE); copyright2026 Hanxven.
- [239-file dependency notice inventory](<akari/license-inventory.json>) and [all copied notice files](<akari/licenses/>), including original Electron license and Chromium/Node license HTML. Copyright/license bodies are copied byte-for-byte.
- [Release/per-file manifest](<akari/manifest.json>), [packaged application metadata](<akari/bundled-package.json>) and [static PE imports](<akari/pe-imports.json>). Original application bytes retained; one unmodified upstream MIT notice added to its packaging payload.

The public [tagged README/source entry](https://github.com/LeagueAkari/LeagueAkari/tree/v1.5.1) describes build access to private packages. This source package does not include unavailable private dependencies or promise reproducible rebuilding of the full Akari application. Root MIT is not a blanket determination for Chromium, FFmpeg, native modules, dependency source offers or every codec/build variant. The complete shipped runtime and its license files must stay together.

## LTK patcher pair from Manager 1.30.4

- [Official release/source](https://github.com/LeagueToolkit/ltk-manager/releases/tag/v1.30.4), source commit2adda384347dd43e76d6295d77fe1f13623b2808.
- [Exact full custom patcher license](<ltk/LTK-PATCHER-LICENSE.md>) from [official tag](https://github.com/LeagueToolkit/ltk-manager/blob/v1.30.4/LTK-PATCHER-LICENSE.md); effective2026-07-14.
- Only ltk_patcher_host.exe and ltk_patcher_dll.dll are repackaged, as one coherent unsigned pair, both beside Rose tools and as a standalone dependency component. The full GPL-3.0-or-later Manager application is not included. Its separate GPL terms are not the license for this custom binary pair.
- Under custom license section2, official League Toolkit signatures were removed by static PE edits; no signature/endorsement is impersonated. [Original and transformed hashes/sizes](<ltk/payload-manifest.json>) record the modification, including certificate bytes removed; [official updater metadata copy](<ltk/latest.json>) preserves release attribution.
- Unsigned host SHA256: 2d05f2a4224805e419fe6952214579d0fc469c52e718c7a6b1dd7d9729a98613.
- Unsigned DLL SHA256: 1fe703397b9222cdb41d3fe3198305d2236c97e4788b484544265c6af38ce55d.
- Static inspected DLL cutoff2026-10-28T23:06:41Z applies to the game executable BUILD timestamp, not merely today's date. No future game compatibility or successful injection is promised. Pair replacement requires a coherent new version and renewed license/hash/compatibility review.

## Integrity and limits

[Copied-file SHA256 proof](<copied-files-sha256.json>) records every source-to-destination notice/metadata copy and preservation of the original embedded BBS notice body. Evidence hashes establish byte identity, not safety or legal clearance. No third-party app, patcher, injector or game was executed during this license-copy task. Existing user installations are not modified by these notice changes.

All shipped notice files remain in their respective application runtimes; these copies improve source-package access. Preserve copyright, permission and warranty disclaimers in full. Full transitive-dependency licensing, any corresponding-source/source-offer duties, codecs, skins and trademarks still require review. This directory makes no claim that all such obligations are resolved.
