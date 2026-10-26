// SPDX-License-Identifier: GPL-3.0-or-later
//
// Podcasts: the Apple Podcasts charts, plus any feeds of your own.
//
// The tree:
//   /                    My podcasts (FEEDS below), Top podcasts
//   mine                 the feeds in FEEDS
//   top                  the chart for COUNTRY
//   feed:<url>           a show's episodes, newest first, from its RSS feed
//   apple:<id>           a charted show: its feed is looked up, then as above
//                        (Apple's episode list if the feed cannot be read)
//
// Episodes are long enough to count as audiobooks for the app, so they
// remember where they were stopped, show up under Continue watching and
// get a tick when finished, like videos do.

// Your own shows: RSS feed URLs. Edit to taste.
const FEEDS = [
    "https://feeds.simplecast.com/54nAGcIl",      // The Daily
    "https://feeds.simplecast.com/BqbsxVfO",      // 99% Invisible
    "https://feeds.npr.org/510289/podcast.xml"    // Planet Money
];

// Which country's chart "Top podcasts" shows (two-letter store code).
const COUNTRY = "us";
const CHART_SIZE = 50;
const MAX_EPISODES = 200;

// Long-running shows have feeds of 10 MB and more, every episode since the
// first. Only the head of a feed is read (newest episodes come first), which
// keeps both the download and the plugin's 32 MiB heap in bounds.
const FEED_BYTES = 4 << 20;

const cache = {};
function cached(key, ttl_min, produce) {
    const hit = cache[key];
    if (hit && Date.now() - hit.time < ttl_min * 60 * 1000)
	return hit.value;
    const value = produce();
    cache[key] = { time: Date.now(), value: value };
    return value;
}

function getText(url, options) {
    const res = http.get(url, options || { timeout: 20 });
    if (!res.ok)
	throw new Error("HTTP " + res.status + " from " + url);
    return res.body;
}

function getJson(url) {
    return JSON.parse(getText(url));
}

// The first FEED_BYTES of a feed. Servers that honour Range send just that;
// for the rest, maxBytes makes the app cut the body short instead of
// refusing it. Uncompressed, so a byte range is a range of the text.
function getFeedText(url) {
    return getText(url, {
	timeout: 20,
	maxBytes: FEED_BYTES,
	headers: {
	    "Range": "bytes=0-" + (FEED_BYTES - 1),
	    "Accept-Encoding": "identity"
	}
    });
}

// --- A little RSS reading ------------------------------------------------

const NAMED = {
    amp: "&", lt: "<", gt: ">", quot: '"', apos: "'", nbsp: " ",
    lsquo: "\u2018", rsquo: "\u2019", ldquo: "\u201c", rdquo: "\u201d",
    ndash: "\u2013", mdash: "\u2014", hellip: "\u2026", bull: "\u2022",
    middot: "\u00b7", copy: "\u00a9", reg: "\u00ae", trade: "\u2122"
};

function codePoint(n) {
    return n > 0 && n <= 0x10ffff && !(n >= 0xd800 && n <= 0xdfff)
	? String.fromCodePoint(n) : "\ufffd";
}

// One pass, so "&amp;lt;" stays "&lt;".
function decodeEntities(s) {
    return s.replace(/&(#x[0-9a-f]+|#\d+|[a-z]+\d*);/gi, (m, e) => {
	if (e[0] === "#")
	    return codePoint(e[1] === "x" || e[1] === "X"
			     ? parseInt(e.slice(2), 16) : Number(e.slice(1)));
	const v = NAMED[e.toLowerCase()];
	return v === undefined ? m : v;
    });
}

const stripTags = s => s.replace(/<[^>]*>/g, " ");
const unwrapCdata = s => s.replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, "$1");

// Markup, entities and HTML that was escaped into the text (common in
// <itunes:summary>) all removed: plain, single-spaced text.
function plainText(s) {
    return stripTags(decodeEntities(stripTags(unwrapCdata(s))))
	.replace(/\s+/g, " ").trim();
}

const tagRes = {};
function tagRe(tag) {
    return tagRes[tag] || (tagRes[tag] =
	new RegExp("<" + tag + "(?:\\s[^>]*)?>([\\s\\S]*?)</" + tag + ">", "i"));
}

// Text of the first <tag> in 'xml', or "".
function tagText(xml, tag) {
    const m = tagRe(tag).exec(xml);
    return m ? plainText(m[1]) : "";
}

