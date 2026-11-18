// SPDX-License-Identifier: GPL-3.0-or-later
//
// TV channels from iptv-org (https://iptv-org.github.io): a community-kept
// collection of some ten thousand publicly available streams from all over
// the world. No account or key is needed.
//
// The channels come from the project's playlists, one per country, category
// and language, and everything else (the names of the countries, categories
// and languages) from its API:
//
//   /                  By country, By category, By language
//   countries          every country, by name, with its flag
//   country:se         that country's channels
//   categories         news, sports, music, ...
//   category:news      that category's channels, from everywhere
//   languages          the languages of the countries
//   language:swe       the channels in that language
//
// Search (options menu) looks channels up by name: among the folder's own
// channels from inside a country, category or language, and among all of
// them from anywhere else.
//
// The streams are whatever their broadcasters put out, checked by the
// project now and then: a good many are geo-blocked, some are not on round
// the clock, and some have gone since. The playlist says which are known
// to be one or the other, and so does the description here.

const API = "https://iptv-org.github.io/api/";
const PLAYLISTS = "https://iptv-org.github.io/iptv/";

// The project lists every channel ever known; these are its active ones.
const LIST_TTL_S = 6 * 3600;      // countries, categories, languages
const CHANNELS_TTL_S = 30 * 60;   // a playlist of channels

// The playlist of everything, for a search from outside a folder of
// channels, is a few megabytes; more than the host's most is not read.
const INDEX_MAX_BYTES = 8 << 20;

// How many hits a search shows at most.
const SEARCH_LIMIT = 300;

function folder(id, name, description) {
    return { id: id, type: "folder", name: name, description: description || "" };
}

function json(path) {
    return http.json(API + path, { timeout: 30 });
}

// ---------------------------------------------------------------- playlists

// The attribute 'name' of an #EXTINF line: tvg-logo="x" -> x.
function attr(line, name) {
    const m = new RegExp("\\s" + name + "=\"([^\"]*)\"").exec(line);
    return m ? m[1].trim() : "";
}

// An #EXTINF line's title (after the first comma outside quotes: the
// title may have commas of its own), as the name of the channel and what
// the playlist says about it:
// "SVT 1 (720p) [Geo-blocked]" -> { name: "SVT 1", quality: "720p",
// labels: ["Geo-blocked"] }.
function title(line) {
    let quoted = false, cut = -1;
    for (let i = 0; i < line.length && cut < 0; i++) {
        const c = line.charAt(i);
        if (c === "\"")
            quoted = !quoted;
        else if (c === "," && !quoted)
            cut = i;
    }
    let text = cut < 0 ? "" : line.substring(cut + 1).trim();
    const labels = [];
    text = text.replace(/\s*\[([^\]]*)\]/g, (all, label) => {
        labels.push(label.trim());
        return "";
    });
    let quality = "";
    text = text.replace(/\s*\((\d{3,4}[pi])\)/i, (all, q) => {
        quality = q;
        return "";
    });
    return { name: text.trim(), quality: quality, labels: labels };
}

// What the app keeps as an id is the stream's address, with the request
// headers it needs where it needs any ("<url> #referrer=<r> #ua=<u>"; an
// address has no spaces), so a favorite plays without the playlist.
function streamId(url, referrer, agent) {
    let id = url;
    if (referrer)
        id += " #referrer=" + referrer;
    if (agent)
        id += " #ua=" + agent;
    return id;
}

function parseStreamId(id) {
    const parts = String(id).split(" #");
    const out = { url: parts[0], headers: {} };
    for (let i = 1; i < parts.length; i++) {
        const eq = parts[i].indexOf("=");
        const key = parts[i].substring(0, eq), value = parts[i].substring(eq + 1);
        if (key === "referrer" && value)
            out.headers.Referer = value;
        else if (key === "ua" && value)
            out.headers["User-Agent"] = value;
    }
    return out;
}

// A playlist (M3U) as entries.
function parsePlaylist(body) {
    const lines = String(body).split(/\r?\n/);
    const out = [];
    let info = null, referrer = "", agent = "";
    for (const raw of lines) {
        const line = raw.trim();
        if (!line)
            continue;
        if (line.startsWith("#EXTINF")) {
            info = line;
            referrer = agent = "";
        } else if (line.startsWith("#EXTVLCOPT:http-referrer=")) {
            referrer = line.substring(25).trim();
        } else if (line.startsWith("#EXTVLCOPT:http-user-agent=")) {
            agent = line.substring(27).trim();
        } else if (line.charAt(0) !== "#" && info) {
            const t = title(info);
            const facts = [];
            const group = attr(info, "group-title");
            if (group && group !== "Undefined")
                facts.push(group.split(";").join(" · "));
            if (t.quality)
                facts.push(t.quality);
            for (const label of t.labels)
                facts.push(label);
            out.push({
                id: streamId(line, referrer, agent),
                type: "video",
                name: t.name || attr(info, "tvg-id") || "(unnamed channel)",
                description: facts.join("  ·  "),
                image: attr(info, "tvg-logo")
            });
            info = null;
        }
    }
    return out;
}

