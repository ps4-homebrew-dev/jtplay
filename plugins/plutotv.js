// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pluto TV: free, ad-supported live channels and on-demand films and series.
//
// Pluto is anonymous. boot.pluto.tv trades a description of the device for a
// short-lived session token (a JWT), and that token is the only credential
// there is. It also decides the region: Pluto works out where the caller is
// from the address the boot request arrives on and writes the answer into the
// token, so the catalogue is whatever the console's own connection is
// entitled to see.
//
// Live channels and their guide come from api.pluto.tv/v2/channels in one
// response. On demand comes from /v3/vod/categories, which carries its items
// inline, so every genre and collection below is served from a single request.
//
// Playback is a URL built from the token rather than a request of its own:
// the stitcher takes a channel or episode id and answers with an HLS manifest
// with the advertising already spliced in. drmCapabilities is deliberately not
// sent in the boot request, so Pluto serves the unencrypted rendition and
// there is no licence exchange anywhere in this file.
//
// The request parameters are the ones the web player sends; see
// https://github.com/ps5-payload-dev/plutotv for the browser version this was
// written from.

const BOOT_API = "https://boot.pluto.tv/v4/start";
const API = "https://api.pluto.tv";
const CHANNELS_API = "https://service-channels.clusters.pluto.tv";
const IMAGE_HOST = "https://images.pluto.tv";

// Where the manifests come from when the boot response names no stitcher.
const STITCHER_FALLBACK =
      "https://cfd-v4-service-channel-stitcher-use1-1.prd.pluto.tv";

// What this client claims to be. Pluto reads these to decide which catalogue
// to serve; the web values are the ones that produce an ordinary HLS stream.
const DEVICE = {
    appName: "web",
    appVersion: "9.1.0",
    deviceType: "web",
    deviceModel: "web",
    deviceMake: "chrome",
    deviceVersion: "122.0.0",
    clientModelNumber: "1.0.0"
};

// How far ahead of now to ask for programme data: "now, and what follows".
const EPG_WINDOW_MIN = 120;

// The guide goes stale faster than the catalogue does.
const CACHE_TTL_MS = 10 * 60 * 1000;
const CHANNEL_TTL_MS = 5 * 60 * 1000;

// Items inside a category are capped by the API however they are asked for;
// totalItemsCount is how a category says it held more.
const VOD_ITEM_CAP = 100;

const FILM_TYPES = {movie: true, film: true, feature: true};

const cache = {};

function cached(key, ttl, produce) {
    const hit = cache[key];
    const now = Date.now();
    if (hit && now - hit.time < ttl) {
        return hit.value;
    }
    const value = produce();
    cache[key] = {time: now, value: value};
    return value;
}

function text(s) {
    return String(s == null ? "" : s).trim();
}

function get(obj, path) {
    let cur = obj;
    const parts = path.split(".");
    for (let i = 0; i < parts.length; i++) {
        if (cur == null) {
            return null;
        }
        cur = cur[parts[i]];
    }
    return cur == null ? null : cur;
}

function query(params) {
    return Object.keys(params).filter((k) => {
        return params[k] !== null && params[k] !== undefined;
    }).map((k) => {
        return encodeURIComponent(k) + "=" + encodeURIComponent(params[k]);
    }).join("&");
}

function uuid() {
    let s = "";
    for (let i = 0; i < 36; i++) {
        if (i === 8 || i === 13 || i === 18 || i === 23) {
            s += "-";
        } else if (i === 14) {
            s += "4";
        } else {
            const r = Math.floor(Math.random() * 16);
            s += (i === 19 ? (r & 3) | 8 : r).toString(16);
        }
    }
    return s;
}

// QuickJS has no atob().
const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

function base64Decode(s) {
    let bits = 0;
    let nbits = 0;
    let out = "";
    for (let i = 0; i < s.length; i++) {
        const v = B64.indexOf(s.charAt(i));
        if (v < 0) {
            continue;
        }
        bits = (bits << 6) | v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out += String.fromCharCode((bits >> nbits) & 255);
            bits &= (1 << nbits) - 1;
        }
    }
    // The payload is UTF-8; city names are not always ASCII.
    try {
        return decodeURIComponent(out.replace(/[^\x00-\x7f]/g, (c) => {
            return "%" + ("0" + c.charCodeAt(0).toString(16)).slice(-2);
        }));
    } catch (e) {
        return out;
    }
}

