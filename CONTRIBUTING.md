# Contributing

thanks for wanting to help. swifty is a local music player in C++ and Qt Quick on
a rust audio core. it follows Material 3 Expressive and runs on linux, macos and
windows. it is a fork of [Swiftify](https://github.com/swiftify-org/swiftify), the spotify
client it was made from, and has nothing of spotify left in it. a fix that also applies
to Swiftify is welcome there too.

> Dont expect frequent updates, maybe once in a while

## Before you start

- look through the open issues and pull requests so you dont do something twice
- for anything bigger than a fix, open an issue first and say what you want to change
- branch off an up to date `main`

## Building

the requirements and the steps for each platform are in the
[readme](README.md#building-from-source). for development keep the tests on

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
```

the first configure needs internet, it fetches corrosion and two fonts, each
pinned by hash. on windows use `tools/build-windows.ps1` as the readme says.
build in a fresh folder: files left from an older version get compiled in with the new.

## Where things are

| path | what lives there |
|---|---|
| `src/` | the C++ side: app and settings, playback, the library (your files, their tags, playlists, favourites, lyrics and artist pictures), the rust bridge in `core`, theme, rendering, artwork, audio and the system integration (MPRIS, Discord rich presence, the sleep and device monitors on linux, the media controls on macos and windows) |
| `qml/` | the ui, with the `design` singletons (Appearance, Shapes, UiState), components, views, the player, the immersive player, visualizers and settings |
| `shaders/` | glsl, compiled to `.qsb` by `qt_add_shaders` |
| `audio-core/` | the rust core: playing files, the equalizer, tempo and pitch, the spectrum, and reading and writing tags. built through corrosion with the `capi` feature |
| `tools/shape_atlas` | draws Material 3's shapes into the distance field atlas `MaterialShape` reads, at build time |
| `tools/` | the build scripts for debian, fedora (rpm), a linux tarball, a macos dmg and windows |
| `third_party/` | material-color-utilities and rounded-polygon-qmljs |
| `tests/` | Qt Test suites, `bench_frames`, two python checks and a check of the windows build script |
| `packaging/` | desktop entry, icons, PKGBUILD, the rpm spec and the macos Info.plist |
| `THIRD_PARTY_LICENSES.md` | what swifty is made from and under which licences |

## Tests

every test has to pass and the build has to be free of warnings.

- `tst_views` draws every view on the gpu and fails on any qml warning, so a warning is a bug
- `tst_views` also checks that nothing animates at rest, add new views to it
- `tst_local` also covers the SQLite playlists and library cache (the playlists need Qt's SQLite driver to be installed to run)
- `tst_local`, `tst_playback`, `tst_lyrics`, `tst_artistimages` and `tst_settings` cover the library, the player, lyrics, artist pictures and settings (including moving an older Swiftify's data)
- `icon_names` checks that every material symbol named in qml exists in the pinned font
- `qml_statements` catches script lines that open with `(` or `[`
- `windows_build_script` (on windows) runs `tools/build-windows.ps1` against a fake toolchain
- `bench_frames` measures what a frame costs, ctest skips it, run it by hand when you touch timers or frame requests

lint the qml you changed. leave nothing new besides "can be shadowed" or "could
not compile" notes

```sh
qmllint --compiler warning -I build/qml_modules <files>
```

the rust core can be tested on its own

```sh
cd audio-core
cargo test --no-default-features --features capi
```

## Rules

### keep it light

swifty exists to be light, so cpu and ram cost matter as much as how it looks. even
with every effect turned on it should stay light on a modest computer.

- an idle or hidden window should cost close to nothing
- dont add timers, polling or per frame work when a signal or binding does the job
- free images, buffers and models when a view goes away instead of keeping them around
- if a change touches rendering, run `bench_frames` before and after and say what changed in the pr
- if a feature costs a lot, make it something people can turn off


### nothing animates at rest

when nothing plays and nobody touches the window, nothing on screen should move.
animations are fine in two cases

- on hover or interaction, and they stop when it ends
- during playback, for things like the spinning shapes and squiggles. drive these off `UiState.spin` instead of your own timer or `NumberAnimation`. it only turns while music plays and the window is visible, so they stop on pause or when the window is hidden. it also ticks at 30 hz so slow motion doesnt redraw the window at full rate

- never lower an existing frame rate, compare frame counts before and after changing timers or frame requests

### quiet by default

a normal run prints nothing to the terminal. status lines about the sound, the devices, the
media controls and the library go through `diagnostic()` from `src/core/Diagnostics.h`, which
prints only when swifty is started with `SWIFTY_DEBUG=1`. what has actually failed is always
told, and never through a logging category alone: some desktops turn Qt's logging off.

### online lookups

lyrics (LRCLIB, MusicBrainz) and artist pictures (Discogs) are the only things that go online,
and each has a switch in the settings. a new lookup needs its own switch and a line in
the readme saying what is sent where. Discord rich presence stays on the computer: it only
talks to the Discord app's local socket.

### storage

playlists (`local-playlists.db`) and the library cache (`local-files.db`) are SQLite, through
Qt SQL. keep the work off the window's thread when it can be slow (the cache is read and
written on a worker thread), write only what changed, and keep a change in one transaction.
dont put keys or tokens in the repository: the Discogs token and the Discord application id
live in the user's settings file, outside it.

### qml

- use `pragma ComponentBehavior: Bound` and typed properties and functions so the AOT compiler takes them
- animations respect `Appearance.reduceMotion` and `Appearance.duration()`
- new qml files are picked up by glob, you dont need to list them

### shapes and shaders

- draw shapes with `MaterialShape` or `ShapeIcon`, by name from `qml/design/Shapes.qml`, never as hand drawn paths
- shape names follow the atlas order in `tools/shape_atlas/ShapeAtlasLayout.h`, a test checks this
- a `ShaderEffect` has to declare a property for every uniform in its shader, or qt warns and the tests fail

### licences

new code and dependencies need a line in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md),
the licence file if the code is vendored, and a licence that can sit under the
GPL-3.0-or-later. keep the original authors' notices on code taken from elsewhere.

### no filler

a note or description in the ui has to tell the user something the label
doesnt. a cache setting doesnt need a note saying it caches songs. the same goes for code comments.

## AI Usage

Unreviewed code from an LLM or AI Model wont be accepted.

## Commits

- start with `feat:` for a feature or `fix:` for a fix
- the body says what changed in a line or two
- bump the version in `project(Swifty VERSION x.y.z)` in `CMakeLists.txt`, patch for fixes and minor for features

```
fix: remove broken effect for textbuttons

textbuttons getting hold-clicked would turn into rectangles, this was removed
```

```
feat: add a most played shelf

songs listened to for half a minute are counted, and the twelve played most are
shown on home, version is 0.4.0
```

## Pull requests

- keep one change per pull request
- say what changed, how you tested it and on which platform
- add a screenshot for ui changes

## License

swifty is under the GNU General Public License, version 3 or any later version
([LICENSE](LICENSE)), made from Swiftify and other projects
([THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)). by sending a pull request you agree that
your change is under the same license.
