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
- **JavaScript plugins** (see below). Bundled: internet radio from
  radio-browser.info (by popularity, country and genre), podcasts (the
  Apple Podcasts chart; add the shows you follow to Favorites), Sveriges Radio, SVT Play and Pluto TV (free live
  channels, films and series; the catalogue is whatever Pluto serves your
  region).

## Home

The home screen has tabs of places across the top, a banner, and rows
of cards from your library below it: **Continue watching** and
**Favorites**. The tabs are always these five, however many sources
there are:

- **Home**: the shelves hold everything.
- **Local filesystem**: USB drives and internal storage on the console,
  the home folder on a PC, and the entire filesystem.
- **Local network**: UPnP/DLNA media servers found on the network.
- **Plugins**: one source per JavaScript plugin source.
- **Settings**: playback, languages and the screensaver.

Left and right move along the tabs. A group has no banner: right under
the tabs, the rows begin with the group's sources, a grid of tiles, four
to a row and as many rows as it takes, headed by how many there are
(fifty plugins are thirteen rows) and by a search under way or what went
wrong with the last one. A group with no sources says so where the grid
would be. Up and down move through it as through any
rows, keeping the column; select on a tile opens that source. After the
sources come the group's own Continue watching and Favorites rows.
Only the local drives are looked for at start-up: network servers and
plugins are searched for the first time their tab is reached, and square
searches the group again. A card from a group not searched yet searches
it first, then opens.

Down moves from the tabs into the rows, and up from the first row back
to the tabs. On Home, the banner shows the card in focus, large, over its artwork
across the top of the screen -- where it is from, what is left of it, its
description -- and follows the focus along the rows; the focused row
sits right under it. Whatever a card has no data for is left out. Select
on a card plays it (or opens a favorite folder), and select on a source
browses it.

## Browsing