function jwtPayload(token) {
    try {
        const part = token.split(".")[1].replace(/-/g, "+").replace(/_/g, "/");
        return JSON.parse(base64Decode(part));
    } catch (e) {
        return null;
    }
}

/* ---------------------------------------------------------------- session */

// One device id and one session id for as long as the plugin lives. There is
// no account behind either; they only tie a run of requests together.
const deviceId = uuid();
const sessionId = uuid();

let session = null;

function bootParams() {
    return Object.assign({}, DEVICE, {
        deviceId: deviceId,
        clientID: deviceId,
        sid: sessionId,
        serverSideAds: "false",
        blockingMode: ""
    });
}

function boot() {
    const res = http.get(BOOT_API + "?" + query(bootParams()), {
        headers: {Accept: "application/json"},
        timeout: 20
    });
    if (!res.ok) {
        throw new Error("Pluto session request: HTTP " + res.status);
    }

    const json = JSON.parse(res.body);
    const token = text(json.sessionToken);
    if (!token) {
        throw new Error("no session token from Pluto");
    }
    const claims = jwtPayload(token) || {};

    // The stitcher wants the device description the boot request carried, and
    // the response usually hands back a prepared query string saying exactly
    // that. Where it does not, what was sent is what to send again.
    let params = text(json.stitcherParams);
    if (params.charAt(0) === "?") {
        params = params.substring(1);
    }
    if (!params) {
        params = query(bootParams());
    }

    session = {
        token: token,
        params: params,
        stitcher: text(get(json, "servers.stitcher")) || STITCHER_FALLBACK,
        region: text(claims.activeRegion) || text(claims.country),
        language: text(claims.preferredLanguage),
        expires: claims.exp ? claims.exp * 1000 : Date.now() + 3600 * 1000
    };
    console.log("pluto session: region " + (session.region || "?") +
                ", stitcher " + session.stitcher);
    return session;
}

// The live session, minting one when what is held has (nearly) expired.
function authorize() {
    if (!session || Date.now() > session.expires - 60000) {
        boot();
    }
    return session;
}

// A GET against one of the JSON services, with the session token attached.
function api(base, path, params, retried) {
    const s = authorize();
    let url = base + path;
    const q = query(params || {});
    if (q) {
        url += (url.indexOf("?") < 0 ? "?" : "&") + q;
    }

    const res = http.get(url, {
        headers: {Accept: "application/json", Authorization: "Bearer " + s.token},
        timeout: 30
    });
    if (res.status === 401 || res.status === 403) {
        // The token went stale early, or the region moved under us.
        session = null;
        if (!retried) {
            return api(base, path, params, true);
        }
        throw new Error("Pluto rejected the session (HTTP " + res.status + ")");
    }
    if (!res.ok) {
        throw new Error("Pluto: HTTP " + res.status + " for " + path);
    }
    return JSON.parse(res.body);
}

/* ----------------------------------------------------------------- images */

function absUrl(u) {
    u = text(u);
    if (!u) {
        return "";
    }
    if (u.indexOf("//") === 0) {
        return "https:" + u;
    }
    if (u.charAt(0) === "/") {
        return IMAGE_HOST + u;
    }
    return u;
}

// Cards are 16:9, so look for that first. Pluto describes its artwork three
// different ways depending on which service answered (a list of covers with
// aspect ratios, a set of named fields, a list of typed images), so try each
// shape rather than guessing where an object came from.
function pickImage(obj) {
    if (!obj) {
        return "";
    }

    const covers = obj.covers;
    if (covers && covers.length) {
        for (let i = 0; i < covers.length; i++) {
            if (text(covers[i].aspectRatio) === "16:9") {
                return absUrl(covers[i].url || covers[i].path);
            }
        }
    }

    const named = ["poster16_9.path", "featuredImage.path", "tile.path",
                   "thumbnail.path", "poster.path", "logo.path",
                   "colorLogoPNG.path"];
    for (let i = 0; i < named.length; i++) {
        const hit = get(obj, named[i]);
        if (hit) {
            return absUrl(hit);
        }
    }

    const images = obj.images;
    if (images && images.length) {
        const want = ["featuredImage", "tile", "thumbnail", "colorLogoPNG",
                      "solidLogoPNG", "logo"];
        for (let j = 0; j < want.length; j++) {
            for (let i = 0; i < images.length; i++) {
                if (text(images[i].type) === want[j]) {
                    return absUrl(images[i].url || images[i].path);
                }
            }
        }
        return absUrl(images[0].url || images[0].path);
    }

    if (covers && covers.length) {
        return absUrl(covers[0].url || covers[0].path);
    }
    return "";
}

