# Documentation

swifty is a local music player for desktop, built with Qt Quick on top of a Rust audio core. it plays the music files on your computer: no account and no login. it follows Material 3 Expressive (heavy inspo from end4) and has an immersive player, synced lyrics, visualizers, an equalizer with tempo and pitch, favourites, playlists and a tag editor for your own files.

swifty is a fork of [Swiftify](https://github.com/swiftify-org/swiftify) by shred0s, which was a spotify client. everything spotify is gone from this fork: the player only knows your own files. forked by ImAlekzzz.

[Home](https://claude.ai/chat/Home.png)

runs on linux (made for wayland and hyprland), macos and windows.

**On this page**

1. [Getting started](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#getting-started)
2. [Using Swifty](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#using-swifty)
3. [The player](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#the-player)
4. [Your music](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#your-music)
5. [Lyrics](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#lyrics)
6. [Artist pictures](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#artist-pictures)
7. [Discord rich presence](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#discord-rich-presence)
8. [Keyboard shortcuts](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#keyboard-shortcuts)
9. [Command line and links](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#command-line-and-links)
10. [Settings](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#settings)
11. [Where Swifty keeps things](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#where-swifty-keeps-things)
12. [Privacy: what goes over the network](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#privacy-what-goes-over-the-network)
13. [Troubleshooting](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#troubleshooting)
14. [Credits and license](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#credits-and-license)

***

## Getting started

there are no prebuilt builds of this fork yet, so swifty is built from source. the [README](https://github.com/ImAlekzzz/swifty#building-from-source) has the steps for every system. in short:

| system              | how                                                                                                                                                                      |
| ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **Arch Linux**      | `cd packaging/arch && makepkg -si`                                                                                                                                       |
| **Linux (cmake)**   | `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$HOME/.local"`, then `cmake --build build && cmake --install build` |
| **Debian / Ubuntu** | `./tools/build-debian.sh --install-deps`                                                                                                                                 |
| **Fedora (rpm)**    | `./tools/build-rpm.sh --install-deps`, then `sudo dnf install dist/swifty-*.rpm`                                                                                         |
| **macOS**           | cmake build, then copy `build/swifty.app` to `/Applications/Swifty.app`                                                                                                  |
| **Windows**         | `.\tools\build-windows.ps1` in PowerShell                                                                                                                                |

you need Qt 6.10 or newer, QCoro, CMake, Ninja, a C++20 compiler and Rust. build in a fresh folder, not on top of an older checkout.

### First start

a clean start shows an **Add your music** card on Home. add a folder, a few files, or drop them onto the window. the same pickers are in Settings → Files.

swifty reads each song's title, artist, album, genre and cover from its own tags, so a library with good tags looks good right away.

***

## Using Swifty

the sidebar has these pages.

### Home

Home shows what is playing, your favourites, your playlists, what you played recently and what you play most.

[Home](https://claude.ai/chat/Home.png)

[Recently played on Home](https://claude.ai/chat/Home-Recently.png)

[Home with lyrics](https://claude.ai/chat/Home-Lyrics.png)

a song counts as played after half a minute of listening (or half of a short one). **Most Played** is made from those plays, and a song needs two plays to be in it.

### Search

Search looks through title, artist, album and genre. it is forgiving about spelling of genres: `hip hop` finds `Hip-Hop/Rap`.

### Songs, Albums and Artists

these three pages show everything in your library. the search field at the top filters the page you are on, and Songs, Favourites and Albums can be sorted.

| [Songs](https://claude.ai/chat/Songs.png) | [Albums](https://claude.ai/chat/Albums.png) | [Artists](https://claude.ai/chat/Artists.png) |
| ----------------------------------------- | ------------------------------------------- | --------------------------------------------- |

### Playlists and favourites

* **Playlists** are made from your own files.
* **Favourite Songs** is the star on any song, and is kept with the playlists.

[Favourite songs](https://claude.ai/chat/Favorite.png)

a playlist is only a list of where its songs are, so a song in ten playlists is still one file. if a file is moved or deleted it leaves the playlists until it is back where it was. a file from outside your library folders joins the library when you add it to a playlist.

### Settings

see [Settings](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#settings).

***

## The player

the player has shuffle, repeat, a sleep timer, a queue, and an equalizer with tempo and pitch. it remembers the song, the queue and the place in it when you close it, and offers it again, paused, when you open it.

[Player](https://claude.ai/chat/Play.png)

### Queue

the queue shows what plays next. songs can be added to it from a song's options menu.

[Queue](https://claude.ai/chat/Play-List.png)

### Lyrics

synced lyrics follow the song as it plays. the immersive player shows them full size. where they come from is explained in [Lyrics](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#lyrics).

[Lyrics in the player](https://claude.ai/chat/Play-Lyrics.png)

### Equalizer, tempo and pitch

the sound panel has:

* an **equalizer**, with a fader for each band from -12 to +12 dB
* **tempo**, from 0.5× to 2×
* **pitch**, from -12 to +12 semitones (available while tempo keeps the pitch)
* **crossfade** between songs, from 1 to 12 seconds, which you can switch off

[Equalizer](https://claude.ai/chat/Play-Eq.png)

### Immersive player and visualizers

the immersive player fills the window with the cover, the lyrics and the visualizers. you can open it from the player, or with `swifty --immersive`. what the visualizers look like (round or not, their shape and how sensitive they are) is set in Settings → Visualizer, and they can all be turned off there.

### Sleep timer

pick hours and minutes in the sleep timer, and playback stops when the time is up. what happens before that is _Fade Out_ in Settings → Playback → Sleep Timer: the song can just stop, fade out over the final seconds, or fade out slowly over the whole time (the volume can't be changed while it does).

***

## Your music

each song's title, artist, album, genre and cover come from its own tags. supported files are mp3, flac, ogg, m4a, aac, alac, wav and aiff. the **tag editor** writes changes back into the file, so only use it on files you are happy to change.

new files in your folders are found when you press _Look Again_, and when you come back to the window after more than five minutes.

settings, playlists and caches left by an earlier Swiftify are moved over on the first start. playlists made by older versions held copies of their files: the ones whose original is still there are moved back onto it and the copies are deleted (a copy whose original is gone is kept, so the song still plays).

***

## Lyrics

for each song, swifty looks in this order and uses the first that has lyrics, preferring synced (timed) ones:

1. **online**, from [LRCLIB](https://lrclib.net/) and then MusicBrainz, matched by title, artist and length (Settings → Playback → Lyrics, on by default)
2. a **`.lrc` file** with the same name beside the song
3. **lyrics inside the file's tags**

turn the online lookup off and swifty only reads your files. lyrics found online are kept in the cache folder, so a song is only looked up once.

***

## Artist pictures

artist pages and the Artists grid show a picture from, in this order:

1. [Discogs](https://www.discogs.com/), if you give it a free personal token in Settings → Files → Artist Pictures (discogs.com/settings/developers, _Generate new token_). Discogs refuses searches without one, so with no token it is not asked
2. a file called `artist` in the folder named for the artist, as a jpg, png, webp, gif, bmp, svg or ico (and tiff, avif, jxl or heic where your Qt can read them)
3. the cover of one of their songs

[Artists](https://claude.ai/chat/Artists.png)

pictures are looked up one artist at a time, a bit over a second apart, and kept in the cache folder. artist names are sent to Discogs, and nowhere else. this lookup can be turned off in the same place.

***

## Discord rich presence

swifty can show what is playing on your Discord profile, as _Listening to_ with the title, the artists, the album and how far into the song you are. nothing shows while it is paused.

it talks to the Discord app on your computer over its local socket (a unix socket, or a named pipe on windows), with no Discord SDK, and nothing goes through the network by swifty itself. it looks for Discord's normal socket, and the ones of the flatpak and snap installs.

it works out of the box, as _Listening to Swifty_. the presence shows on whoever is logged in to the Discord app on your computer, so nothing is sent to the author or tied to their account.

to show another name instead:

1. make an application at the [Discord developer portal](https://discord.com/developers/applications). its name is what Discord shows after _Listening to_
2. copy its **Application ID** (not the secret or the token)
3. paste it in Settings → Files → Discord (the grey number there is Swifty's own) and press enter. empty goes back to Swifty's

_Show what I'm playing on Discord_ in the same place turns it off.

the Discord desktop app has to be running (the browser version has no local socket), and _Share my activity_ has to be on in Discord's own Activity Privacy settings. local files have no public address, so there is no cover picture.

***

## Keyboard shortcuts

on linux and windows:

| keys                       | what it does                   |
| -------------------------- | ------------------------------ |
| `Space`                    | play or pause                  |
| `Ctrl` + `→` / `←`         | next / previous song           |
| `Ctrl` + `Alt` + `→` / `←` | seek forward / back 10 seconds |
| `Ctrl` + `↑` / `↓`         | volume up / down               |
| `Ctrl` + `Alt` + `M`       | mute                           |
| `Ctrl` + `Alt` + `S`       | shuffle                        |
| `Ctrl` + `Alt` + `R`       | cycle repeat mode              |
| `Ctrl` + `F`               | search                         |
| `Ctrl` + `,`               | settings                       |
| `Alt` + `←`                | go back                        |

on macOS swifty uses the menu bar for these.

***

## Command line and links

only one swifty runs at a time. launching it again just passes the command to the one that is already running, so these work great as keybinds:

| flag           |                           |
| -------------- | ------------------------- |
| `--immersive`  | open the immersive player |
| `--play-pause` | play or pause             |
| `--next`       | next song                 |
| `--previous`   | previous song             |

on hyprland for example:

```ini
bind = SUPER SHIFT, M, exec, swifty --immersive
bind = , XF86AudioPlay, exec, swifty --play-pause
bind = , XF86AudioNext, exec, swifty --next
bind = , XF86AudioPrev, exec, swifty --previous
```

### `swifty://` links

`swifty:` links open the app, or pass a command to the copy that is running, from a browser, a bookmark, a launcher or Discord:

```html
<a href="swifty://play-pause">play or pause</a>
```

the commands are `play-pause`, `next`, `previous`, `immersive`, `close-immersive`, `play-recent`, and the pages `home`, `playlists`, `songs`, `favourites`, `albums`, `artists` and `back`. `swifty://` alone (or a name it does not know) just raises the window. a link can never change a setting or name a file.

* **linux:** the desktop entry registers the scheme. from a build, run `update-desktop-database ~/.local/share/applications` after installing. test it with `xdg-open swifty://play-pause`.
* **macos:** the app registers it itself, once it has been opened from the dmg.
* **windows:** nothing registers it for you, see the README for the PowerShell snippet.

### Linux desktop integration

on linux swifty hooks into MPRIS (media keys, `playerctl`, shell media widgets, what's playing in KDE, GNOME or Caelestia), logind (pauses before the machine sleeps), pipewire/pulseaudio device changes (pauses when the output you play on goes away) and the tray. build with `-DSWIFTY_DESKTOP_INTEGRATION=OFF` to leave those out. closing the window ends the program.

***

## Settings

the settings dialog (`Ctrl` + `,`) has seven pages:

| page            | what is in it                                                                                                |
| --------------- | ------------------------------------------------------------------------------------------------------------ |
| **Visualizer**  | turn visualizers on or off, round or not, their shape, how sensitive they are                                |
| **Background**  | the live backdrop: turn it on or off, its blur and how much it reacts to the music                           |
| **Playback**    | the lyrics lookup, the sleep timer and its fade                                                              |
| **Files**       | your music folders, _Look Again_, artist pictures and the Discogs token, Discord                             |
| **Appearance**  | colours, window options                                                                                      |
| **Performance** | motion and shapes, and what the visualizers and backdrop cost (turn the live backdrop off for lower cpu use) |
| **About**       | version and credits, and debugging options                                                                   |

every change applies at once.

***

## Where Swifty keeps things

linux paths (other systems use Qt's standard folders for the app):

| what                                                                                                       | where                         |
| ---------------------------------------------------------------------------------------------------------- | ----------------------------- |
| settings (the Discogs token and the Discord application id too), favourites, play counts, the last session | `~/.config/Swifty/Swifty.ini` |
| playlists (`local-playlists.db`) and the library cache (`local-files.db`)                                  | `~/.local/share/Swifty/`      |
| covers, lyrics and artist pictures                                                                         | `~/.cache/Swifty/`            |

the cache can be deleted at any time, swifty makes it again. the playlist covers you chose are kept in `~/.local/share/Swifty/local-playlist-covers/`, so back that folder up if you care about them.

### Starting over

> this removes your settings, **playlists and favourites**. your music files are not touched.

```sh
rm -rf ~/.config/Swifty ~/.local/share/Swifty ~/.cache/Swifty
```

***

## Privacy: what goes over the network

swifty has no account and no tracking. the only things that go over the network are:

| what            | sent to                  | what is sent                                                                       | can be turned off                  |
| --------------- | ------------------------ | ---------------------------------------------------------------------------------- | ---------------------------------- |
| lyrics          | LRCLIB, then MusicBrainz | title, main artist, album and length (LRCLIB); title and main artist (MusicBrainz) | Settings → Playback → Lyrics       |
| artist pictures | Discogs                  | the artist's name, and your token                                                  | Settings → Files → Artist Pictures |

every request also carries your IP address, as any web request does, and a `Swifty/<version>` User-Agent with a link to this project. nothing else is sent: no file contents, no account, no identifiers. with both lookups off, swifty sends nothing at all.

Discord rich presence does not use the network from swifty: it talks to the Discord app on your own computer.

***

## Troubleshooting

swifty prints nothing to the terminal when all is well, and says so when a sound stream or the media controls fail. to see what it is doing about the sound, the devices, the media controls and the library, start it with `SWIFTY_DEBUG=1`:

```sh
SWIFTY_DEBUG=1 swifty 2>&1 | grep -a "^swifty "
```

**a shell shows nothing playing (linux).** swifty offers itself on the session bus as `org.mpris.MediaPlayer2.swifty`. with `SWIFTY_DEBUG=1` it says whether it got that name, and `busctl --user list | grep -i swifty` shows it from outside.

**no sound while the song plays.** if the sound system reports the output broken, swifty opens a new one once and carries on, and pauses if it breaks again within half a minute. with `SWIFTY_DEBUG=1` the lines `the audio output failed` and `opened anew` show it.

**playlists or the library do not survive a restart.** the SQLite driver is missing. start swifty from a terminal: it prints `Qt's SQLite driver (QSQLITE) is not installed` and names the package (on debian/ubuntu it is `libqt6sql6-sqlite`). until it is installed the playlists are kept in `local-playlists.json`, so nothing is lost, and they move into the database once it is.

**the launcher still shows an old icon.** run `gtk-update-icon-cache -f -t ~/.local/share/icons/hicolor`, then restart your panel or launcher.

**nothing shows on Discord.** check the application id, that the Discord desktop app is running, and that _Share my activity_ is on. to see what swifty is doing:

```sh
QT_LOGGING_RULES="swifty.discord.debug=true" swifty
```

**a link like `swifty://next` does nothing.** see [`swifty://` links](https://claude.ai/chat/2c60d625-6679-455b-890e-c0cc15767373#swifty-links) for how the scheme is registered on your system.

***

## Credits and license

swifty is a fork of [Swiftify](https://github.com/swiftify-org/swiftify) by shred0s, modified by ImAlekzzz. the design follows Material 3 Expressive, with heavy inspiration from end4.

swifty is free software, released under the GNU General Public License, version 3 or (at your option) any later version. see `LICENSE` for the terms and `THIRD_PARTY_LICENSES.md` for the libraries it uses, including Qt.
