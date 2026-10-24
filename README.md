# JTPlay

An audio/video player for jailbroken PS4s and PS5s where the majority of the code
was produced by claude.ai

It is built for the couch: a 10-foot UI driven entirely by a remote or game
controller, browsing local drives, UPnP/DLNA media servers and JavaScript
plugins, in the spirit of Kodi.

## Sources

- **Local storage**: on the console, every connected USB drive, extended
  storage and `/data` are listed as their own sources; on a PC, the home
  folder. The whole filesystem is always available as well.
- **UPnP/DLNA servers** on the local network (MiniDLNA, Jellyfin, Plex, ...).
- **JavaScript plugins** (see below).

## Home

The home screen is a rail of places down the left (Home, every source,
Rescan) and, on the right, shelves of cards from your library:
**Continue watching** and **Favorites**. On Home the shelves hold
everything; on a source, only what came from that source, under a header
for it. Right moves from the rail onto the shelves, select on a card plays
it (or opens a favorite folder), and select on a source browses it. The
focused card's artwork fills the background.

## Browsing

Folders are always listed first. A folder is shown as a **list** (with a
thumbnail per row and an info panel with artwork, type, size, duration,
date and resume point) or as a **grid** of artwork tiles with the focused
item's details along the bottom; switch with View in the options menu.
Artwork is loaded in the background, newest request first, and shrunk to
thumbnails so even a folder of camera photos stays light; the selected
item's artwork is the backdrop.

The list can be sorted by name (natural order, so "Episode 2" comes before
"Episode 10"), date or size, ascending or descending, or kept in the order
the source returned it ("Default", which is track order on most DLNA
servers). The right-hand column of the list shows whatever it is sorted by.
View and sort are remembered between sessions.

Moving past the last row of a list (or the last row of the grid) wraps
around to the first, and the other way round.

The options button opens a side menu: what can be done with the selected
item (play, resume, play from the beginning, add to favorites, mark as
watched) followed by the view and sort settings, refresh, and Home.

## Favorites and Continue watching

Any folder or item, from any source, can be added to **Favorites**. A
favorite folder is a shortcut: it browses (and plays) through the source it
came from, so a DLNA folder works as long as that server is on the network.

Videos (and audio longer than 20 minutes) remember where they were stopped.
Playing one again asks whether to resume, unfinished items show a progress
bar on their art and gather under **Continue watching**, and anything played
past 92% is marked watched with a tick. A video without artwork of its own
gets a snapshot of the frame it was stopped on, so its card shows where you
left off. Moving to the next item in a folder resumes it without asking.
All of this is stored in `library.xml` in the cache directory.

## Playing

The bar along the bottom shows title, tracks, elapsed and total time; its
top edge is the timeline, with the played part in the accent colour,
what a network stream has buffered ahead in a lighter shade, and a knob at
the playhead. Seeking shows the jump ("+30 s") in a bubble on the knob, a
paused video shows a large pause badge, and streams that cannot seek are
marked LIVE and have no timeline.

Audio-only playback shows a now-playing card (cover art, title, track,
progress, what plays next) in frosted glass over a visualizer drawn with
OpenGL: an aurora night sky, the spectrum as a mirrored mountain range on
the horizon (bass at the edges, treble in the middle) with its reflection,
and a perspective grid floor that rolls faster with louder music and
flashes on beats. mpv has no PCM tap, so the player has it render a small
`showfreqs` spectrum into an offscreen framebuffer and reads the bars back;
mpv keeps those frames in step with what is audible.

The options button opens the playback menu: audio track, subtitles, video
track, speed (0.5× to 2×), favorites, play next and stop. An item that
fails to open while going through a folder is skipped.

After three minutes without input (and no video running) the screen dims to
a slowly drifting clock; the first key press only wakes it.

## JavaScript plugins

Besides UPnP/DLNA servers and the local filesystem, media sources can be
added with JavaScript. Every `*.js` file in the plugin directory
(`--plugins DIR`, default `<assets>/../plugins`) is evaluated by QuickJS and
becomes one provider; the script returns an object implementing the same
interface as `browse::Provider` and `browse::Source`:

```js
return {
  name: "My provider",
  discover() {
    return [
      {
        name: "My source",         // row title
        detail: "somewhere",       // row subtitle (optional)
        icon: "📻",                // emoji glyph (optional)
        root: "/",                 // root folder id (optional)
        browse(id) {               // list the children of 'id'
          return [
            {
              id: "bbc1",                        // optional; defaults to uri
              type: "audio",                     // folder|audio|video|image
              name: "A stream",                  // required
              description: "Some station",       // optional
              image: "https://example.com/a.jpg",// optional
              uri: "https://example.com/stream",
            },
          ];
        },
      },
    ];
  },
};
```

The script body runs inside a function (like a CommonJS module), so it ends
with `return` rather than an `export`, and top-level `const`/`let` stay
private to the plugin. In `browse()`, `this` is the source object, so a
plugin can keep per-source state on it.