/* ------------------------------------------------------------ collation */

// QuickJS is built without Intl, so compare by a folded key: case-insensitive,
// accents dropped, and the Nordic letters after Z where those catalogues put
// them.
const NORDIC = {"Å": "Z1", "Ä": "Z2", "Ö": "Z3", "Æ": "Z2", "Ø": "Z3"};

function sortKey(name) {
    let key = text(name).toUpperCase();
    if (/^(sv|da|nb|nn|no|fi)/i.test(session ? session.language : "")) {
        key = key.replace(/[ÅÄÖÆØ]/g, (c) => NORDIC[c]);
    }
    try {
        key = key.normalize("NFD").replace(/[̀-ͯ]/g, "");
    } catch (e) {
        // No normalize(): leave the accents in.
    }
    return key;
}

function byName(a, b) {
    const ka = sortKey(a);
    const kb = sortKey(b);
    return ka < kb ? -1 : (ka > kb ? 1 : 0);
}

function byEntryName(a, b) {
    return byName(a.name, b.name);
}

/* ---------------------------------------------------------------- entries */

function durationText(ms) {
    const mins = Math.round((ms || 0) / 60000);
    if (mins <= 0) {
        return "";
    }
    if (mins < 60) {
        return mins + " min";
    }
    const m = mins % 60;
    return Math.floor(mins / 60) + " h" + (m ? " " + m + " min" : "");
}

function describe(parts) {
    return parts.filter((p) => !!text(p)).join(" · ");
}

function plural(n, one, many) {
    return n + " " + (n === 1 ? one : many);
}

// Where a playable thing's manifest lives. Items carry their own stitcher
// path, which is better than a guess built from the id (it is correct for the
// odd title whose media id is not its item id). Remembered here for resolve();
// anything resolved without a listing in this run (a favorite, say) is
// addressed by its id instead.
const paths = {};

function rememberPath(id, obj) {
    const stitched = obj && obj.stitched;
    if (!stitched) {
        return;
    }
    let found = "";
    const list = stitched.paths || stitched.urls || [];
    for (let i = 0; i < list.length && !found; i++) {
        const candidate = text(list[i].path || list[i].url);
        const kind = text(list[i].type);
        if (candidate && (!kind || kind === "hls")) {
            found = candidate;
        }
    }
    found = found || text(stitched.path);
    if (found) {
        paths[id] = found;
    }
}

function itemId(item) {
    return text(item._id || item.id);
}

function isSeries(item) {
    return text(item.type) === "series" ||
        !!(item.seasonsNumbers && item.seasonsNumbers.length > 0);
}

// A film, asked for by name rather than by elimination: Sports and News are
// full of single items (a game, a segment) that are neither film nor series.
function isMovie(item) {
    return !isSeries(item) && FILM_TYPES[text(item.type).toLowerCase()] === true;
}

function seconds(ms) {
    return ms > 0 ? Math.round(ms / 1000) : undefined;
}

function channelEntry(item) {
    const id = itemId(item);
    rememberPath("channel:" + id, item);

    // What is on now, and next: what someone scanning a wall of channel logos
    // wants, rather than a sentence about the channel itself. Clock times are
    // left out: the console's time zone is not the viewer's.
    const timelines = item.timelines || [];
    const now = Date.now();
    let current = 0;
    for (let i = 0; i < timelines.length; i++) {
        if (Date.parse(timelines[i].start) <= now &&
            now < Date.parse(timelines[i].stop)) {
            current = i;
            break;
        }
    }
    const title = (t) => t ? text(t.title) || text(get(t, "episode.name")) : "";
    const next = title(timelines[current + 1]);
    const showing = title(timelines[current]);

    return {
        id: "channel:" + id,
        type: "video",
        name: (item.number ? item.number + ". " : "") + text(item.name),
        description: showing
            ? describe([showing, next ? "Next: " + next : ""])
            : text(item.summary),
        image: pickImage(item)
    };
}

function vodEntry(item) {
    const id = itemId(item);

    if (isSeries(item)) {
        const n = (item.seasonsNumbers || []).length;
        return {
            id: "series:" + id,
            type: "folder",
            name: text(item.name),
            description: describe([
                text(item.genre), n ? plural(n, "season", "seasons") : ""
            ]) || text(item.summary),
            image: pickImage(item)
        };
    }

    rememberPath("episode:" + id, item);
    return {
        id: "episode:" + id,
        type: "video",
        name: text(item.name),
        description: describe([
            text(item.genre), durationText(item.duration), text(item.rating)
        ]) || text(item.summary),
        image: pickImage(item),
        duration: seconds(item.duration)
    };
}

