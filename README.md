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
- **JavaScript plugins** (see below). Bundled:
  - *Internet Radio*: the stations of radio-browser.info by popularity,
    country and genre, with search by name.
  - *SomaFM*: listener-supported, commercial-free channels.
  - *Podcasts*: the Apple Podcasts top 50, of your country (a setting), of
    every country it is in and of every genre; search finds any show. Add
    the ones you follow to Favorites.
  - *Internet Archive*: public domain feature films, cartoons and classic
    TV, live concerts, old time radio and LibriVox audiobooks.
  - *media.ccc.de*: the talks of the Chaos Communication Congress and a few
    hundred other conferences, with their subtitles.
  - *Art gallery*: the Art Institute of Chicago's open collection, room by
    room; start the slideshow and the TV is a picture frame.
  - *TV4 Play*: TV4, Sjuan, TV12: series, films, news, clips. Browsing
    needs no account; playing needs a sign-in token (see below). What TV4
    protects with DRM, most films and live sport among it, cannot be
    played.
  - *Riksdagen*: the Swedish parliament's web TV: debates, interpellations,
    hearings, by committee, with search; as video or sound only.
  - *Sveriges Radio*, *SVT Play* and *Pluto TV* (free live channels, films
    and series; the catalogue is whatever Pluto serves your region).

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
The local drives and the plugins are looked for at start-up; network
servers are searched for the first time their tab is reached, and square
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

A folder on a slow server says so where the eyes are: the focused row
pulses while what it opens is on its way, and back gives up on it and
stays put. A folder that cannot be listed says why in place of the list;
select tries again. The item that is playing is marked in the list.

The options button opens a side menu, in groups under headings that come
in the same order in every menu: first what can be done with the selected
item (play, resume, play from the beginning, add to favorites, mark as
watched); then **Folder** (play it all in order or shuffled, a slideshow
of its pictures, refresh), **Find** (search, where the source has it, and
filter), **View** (list or grid, sort key and order), **Go to** (the top of
the list, Home), **Plugin** (the settings of the plugin the folder is
from, see Settings) and, while something plays behind the menus, **Now
playing** (back to full screen, stop).

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
podcast plugin searches all of Apple Podcasts in the country you are in,
not just its chart, and SVT
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
keyboard), or the options menu's **Back to full screen**. The stop button
of the controls, holding back in full screen, the options menu's **Stop
playback**, a remote's stop key or X on a keyboard stops it.

The bar along the bottom has the title and, on the right, elapsed and total
time; under them one line across the bar (chapter, what is up next,
description); then the controls, and on the right the tracks being played.
Its top edge
is the timeline, with the played part in the accent colour,
what a network stream has buffered ahead in a lighter shade, and a knob at
the playhead. Seeking shows the jump ("+30 s") in a bubble on the knob, a
paused video shows a large pause badge, and streams that cannot seek are
marked LIVE and have no timeline.

Audio-only playback shows a now-playing card (cover art, title, the song a
radio station says is on or a file's own artist and title, track,
progress, what plays next) in frosted glass over a visualizer drawn with
OpenGL: an aurora night sky, the spectrum as a mirrored mountain range on
the horizon (bass at the edges, treble in the middle) with its reflection,
and a perspective grid floor that rolls faster with louder music and
flashes on beats. mpv has no PCM tap, so the player has it render a small
`showfreqs` spectrum into an offscreen framebuffer and reads the bars back;
mpv keeps those frames in step with what is audible.

The last row of the bar (and of the now-playing card) is the **controls**,
a button each: previous item, rewind, play / pause, fast forward, next item
and stop; audio track and subtitles, each where there is a choice, picked
from a list; shuffle and repeat (all / one). Whenever they are on screen
the focus is on them, on play / pause to begin with: left and right go
along them, select presses the one in focus, and what that one is (the
name of the previous or next item, what a track is set to) stands beside
the row. Rewind and fast forward speed up when pressed again and again.

The video bar comes up by itself when an item starts and when a pause
ends, and goes again after five seconds unless a key is pressed meanwhile.
Brought up by hand (up, down or options) it stays until back or options
sends it away. While it is away, left and right seek, with just the
timeline up for as long as that goes on, and select pauses. Artwork, where
an item has any, is at the left of the bar, as tall as its rows.

