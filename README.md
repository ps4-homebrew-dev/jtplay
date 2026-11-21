# JTPlay

An audio/video player for jailbroken PS4s and PS5s where the majority of
the code was produced by claude.ai

It is built for the couch: a 10-foot UI driven entirely by a remote or game
controller, browsing local drives, UPnP/DLNA media servers and JavaScript
plugins, in the spirit of Kodi.

## What it plays

- **Local storage**: USB drives, extended storage and `/data` on a console
  (the home folder on a PC), and the whole filesystem.
- **UPnP/DLNA servers** on your network: MiniDLNA, Jellyfin, Plex, ...
- **Plugins** (JavaScript), bundled:
  - *Internet Radio* (radio-browser.info), *SomaFM*, *Sveriges Radio*
  - *Podcasts*: the Apple Podcasts charts, and search for any show
  - *Internet Archive*: films, cartoons, concerts, old radio, audiobooks
  - *media.ccc.de*: Chaos Communication Congress talks, with subtitles
  - *Art gallery*: the Art Institute of Chicago, as a slideshow
  - *SVT Play*, *TV4 Play* (needs a sign-in token, no DRM content),
    *Pluto TV*, *Riksdagen*

## Getting started

Install the package, start jtplay, and:

1. **Sources** (a tab along the top) lists everything there is to play.
   Select opens a folder or plays an item, back goes back.
2. **The options menu** has everything else for what is in focus: add to
   Favorites, search, filter, sort, list or grid, settings of a plugin.
   Open it with the options button, or with **left** in any list.
3. **Favorites** holds what you saved, in folders of your own. **Home**
   has what you left unfinished: select carries on where you stopped.
4. While something plays, **back** returns to the menus and it plays on
   behind them; **back held** stops it.

## Controls

The same buttons on a game controller, a keyboard and a TV remote. Five are
enough for everything: the directions, select and back.

| Button     | Controller | Keyboard        | Remote (HDMI-CEC)            | Does                         |
|------------|------------|-----------------|------------------------------|------------------------------|
| directions | d-pad      | arrows          | arrows                       | move; in a list, left: options menu, right: the scroll bar (page by page) |
| select     | cross      | Return          | OK                           | open, play; playing: the controls |
| back       | circle     | Backspace       | back                         | back; playing: to the menus  |
| options    | options    | Menu, F4, F10   | options or menu; OK held     | the options menu             |
| triangle   | triangle   | T, Tab          |                              | add to / remove from Favorites |
| square     | square     | Space           |                              | play / pause; in lists: next sort |

While playing full screen, left / right seek (faster when held), up / down
jump five minutes, and the media keys of a remote work anywhere. The
complete list, per screen and device, is in
[docs/guide.md](docs/guide.md#controls).

## Your data

Favorites, resume points, settings and remembered places are in
`library.xml` and `settings.ini` in jtplay's cache directory (on a
console, the title's download data), next to the artwork cache. Each is
written whole or not at all, and the copy from the last start is kept as
a `.bak` beside it, used should the file ever be damaged.

## Plugins

Besides UPnP/DLNA servers and the local filesystem, media sources can be
added with JavaScript: every `*.js` file in the plugin directories is run
by QuickJS and its sources join the list of sources.
**[docs/plugins.md](docs/plugins.md)** is the guide to writing one; in
short, a plugin returns an object like this

```js
return {
  name: "My provider",
  discover() {
    return [{
      name: "My source",
      icon: "📻",
      browse(id) {               // list the folder 'id'
        return http.json("https://api.example.com/stations").map(s => ({
          type: "audio", name: s.name, image: s.logo, uri: s.stream,
        }));
      },
    }];
  },
};
```

and may add `search(query, folderId)` to get Search in the options menu,
hand out a long listing a page at a time (`{ entries, next }`),
`resolve(id, entry)` to mint the address (and the request headers and
subtitles that go with it) at the moment playback starts, and `settings`
for choices that appear on the Settings page. The host gives it blocking
`http` (with `http.all()` for requests side by side), a small persistent
`storage`, and a library of what every plugin otherwise writes for itself
(`util`, `xml`).

Plugins of your own go in `plugins` in jtplay's preference directory, as
SDL gives it: in the title's data on a console,
`~/.local/share/jtplay/plugins` on a PC (or `--user-plugins DIR`); the log
names it at start. A file with the name of a bundled plugin replaces it.
They are picked up, and changed ones loaded afresh, whenever the sources
are looked for again (square on Home): no restart, no new package.

`jtplay --plugin FILE [--browse ID | --search QUERY | --resolve ID]
[--next CURSOR]` runs a plugin without the app and prints what it answers,
which is the quick way to write one.

TV4 Play needs a sign-in token: [docs/guide.md](docs/guide.md#tv4-play-signing-in)
says how to get one onto the console.

## Building

Besides SDL2 (and SDL2_image), RmlUi 6.2 or later, libmpv and tinyxml2, the
build needs [QuickJS](https://bellard.org/quickjs/), libcurl and OpenSSL.
On a PC:

    cmake -S . -B build && cmake --build build
    ./build/jtplay -a assets

That also builds `jtplay-plugin`, the plugin runner on its own (see
docs/plugins.md). `-DJTPLAY_WERROR=OFF` builds with a compiler whose
warnings are not the ones the code is kept clean of.

For the PS4, with the ps4-homebrew-dev SDK, the same CMakeLists.txt builds
`build-ps4/IV0000-BREW04327_00-JTPLAY0000000000.pkg`:

    source $PS4_HOMEBREW_SDK/toolchain/orbis.sh
    cmake -S . -B build-ps4 -DCMAKE_TOOLCHAIN_FILE=$PS4_HOMEBREW_SDK/toolchain/orbis.cmake
    cmake --build build-ps4

The package is a non-game application (param.sfo category `gdc`), which a
PS5 treats as a media app: that is what gets the remote's play, pause,
stop, previous / next and seek keys to jtplay there, as a game they stay
with the system. Its cache directory, with the artwork, `settings.ini` and
`library.xml`, is in the title's download data, `/download0` (1 GiB, of
which the artwork may take 512 MiB, so the library always has room),
which SDL2main sets up (it needs the SDL port's
`0006-ps4-SDL2main-the-XDG-base-directories-in-download0` patch; without
it, it is `common_temp`, which all homebrew shares). Versions built as a
game kept it in `/temp0` instead, which a non-game cannot reach: favorites,
resume points and settings start over.

### Icons

The UI's icons come from [Lucide](https://lucide.dev) (ISC licence, see
`fonts/LICENSE-lucide.txt`): `fonts/lucide.ttf` holds just the ones in use,
lowered a little to sit centred on text, and is loaded as a fallback font
so an icon is a character in any string (`src/icons.h`). A plugin names its
icon with an emoji, which is shown as the matching icon where there is one.

## More

- [docs/guide.md](docs/guide.md): how every screen behaves, and every key
- [docs/plugins.md](docs/plugins.md): writing a plugin
