# Swifty

a local music player for desktop, built with Qt Quick on top of a Rust audio core.
it plays the music files on your computer: no account and no login. it follows
Material 3 Expressive (heavy inspo from end4).

swifty is a fork of [Swiftify](https://github.com/swiftify-org/swiftify) by shred0s,
which was a spotify client. everything spotify is gone from this fork: the player
only knows your own files. forked by ImAlekzzz.

> don't expect frequent updates, maybe once in a while.

![Home](assets/Home.png)

<table border="0">
  <tr>
    <td><img src="assets/Play-Eq.png" alt="equalizer, tempo and pitch" width="400"/></td>
    <td><img src="assets/Play-Lyrics.png" alt="synced lyrics in the player" width="400"/></td>
  </tr>
</table>

**[Documentation](https://imalekzzz.github.io/swifty/)**
[Install](#install)
[Privacy](#privacy)
[Troubleshooting](#troubleshooting)
[License](#license)

runs on linux (made for wayland and hyprland), macos and windows.

## What it does

- plays **mp3, flac, ogg, m4a, aac, alac, wav and aiff** from folders or files you add
- **Home, Search, Songs, Albums, Artists, Playlists** and **Favourites**, with the title, artist, album,
  genre and cover read from each file's own tags
- a player with **shuffle, repeat, a queue and a sleep timer**, that remembers where you stopped
- an **equalizer** (-12 to +12 dB), **tempo** (0.5× to 2×), **pitch** and **crossfade**
- the **immersive player** with **visualizers** and **synced lyrics**
- a **tag editor** that writes changes back into your files
- **lyrics** from LRCLIB and MusicBrainz, a `.lrc` file beside the song, or the file's tags
- **artist pictures** from Discogs (with your own free token), a file in the artist's folder, or a cover
- **Discord rich presence** (*Listening to*), through the Discord app on your computer
- **MPRIS**, media keys and the tray on linux, and media controls on windows
- a **command line** and `swifty://` links, to use from keybinds

## Install

there are no prebuilt packages in the repo: swifty is built from source, and the scripts in `tools/`
make packages for each system (`build-debian.sh`, `build-rpm.sh`, `build-linux-tarball.sh`,
`build-macos-dmg.sh` and `build-windows.ps1`). you need Qt 6.10+, QCoro, CMake, Ninja, a C++20
compiler and [Rust](https://rustup.rs). build in a fresh folder.

**Arch Linux**

```sh
git clone https://github.com/ImAlekzzz/swifty.git
cd swifty/packaging/arch
makepkg -si
```

**Linux, macOS (cmake)**

```sh
git clone https://github.com/ImAlekzzz/swifty.git
cd swifty
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build
cmake --install build
```

**Windows**

```powershell
git clone https://github.com/ImAlekzzz/swifty.git
Set-Location swifty
.\tools\build-windows.ps1
```

the requirements per system, the Debian, Fedora, macOS and Windows steps and their options are in the
[install guide](https://imalekzzz.github.io/swifty/install).

## Command line

only one swifty runs at a time. launching it again passes the command to the one that's running,
so these work great as keybinds:

| flag           |                           |
| -------------- | ------------------------- |
| `--immersive`  | open the immersive player |
| `--play-pause` | play or pause             |
| `--next`       | next song                 |
| `--previous`   | previous song             |

`swifty://` links do the same from a browser or a launcher. more in the docs.

## Privacy

swifty has no account, no analytics and no telemetry, and the author receives nothing. it keeps its
settings, playlists and caches on your own computer (`~/.config/Swifty`, `~/.local/share/Swifty` and
`~/.cache/Swifty` on linux).

the only things that leave your computer are the lookups below, and both can be turned off in the settings.
every request also carries your IP address and a `Swifty/<version>` user agent.

| what            | sent to                                                                   | what is sent                                                                       |
| --------------- | ------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| lyrics          | [LRCLIB](https://lrclib.net), then [MusicBrainz](https://musicbrainz.org) | title, main artist, album and length (LRCLIB), title and main artist (MusicBrainz) |
| artist pictures | [Discogs](https://www.discogs.com)                                        | the artist's name, and your token                                                  |

Discord rich presence talks to the Discord app on your own computer, not over the network by swifty.
the full [privacy policy](https://imalekzzz.github.io/swifty/privacy) and the
[terms of use](https://imalekzzz.github.io/swifty/terms) are in the docs.

## Troubleshooting

swifty prints nothing to the terminal when all is well. to see what it is doing, start it with
`SWIFTY_DEBUG=1`:

```sh
SWIFTY_DEBUG=1 swifty 2>&1 | grep -a "^swifty "
```

- **playlists don't survive a restart:** Qt's SQLite driver is missing (on debian/ubuntu it's `libqt6sql6-sqlite`)
- **nothing shows on Discord:** the Discord desktop app must be running, with *Share my activity* on
- **it runs an old version after an update:** quit it first (`pkill swifty`)

the rest is in the [troubleshooting page](https://imalekzzz.github.io/swifty/troubleshooting).

## License

swifty is free software, under the GNU General Public License, version 3 or any later
version. the full text is in [LICENSE](LICENSE), and it comes with no warranty. you may use, study, change
and share it, and anything you share has to stay open source under the same license.

it is made from [Swiftify](https://github.com/swiftify-org/swiftify) by shred0s, and uses other projects under
their own licences: Qt, SQLite, QCoro, the Rust crates the audio core is built from and the code in
`third_party/`. the fonts are downloaded when you build, under their own licences. they are all listed in
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md), and every install carries it, with the licences it needs,
in `share/licenses/swifty`.
