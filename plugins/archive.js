// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Internet Archive (archive.org): feature films and cartoons that are
// in the public domain, concert recordings their bands allow to be traded,
// old time radio, and audiobooks read by LibriVox volunteers. No account or
// key is needed.
//
// The tree:
//   /                    the collections below, films first
//   c:<collection>       its most downloaded items
//   i:<identifier>       the tracks of an audio item (a concert's songs, a
//                        book's chapters, a radio series' episodes)
//
// A film is one item and one thing to play: resolve() looks up the item's
// files and picks the one made for streaming. An audio item is a folder of
// its tracks. Which of the two an item is goes by its media type.
//
// Search (options menu) asks the Archive for items matching what was typed:
// inside a collection, that collection; from the top, all of the films and
// audio there is.
//
// Two APIs, both plain JSON:
//   advancedsearch.php?q=...&fl[]=...&sort[]=...&rows=...&output=json
//       -> { response: { numFound, docs: [{ identifier, title, ... }] } }
//   metadata/<identifier>
//       -> { metadata: { title, ... }, files: [{ name, format, source,
//            length, size, title, track, ... }] }
// A file is at https://archive.org/download/<identifier>/<name>, an item's
// picture at https://archive.org/services/img/<identifier>.

const SITE = "https://archive.org";
const ROWS = 100;
const CACHE_SEC = 15 * 60;

// [collection, name, what it is, kind]. "video": its items are films, each
// played as one; "audio": its items are folders of tracks.
const COLLECTIONS = [
    ["feature_films", "Feature films", "Full-length films in the public domain", "video"],
    ["Film_Noir", "Film noir", "Shadows, detectives and bad decisions", "video"],
    ["SciFi_Horror", "Sci-fi and horror", "Flying saucers and creature features", "video"],
    ["Comedy_Films", "Comedies", "Chaplin, Keaton, the Stooges and company", "video"],
    ["silent_films", "Silent films", "From the first decades of cinema", "video"],
    ["classic_cartoons", "Classic cartoons", "Animated shorts from the golden age", "video"],
    ["classic_tv", "Classic TV", "Television from its first decades", "video"],
    ["prelinger", "Prelinger Archives", "Educational, industrial and amateur films", "video"],
    ["etree", "Live concerts", "Shows recorded by tapers, shared with the bands' blessing", "audio"],
    ["oldtimeradio", "Old time radio", "Drama, comedy and mystery from radio's golden age", "audio"],
    ["librivoxaudio", "Audiobooks", "Public domain books read by LibriVox volunteers", "audio"]
];

const FIELDS = ["identifier", "title", "description", "creator", "year", "date",
                "runtime", "downloads", "mediatype", "publicdate"];

function search(query, sort) {
    const params = ["q=" + encodeURIComponent(query)]
        .concat(FIELDS.map(f => "fl%5B%5D=" + f))
        .concat(["sort%5B%5D=" + encodeURIComponent(sort || "downloads desc"),
                 "rows=" + ROWS, "page=1", "output=json"]);
    const json = http.json(SITE + "/advancedsearch.php?" + params.join("&"), { timeout: 30 });
    return (json.response && json.response.docs) || [];
}

// A field the Archive gives as a string, a number, or a list of either.
function first(v) {
    return Array.isArray(v) ? (v.length ? String(v[0]) : "") : v == null ? "" : String(v);
}

const picture = id => SITE + "/services/img/" + encodeURIComponent(id);
const fileUrl = (id, name) => SITE + "/download/" + encodeURIComponent(id) + "/" +
      String(name).split("/").map(encodeURIComponent).join("/");

// An item of a listing: a film to play, or a folder of tracks.
function itemEntry(doc, kind) {
    const id = first(doc.identifier);
    const video = (first(doc.mediatype) || (kind === "video" ? "movies" : "audio")) === "movies";
    const facts = [];
    const creator = first(doc.creator);
    if (creator)
        facts.push(creator);
    const year = first(doc.year) || first(doc.date).slice(0, 4);
    if (/^\d{4}$/.test(year))
        facts.push(year);
    const text = util.clip(util.text(first(doc.description)), 600);
    return {
        id: (video ? "v:" : "i:") + id,
        type: video ? "video" : "folder",
        name: util.text(first(doc.title)) || id,
        description: facts.join("  ·  ") + (facts.length && text ? "\n" : "") + text,
        image: picture(id),
        duration: video ? util.duration(first(doc.runtime)) : 0,
        date: first(doc.publicdate) || undefined
    };
}

function metadata(id) {
    return util.memo("meta:" + id, CACHE_SEC, () => {
        const json = http.json(SITE + "/metadata/" + encodeURIComponent(id), { timeout: 30 });
        if (!json || !Array.isArray(json.files))
            throw new Error("the Archive has no files for this item");
        return json;
    });
}

const ext = name => (/\.([a-z0-9]+)$/i.exec(String(name)) || ["", ""])[1].toLowerCase();