const attrRes = {};
// Value of 'attr' on the first <tag ...> in 'xml', or "".
function tagAttr(xml, tag, attr) {
    const key = tag + " " + attr;
    const re = attrRes[key] || (attrRes[key] =
	new RegExp("<" + tag + "\\s[^>]*?\\b" + attr + "\\s*=\\s*([\"'])(.*?)\\1", "i"));
    const m = re.exec(xml);
    return m ? decodeEntities(m[2]).trim() : "";
}

const MONTHS = { jan: 0, feb: 1, mar: 2, apr: 3, may: 4, jun: 5,
		 jul: 6, aug: 7, sep: 8, oct: 9, nov: 10, dec: 11 };

// RFC 822 dates ("Tue, 10 Jun 2025 04:00:00 +0000") -> ms since the epoch,
// by hand: not every JavaScript engine's Date.parse takes them.
function parseDate(s) {
    const m = /(\d{1,2})\s+([A-Za-z]{3})[a-z]*\.?\s+(\d{4})\s+(\d{1,2}):(\d{2})(?::(\d{2}))?\s*([+-]\d{4}|[A-Z]{1,4})?/.exec(s || "");
    if (!m || !(m[2].toLowerCase() in MONTHS))
	return 0;
    let t = Date.UTC(+m[3], MONTHS[m[2].toLowerCase()], +m[1], +m[4], +m[5], +(m[6] || 0));
    const zone = m[7] || "";
    if (/^[+-]\d{4}$/.test(zone)) {
	const minutes = (+zone.slice(1, 3)) * 60 + (+zone.slice(3, 5));
	t -= (zone[0] === "-" ? -1 : 1) * minutes * 60 * 1000;
    }
    return t;
}

// "1:02:03", "62:03" or "3723" -> seconds.
function parseDuration(s) {
    if (!s)
	return 0;
    const parts = String(s).trim().split(":").map(Number);
    if (parts.some(isNaN))
	return 0;
    return Math.round(parts.reduce((total, p) => total * 60 + p, 0));
}

function clip(s, n) {
    return s.length > n ? s.slice(0, n - 3) + "..." : s;
}

// Index of the next "<item>" or "<item ...>" at or after 'from', or -1.
// indexOf rather than a regex over the whole feed: it allocates nothing.
function nextItem(xml, from) {
    for (;;) {
	const i = xml.indexOf("<item", from);
	if (i < 0)
	    return -1;
	const c = xml[i + 5];
	if (c === ">" || c === " " || c === "\n" || c === "\r" || c === "\t")
	    return i;
	from = i + 5;
    }
}

function parseEpisode(item, showImage) {
    const uri = tagAttr(item, "enclosure", "url");
    if (!uri)
	return null;
    const type = tagAttr(item, "enclosure", "type");
    const length = Number(tagAttr(item, "enclosure", "length"));
    const summary = tagText(item, "itunes:summary") ||
	  tagText(item, "description") || tagText(item, "content:encoded");
    return {
	id: tagText(item, "guid") || uri,
	type: /^video\//i.test(type) ? "video" : "audio",
	name: tagText(item, "title") || "(untitled episode)",
	description: clip(summary, 600),
	image: tagAttr(item, "itunes:image", "href") || showImage,
	uri: uri,
	date: parseDate(tagText(item, "pubDate")),
	duration: parseDuration(tagText(item, "itunes:duration")),
	size: length > 0 ? length : -1
    };
}

function parseFeed(xml) {
    let pos = nextItem(xml, 0);
    const head = pos < 0 ? xml : xml.slice(0, pos);
    // iTunes artwork, else the plain RSS <image><url>.
    const rssImage = /<image>[\s\S]*?<\/image>/i.exec(head);
    // The channel's own <title>, not the one inside <image>.
    const show = {
	title: tagText(head.replace(/<image>[\s\S]*?<\/image>/gi, ""), "title"),
	image: tagAttr(head, "itunes:image", "href") ||
	    (rssImage ? tagText(rssImage[0], "url") : ""),
	episodes: []
    };
    // A feed cut short ends mid-item; that last item has no </item> and is
    // left out.
    while (pos >= 0 && show.episodes.length < MAX_EPISODES) {
	const end = xml.indexOf("</item>", pos);
	if (end < 0)
	    break;
	const ep = parseEpisode(xml.slice(pos, end), show.image);
	if (ep)
	    show.episodes.push(ep);
	pos = nextItem(xml, end + 7);
    }
    if (!show.episodes.length && !/<rss[\s>]/i.test(head))
	throw new Error("not a podcast feed");
    // Most feeds are newest first already; serial shows are not.
    show.episodes.sort((a, b) => b.date - a.date);
    return show;
}