Folders are always listed first, and files are named without their
extension ("Episode 1", not "Episode 1.mkv"). A folder is shown as a **list** (with a
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
around to the first, and the other way round. In a list sorted by name,
left and right jump to the previous / next letter (shown large for a
moment) instead of paging.

The options button opens a side menu: what can be done with the selected
item (play, resume, play from the beginning, add to favorites, mark as
watched), playing the whole folder in order or shuffled, a slideshow of
its pictures, followed by filter (and search, where the source has it), the
view and sort settings, refresh, and Home.

## Filter and search

**Filter**, in the options menu of a folder (or Ctrl+F on a keyboard), narrows
the list on screen to the entries whose name or description hold every word
you type. A bar opens above the list and the list follows the typing; on the
console, focusing the field brings up the system keyboard. Up and down still
move in the list meanwhile. Return keeps the filter and goes back to the
list, Escape drops it (so does back, on an empty field). While a filter is
on, the head of the list says how many of the entries are shown, the options
menu has Clear filter and back clears it before it leaves the folder. A
filter belongs to its folder, survives a refresh and works in every source,
since it only looks at what is listed. Names match in any case for plain
letters; other characters have to match as typed.

**Search**, in the options menu (or Ctrl+S), is for sources that can ask a
backend for content that is not listed, and is only there for those. The
podcast plugin searches all of Apple Podcasts, not just its chart, and SVT
Play searches its whole catalogue (shows, episodes, clips and genres), and
Pluto TV everything its region can watch (channels, films and series). What the
backend finds is a folder of its own that sorts, filters, plays and takes
favorites like any other, and back returns to the folder, with the selection
where it was. Searching from the hits searches again; Refresh asks again.

## Photos

Pictures open full screen. Left / right step through the folder's
pictures; select starts and pauses a slideshow that cross-fades from one
picture to the next while slowly panning and zooming across each (Ken
Burns); up / down show the caption (name, position, date, size); triangle
turns a picture a quarter. The options menu has the seconds per picture,
the transition (pan & zoom, cross-fade, none), shuffle and rotation.

Pictures are shown from screen-sized copies the artwork cache makes in
the background, so a camera original is never uploaded as a 100 MiB
texture, and photos are turned upright from their EXIF orientation.

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

Playback starts full screen. Back leaves full screen without stopping, as
in Kodi: the menus fade in over the video, which plays on behind them as
it is, under a shade that keeps them readable (on Home the video takes
the place of the banner's picture), and can be used as ever while it
plays on (music plays on under the usual menus). It keeps moving through
the folder it was started from, wherever the menus are by then.

What plays is in the top bar, by the clock, with its time; Home's first
row, **Now playing**, holds its card. The way back to full screen is the
way out of it: back, step by step (from a list to Home, from the rows to
the tabs, from the tabs to the video), as on any remote; or selecting the
Now playing card or the item that is playing, triangle (Tab on a
keyboard), or the options menu's **Back to full screen**. Holding back in
full screen, the options menu's **Stop playback**, a remote's stop key or
X on a keyboard stops it.

The bar along the bottom shows title, tracks, elapsed and total time and,
for anything longer than five minutes, the time it will end; its top edge
is the timeline, with the played part in the accent colour,
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

Holding (or repeatedly pressing) left / right seeks faster and faster: 10 s
steps, then 30 s, a minute, two and five. Files with chapters show them
as notches on the timeline and by name in the bar, and L1 / R1 skip by
chapter (going on to the previous / next item past either end).

Over the last 15 seconds of an episode an **Up next** card counts down
to the next item in the folder; select plays it at once, back waves it
away and the player then stops at the end.

The options button opens the playback menu: audio track, subtitles,
chapter, speed (0.5× to 2×), shuffle, repeat (all / one),
night mode (dynamic range compression: dialogue up, explosions down),
picture (fit / fill / stretch), subtitle delay and size, audio delay, a
sleep timer (15 min to 2 h, or the end of the item), favorites, play next
and stop. An item that fails to open while going through a folder is
skipped.

## Settings

Settings, the last of the home tabs, lists the settings right under the
tab: down from the tab into the list, left and right change the one in
focus (select steps it on), and what it does is shown beside the list.
They are grouped as playback, screen, library and diagnostics: whether the
next episode plays by itself,
**Show input** (every key and button as it arrives, in a corner of the
screen and the log, also those nothing reacts to: for finding out what a
TV remote sends; `JTPLAY_INPUT_LOG=1` turns it on from the start),
the preferred audio and subtitle language (a matching track is picked when
a file opens), the screensaver delay, night mode and forgetting Continue
watching (which asks first). These and the playback choices
above (repeat, shuffle, night mode, picture, audio delay, subtitle size)
are kept in `settings.ini` in the cache directory.

After three minutes (see Settings) without input, and no media playing
(video, audio or a slideshow), the screen dims to a slowly drifting clock; the first
key press only wakes it.

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

A source that can ask a backend for content that is not listed adds
`search(query, folderId)` and gets Search in the options menu (there is
nothing to declare: having the function is enough). It is called with what
was typed and the id of the folder on screen, which most searches have no use
for, and returns entries like `browse()` does; an empty array is an answer:
nothing found. Narrowing what is listed is the app's own filter and needs
nothing from the plugin. See `plugins/podcasts.js`.

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
A response over 8 MiB is normally an error; with `maxBytes: n` (at most
8 MiB) the first `n` bytes are returned instead and the response has
`truncated: true`, which suits reading the newest end of a large RSS feed.

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

| Controller            | Key             | Home                         | Browsing                             | Playing (full screen)              | Photos               |
|-----------------------|-----------------|------------------------------|--------------------------------------|------------------------------------|----------------------|
| d-pad up / down       | arrows          | tabs ↔ rows; between rows    | move (wraps around)                  | seek +5 / -5 min                   | caption              |
| d-pad left / right    | arrows          | along the tabs or a row      | list: page or letter; grid: move     | seek (speeds up)                   | previous / next      |
| cross (A)             | Return          | open / play                  | open / play                          | pause; up next: play now           | slideshow on / off   |
| circle (B)            | Backspace       | to the tabs; from them, back to full screen* | back (Home from a source root)       | the menus, playing on; held: stop; up next: dismiss | close |
| options (start)       | F1              | options for the card or tile | options menu                         | playback menu                      | photo menu           |
| square (X)            | Space           | search the group again       | next sort key                        |                                    |                      |
| triangle (Y)          | T, Tab          | back to full screen*         | reverse sort order; back to full screen* | the menus, playing on          | rotate               |
| L1 / R1               | PageUp/PageDown |                              | page up / down                       | previous / next chapter or item    | previous / next      |
| L2 / R2               | Home / End      | first / last                 | first / last                         |                                    | first / last         |
| back (select)         | Escape          | to the tabs; from them, back to full screen* | back                                 | the menus, playing on              | close                |
|                       | X               | stop*                        | stop*                                | stop                               |                      |

\* While something plays behind the menus.

A media remote's (or keyboard's) play/pause, stop and previous/next keys
work on playback wherever it is, and its back key is circle. What a TV
remote connected over HDMI-CEC sends depends on the console; Settings >
Show input shows it.

Held directions repeat. Ctrl+Q quits, Ctrl+F filters the folder on screen and
Ctrl+S searches the source.
The mapping lives in `src/gamepad.cpp`.

## Playback

Playback is libmpv's: mpv opens the URL, decodes and plays the audio, and
draws the video with its OpenGL renderer under the UI. Audio-only items get
the visualizer (`src/visualizer.cpp`) behind the now-playing card. Extra mpv
options can be given for testing or tuning, and mpv's log made more verbose:

    JTPLAY_MPV="hwdec=auto-safe profile=high-quality" JTPLAY_MPV_LOG=v ./jtplay

## Icons

The UI's icons come from [Lucide](https://lucide.dev) (ISC licence, see
`fonts/LICENSE-lucide.txt`): `fonts/lucide.ttf` holds just the ones in use,
lowered a little to sit centred on text, and is loaded as a fallback font
so an icon is a character in any string (`src/icons.h`). A plugin names its
icon with an emoji, which is shown as the matching icon where there is one.

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
