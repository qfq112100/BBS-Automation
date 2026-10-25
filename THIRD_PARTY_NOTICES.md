# Third-party notices — BBS single-EXE distribution with optional tool runtimes

> 当前3.3.9：Rose1.4.9、Akari1.5.1及用户提供的ABC；LTK是未修改、保留原始官方签名的1.29.0配对，没有独立Manager。ABC未附完整开发源码或公开再分发授权。本包用于本机开发交接，不等于全部第三方软件开源或公开分发许可。详见[当前交接说明](<源码交接说明_3.3.9.md>)。


The BBS interface is a native x86 Win32 application with static C++ support. The downloadable distribution is one EXE, but optional tool-center applications are full third-party runtimes embedded as data and extracted into versioned user-owned directories before an explicit user launch. They are not linked into the native interface: Rose uses bundled Python and a .NET Framework Pengu loader; League Akari uses Electron/Chromium/Node. The former claim of no extracted runtime applies only to the original BBS core, fonts and visual assets, not these integrated tools. Font decompression and font registration remain memory-only. Open “关于与开源许可” to read/copy the embedded notices.

## Pinned tool-center integrations

- **Rose 1.4.9**: [official release](https://github.com/Alban1911/Rose/releases/tag/1.4.9), MIT copyright 2026 Alban and Florent; vendored Pengu Loader MIT copyright 2024 Pengu Loader. Preserve both complete notices and bundled dependency notices. [Rose/Pengu/CPython and shipped notices](<LICENSES/integrations/rose/>) · [archive and per-file hash proof](<LICENSES/integrations/rose/payload-provenance.json>). The original Rose release bytes are retained; the BBS packaging overlay adds the separately licensed original signed LTK 1.29.0 pair and notices.
- **League Akari 1.5.1**: [official release](https://github.com/LeagueAkari/LeagueAkari/releases/tag/v1.5.1), MIT copyright 2026 Hanxven. The MIT notice, Electron license, Chromium/Node notices and all 239 collected shipped dependency notice files are retained without changes. [Notices](<LICENSES/integrations/akari/licenses/>) · [notice hash inventory](<LICENSES/integrations/akari/license-inventory.json>) · [release/per-file hash proof](<LICENSES/integrations/akari/manifest.json>). Public README says rebuilding needs access to private packages; this source package does not claim a reproducible full Akari build or include unavailable private source.
- **LTK patcher pair, current tag 1.29.0**: only the original host/DLL pair is embedded as a dependency; original publisher signatures are preserved. The full GPL LTK Manager UI is not bundled. Retained [binary license](<LICENSES/integrations/ltk/LTK-PATCHER-LICENSE.md>) and [provenance records](<LICENSES/integrations/ltk/>) include older packaging history; records mentioning unsigned1.30.4 do not describe this active pair. This is a private development handoff, not a claim of public redistribution clearance.
- **ABC**: user-provided prebuilt application, observed UI1.91; executable/DLL bytes preserved. Personal account database rows and private seed values are not supplied, nor the original translation cache and logs. No complete ABC source or public redistribution license was provided; do not infer either.

Upstream source links and collected exact notice texts are recorded in the [integration evidence index](<LICENSES/integrations/README.md>) and [copied-file integrity manifest](<LICENSES/integrations/copied-files-sha256.json>). This is not complete legal clearance for every transitive component, asset, codec or source-offer obligation. Root MIT licenses do not blanket-license all dependencies. All original shipped notice files remain with their respective runtime payloads. No claim is made that signatures, static checks or checksums establish security, clean-machine readiness, Riot-policy compliance or future game compatibility. Programs run only after explicit user action; application/injection testing was not part of packaging verification.

## Source and runtime libraries

- nlohmann/json 3.11.3, MIT: [license](<LICENSES/nlohmann-json-MIT.txt>).
- LLVM-MinGW/MinGW runtime C++ support is statically linked. Exact supplied texts are under [runtime notices](<LICENSES/runtime/README.md>).
- Kernel32, User32, GDI/GDI+, COM, Shell, DWM when available, system UCRT, and Cabinet Compression API are Windows system components. For the native BBS core these are Windows components, not an additional browser or .NET runtime; the optional tool runtimes and their separate requirements are described above. Do not download system DLLs from arbitrary websites.

## Downloaded visual assets (no self-generated image material)

- Hero: NASA HiRISE PIA22783, “The Velvety Blue Dunes of Melas Chasma”, credit **NASA/JPL-Caltech/Univ. of Arizona**. [Official source](https://science.nasa.gov/photojournal/the-velvety-blue-dunes-of-melas-chasma/). No endorsement is implied. Official photograph/API were retrieved; live policy-body capture was blocked by publisher maintenance/limits, so no error page is claimed as license text. See [precise provenance and limitation](<LICENSES/visual/NASA-HiRISE-PIA22783-NOTICE.txt>).
- Noto Sans SC: SIL OFL 1.1, original Google Fonts variable outlines mechanically instanced/subset. Regular retains common Chinese paths; Bold is for fixed UI text. [OFL](<LICENSES/visual/Noto-Sans-SC-OFL-1.1.txt>).
- Font Awesome Free 6.7.2 solid: font OFL 1.1, icons CC BY 4.0 and code MIT as applicable. [Full license](<LICENSES/visual/Font-Awesome-Free-6.7.2-LICENSE.txt>).
- App shield: Google Noto Emoji v2.038 original downloaded PNG; only ICO format/size conversion. [Apache-2.0](<LICENSES/visual/Noto-Emoji-2.038-Apache-2.0.txt>).
- Full URL/hash/conversion records: [asset sources](<resources/assets/asset_sources.json>). Original generated v3 artwork is not used by this release.

The design references modern productivity workspaces, not copied proprietary website artwork: [Linear](https://linear.app/) and [Raycast](https://www.raycast.com/). All executable runtime resources are embedded, not fetched from those sites.