Holding (or repeatedly pressing) left / right seeks faster and faster: 10 s
steps, then 30 s, a minute, two and five. Files with chapters show them
as notches on the timeline and by name in the bar, and L1 / R1 skip by
chapter (going on to the previous / next item past either end).

Over the last 15 seconds of an episode an **Up next** card counts down
to the next item in the folder; select plays it at once, back waves it
away and the player then stops at the end.

An item that fails to open while going through a folder is skipped.

## Settings

Settings, the last of the home tabs, lists the settings right under the
tab: down from the tab into the list, left and right change the one in
focus (select steps it on), and what it does is shown beside the list.
Select on a setting with many choices (a language, a plugin's country)
lists them all. They are grouped as playback, screen, library, a section
for each plugin that has settings of its own, and diagnostics. A plugin's
settings are also in the options menu of anything that is the plugin's (its
tile, its folders, a favorite from it), as **Plugin** > *name* **settings**:
a menu of their own, and a folder on screen is listed again when they have
changed. The others: whether the
next episode plays by itself,
**Show input** (every key and button as it arrives, in a corner of the
screen and the log, also those nothing reacts to: for finding out what a
TV remote sends; `JTPLAY_INPUT_LOG=1` turns it on from the start),
the preferred audio and subtitle language (a matching track is picked when
a file opens), the screensaver delay, night mode (dynamic range
compression: dialogue up, explosions down) and forgetting Continue
watching (which asks first). These and repeat and shuffle are kept in
`settings.ini` in the cache directory; so are picture scaling (`fit`),
audio delay (`audio_delay`) and subtitle size (`sub_scale`), which have
no menu and are set there.

After three minutes (see Settings) without input, and no media playing
(video, audio or a slideshow), the screen dims to a slowly drifting clock; the first
key press only wakes it.

## JavaScript plugins

Besides UPnP/DLNA servers and the local filesystem, media sources can be
added with JavaScript: every `*.js` file in the plugin directories is run by
QuickJS and becomes a source on the Plugins tab. **[docs/plugins.md](docs/plugins.md)**
is the guide to writing one; in short, a plugin returns an object like this

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
`resolve(id, entry)` to mint the address (and the request headers and
subtitles that go with it) at the moment playback starts, and `settings`
for choices that appear on the Settings page. The host gives it blocking
`http` (with `http.all()` for requests side by side), a small persistent
`storage`, and a library of what every plugin otherwise writes for itself
(`util`, `xml`).

Plugins of your own go in `plugins` in jtplay's preference directory, as
SDL gives it: in the title's data on a console, `~/.local/share/jtplay/plugins`
on a PC (or `--user-plugins DIR`); the log names it at start. A file
with the name of a bundled plugin replaces it. They are picked up, and
changed ones loaded afresh, whenever the Plugins tab is searched (square):
no restart, no new package.

`jtplay --plugin FILE [--browse ID | --search QUERY | --resolve ID]` runs a
plugin without the app and prints what it answers, which is the quick way
to write one.

A plugin runs in a runtime of its own, 32 MiB at most, and a call that
spends more than ten seconds *executing* (waiting on the network does not
count) is interrupted. Backing out of a folder that is still loading drops
the request it was waiting for.

### TV4 Play: signing in

TV4 Play plays only for a signed-in account, even what is free (a free
account will do). There is no sign-in from a TV yet, so the plugin uses the
token the website keeps, as svtplay-dl does:

1. Sign in at tv4play.se in a computer's browser.
2. Open the developer tools (F12), then Application (Storage in Firefox),
   Cookies, `https://www.tv4play.se`, and copy the value of
   `tv4-refresh-token`: a long text starting with `ey`.
3. On a PC, paste it under Settings, TV4 Play, Sign-in token. On a console,
   put this line in a file `settings.ini` in the folder of your own
   plugins (see Plugins; the log names it at start) over FTP, then search
   the Plugins tab (square):

       tv4play.token=ey...

The token lasts for months; when TV4 stops accepting it, the plugin says so,
and a new one goes in the same way. Left or right on the setting signs out
(and removing the line from the file, for good).

