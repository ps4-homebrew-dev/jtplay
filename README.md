# JTPlay

An audio/video player for jailbroken PS4s and PS5s where the majority of the code
was produced by claude.ai

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

An entry has six fields and no more: `id`, `type`, `name`, `description`,
`image` and `uri` (see `browse::Entry` in `src/browse/source.h`). Only `name`
is required; a `folder` entry is browsed by handing its `id` back to
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
  timeout: 20,        // seconds; default 15, capped at 60
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
failures (DNS, TLS, timeout) throw an `Error`, so wrap calls in `try`/`catch`
if a source should survive an unreachable server. Only `http` and `https`
URLs are permitted, and a response over 8 MiB is refused. Time spent waiting
on a request does not count towards the 10-second watchdog.

If the build's libcurl has no default CA bundle, point it at one with the
`SSL_CERT_FILE` (or `CURL_CA_BUNDLE`) environment variable, otherwise https
requests fail with a certificate error.

## Controls

Keyboard and game controller (any pad SDL knows, and the DualShock 4 on a
PS4) do the same things:

| Controller            | Key             | Browsing                 | Playing                    |
|-----------------------|-----------------|--------------------------|----------------------------|
| d-pad / left stick    | arrows          | move (held: repeats)     | seek ±10 s / ±5 min        |
| cross (A)             | Return          | open / play              | pause                      |
| circle (B)            | Backspace       | back                     | stop                       |
| square (X)            | Space           | rescan (sources)         | next video track           |
| triangle (Y)          | T               |                          | next audio track           |
| L1 / R1               | PageUp/PageDown | page up / down           | previous / next item       |
| L2 / R2               | Home / End      | first / last row         |                            |
| options (start)       | F1              |                          | info bar                   |
| back (select)         | Escape          | back                     | stop                       |

Ctrl+Q quits. The mapping lives in `src/gamepad.cpp`.

## Playback

Playback is libmpv's: mpv opens the URL, decodes and plays the audio, and
draws the video with its OpenGL renderer under the UI. Audio-only items get
a spectrum (an ffmpeg `showcqt` graph) behind the now-playing card. Extra mpv
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