function episodeEntry(item, seasonNumber) {
    const id = itemId(item);
    rememberPath("episode:" + id, item);

    const season = item.season || seasonNumber;
    let label = "";
    if (season && item.number) {
        label = "S" + season + "E" + item.number;
    } else if (item.number) {
        label = "Episode " + item.number;
    }

    return {
        id: "episode:" + id,
        type: "video",
        name: (label ? label + " - " : "") + text(item.name),
        description: describe([
            durationText(item.duration),
            text(item.description) || text(item.summary)
        ]),
        image: pickImage(item),
        duration: seconds(item.duration)
    };
}

/* --------------------------------------------------------------- channels */

function fetchChannels() {
    const start = new Date();
    const stop = new Date(start.getTime() + EPG_WINDOW_MIN * 60000);
    const span = {start: start.toISOString(), stop: stop.toISOString()};

    try {
        const json = api(API, "/v2/channels", span);
        const list = json && json.length ? json : (json && json.data) || [];
        if (list.length) {
            return list;
        }
        console.warn("empty channel list, trying the channels service");
    } catch (e) {
        console.warn("channel list failed (" + e.message +
                     "), trying the channels service");
    }

    // The same channels in a different shape: a second door, not a first.
    const json = api(CHANNELS_API, "/v2/guide/channels", Object.assign({
        offset: 0, limit: 1000, sort: "number:asc"
    }, span));
    const list = (json && json.data) || json || [];
    if (!list.length) {
        throw new Error("Pluto has no live channels for this region");
    }
    return list;
}

// Number zero and hidden channels are Pluto's own filler, not a lineup entry.
function isRealChannel(channel) {
    return text(channel.visibility) !== "hidden" && itemId(channel) !== "";
}

function channels() {
    return cached("channels", CHANNEL_TTL_MS, () => {
        const list = fetchChannels().filter(isRealChannel);
        list.sort((a, b) => (a.number || 0) - (b.number || 0));
        return list;
    });
}

// Pluto's own category for a channel, under whichever key the answering
// service uses.
function categoryOf(channel) {
    return text(channel.category) ||
        text(get(channel, "categories.0.name")) ||
        text(channel.genre) || "Other";
}

function channelCategories() {
    const counts = {};
    const order = [];
    channels().forEach((c) => {
        const name = categoryOf(c);
        if (!counts[name]) {
            counts[name] = 0;
            order.push(name);
        }
        counts[name]++;
    });

    return [{
        id: "live:*",
        type: "folder",
        name: "All channels",
        description: plural(channels().length, "channel", "channels")
    }].concat(order.map((name) => ({
        id: "live:" + name,
        type: "folder",
        name: name,
        description: plural(counts[name], "channel", "channels")
    })));
}

function channelsIn(category) {
    return channels().filter((c) => {
        return category === "*" || categoryOf(c) === category;
    }).map(channelEntry);
}

/* ------------------------------------------------------------- on demand */

// The whole on-demand catalogue in one request. A single answer is capped at
// 8 MiB by the plugin runtime; if Pluto ever outgrows that, this is what
// fails, with the runtime's own message.
function vodCategories() {
    return cached("vod", CACHE_TTL_MS, () => {
        // offset is the page size of the category list, not a starting point:
        // it defaults to 100 and there are rather more categories than that.
        // limit is not read by this endpoint; it costs nothing to send.
        const json = api(API, "/v3/vod/categories", {
            includeItems: "true",
            deviceType: DEVICE.deviceType,
            offset: 1000,
            limit: 1000
        });
        return ((json && json.categories) || []).filter((c) => {
            return (c.items || []).length > 0;
        });
    });
}

function categoryEntry(category) {
    const held = (category.items || []).length;
    const total = category.totalItemsCount || held;
    const more = held >= VOD_ITEM_CAP && total > held;
    return {
        id: "cat:" + itemId(category),
        type: "folder",
        name: text(category.name),
        description: held ? held + (more ? "+" : "") +
            (held === 1 && !more ? " title" : " titles") : "",
        image: pickImage(category) || pickImage((category.items || [])[0])
    };
}