// The file of a film to stream: the Archive derives an h.264 .mp4 from
// whatever was uploaded, which is what plays everywhere; failing that, the
// upload itself if it is something a player opens. Of several (a film in
// parts, the same film in two sizes) the largest.
function pickVideo(files) {
    const playable = ["mp4", "m4v", "mkv", "webm", "ogv", "avi", "mpg", "mpeg", "mov"];
    const rank = f => {
        const e = ext(f.name);
        const at = playable.indexOf(e);
        if (at < 0)
            return -1;
        // .mp4 first; among those, the stream-ready derivative.
        return (playable.length - at) * 10 + (/h\.264/i.test(f.format || "") ? 5 : 0);
    };
    let best = null;
    files.forEach(f => {
        const r = rank(f);
        if (r < 0)
            return;
        const size = Number(f.size) || 0;
        if (!best || r > best.rank || (r === best.rank && size > best.size))
            best = { file: f, rank: r, size: size };
    });
    return best ? best.file : null;
}

// The tracks of an audio item: one format of them (the item usually has
// each track as FLAC, VBR MP3 and a low-bitrate MP3), in track order.
function pickAudio(files) {
    const groups = [
        f => /^VBR MP3$/i.test(f.format || ""),
        f => ext(f.name) === "mp3" && !/64Kbps|sample/i.test(f.format || ""),
        f => ext(f.name) === "mp3",
        f => ext(f.name) === "ogg" || ext(f.name) === "m4a",
        f => ext(f.name) === "flac"
    ];
    for (const match of groups) {
        const hit = files.filter(match);
        if (hit.length)
            return hit;
    }
    return [];
}

function trackNumber(f) {
    const n = parseInt(String(f.track || "").split("/")[0], 10);
    return isFinite(n) ? n : 1e9;
}

function browseItem(id) {
    const meta = metadata(id);
    const album = util.text(first(meta.metadata && meta.metadata.title));
    const creator = util.text(first(meta.metadata && meta.metadata.creator));
    const tracks = pickAudio(meta.files).slice().sort((a, b) =>
        trackNumber(a) - trackNumber(b) || String(a.name).localeCompare(String(b.name)));
    if (!tracks.length) {
        // Not audio after all (search finds all kinds): play it as a film.
        const video = pickVideo(meta.files);
        if (!video)
            return [];
        return [{ id: "f:" + id + "/" + video.name, type: "video", name: album || id,
                  image: picture(id), uri: fileUrl(id, video.name),
                  duration: util.duration(video.length), size: Number(video.size) || undefined }];
    }
    return tracks.map(f => ({
        id: "f:" + id + "/" + f.name,
        type: "audio",
        name: util.text(f.title) || String(f.name).replace(/\.[a-z0-9]+$/i, ""),
        description: [creator || util.text(f.creator || f.artist), album]
            .filter(s => s).join("  ·  "),
        image: picture(id),
        uri: fileUrl(id, f.name),
        duration: util.duration(f.length),
        size: Number(f.size) || undefined
    }));
}

function kindOf(collection) {
    const hit = COLLECTIONS.find(c => c[0] === collection);
    return hit ? hit[3] : "video";
}

return {
    name: "Internet Archive",
    discover() {
        return [{
            name: "Internet Archive",
            detail: "Public domain films, concerts, old radio",
            icon: "🎞",
            image: "https://archive.org/offshoot_assets/apple-touch-icon.png",
            root: "/",

            browse(id) {
                if (id === "/")
                    return COLLECTIONS.map(c => ({
                        id: "c:" + c[0], type: "folder", name: c[1], description: c[2]
                    }));
                if (id.startsWith("c:")) {
                    const collection = id.slice(2);
                    return util.memo(id, CACHE_SEC, () =>
                        search("collection:" + collection + " AND mediatype:(movies OR audio OR etree)")
                            .map(doc => itemEntry(doc, kindOf(collection))));
                }
                if (id.startsWith("i:"))
                    return browseItem(id.slice(2));
                throw new Error("unknown folder " + id);
            },

            // In a collection: that collection. Anywhere else: everything
            // there is to watch or listen to.
            search(query, folderId) {
                const words = query.replace(/[^\p{L}\p{N}\s'-]/gu, " ").trim();
                if (!words)
                    return [];
                const scope = folderId && folderId.startsWith("c:")
                    ? "collection:" + folderId.slice(2)
                    : "mediatype:(movies OR audio OR etree)";
                const kind = folderId && folderId.startsWith("c:") ? kindOf(folderId.slice(2)) : "";
                return search("(" + words + ") AND " + scope)
                    .map(doc => itemEntry(doc, kind || (first(doc.mediatype) === "movies"
                                                        ? "video" : "audio")));
            },

            resolve(id, entry) {
                if (id.startsWith("v:")) {
                    const item = id.slice(2);
                    const file = pickVideo(metadata(item).files);
                    if (!file)
                        throw new Error("the Archive has nothing playable for this title");
                    return fileUrl(item, file.name);
                }
                if (entry && entry.uri)
                    return entry.uri;
                if (id.startsWith("f:")) {
                    const slash = id.indexOf("/");
                    return fileUrl(id.slice(2, slash), id.slice(slash + 1));
                }
                throw new Error("this item has nothing to play");
            }
        }];
    }
};
