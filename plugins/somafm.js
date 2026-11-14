// SPDX-License-Identifier: GPL-3.0-or-later
//
// SomaFM: some forty hand-curated, commercial-free internet radio channels
// from San Francisco, supported by their listeners (somafm.com).
//
// The tree is one folder: the channels, the most listened to first. (There
// are no genre folders: the channels fit a few screens, each says its genre,
// and the app's own Filter narrows them down.)
//
// One request lists everything (channels.json: id, title, description, dj,
// genre, artwork in three sizes, the number of listeners, the song on air
// and a playlist per format and quality). A channel's playlists are .pls
// files that name the stream servers; resolve() reads the one that suits
// the Quality setting and hands the player a stream of it. While it plays,
// the station's own titles (ICY) say what is on.

const API = "https://api.somafm.com";

// Playlist qualities as channels.json names them, best first, and the
// formats the player is happiest with, best first.
const QUALITIES = ["highest", "high", "low"];
const FORMATS = ["aac", "mp3", "aacp"];

function channels() {
    return util.memo("channels", 120, () => {
        const list = http.json(API + "/channels.json", { timeout: 20 }).channels;
        if (!Array.isArray(list))
            throw new Error("SomaFM sent no channels");
        return list;
    });
}

// The playlist of 'channel' to play at the wanted quality: that quality if
// the channel has it, else the next lower, else whatever there is.
function pickPlaylist(channel, wanted) {
    const lists = (channel.playlists || []).filter(p => p && p.url);
    const from = Math.max(0, QUALITIES.indexOf(wanted));
    const order = QUALITIES.slice(from).concat(QUALITIES.slice(0, from).reverse());
    for (const quality of order) {
        for (const format of FORMATS) {
            const hit = lists.find(p => p.quality === quality && p.format === format);
            if (hit)
                return hit;
        }
    }
    return lists[0] || null;
}

// The stream addresses in a .pls ("File1=https://ice1.somafm.com/...").
function streamsOf(pls) {
    const out = [];
    String(pls).split(/\r?\n/).forEach(line => {
        const m = /^\s*File\d+\s*=\s*(\S+)/i.exec(line);
        if (m)
            out.push(m[1]);
    });
    return out;
}

function entry(c) {
    const facts = [];
    if (c.genre)
        facts.push(String(c.genre).split("|").join(" · "));
    const listeners = Number(c.listeners);
    if (listeners > 0)
        facts.push(listeners + (listeners === 1 ? " listener" : " listeners"));
    let description = util.text(c.description);
    if (c.lastPlaying)
        description += (description ? "\n" : "") + "On air: " + util.text(c.lastPlaying);
    if (facts.length)
        description += (description ? "\n" : "") + facts.join("  ·  ");
    return {
        id: String(c.id),
        type: "audio",
        name: util.text(c.title) || String(c.id),
        description: description,
        image: c.xlimage || c.largeimage || c.image || ""
    };
}

return {
    name: "SomaFM",
    settings: [{
        key: "quality",
        label: "Sound quality",
        about: "Highest is 128 kbit/s AAC or 256 kbit/s MP3 where a channel " +
               "has it. Low is 64 kbit/s and under, for a connection that stutters.",
        options: [
            { value: "highest", label: "Highest" },
            { value: "high", label: "Good" },
            { value: "low", label: "Low" }
        ],
        default: "highest"
    }],
    discover() {
        return [{
            name: "SomaFM",
            detail: "Listener-supported, commercial-free radio",
            icon: "📻",
            root: "/",
            browse(id) {
                if (id !== "/")
                    throw new Error("unknown folder " + id);
                return channels()
                    .slice()
                    .sort((a, b) => (Number(b.listeners) || 0) - (Number(a.listeners) || 0))
                    .map(entry);
            },
            resolve(id) {
                const channel = channels().find(c => String(c.id) === id);
                if (!channel)
                    throw new Error("SomaFM no longer has this channel");
                const playlist = pickPlaylist(channel, settings.get("quality"));
                if (playlist) {
                    try {
                        const res = http.get(playlist.url, { timeout: 15 });
                        const streams = res.ok ? streamsOf(res.body) : [];
                        if (streams.length)
                            return streams[0];
                    } catch (e) {
                        console.warn("playlist of " + id + ": " + e.message);
                    }
                }
                // The servers name their streams after the channel; this one
                // every channel has.
                return "https://ice1.somafm.com/" + encodeURIComponent(id) + "-128-mp3";
            }
        }];
    }
};