// The channels of one playlist ("countries/se"), by name.
function channels(path) {
    return util.memo("m3u:" + path, CHANNELS_TTL_S, () => {
        const res = http.get(PLAYLISTS + path + ".m3u", { timeout: 30 });
        if (res.status === 404)
            return [];
        if (!res.ok)
            throw new Error("HTTP " + res.status + " for the channel list");
        return parsePlaylist(res.body).sort((a, b) =>
            a.name.localeCompare(b.name, undefined, { sensitivity: "base" }));
    });
}

// Every channel, for a search from anywhere else.
function everything() {
    return util.memo("m3u:index", CHANNELS_TTL_S, () => {
        const res = http.get(PLAYLISTS + "index.m3u",
                             { timeout: 60, maxBytes: INDEX_MAX_BYTES });
        if (!res.ok)
            throw new Error("HTTP " + res.status + " for the channel list");
        let body = String(res.body);
        if (res.truncated) // the last line may be cut short
            body = body.substring(0, body.lastIndexOf("\n"));
        return parsePlaylist(body);
    });
}

// ------------------------------------------------------------------ folders

function browseRoot() {
    return [
        folder("countries", "By country", "Channels from every country"),
        folder("categories", "By category", "News, sports, music, movies, ..."),
        folder("languages", "By language", "Channels in a language, from anywhere")
    ];
}

function countries() {
    return util.memo("countries", LIST_TTL_S, () => json("countries.json"));
}

function browseCountries() {
    return countries()
        .filter(c => c.code && c.name)
        .sort((a, b) => a.name.localeCompare(b.name))
        .map(c => folder("country:" + c.code.toLowerCase(),
                         (c.flag ? c.flag + "  " : "") + c.name));
}

function browseCategories() {
    return util.memo("categories", LIST_TTL_S, () => json("categories.json"))
        .filter(c => c.id && c.name)
        .sort((a, b) => a.name.localeCompare(b.name))
        .map(c => folder("category:" + c.id, c.name, c.description || ""));
}

// Of the world's seven thousand languages, those of some country (the ones
// that have channels, give or take).
function browseLanguages() {
    const spoken = {};
    for (const c of countries())
        for (const code of (c.languages || []))
            spoken[code] = true;
    return util.memo("languages", LIST_TTL_S, () => json("languages.json"))
        .filter(l => l.code && l.name && spoken[l.code])
        .sort((a, b) => a.name.localeCompare(b.name))
        .map(l => folder("language:" + l.code, l.name));
}

// The playlist behind a folder of channels, or "" for any other folder.
function playlistOf(id) {
    const i = String(id || "").indexOf(":");
    if (i < 0)
        return "";
    const kind = id.substring(0, i), code = encodeURIComponent(id.substring(i + 1));
    if (kind === "country")
        return "countries/" + code;
    if (kind === "category")
        return "categories/" + code;
    if (kind === "language")
        return "languages/" + code;
    return "";
}

function search(query, folderId) {
    const words = String(query).toLowerCase().split(/\s+/).filter(w => w);
    if (!words.length)
        return [];
    const path = playlistOf(folderId);
    const pool = path ? channels(path) : everything();
    const hits = [];
    for (const e of pool) {
        const name = e.name.toLowerCase();
        if (words.every(w => name.indexOf(w) >= 0)) {
            hits.push(e);
            if (hits.length >= SEARCH_LIMIT)
                break;
        }
    }
    return hits;
}

return {
    name: "iptv-org",
    discover() {
        return [{
            name: "IPTV channels",
            detail: "Free channels from all over the world, by iptv-org",
            icon: "📺",
            image: "https://github.com/iptv-org.png?size=400",
            root: "/",
            browse(id) {
                if (id === "/")
                    return browseRoot();
                if (id === "countries")
                    return browseCountries();
                if (id === "categories")
                    return browseCategories();
                if (id === "languages")
                    return browseLanguages();
                const path = playlistOf(id);
                if (path) {
                    const list = channels(path);
                    if (!list.length)
                        throw new Error("No channels here");
                    return list;
                }
                throw new Error("unknown folder " + id);
            },
            search(query, folderId) {
                return search(query, folderId);
            },
            // The stream, with the headers its server wants.
            resolve(id) {
                const s = parseStreamId(id);
                if (!/^https?:\/\//i.test(s.url) && !/^rtmp/i.test(s.url))
                    throw new Error("this channel has no stream");
                return Object.keys(s.headers).length ? { uri: s.url, headers: s.headers }
                                                     : s.url;
            }
        }];
    }
};
