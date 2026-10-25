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

const cache = {};
function cached(key, ttl_min, produce) {
    const hit = cache[key];
    if (hit && Date.now() - hit.time < ttl_min * 60 * 1000)
	return hit.value;
    const value = produce();
    cache[key] = { time: Date.now(), value: value };
    return value;
}

function getText(url) {
    const res = http.get(url, { timeout: 20 });
    if (!res.ok)
	throw new Error("HTTP " + res.status + " from " + url);
    return res.body;
}

function getJson(url) {
    return JSON.parse(getText(url));
}

// --- A little RSS reading ------------------------------------------------

function decodeEntities(s) {
    return s.replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, "$1")
	.replace(/&#(\d+);/g, (m, n) => String.fromCodePoint(Number(n)))
	.replace(/&#x([0-9a-f]+);/gi, (m, n) => String.fromCodePoint(parseInt(n, 16)))
	.replace(/&quot;/g, '"').replace(/&apos;/g, "'")
	.replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&amp;/g, "&");
}

// Text of the first <tag> in 'xml' (CDATA unwrapped, markup and entities
// removed), or "".
function tagText(xml, tag) {
    const m = new RegExp("<" + tag + "(?:\\s[^>]*)?>([\\s\\S]*?)</" + tag + ">", "i").exec(xml);
    if (!m)
	return "";
    const inner = m[1].replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, "$1");
    return decodeEntities(inner.replace(/<[^>]*>/g, " ")).replace(/\s+/g, " ").trim();
}

// Value of 'attr' on the first <tag ...> in 'xml', or "".
function tagAttr(xml, tag, attr) {
    const m = new RegExp("<" + tag + "\\s[^>]*?\\b" + attr + "\\s*=\\s*([\"'])(.*?)\\1", "i").exec(xml);
    return m ? decodeEntities(m[2]) : "";
}

const MONTHS = { jan: 0, feb: 1, mar: 2, apr: 3, may: 4, jun: 5,
		 jul: 6, aug: 7, sep: 8, oct: 9, nov: 10, dec: 11 };

// RFC 822 dates ("Tue, 10 Jun 2025 04:00:00 +0000") -> ms since the epoch,
// by hand: not every JavaScript engine's Date.parse takes them.
function parseDate(s) {
    const m = /(\d{1,2})\s+([A-Za-z]{3})[a-z]*\s+(\d{4})\s+(\d{1,2}):(\d{2})(?::(\d{2}))?\s*([+-]\d{4}|[A-Z]{1,3})?/.exec(s || "");
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
    return parts.reduce((total, p) => total * 60 + p, 0);
}

function readFeed(url) {
    return cached("feed:" + url, 15, () => {
	const xml = getText(url);
	const head = xml.split(/<item[\s>]/i)[0];
	// iTunes artwork, else the plain RSS <image><url>.
	const rssImage = /<image>[\s\S]*?<\/image>/i.exec(head);
	const showImage = tagAttr(head, "itunes:image", "href") ||
	      (rssImage ? tagText(rssImage[0], "url") : "");
	const show = {
	    title: tagText(head, "title"),
	    image: showImage,
	    episodes: []
	};
	const items = xml.match(/<item[\s>][\s\S]*?<\/item>/gi) || [];
	for (const item of items.slice(0, MAX_EPISODES)) {
	    const uri = tagAttr(item, "enclosure", "url");
	    if (!uri)
		continue;
	    const type = tagAttr(item, "enclosure", "type");
	    const length = Number(tagAttr(item, "enclosure", "length"));
	    const summary = tagText(item, "itunes:summary") || tagText(item, "description");
	    show.episodes.push({
		id: tagText(item, "guid") || uri,
		type: /^video\//i.test(type) ? "video" : "audio",
		name: tagText(item, "title") || "(untitled episode)",
		description: summary.length > 600 ? summary.slice(0, 597) + "..." : summary,
		image: tagAttr(item, "itunes:image", "href") || show.image,
		uri: uri,
		date: parseDate(tagText(item, "pubDate")),
		duration: parseDuration(tagText(item, "itunes:duration")),
		size: length > 0 ? length : -1
	    });
	}
	return show;
    });
}

function showFolder(id, name, description, image) {
    return { id: id, type: "folder", name: name, description: description || "", image: image || "" };
}

function browseMine() {
    const out = [];
    for (const url of FEEDS) {
	try {
	    const show = readFeed(url);
	    out.push(showFolder("feed:" + url, show.title || url,
				show.episodes.length + " episodes", show.image));
	} catch (e) {
	    console.warn("podcasts: " + url + ": " + e.message);
	}
    }
    return out;
}

function browseTop() {
    return cached("top", 60, () => {
	const data = getJson("https://rss.applemarketingtools.com/api/v2/" + COUNTRY +
			     "/podcasts/top/" + CHART_SIZE + "/podcasts.json");
	const results = (data.feed && data.feed.results) || [];
	return results.map((r, i) => showFolder(
	    "apple:" + r.id,
	    r.name,
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
	if (!hit)
	    throw new Error("this show has no public feed");
	return hit.feedUrl;
    });
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
		    return readFeed(feedOfApple(id.slice(6))).episodes;
		throw new Error("unknown folder " + id);
	    }
	}];
    }
};
