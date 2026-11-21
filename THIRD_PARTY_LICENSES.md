# Third party licences and credits

swifty is free software under the GNU General Public License, version 3 or (at your
option) any later version ([LICENSE](LICENSE)). it is made from other work, and uses
other projects under their own licences. this file lists them. it is installed with the
app in `share/licenses/swifty`, and goes in the dmg.

## Where swifty comes from

swifty is a fork of **Swiftify** by addenator, a spotify client built with Qt Quick and a
Rust core around librespot:

- <https://github.com/swiftify-org/swiftify> (current home)
- <https://github.com/shredos67/swiftify> (where it was before; old release links point here)

the fork keeps Swiftify's licence and its authors' copyright, removes everything spotify, and
plays local files instead. the interface follows Material 3 Expressive, with a lot of
inspiration from [end4](https://github.com/end-4)'s dots-hyprland.

## Code in this repository

| what | licence | where |
|---|---|---|
| material-color-utilities (Google's colour library, C++) | Apache-2.0 | `third_party/material-color-utilities` |
| rounded-polygon-qmljs (end-4's QML port of androidx graphics-shapes, with Material 3's shapes) | Apache-2.0 | `third_party/rounded-polygon-qmljs` |

each folder has its own `LICENSE`, and every install carries copies as
`material-color-utilities.LICENSE` and `rounded-polygon-qmljs.LICENSE`.

## Libraries it is built with

| what | licence | used for |
|---|---|---|
| [Qt 6](https://www.qt.io) (Base, Declarative, Shader Tools, D-Bus, Wayland) | LGPL-3.0 or GPL-3.0 (also sold commercially) | the app, the interface and the rendering |
| Qt SQL with its SQLite driver, and [SQLite](https://sqlite.org) behind it | LGPL-3.0 or GPL-3.0 for the driver; SQLite is public domain | playlists and the library cache |
| [QCoro](https://github.com/danvratil/qcoro) | MIT | coroutines over Qt |
| [Corrosion](https://github.com/corrosion-rs/corrosion) | MIT | building the Rust core from CMake |

the Qt that comes in the tarball, the dmg and the Windows install is Qt's own build, used
under the LGPL-3.0 and shipped as shared libraries you can replace. its source is at
<https://download.qt.io/official_releases/qt/>. those installs also carry the LGPL-3.0 text
(`qt.LICENSE`), a notice with Qt's version and where its source is (`qt.NOTICE`), and QCoro's
MIT licence (`qcoro.LICENSE`). Corrosion is only used at build time and is not shipped.

## The Rust audio core (`audio-core/`)

the direct dependencies are

| crate | licence |
|---|---|
| [symphonia](https://github.com/pdeljanov/Symphonia) 0.5.5 (decoding and reading tags) | MPL-2.0 |
| [rodio](https://github.com/RustAudio/rodio) 0.21.1 (playback) | MIT OR Apache-2.0 |
| [lofty](https://github.com/Serial-ATA/lofty-rs) 0.25.4 (writing tags and cover art) | MIT OR Apache-2.0 |
| [rustfft](https://github.com/ejmahler/RustFFT) 6.4.1 (the spectrum) | MIT OR Apache-2.0 |
| [hound](https://github.com/ruuda/hound) 3.5.1 (wav) | Apache-2.0 |
| [serde](https://serde.rs) and serde_json | MIT OR Apache-2.0 |
| [tokio](https://tokio.rs) | MIT |

together with the crates they pull in, the core links about 140 crates, and every licence text
they ship is in `packaging/licenses/rust-crates.LICENSE`, installed as `rust-crates.LICENSE`. nearly all are
MIT or Apache-2.0 (some also offer Zlib, BSD or Unlicense, and unicode-ident adds the Unicode licence), and the symphonia crates are
MPL-2.0. the MPL-2.0 files are unmodified, and their source is at
<https://github.com/pdeljanov/Symphonia>. to list every crate and its licence for the exact
version you build:

```sh
cd audio-core
cargo metadata --format-version 1 | python3 -c "import json,sys;[print(p['name'],p['version'],p.get('license')) for p in json.load(sys.stdin)['packages']]"
```

## Fonts

downloaded when you build, each from a pinned commit and checked against a hash:

| font | licence | from |
|---|---|---|
| Google Sans Flex (interface text) | SIL Open Font Licence 1.1 | [google/fonts](https://github.com/google/fonts) `ofl/googlesansflex`. the licence text is installed beside the font |
| Material Symbols Rounded (icons) | Apache-2.0 | [google/material-design-icons](https://github.com/google/material-design-icons). the licence text is installed as `material-symbols.LICENSE` |

## Services

these are not code, and nothing of theirs is shipped. each is used under its own terms, and
each can be turned off in the settings:

- [LRCLIB](https://lrclib.net): synced lyrics
- [MusicBrainz](https://musicbrainz.org): song metadata used to find lyrics
- [Discogs](https://www.discogs.com): artist pictures, with your own personal token
- [Discord](https://discord.com): rich presence, through the Discord app on your computer
  and an application you make yourself. swifty talks to Discord's local socket and uses no
  Discord SDK or code

## Adding something

new code or a new dependency needs a line here, its licence file if it is vendored, and a
licence that can sit under the GPL-3.0-or-later. see [CONTRIBUTING.md](CONTRIBUTING.md).
