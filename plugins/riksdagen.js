// SPDX-License-Identifier: GPL-3.0-or-later
//
// Riksdagen: the Swedish parliament's web TV. Every debate in the chamber,
// interpellation, open hearing and committee debate since the 2000s,
// recorded and published as open data by the Riksdag administration.
//
// The tree:
//   /                    Senaste, Interpellationsdebatter, Öppna
//                        utfrågningar, Öppna sammanträden, Utskotten
//   list:<query>         recordings matching a query of the open data API,
//                        the latest debated first
//   committees           a folder per committee: the debates of its reports
//
// Search (options menu) searches all recordings: a subject, a member, a
// minister.
//
// The listings come from the open data API's podcast format, which has
// what a listing needs per recording: title, speaker, poster frame,
// duration, date, the debate's document id and its sound as MP3:
//   https://data.riksdagen.se/dokumentlista/?avd=webbtv&webbtv=1
//       &utformat=podd&sort=debattdagtid&sz=50&p=<page>[&doktyp=..]
//       [&utskott=..][&sok=..]
// Playing a recording as video asks the media API for its files,
//   https://data.riksdagen.se/api/mhs-vodapi?<document id>
//       -> { videodata: [{ streams: { files: [...] }, ... }] }
// and takes the stream or the largest MP4 among them; with nothing found
// there, or with the Play setting at Sound only, the MP3 plays instead.
// (svtplay-dl, lib/svtplay_dl/service/riksdagen.py, reads the same video
// from the web page.)

const LIST = "https://data.riksdagen.se/dokumentlista/";
const MEDIA = "https://data.riksdagen.se/api/mhs-vodapi?";
const PAGE_SIZE = 50;
const PAGES = 2;
const CACHE_SEC = 10 * 60;

// [name, what it is, extra query]. The document types are the open data
// API's own codes.
const FOLDERS = [
    ["Senaste", "Everything recorded, the latest first", {}],
    ["Interpellationsdebatter", "A member asks a minister, in the chamber", { doktyp: "ip" }],
    ["Öppna utfrågningar", "Committee hearings open to the public", { doktyp: "sam-ou" }],
    ["Öppna sammanträden", "Open committee meetings, the EU committee's among them",
     { doktyp: "sam-st" }]
];

// [code, name]: the committees, by the codes the API filters on.
const COMMITTEES = [
    ["au", "Arbetsmarknadsutskottet"], ["cu", "Civilutskottet"], ["fiu", "Finansutskottet"],
    ["fou", "Försvarsutskottet"], ["juu", "Justitieutskottet"],
    ["ku", "Konstitutionsutskottet"], ["kru", "Kulturutskottet"],
    ["mju", "Miljö- och jordbruksutskottet"], ["nu", "Näringsutskottet"],
    ["sfu", "Socialförsäkringsutskottet"], ["sku", "Skatteutskottet"],
    ["sou", "Socialutskottet"], ["tu", "Trafikutskottet"], ["ubu", "Utbildningsutskottet"],
    ["uu", "Utrikesutskottet"]
];

function listUrl(extra, page) {
    return LIST + "?" + util.query(Object.assign({
        avd: "webbtv", webbtv: 1, utformat: "podd", sort: "debattdagtid",
        sz: PAGE_SIZE, p: page
    }, extra));
}

// The text of a feed item's first child called 'name'; the itunes: ones
// keep their prefix.
const field = (item, name) => xml.text(item, name);

