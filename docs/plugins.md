# Writing plugins

A plugin is one JavaScript file that adds a source to the **Plugins** tab:
something to browse, search and play from. It runs in a QuickJS runtime of
its own, with the handful of globals described here and nothing else: no
file system, no modules, no timers.

- [Where plugins live](#where-plugins-live)
- [The shape of a plugin](#the-shape-of-a-plugin)
- [Entries](#entries)
- [Search](#search)
- [Playing: resolve()](#playing-resolve)
- [Settings](#settings)
- [The host library](#the-host-library): `http`, `storage`, `settings`,
  `util`, `xml`, `console`, `jtplay`
- [Rules of the house](#rules-of-the-house): time, memory, errors
- [Trying a plugin without the app](#trying-a-plugin-without-the-app)

## Where plugins live

| | Bundled | Your own |
|---|---|---|
| PS4 / PS5 | `/app0/plugins` (in the package) | `plugins` in the preference directory |
| PC | `<assets>/../plugins`, or `--plugins DIR` | `~/.local/share/jtplay/plugins`, or `--user-plugins DIR` |

The preference directory is the one SDL gives the app (`SDL_GetPrefPath`):
in the title's data on a console, `$XDG_DATA_HOME/jtplay` on Linux. The
log names the plugins directory at start, and it is made if it isn't
there.

Every `*.js` file in either directory is a plugin; a `settings.ini` beside
them gives plugins' settings (see [Settings](#settings)). A file of your own with
the name of a bundled one replaces it, which is how to try a fix to a
bundled plugin on a console without rebuilding the package.

Plugins are loaded when jtplay starts looking for its sources, and looked
at again every time it does so later (square, or Space, on Home): a new file shows
up, a changed one is loaded afresh, a deleted one goes. An unchanged plugin
keeps running, with whatever it has cached. So the loop on a console is:
upload the file over FTP, press square.

A plugin that fails to load says why beside the head of Home's row of
sources, and
does not hold up the others.

## The shape of a plugin

The file's body runs inside a function, so it ends with `return` (not
`export`) and its top-level `const` and `let` stay private to it. It returns
a *provider*:

```js
return {
  name: "My provider",
  discover() {
    return [
      {
        name: "My source",         // the tile's title
        detail: "somewhere",       // its second line (optional)
        icon: "📻",                // an emoji (optional; see below)
        root: "/",                 // id of the root folder (optional, "/")
        browse(id) {               // list the folder 'id'
          return [
            { type: "audio", name: "A stream", uri: "https://example.com/stream" },
          ];
        },
      },
    ];
  },
};
```

`discover()` describes the plugin's sources; most have one. It runs when
sources are looked for, so it should not fetch anything: return the
description and do the work in `browse()`.

A source may also have `imageHeaders`, headers the app sends when it
downloads artwork (an entry's `image`, or the picture of an `image` entry)
from addresses that start with a prefix. It is for an image server that
answers only requests carrying, say, its own site as Referer:

```js
imageHeaders: {
  "https://images.example.com/": { Referer: "https://example.com/" },
},
```

A prefix names one site at the least (`https://host/`). Media that plays
takes its headers from `resolve()` instead (see below).

`browse(id)` lists one folder, starting with the root. In every function of
a source, `this` is the source object, so a plugin can keep per-source
state on it.

The icon is named with an emoji, which is shown as the matching icon of the
UI's own set where there is one: 📺 📻 🎙 🎤 🎧 🎵 🎬 🎞 🎥 📷 🖼 📁 🖥 💾.

Functions may be `async`. There is nothing asynchronous to wait for (`http`
answers before it returns), but code written with `await` works as is.

## Entries

`browse()` and `search()` return an array of entries:

| Field | | |
|---|---|---|
| `name` | required | One line of title. |
| `type` | | `"folder"`, `"audio"`, `"video"` or `"image"`. Anything else lists but does not play. |
| `id` | | What `browse()` (for a folder) or `resolve()` (for an item) is handed back. Defaults to `uri`, then to `name`. Keep it stable: favorites and resume points are saved against it. |
| `uri` | | What plays: `http(s)://` or anything else FFmpeg opens. Leave it out and add `resolve()` when it has to be made at the last moment. |
| `description` | | Anything else worth a line or a paragraph. |
| `image` | | Artwork URL. Fetched, shrunk and cached by the app. |
| `duration` | | Seconds. |
| `size` | | Bytes. |
| `date` | | Anything `new Date(x)` accepts: milliseconds, an ISO 8601 string, a `Date`. |

`duration`, `size` and `date` are what the list sorts by and the info panel
shows. An entry without a `name` is left out of the listing (with a remark
in the log) rather than failing all of it.

An `"image"` entry opens in the photo viewer, and a folder of them is a
slideshow. Items with a duration over twenty minutes, and all videos,
remember where they were stopped.

## Search

A source with a `search(query, folderId)` function gets **Search** in the
options menu; having the function is all it takes. It is called with what
was typed and the id of the folder on screen, and returns entries like
`browse()`; an empty array means nothing was found.

`folderId` is there for searches that have a scope: `plugins/archive.js`
searches the collection it is asked from, `plugins/radio.js` the country or
genre. Most searches ignore it.

Narrowing down what is already listed is the app's own **Filter** and needs
nothing from the plugin.

## Playing: resolve()

Without a `resolve()` function, an item plays from the `uri` it was listed
with. With one, `resolve(id, entry)` is called at the moment playback
starts, which is the place for anything short-lived or costly: a signed
URL, a token, a choice among formats, a lookup that would be wasteful to do
for every row of a listing.

It returns the address, or an object when there is more to say:

```js
resolve(id, entry) {
  const video = http.json("https://api.example.com/videos/" + id);
  return {
    uri: video.hls,
    // Sent with every request for the stream:
    headers: { Referer: "https://example.com/", Authorization: "Bearer " + token() },
    // Subtitle files beside the stream:
    subtitles: video.captions.map(c => ({ uri: c.url, lang: c.language, name: c.label })),
  };
}
```

A subtitle in the language set under Settings > Subtitle language is
switched on by itself; the others are in the playback menu.

`avoidAudio: ["qas"]` names audio languages not to start with, though the
stream marks one of them as its default; another track plays instead,
unless Settings > Audio language asks for this one. (TV4 Play marks a
dubbed track as the default of its foreign titles.)

Throw an `Error` when there is nothing to play; its message is what the
viewer is told (`throw new Error("not available from here")`).

## Settings

A provider can ask the user for a choice or two. They appear on the
Settings page under the plugin's name, and in the options menu of the
plugin's tile and folders (*name* settings), are remembered, and are read
back with `settings.get(key)`:

```js
return {
  name: "My provider",
  settings: [{
    key: "quality",
    label: "Video quality",
    about: "High needs a fast connection.",   // shown beside the list
    options: [
      { value: "hd", label: "High" },
      { value: "sd", label: "Standard" },
    ],
    default: "hd",                            // else the first option
  }],
  discover() { ... },
};
```

An option can also be a plain string, which is then both value and label.
A long list (a country) is fine: select on it opens the list.

A setting can also be a line of text, such as a sign-in token or a server
address:

```js
{ key: "token", type: "text", secret: true, label: "Sign-in token",
  about: "Where to find it, in a sentence or two." }
```

Select types it in (on a console, with the system keyboard); left or right
clears it. A `secret` one shows as dots while typed and only as Set / Not
set afterwards, and the plugin runner prints it as `(set)`.

Typing a long token with a controller is no fun, so any setting can also
be given in a file `settings.ini` in the plugin directory of your own
(reachable over FTP on a console), one per line:

```ini
# <plugin file name>.<key>=<value>
tv4play.token=eyJhbGciOi...
```

It is read with the scripts (when the sources are first looked for, and
at a later one when it has changed), and its values replace what the
settings were. Say so in the setting's `about`. Clearing a setting in the
app lasts until the next start while the file still has it.

Read a setting where it is used rather than once at the top of the file, so
a change takes effect at once. What `util.memo()` has cached is dropped
when any setting of any plugin changes.

## The host library

### http

Requests block and return the response; there are no callbacks.

```js
const res = http.get(url, options);        // options are optional
http.post(url, body, options);
http.request({ method, url, body, ...options });
```

| Option | |
|---|---|
| `headers` | `{ name: value }` |
| `timeout` | Seconds without any data before giving up. Default 30, at most 60. Not a limit on the whole transfer. |
| `redirect` | `false` to not follow redirects. |
| `binary` | `true` for the body as an `ArrayBuffer` instead of a string. |
| `maxBytes` | Read at most this much and hand it over with `truncated: true`, instead of failing on a larger response. |

A response is `{ ok, status, url, headers, body, truncated }`: `url` is the
final one after redirects, `headers` has lower-case names, and `ok` is true
for a 2xx status. It also has `json()` and `xml()`, which parse the body.

An HTTP error status is **not** an exception: check `res.ok`. Transport
failures (no network, DNS, TLS, a server that stops sending) throw an
`Error`. A response over 8 MiB is refused unless `maxBytes` says how much
of it is wanted. Only `http` and `https` URLs are allowed.

For the common case there is

```js
const data = http.json(url, options);
```

which is a GET that returns the parsed JSON, and throws for anything but a
2xx answer (`"HTTP 404 from api.example.com"`).

Several requests that do not depend on each other go out together:

```js
const [a, b, c] = http.all([urlA, urlB, { url: urlC, headers: {...} }]);
```

`http.all()` takes URLs or `request()`-style objects and returns the
responses in the same order, after the slowest one, rather than after the
sum of them all. A request that fails in transport does not throw here: its
place holds `{ ok: false, status: 0, error: "..." }`, so one dead server
does not cost the answers of the others. At most 64 requests a call, and 24
MiB between them.

### storage

A small JSON file per plugin, for what should outlive the session: a device
id, a token, the last thing picked.

```js
storage.set("token", { value: "...", expires: 1767225600000 });
const token = storage.get("token");          // undefined when absent
const count = storage.get("count", 0);       // or a fallback
storage.remove("token");
storage.keys();                              // ["count"]
```

Values are anything JSON can hold, and are copied in and out: changing what
`get()` returned changes nothing until it is `set()`. The whole store is
limited to 1 MiB; it is not a cache for listings (that is `util.memo`).

### settings

`settings.get(key)` is the value of one of the plugin's declared
[settings](#settings), as a string.

### util

| | |
|---|---|
| `util.memo(key, seconds, produce)` | The result of `produce()`, kept for `seconds` and reused for the same `key` meanwhile. Listings are browsed back and forth; this is what makes going back to one instant. `util.forget(key)` drops one, `util.forget()` all. |
| `util.text(markup)` | HTML or feed text as a person would read it: no tags, no CDATA wrapper, entities decoded, whitespace tidied. Takes `null` in its stride. |
| `util.clip(text, n)` | At most `n` characters, cut at a word, with an ellipsis. |
| `util.decodeEntities(s)`, `util.stripTags(s)` | The two halves of `text()`. |
| `util.query(params)` | `{ a: 1, b: ["x", "y"], c: null }` → `"a=1&b=x&b=y"` |
| `util.resolveUrl(base, ref)` | A relative URL made absolute. |
| `util.duration(s)` | `"1:02:03"`, `"62:03"`, `"3723"` or `"PT1H2M3S"` → seconds; 0 when it is none of them. |
| `util.uuid()` | A random (version 4) UUID. |
| `util.base64Encode(text)`, `util.base64Decode(b64)` | Text as UTF-8 in base64, and back. Decoding takes the URL-safe alphabet too (a JWT's payload). |

`atob()` and `btoa()` are there as in a browser, for binary strings.

### xml

```js
const feed = xml.parse(text);          // or: http.get(url).xml()
const channel = xml.child(feed, "channel");
for (const item of xml.children(channel, "item")) {
  xml.text(item, "title");                         // "" when there is none
  xml.attr(xml.child(item, "enclosure"), "url");
  xml.text(item, "itunes:duration");               // prefixes are part of the name
}
xml.find(feed, "item");                // every descendant of that name
```

An element is a plain object, `{ name, attrs, children, text }`, where
`text` is its own text without its children's. The accessors take a missing
node without complaint, so a chain of them needs no checks in between.

Parsing is strict: a document cut short (the first megabytes of a long
feed, fetched with `maxBytes`) is an error, not half a tree.
`plugins/podcasts.js` reads such feeds with regular expressions for that
reason.

### console

`console.log()`, `info()`, `debug()`, `warn()` and `error()` write to the
app's log (standard error on a PC), tagged with the plugin's name. Objects are printed as JSON.

### jtplay

`jtplay.api` is the version of this interface (2), `jtplay.version` the
app's, `jtplay.platform` `"ps4"` or `"linux"`. Version 1 had `http.get`,
`post` and `request`, `console`, and the four functions; a plugin that
wants to run there too can check before using the rest:

```js
const country = typeof settings === "object" ? settings.get("country") : "us";
```

## Rules of the house

**Time.** A call may *run* for ten seconds before it is interrupted; time
spent waiting in `http` does not count. A request gives up after thirty
seconds without data.

**Giving up.** When the viewer backs out of a folder that is still loading,
or picks something else, the call under way is abandoned: the request in
flight is dropped and the script is interrupted, within about a second. It
cannot be caught, and needs no handling; just do not assume a function that
was started runs to its end.

**Memory.** A runtime has 32 MiB. Response bodies are held outside it until
they are handed over, which is why they have limits of their own.

**Errors.** Throw an `Error` with a message for the viewer; a folder that
fails shows it, with the offer to try again. The script location goes to
the log. An error of another kind (`TypeError`, ...) is shown with its
name, as the bug it is.

**Threads.** One call at a time per plugin; the host sees to that.

## Trying a plugin without the app

`jtplay --plugin FILE` runs a plugin headless and prints what it answers as
JSON (the build also makes `jtplay-plugin`, the same without the window's
libraries):

```sh
jtplay --plugin plugins/radio.js                       # sources and settings
jtplay --plugin plugins/radio.js --browse              # the root folder
jtplay --plugin plugins/radio.js --browse country:SE
jtplay --plugin plugins/radio.js --search "jazz" --folder country:SE
jtplay --plugin plugins/radio.js --resolve <station uuid>
jtplay --plugin plugins/somafm.js --set quality=low --resolve dronezone
```

`console.log()` and errors go to standard error, and the exit status says
whether it worked. `--data DIR` gives `storage` a place to keep its file
between runs; without it nothing is kept.
