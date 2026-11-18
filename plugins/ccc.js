// SPDX-License-Identifier: GPL-3.0-or-later
//
// media.ccc.de: the talks of the Chaos Communication Congress and of a few
// hundred other hacker, free software and maker conferences, recorded and
// published by the C3 video operation centre (c3voc). No account is needed.
//
// The tree:
//   /                 Recent talks, Conferences
//   recent            the talks released last
//   conferences       every conference, the one with the newest talks first
//   c:<acronym>       the talks of one conference, most watched first
//
// Search (options menu) searches the talks of all conferences.
//
// Recent talks and search results come a page at a time (the API pages
// them); the number of the page after is the cursor the app hands back for
// more (browse(id, next)).
//
// A talk is played through resolve(), which asks for its recordings and
// picks one: video rather than the audio-only file, the size the Video
// quality setting asks for, and of those the file with the most languages
// in it (a talk given in German often has the interpreters' English as a
// second audio track, which the player's Audio language setting or the
// playback menu then picks). Subtitles a talk has are handed over with it.
//
// The API (https://github.com/voc/voctoweb) is plain JSON:
//   /public/conferences               { conferences: [{ acronym, title,
//                                       logo_url, event_last_released_at }] }
//   /public/conferences/<acronym>     the same for one, plus { events: [...] }
//   /public/events/recent?page=       { events: [{ guid, title, subtitle,
//   /public/events/search?q=&page=      description, persons, duration,
//                                       poster_url, thumb_url, release_date,
//                                       view_count, conference_title }] }
//   /public/events/<guid>             one event, plus { recordings: [{
//                                       recording_url, mime_type, language,
//                                       high_quality, width, height }] }
// The two lists are paged: a response that has a page after it says so in
// a Link header (rel="next").

const API = "https://api.media.ccc.de/public";
const CACHE_SEC = 10 * 60;
// Talks a page of search results asks for (the API's default is 25).
const PER_PAGE = 100;

function get(path) {
    return http.json(API + path, { headers: { Accept: "application/json" }, timeout: 30 });
}

// The page a cursor names: 1 for none.
const pageOf = next => Math.max(1, Math.floor(Number(next)) || 1);

// One page of a paged list of talks ('path' takes a page= parameter), and
// the number of the next when the API says there is one.
function talksPage(path, page) {
    const res = http.get(API + path + (path.indexOf("?") < 0 ? "?" : "&") + "page=" + page,
                         { headers: { Accept: "application/json" }, timeout: 30 });
    if (!res.ok)
        throw new Error("HTTP " + res.status + " from media.ccc.de");
    const events = res.json().events;
    const more = /rel="?next"?/.test(res.headers.link || "");
    return {
        entries: talks(events),
        next: more && Array.isArray(events) && events.length ? page + 1 : undefined
    };
}

function talk(e) {
    const people = (Array.isArray(e.persons) ? e.persons : []).join(", ");
    const facts = [people, util.text(e.conference_title)].filter(s => s);
    const subtitle = util.text(e.subtitle);
    const text = util.clip(util.text(e.description), 600);
    return {
        id: "e:" + e.guid,
        type: "video",
        name: util.text(e.title) || "(untitled talk)",
        description: [facts.join("  ·  "), subtitle, text].filter(s => s).join("\n"),
        image: e.poster_url || e.thumb_url || "",
        duration: Number(e.duration || e.length) || 0,
        date: e.release_date || e.date || undefined
    };
}

function talks(events, byViews) {
    const list = (Array.isArray(events) ? events : []).filter(e => e && e.guid);
    if (byViews)
        list.sort((a, b) => (Number(b.view_count) || 0) - (Number(a.view_count) || 0));
    return list.map(talk);
}

function conferences() {
    return util.memo("conferences", CACHE_SEC, () => {
        const list = get("/conferences").conferences || [];
        return list
            .filter(c => c && c.acronym && c.event_last_released_at)
            .sort((a, b) => String(b.event_last_released_at)
                  .localeCompare(String(a.event_last_released_at)))
            .map(c => ({
                id: "c:" + c.acronym,
                type: "folder",
                name: util.text(c.title) || c.acronym,
                description: util.text(c.description || ""),
                image: /\.(png|jpe?g)$/i.test(c.logo_url || "") ? c.logo_url : "",
                date: c.event_last_released_at
            }));
    });
}

// "deu-eng" -> 2: how many languages a recording carries.
const languages = r => String(r.language || "").split("-").filter(s => s).length;

// The recording of 'event' to play, for the quality asked for ("hd": the
// largest picture; "sd": the largest that is no taller than 576 lines).
function pickRecording(recordings, quality) {
    const videos = recordings.filter(r => r && r.recording_url &&
                                     /^video\//.test(r.mime_type || ""));
    const pool = videos.length ? videos
        : recordings.filter(r => r && r.recording_url && /^audio\//.test(r.mime_type || ""));
    const score = r => {
        const height = Number(r.height) || 0;
        const fits = quality === "sd" ? height <= 576 : true;
        return (fits ? 1e9 : 0) +
            // mp4 plays everywhere; webm needs more of a console.
            (/mp4/.test(r.mime_type) ? 1e8 : 0) +
            languages(r) * 1e6 +
            height * 100 +
            (r.high_quality ? 1 : 0);
    };
    let best = null;
    pool.forEach(r => {
        if (!best || score(r) > score(best))
            best = r;
    });
    return best;
}

function resolveTalk(guid) {
    const event = get("/events/" + encodeURIComponent(guid));
    const recordings = Array.isArray(event.recordings) ? event.recordings : [];
    const best = pickRecording(recordings, settings.get("quality"));
    if (!best)
        throw new Error("this talk has no recording yet");
    const subtitles = recordings
        .filter(r => r && r.recording_url && /subrip|vtt/.test(r.mime_type || ""))
        .map(r => ({ uri: r.recording_url, lang: String(r.language || "") }));
    return { uri: best.recording_url, subtitles: subtitles };
}

return {
    name: "media.ccc.de",
    settings: [{
        key: "quality",
        label: "Video quality",
        about: "High plays the full HD recording. Standard plays the smaller " +
               "one, which starts sooner and survives a slow connection.",
        options: [
            { value: "hd", label: "High (1080p)" },
            { value: "sd", label: "Standard (576p)" }
        ],
        default: "hd"
    }],
    discover() {
        return [{
            name: "media.ccc.de",
            detail: "Talks from the Chaos Computer Club and friends",
            icon: "🎤",
            image: "https://media.ccc.de/favicon-96x96.png",
            root: "/",

            browse(id, next) {
                if (id === "/")
                    return [
                        { id: "recent", type: "folder", name: "Recent talks",
                          description: "The recordings released last" },
                        { id: "conferences", type: "folder", name: "Conferences",
                          description: "Every event, the latest first" }
                    ];
                if (id === "recent") {
                    const page = pageOf(next);
                    return util.memo(id + "#" + page, CACHE_SEC,
                                     () => talksPage("/events/recent", page));
                }
                if (id === "conferences")
                    return conferences();
                if (id.startsWith("c:"))
                    return util.memo(id, CACHE_SEC, () =>
                        talks(get("/conferences/" + encodeURIComponent(id.slice(2))).events, true));
                throw new Error("unknown folder " + id);
            },

            search(query, folderId, next) {
                return talksPage("/events/search?q=" + encodeURIComponent(query) +
                                 "&per_page=" + PER_PAGE, pageOf(next));
            },

            resolve(id) {
                return resolveTalk(id.replace(/^e:/, ""));
            }
        }];
    }
};