function recording(item) {
    const link = field(item, "link") || field(item, "guid");
    const doc = (/\/dokumentstatus\/([^\/?#.]+)/.exec(link) || [])[1];
    const enclosure = xml.attr(xml.child(item, "enclosure"), "url");
    if (!doc && !enclosure)
        return null;
    const picture = xml.child(item, "itunes:image");
    const speaker = util.text(field(item, "itunes:subtitle"));
    const text = util.text(field(item, "description"));
    return {
        id: doc ? "d:" + doc : enclosure,
        type: "video",
        name: util.text(field(item, "title")) || doc,
        description: [speaker, util.clip(text, 900)].filter(s => s).join("\n"),
        image: xml.attr(picture, "href") || xml.text(picture),
        uri: enclosure,
        duration: util.duration(field(item, "itunes:duration")),
        date: field(item, "pubDate") || undefined
    };
}

// The recordings a query finds: its first pages, side by side.
function recordings(extra) {
    const urls = [];
    for (let page = 1; page <= PAGES; page++)
        urls.push({ url: listUrl(extra, page), timeout: 30 });
    const out = [];
    const seen = {};
    let failure = null;
    http.all(urls).forEach(res => {
        if (!res.ok) {
            failure = failure || res.error || "HTTP " + res.status;
            return;
        }
        let feed;
        try {
            feed = res.xml();
        } catch (e) {
            failure = failure || e.message;
            return;
        }
        xml.children(xml.child(feed, "channel"), "item").forEach(item => {
            const entry = recording(item);
            if (entry && !seen[entry.id]) {
                seen[entry.id] = true;
                out.push(entry);
            }
        });
    });
    if (!out.length && failure)
        throw new Error("The Riksdag's open data did not answer (" + failure + ")");
    return out;
}

// Every URL anywhere in a JSON value.
function urls(value, out) {
    if (typeof value === "string") {
        if (/^https?:\/\//.test(value))
            out.push(value);
    } else if (Array.isArray(value)) {
        value.forEach(v => urls(v, out));
    } else if (value && typeof value === "object") {
        Object.keys(value).forEach(k => urls(value[k], out));
    }
    return out;
}

// The video of a recording: an HLS stream if there is one, else the MP4
// with the most lines ("..._720p.mp4" over "..._360p.mp4"). Null if the
// media API has neither.
function videoOf(doc) {
    const res = http.get(MEDIA + encodeURIComponent(doc), { timeout: 20 });
    if (!res.ok)
        return null;
    let json;
    try {
        json = res.json();
    } catch (e) {
        return null;
    }
    const found = urls(json.videodata || json, []);
    const stream = found.find(u => /\.m3u8(\?|$)/i.test(u));
    if (stream)
        return stream;
    const lines = u => Number((/(\d{3,4})p/i.exec(u) || [])[1]) || 0;
    const mp4 = found.filter(u => /\.mp4(\?|$)/i.test(u) && !/_aud\./i.test(u))
        .sort((a, b) => lines(b) - lines(a));
    return mp4[0] || null;
}

return {
    name: "Riksdagen",
    settings: [{
        key: "play",
        label: "Play",
        about: "Video shows the chamber. Sound only plays the debate as audio, which " +
            "starts at once and needs little of the connection.",
        options: [
            { value: "video", label: "Video" },
            { value: "audio", label: "Sound only" }
        ],
        default: "video"
    }],
    discover() {
        return [{
            name: "Riksdagen",
            detail: "Debates of the Swedish parliament",
            icon: "🎤",
            root: "/",

            browse(id) {
                if (id === "/")
                    return FOLDERS.map(f => ({
                        id: "list:" + util.query(f[2]), type: "folder", name: f[0],
                        description: f[1]
                    })).concat([{
                        id: "committees", type: "folder", name: "Utskotten",
                        description: "The debates of each committee's reports"
                    }]);
                if (id === "committees")
                    return COMMITTEES.map(c => ({
                        id: "list:" + util.query({ utskott: c[0] }), type: "folder", name: c[1]
                    }));
                if (id.startsWith("list:")) {
                    const extra = {};
                    id.slice(5).split("&").filter(s => s).forEach(pair => {
                        const eq = pair.indexOf("=");
                        extra[decodeURIComponent(pair.slice(0, eq))] =
                            decodeURIComponent(pair.slice(eq + 1));
                    });
                    return util.memo(id, CACHE_SEC, () => recordings(extra));
                }
                throw new Error("unknown folder " + id);
            },

            search(query) {
                return recordings({ sok: query });
            },

            resolve(id, entry) {
                const audio = entry && entry.uri;
                if (settings.get("play") !== "audio" && id.startsWith("d:")) {
                    try {
                        const video = videoOf(id.slice(2));
                        if (video)
                            return video;
                    } catch (e) {
                        console.warn("video of " + id + ": " + e.message);
                    }
                    if (audio)
                        console.warn(id + ": no video found; playing the sound");
                }
                if (audio)
                    return audio;
                throw new Error("The Riksdag has no recording of this to play");
            }
        }];
    }
};