If the build's libcurl has no default CA bundle, point it at one with the
`SSL_CERT_FILE` (or `CURL_CA_BUNDLE`) environment variable, otherwise https
requests fail with a certificate error.

## Controls

Keyboard, game controller (any pad SDL knows, and the DualShock 4 on a PS4)
and media remote do the same things. The UI shows no button hints; the
options button is the way into everything that is not on the d-pad.

| Controller            | Key             | Home                         | Browsing                             | Playing (full screen)              | Photos               |
|-----------------------|-----------------|------------------------------|--------------------------------------|------------------------------------|----------------------|
| d-pad up / down       | arrows          | tabs ↔ rows; between rows    | move (wraps around)                  | the info bar, to stay | caption |
| d-pad left / right    | arrows          | along the tabs or a row      | list: page or letter; grid: move     | along the controls; bar away: seek (speeds up) | previous / next |
| cross (A)             | Return          | open / play                  | open / play                          | press the button in focus; bar away: pause; up next: play now | slideshow on / off |
| circle (B)            | Backspace       | to the tabs; from them, back to full screen* | back (Home from a source root)       | info bar away; without it: the menus, playing on; held: stop; up next: dismiss | close |
| options (start)       | Menu, F10       | options for the card or tile | options menu                         | info bar, to stay / away | photo menu           |
| square (X)            | Space           | search the group again       | next sort key                        |                                    |                      |
| triangle (Y)          | T, Tab          | back to full screen*         | reverse sort order; back to full screen* | the menus, playing on          | rotate               |
| L1 / R1               | PageUp/PageDown |                              | page up / down                       | previous / next chapter or item    | previous / next      |
| L2 / R2               | Home / End      | first / last                 | first / last                         |                                    | first / last         |
| back (select)         | Escape          | to the tabs; from them, back to full screen* | back                                 | the menus, playing on              | close                |
|                       | X               | stop*                        | stop*                                | stop                               |                      |

\* While something plays behind the menus.

A media remote's (or keyboard's) play/pause, pause, stop, previous/next,
rewind / fast forward (as left / right in full screen, 10 s steps behind the
menus), audio (the next audio track) and subtitle (the next subtitle track,
F1 on a keyboard) keys work on playback wherever it is; its display key
shows or hides the bar in full screen, and its back key is circle. Its
options (or menu) and disc menu keys are options, as are a keyboard's menu
key and F10; and since not every remote has one, nor every TV passes it
on, holding OK (Return) for a moment is options too. What a TV remote connected over HDMI-CEC sends depends on
the console and the TV; Settings > Show input shows it.

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

Besides SDL2 (and SDL2_image), RmlUi 6.2 or later, libmpv and tinyxml2, the
build needs [QuickJS](https://bellard.org/quickjs/), libcurl and OpenSSL.
On a PC:

    cmake -S . -B build && cmake --build build
    ./build/jtplay -a assets

That also builds `jtplay-plugin`, the plugin runner on its own (see
docs/plugins.md). `-DJTPLAY_WERROR=OFF` builds with a
compiler whose warnings are not the ones the code is kept clean of.

For the PS4, with the ps4-homebrew-dev SDK, the same CMakeLists.txt builds
`build-ps4/IV0000-BREW04327_00-JTPLAY0000000000.pkg`:

    source $PS4_HOMEBREW_SDK/toolchain/orbis.sh
    cmake -S . -B build-ps4 -DCMAKE_TOOLCHAIN_FILE=$PS4_HOMEBREW_SDK/toolchain/orbis.cmake
    cmake --build build-ps4

The package is a non-game application (param.sfo category `gdc`), which a
PS5 treats as a media app: that is what gets the remote's play, pause,
stop, previous / next and seek keys to jtplay there, as a game they stay
with the system. Its cache directory, with the artwork, `settings.ini` and
`library.xml`, is in the title's download data, `/download0` (512 MiB),
which SDL2main sets up (it needs the SDL port's
`0006-ps4-SDL2main-the-XDG-base-directories-in-download0` patch; without
it, it is `common_temp`, which all homebrew shares). Versions built as a
game kept it in `/temp0` instead, which a non-game cannot reach: favorites,
resume points and settings start over.