function readFeed(url) {
    return cached("feed:" + url, 15, () => parseFeed(getFeedText(url)));
}

function showFolder(id, name, description, image) {
    return { id: id, type: "folder", name: name, description: description || "", image: image || "" };
}

function browseMine() {
    return FEEDS.map(url => {
	try {
	    const show = readFeed(url);
	    return showFolder("feed:" + url, show.title || url,
			      show.episodes.length + " episodes", show.image);
	} catch (e) {
	    // Still listed: opening it tries again and shows the error.
	    console.warn("podcasts: " + url + ": " + e.message);
	    return showFolder("feed:" + url, url, "Could not load: " + e.message);
	}
    });
}

function browseTop() {
    return cached("top", 60, () => {
	const data = getJson("https://rss.applemarketingtools.com/api/v2/" + COUNTRY +
			     "/podcasts/top/" + CHART_SIZE + "/podcasts.json");
	const results = (data.feed && data.feed.results) || [];
	if (!results.length)
	    throw new Error("the Apple Podcasts chart came back empty");
	return results.map((r, i) => showFolder(
	    "apple:" + r.id,
	    r.name || "(untitled show)",
	    "#" + (i + 1) + (r.artistName ? "  ·  " + r.artistName : ""),
	    // The chart only has small art; the same image comes larger by
	    // asking for it.
	    String(r.artworkUrl100 || "").replace(/\/\d+x\d+bb\./, "/600x600bb.")));
    });
}

function feedOfApple(id) {
    return cached("apple:" + id, 24 * 60, () => {
	const data = getJson("https://itunes.apple.com/lookup?id=" + encodeURIComponent(id));
	const hit = (data.results || []).find(r => r.feedUrl);
	return hit ? hit.feedUrl : "";
    });
}

// Apple's own episode list for a show, for when its feed is unusable.
function appleEpisodes(id) {
    return cached("apple-episodes:" + id, 15, () => {
	const data = getJson("https://itunes.apple.com/lookup?id=" + encodeURIComponent(id) +
			     "&entity=podcastEpisode&limit=" + MAX_EPISODES);
	return (data.results || [])
	    .filter(r => r.wrapperType === "podcastEpisode" && r.episodeUrl)
	    .map(r => ({
		id: r.episodeGuid || r.episodeUrl,
		type: /video/i.test(r.episodeContentType || "") ? "video" : "audio",
		name: r.trackName || "(untitled episode)",
		description: clip(plainText(r.description || r.shortDescription || ""), 600),
		image: r.artworkUrl600 || r.artworkUrl160 || "",
		uri: r.episodeUrl,
		date: r.releaseDate ? Date.parse(r.releaseDate) || 0 : 0,
		duration: r.trackTimeMillis > 0 ? Math.round(r.trackTimeMillis / 1000) : 0
	    }));
    });
}

function browseApple(id) {
    let reason = "this show has no public feed";
    const url = feedOfApple(id);
    if (url) {
	try {
	    return readFeed(url).episodes;
	} catch (e) {
	    reason = e.message;
	    console.warn("podcasts: " + url + ": " + reason + "; trying Apple's episode list");
	}
    }
    const episodes = appleEpisodes(id);
    if (!episodes.length)
	throw new Error(reason);
    return episodes;
}

return {
    name: "Podcasts",
    discover() {
	return [{
	    name: "Podcasts",
	    detail: "Top charts and your own feeds",
	    icon: "🎙",
	    root: "/",
	    browse(id) {
		if (id === "/") {
		    return [
			showFolder("mine", "My podcasts", FEEDS.length + " shows (edit plugins/podcasts.js)"),
			showFolder("top", "Top podcasts", "The Apple Podcasts chart")
		    ];
		}
		if (id === "mine")
		    return browseMine();
		if (id === "top")
		    return browseTop();
		if (id.startsWith("feed:"))
		    return readFeed(id.slice(5)).episodes;
		if (id.startsWith("apple:"))
		    return browseApple(id.slice(6));
		throw new Error("unknown folder " + id);
	    }
	}];
    }
};