Each script gets its own QuickJS runtime with the standard objects (`JSON`,
`Math`, `Date`, `RegExp`, ...), `console.log()`/`warn()`/`error()` writing to
the RmlUi log, and the `http` object below. There is no event loop:
`discover()` and `browse()` must return their result synchronously (an
`async` function returns a promise and is reported as an error), and a call
that spends more than 10 seconds *executing* is interrupted.

An entry has the fields `id`, `type`, `name`, `description`, `image` and
`uri`, plus optional `size` (bytes), `date` (anything `new Date(x)` accepts:
milliseconds since the epoch, an ISO 8601 string or a `Date`) and `duration`
(seconds), which are used for sorting and shown in the info panel (see
`browse::Entry` in `src/browse/source.h`). Only `name` is required; a `folder` entry is browsed by handing its `id` back to
`browse()`, anything else is opened by its `uri`. A source that serves signed
or otherwise short-lived URIs can leave `uri` out and add a
`resolve(id, entry)` function returning a fresh one (as a string, or as an
entry object with a `uri`) at the moment playback starts.

See `plugins/sr.js` (Sveriges Radio live channels) for a working example.

### Fetching over the network

Plugins talk to the network through `http`, backed by libcurl, so https,
redirects and compressed responses work. The calls block (plugins already
run on the browsing worker thread) and return the response directly:

```js
const res = http.get("https://api.example.com/things", {
  headers: { Accept: "application/json" },
  timeout: 20,        // seconds without data; default 30, capped at 60
});
if (!res.ok) throw new Error("HTTP " + res.status);
const things = JSON.parse(res.body);
```

```js
http.post(url, body, options);   // options as above
http.request({ method, url, headers, body, timeout, redirect, binary });
```

A response is `{ ok, status, url, headers, body }`, where `url` is the final
URL after redirects and `headers` keys are lower-cased. Following redirects
can be turned off with `redirect: false`, and `binary: true` returns the body
as an `ArrayBuffer` instead of a string (a string body is decoded as UTF-8).

An HTTP error status is *not* an exception: check `res.ok`. Transport
failures (DNS, TLS, a server that stops sending) throw an `Error`, so wrap calls in `try`/`catch`
if a source should survive an unreachable server. Only `http` and `https`
URLs are permitted, and a response over 8 MiB is refused. Time spent waiting
on a request does not count towards the 10-second watchdog.

If the build's libcurl has no default CA bundle, point it at one with the
`SSL_CERT_FILE` (or `CURL_CA_BUNDLE`) environment variable, otherwise https
requests fail with a certificate error.

## Controls

Keyboard, game controller (any pad SDL knows, and the DualShock 4 on a PS4)
and media remote do the same things. The UI shows no button hints; the
options button is the way into everything that is not on the d-pad.

| Controller            | Key             | Home                         | Browsing                          | Playing                |
|-----------------------|-----------------|------------------------------|-----------------------------------|------------------------|
| d-pad up / down       | arrows          | rail / shelf (rail wraps)    | move (wraps around)               | seek +5 / -5 min       |
| d-pad left / right    | arrows          | rail ↔ cards                 | list: page; grid: previous / next | seek -10 / +10 s       |
| cross (A)             | Return          | open / play                  | open / play                       | pause                  |
| circle (B)            | Backspace       | back to the rail             | back (Home from a source root)    | stop                   |
| options (start)       | F1              | options for the card         | options menu                      | playback menu          |
| square (X)            | Space           | rescan                       | next sort key                     | next video track       |
| triangle (Y)          | T               |                              | reverse sort order                | next audio track       |
| L1 / R1               | PageUp/PageDown |                              | page up / down                    | previous / next item   |
| L2 / R2               | Home / End      | first / last                 | first / last                      |                        |
| back (select)         | Escape          | back to the rail             | back                              | stop                   |

Held directions repeat. Ctrl+Q quits. The mapping lives in `src/gamepad.cpp`.

## Playback

Playback is libmpv's: mpv opens the URL, decodes and plays the audio, and
draws the video with its OpenGL renderer under the UI. Audio-only items get
the visualizer (`src/visualizer.cpp`) behind the now-playing card. Extra mpv
options can be given for testing or tuning, and mpv's log made more verbose:

    JTPLAY_MPV="hwdec=auto-safe profile=high-quality" JTPLAY_MPV_LOG=v ./jtplay

## Building

Besides SDL2, RmlUi, libmpv and tinyxml2, the build needs
[QuickJS](https://bellard.org/quickjs/) and libcurl. On a PC:

    cmake -S . -B build && cmake --build build
    ./build/jtplay -a assets

For the PS4, with the ps4-homebrew-dev SDK, the same CMakeLists.txt builds
`build-ps4/IV0000-BREW04327_00-JTPLAY0000000000.pkg`:

    source $PS4_HOMEBREW_SDK/toolchain/orbis.sh
    cmake -S . -B build-ps4 -DCMAKE_TOOLCHAIN_FILE=$PS4_HOMEBREW_SDK/toolchain/orbis.cmake
    cmake --build build-ps4