function categoryItems(id) {
    const list = vodCategories();
    for (let i = 0; i < list.length; i++) {
        if (itemId(list[i]) === id) {
            return (list[i].items || []).map(vodEntry);
        }
    }
    throw new Error("unknown category " + id);
}

// pluto.tv does not navigate Movies and Shows by category: the ~400
// categories overlap heavily (one film sits in Top Titles, in an A-Z and in a
// themed collection) and many are editorial. It offers a short row of genres
// and puts the titles of the chosen one behind it. Every item names its own
// genre, so the same grouping is built from the items: collect the catalogue,
// drop duplicates and bucket what is left.
//
// Labelling an item "movie" is not enough to belong in Movies, though: a
// handball match, a diving championship and a concert all come back typed as
// films. The catalogue's categories are grouped into a dozen sections by
// mainCategories, and one of them holds nothing but films; the genres that
// appear in it are the film genres. Every film in those genres counts
// wherever it is filed, and nothing is named or numbered here since the ids
// are regional.
function filmGenres(list) {
    const known = {};
    const mixed = {};
    list.forEach((c) => {
        const holdsSeries = (c.items || []).some(isSeries);
        (c.mainCategories || []).forEach((m) => {
            const id = text(m.categoryID);
            if (id) {
                known[id] = true;
                if (holdsSeries) {
                    mixed[id] = true;
                }
            }
        });
    });

    const genres = {};
    let found = false;
    list.forEach((c) => {
        const filed = (c.mainCategories || []).some((m) => {
            const id = text(m.categoryID);
            return known[id] && !mixed[id];
        });
        if (!filed) {
            return;
        }
        (c.items || []).forEach((item) => {
            if (!isSeries(item)) {
                genres[text(item.genre) || "Other"] = true;
                found = true;
            }
        });
    });

    // Null when the catalogue has no section of its own for films: films are
    // then left where they are, so Movies is looser than the site, not empty.
    return found ? genres : null;
}

function vodIndex() {
    return cached("index", CACHE_TTL_MS, () => {
        const list = vodCategories();

        // Not every region labels its films. If none is, accept anything that
        // is not a series rather than show an empty Movies.
        const loose = !list.some((c) => (c.items || []).some(isMovie));
        const films = filmGenres(list);

        const seen = {};
        const index = {movies: {}, shows: {}};
        list.forEach((c) => {
            (c.items || []).forEach((item) => {
                const id = itemId(item);
                if (!id || seen[id]) {
                    return;
                }
                let kind = "";
                if (isSeries(item)) {
                    kind = "shows";
                } else if (loose || isMovie(item)) {
                    kind = "movies";
                }
                if (!kind) {
                    return;
                }
                seen[id] = true;

                const genre = text(item.genre) || "Other";
                if (kind === "movies" && films && films[genre] !== true) {
                    return;
                }
                (index[kind][genre] = index[kind][genre] || []).push(item);
            });
        });

        Object.keys(index).forEach((kind) => {
            Object.keys(index[kind]).forEach((genre) => {
                index[kind][genre].sort((a, b) => byName(a.name, b.name));
            });
        });
        return index;
    });
}

function genres(kind) {
    const by = vodIndex()[kind] || {};
    return Object.keys(by).sort(byName).map((genre) => {
        const items = by[genre];
        let art = "";
        // The first title alphabetically may have no artwork; take one that has.
        for (let i = 0; i < items.length && !art; i++) {
            art = pickImage(items[i]);
        }
        return {
            id: "genre:" + kind + ":" + genre,
            type: "folder",
            name: genre,
            description: plural(items.length, "title", "titles"),
            image: art
        };
    });
}

function genreItems(kind, genre) {
    const items = (vodIndex()[kind] || {})[genre];
    if (!items) {
        throw new Error("unknown genre " + genre);
    }
    return items.map(vodEntry);
}

// A series: every episode of every season arrives in one response, so the
// season list and the episodes behind it come out of one request.
function seriesData(id) {
    return cached("series:" + id, CACHE_TTL_MS, () => {
        return api(API, "/v3/vod/series/" + encodeURIComponent(id) + "/seasons", {
            includeItems: "true",
            deviceType: DEVICE.deviceType
        });
    });
}

function seasonEntries(id, number) {
    const seasons = seriesData(id).seasons || [];
    for (let i = 0; i < seasons.length; i++) {
        if (String(seasons[i].number) === String(number)) {
            return (seasons[i].episodes || []).map((e) => {
                return episodeEntry(e, seasons[i].number);
            });
        }
    }
    throw new Error("unknown season " + number);
}

