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
// A listing comes two pages of fifty at a time, asked for together; the
// number of the page after them is the cursor the app hands back for more
// (browse(id, next)).
//
// The listings come from the open data API's podcast format, which has
// what a listing needs per recording: title, speaker, poster frame,
// duration, date, the debate's document id and its sound as MP3:
//   https://data.riksdagen.se/dokumentlista/?avd=webbtv&webbtv=1
//       &utformat=podd&sort=debattdagtid&sz=50&p=<page>[&doktyp=..]
//       [&utskott=..][&sok=..]
// Every recording's files are named after one media id, the one in the
// feed's sound file, and the web player's page lists them as
//   https://mhdownload.riksdagen.se/VOD1/HD/<id>_aud.mp3        sound
//   https://mhdownload.riksdagen.se/VOD1/HD/<id>_720p.mp4       video
//   https://mamstream.riksdagen.se/VOD1/_definst_/smil:HD/<id>.smil/playlist.m3u8
//   https://mhdownload.riksdagen.se/SUB/<id>_swe.vtt            subtitles
// so playing a recording as video needs no lookup, only a check of which
// of them are there: the MP4 is published about an hour after the
// broadcast, the stream at once, and subtitles not for everything. With
// neither video, or with the Play setting at Sound only, the MP3 plays.
// (The open data media API, mhs-vodapi, answers 404 for recent
// recordings by any of their ids.)

const LIST = "https://data.riksdagen.se/dokumentlista/";
const STREAM = "https://mamstream.riksdagen.se/VOD1/_definst_/smil:";
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

// The page a cursor names: 1 for none.
const pageOf = next => Math.max(1, Math.floor(Number(next)) || 1);

// The recordings a query finds, PAGES pages of them from 'first' on, side
// by side, and the page after them when the last of them was full (the feed
// does not say how many there are).
function recordings(extra, first) {
    const urls = [];
    for (let page = first; page < first + PAGES; page++)
        urls.push({ url: listUrl(extra, page), timeout: 30 });
    const out = [];
    const seen = {};
    let failure = null;
    let full = false;
    http.all(urls).forEach((res, i) => {
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
        const items = xml.children(xml.child(feed, "channel"), "item");
        if (i === urls.length - 1)
            full = items.length >= PAGE_SIZE;
        items.forEach(item => {
            const entry = recording(item);
            if (entry && !seen[entry.id]) {
                seen[entry.id] = true;
                out.push(entry);
            }
        });
    });
    if (!out.length && failure)
        throw new Error("The Riksdag's open data did not answer (" + failure + ")");
    return { entries: out, next: full ? first + PAGES : undefined };
}

// The files of a recording, from the address of its sound; null when the
// address isn't of the usual shape.
function filesOf(audio) {
    const m = /^(https?:\/\/[^\/]+)\/VOD1\/((?:[^\/?#]+\/)*)(\d{12,})_aud\.mp3(?:[?#]|$)/i
        .exec(audio || "");
    if (!m)
        return null;
    const host = m[1], folder = m[2], id = m[3];
    return {
        id: id,
        mp4: host + "/VOD1/" + folder + id + "_720p.mp4",
        hls: STREAM + folder + id + ".smil/playlist.m3u8",
        vtt: host + "/SUB/" + id + "_swe.vtt"
    };
}

// Which of some addresses answer, asked for together and a byte each.
function present(uris) {
    return http.all(uris.map(u => ({
        url: u, headers: { Range: "bytes=0-0" }, maxBytes: 1, timeout: 15
    }))).map(res => res.ok);
}

// The video of a recording, with its subtitles, or null.
function videoOf(audio) {
    const files = filesOf(audio);
    if (!files) {
        console.warn("no media id in " + audio);
        return null;
    }
    const [mp4, hls, vtt] = present([files.mp4, files.hls, files.vtt]);
    // An MP4 file starts and seeks over plain HTTP; the stream is there
    // before it is.
    const uri = mp4 ? files.mp4 : hls ? files.hls : null;
    console.log(files.id + ": " + (uri || "no video") + (vtt ? ", with subtitles" : ""));
    if (!uri)
        return null;
    const playable = { uri: uri };
    if (vtt)
        playable.subtitles = [{ uri: files.vtt, lang: "sv", name: "Svenska" }];
    return playable;
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
            image: "https://www.riksdagen.se/icons/apple-touch-icon-180x180.png",
            root: "/",

            browse(id, next) {
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
                    const page = pageOf(next);
                    return util.memo(id + "#" + page, CACHE_SEC, () => recordings(extra, page));
                }
                throw new Error("unknown folder " + id);
            },

            search(query, folderId, next) {
                return recordings({ sok: query }, pageOf(next));
            },

            resolve(id, entry) {
                const audio = entry && entry.uri;
                if (settings.get("play") !== "audio" && audio) {
                    try {
                        const video = videoOf(audio);
                        if (video)
                            return video;
                    } catch (e) {
                        console.warn("video of " + id + ": " + e.message);
                    }
                    console.warn(id + ": no video found; playing the sound");
                }
                if (audio)
                    return audio;
                throw new Error("The Riksdag has no recording of this to play");
            }
        }];
    }
};