function seriesEntries(id) {
    const data = seriesData(id);
    const seasons = data.seasons || [];

    // One season is not worth a folder of its own.
    if (seasons.length === 1) {
        return seasonEntries(id, seasons[0].number);
    }
    const image = pickImage(data);
    return seasons.map((s) => ({
        id: "season:" + id + ":" + s.number,
        type: "folder",
        name: "Season " + s.number,
        description: plural((s.episodes || []).length, "episode", "episodes"),
        image: image
    }));
}

/* --------------------------------------------------------------- playback */

// Normalises whatever the catalogue handed out into a path below /v2. The
// values differ by service and age: some are relative, some absolute and
// pointing at a retired stitcher host, some carry a version prefix and a
// query of their own. Only the path is worth keeping; the host, version and
// query all come from the live session instead.
function stitchPath(path) {
    let p = text(path).replace(/^https?:\/\/[^/]+/, "").split("?")[0];
    if (p.charAt(0) !== "/") {
        p = "/" + p;
    }
    return "/v2" + p.replace(/^\/v\d+(?=\/)/, "");
}

// Everything below /v2 on the stitcher named by this session, with the
// session's own parameters. Asking v1, or v2 without the full parameter set,
// is answered with a manifest that plays a slate saying Pluto is no longer
// available on this device: an ordinary, playable stream, so it fails as the
// wrong content rather than as an error. includeExtendedEvents is part of
// that set; it reads like a request for metadata but the clients that work
// send it. masterJWTPassthrough carries the token into the variant playlists.
function stitchUrl(s, path) {
    return s.stitcher + stitchPath(path) + "?" + s.params +
        "&jwt=" + encodeURIComponent(s.token) +
        "&masterJWTPassthrough=true" +
        "&includeExtendedEvents=true";
}

function resolveStream(id) {
    const s = authorize();
    let path = paths[id];
    if (!path) {
        if (id.indexOf("channel:") === 0) {
            path = "/stitch/hls/channel/" +
                encodeURIComponent(id.substring(8)) + "/master.m3u8";
        } else if (id.indexOf("episode:") === 0) {
            path = "/stitch/hls/episode/" +
                encodeURIComponent(id.substring(8)) + "/master.m3u8";
        } else {
            throw new Error("nothing to play for " + id);
        }
    }
    const url = stitchUrl(s, path);
    console.log("playing hls: " + url);
    return url;
}

/* ----------------------------------------------------------------- browse */

const ROOT = [
    {id: "live", type: "folder", name: "Live TV",
     description: "Channels, grouped by category"},
    {id: "movies", type: "folder", name: "Movies",
     description: "On demand, by genre"},
    {id: "shows", type: "folder", name: "TV Shows",
     description: "On demand, by genre"},
    {id: "cats", type: "folder", name: "Collections",
     description: "Pluto's own on-demand categories"}
];

// "a:b:rest" -> ["a", "b", "rest"]; the rest may hold colons of its own.
function split3(id) {
    const first = id.indexOf(":");
    const second = id.indexOf(":", first + 1);
    return [id.substring(0, first), id.substring(first + 1, second),
            id.substring(second + 1)];
}

return {
    name: "Pluto TV",

    discover() {
        return [{
            name: "Pluto TV",
            detail: "Free live channels and on-demand TV",
            icon: "📺",
            root: "",

            browse(id) {
                if (!id) {
                    return ROOT;
                }
                if (id === "live") {
                    return channelCategories();
                }
                if (id === "movies" || id === "shows") {
                    return genres(id);
                }
                if (id === "cats") {
                    return vodCategories().map(categoryEntry);
                }
                if (id.indexOf("live:") === 0) {
                    return channelsIn(id.substring(5));
                }
                if (id.indexOf("genre:") === 0) {
                    const p = split3(id);
                    return genreItems(p[1], p[2]);
                }
                if (id.indexOf("cat:") === 0) {
                    return categoryItems(id.substring(4));
                }
                if (id.indexOf("series:") === 0) {
                    return seriesEntries(id.substring(7));
                }
                if (id.indexOf("season:") === 0) {
                    const p = split3(id);
                    return seasonEntries(p[1], p[2]);
                }
                console.warn("cannot browse " + id);
                return [];
            },

            resolve(id) {
                return resolveStream(id);
            }
        }];
    }
};
