// SPDX-License-Identifier: GPL-3.0-or-later
//
// Podcasts: the Apple Podcasts charts, overall, by country and by genre,
// laid out like the radio plugin.
//
// The tree:
//   /                    Most popular, By country, By genre
//   top                  the top CHART_SIZE shows of DEFAULT_COUNTRY
//   countries            every country Apple Podcasts is in, by name
//   chart:<cc>           the top CHART_SIZE shows of country <cc>
//   genres               Apple's podcast genres, by name
//   genre:<id>           the top CHART_SIZE shows of genre <id>, in
//                        DEFAULT_COUNTRY (Apple charts genres per store)
//   apple:<cc>:<id>      a show in <cc>'s store: its feed is looked up, then
//                        its episodes, newest first, from the feed (Apple's
//                        episode list if the feed cannot be read)
//   feed:<url>           a show's episodes straight from its RSS feed
//
// A show is looked up in the store of the country it was found in: some are
// only in that store. "apple:<id>" (a show in the US store) is what earlier
// versions of this plugin made, and is still understood, so favorites made
// with them keep working.
//
// Apple has no worldwide chart, so Most popular is the chart of
// DEFAULT_COUNTRY; and nothing like radio-browser's listener counts, so
// there is no Trending.
//
// Shows you follow are kept with jtplay's own favorites ("Add to favorites"
// in the options menu, on any folder), so there is no list of feeds to edit
// here. "feed:" folders are still understood, so favorites made with an
// earlier version of this plugin keep working.
//
// Search (options menu) asks Apple for shows in the whole Podcasts
// catalogue, not just the chart, and lists them like the chart does, so any
// show can be opened and added to the favorites. It searches the store of
// the country it is asked from (inside a country's chart or one of its
// shows), the US store from anywhere else. Narrowing what is
// listed, like the episodes of a show, is the app's own Filter.
//
// Episodes are long enough to count as audiobooks for the app, so they
// remember where they were stopped, show up under Continue watching and
// get a tick when finished, like videos do.

// The store a show or a search is in when nothing says otherwise.
const DEFAULT_COUNTRY = "us";
// Shows in a country's chart.
const CHART_SIZE = 50;

// Where Apple Podcasts is available (support.apple.com/HT5085), as [store
// code, name], by name. QuickJS has no Intl to name a code, hence the names.
const COUNTRIES = [
    ["af", "Afghanistan"], ["al", "Albania"], ["dz", "Algeria"],
    ["ao", "Angola"], ["ai", "Anguilla"], ["ag", "Antigua and Barbuda"],
    ["ar", "Argentina"], ["am", "Armenia"], ["au", "Australia"],
    ["at", "Austria"], ["az", "Azerbaijan"], ["bs", "Bahamas"],
    ["bh", "Bahrain"], ["bb", "Barbados"], ["by", "Belarus"],
    ["be", "Belgium"], ["bz", "Belize"], ["bj", "Benin"], ["bm", "Bermuda"],
    ["bt", "Bhutan"], ["bo", "Bolivia"], ["ba", "Bosnia and Herzegovina"],
    ["bw", "Botswana"], ["br", "Brazil"], ["vg", "British Virgin Islands"],
    ["bn", "Brunei"], ["bg", "Bulgaria"], ["bf", "Burkina Faso"],
    ["kh", "Cambodia"], ["cm", "Cameroon"], ["ca", "Canada"],
    ["cv", "Cape Verde"], ["ky", "Cayman Islands"], ["td", "Chad"],
    ["cl", "Chile"], ["cn", "China mainland"], ["co", "Colombia"],
    ["cd", "Congo, Democratic Republic of the"],
    ["cg", "Congo, Republic of the"], ["cr", "Costa Rica"],
    ["ci", "Côte d'Ivoire"], ["hr", "Croatia"], ["cy", "Cyprus"],
    ["cz", "Czechia"], ["dk", "Denmark"], ["dm", "Dominica"],
    ["do", "Dominican Republic"], ["ec", "Ecuador"], ["eg", "Egypt"],
    ["sv", "El Salvador"], ["ee", "Estonia"], ["sz", "Eswatini"],
    ["fj", "Fiji"], ["fi", "Finland"], ["fr", "France"], ["ga", "Gabon"],
    ["gm", "Gambia"], ["ge", "Georgia"], ["de", "Germany"], ["gh", "Ghana"],
    ["gr", "Greece"], ["gd", "Grenada"], ["gt", "Guatemala"],
    ["gw", "Guinea-Bissau"], ["gy", "Guyana"], ["hn", "Honduras"],
    ["hk", "Hong Kong"], ["hu", "Hungary"], ["is", "Iceland"],
    ["in", "India"], ["id", "Indonesia"], ["iq", "Iraq"], ["ie", "Ireland"],
    ["il", "Israel"], ["it", "Italy"], ["jm", "Jamaica"], ["jp", "Japan"],
    ["jo", "Jordan"], ["kz", "Kazakhstan"], ["ke", "Kenya"],
    ["xk", "Kosovo"], ["kw", "Kuwait"], ["kg", "Kyrgyzstan"], ["la", "Laos"],
    ["lv", "Latvia"], ["lb", "Lebanon"], ["lr", "Liberia"], ["ly", "Libya"],
    ["lt", "Lithuania"], ["lu", "Luxembourg"], ["mo", "Macao"],
    ["mg", "Madagascar"], ["mw", "Malawi"], ["my", "Malaysia"],
    ["mv", "Maldives"], ["ml", "Mali"], ["mt", "Malta"],
    ["mr", "Mauritania"], ["mu", "Mauritius"], ["mx", "Mexico"],
    ["fm", "Micronesia"], ["md", "Moldova"], ["mn", "Mongolia"],
    ["me", "Montenegro"], ["ms", "Montserrat"], ["ma", "Morocco"],
    ["mz", "Mozambique"], ["mm", "Myanmar"], ["na", "Namibia"],
    ["nr", "Nauru"], ["np", "Nepal"], ["nl", "Netherlands"],
    ["nz", "New Zealand"], ["ni", "Nicaragua"], ["ne", "Niger"],
    ["ng", "Nigeria"], ["mk", "North Macedonia"], ["no", "Norway"],
    ["om", "Oman"], ["pk", "Pakistan"], ["pw", "Palau"], ["pa", "Panama"],
    ["pg", "Papua New Guinea"], ["py", "Paraguay"], ["pe", "Peru"],
    ["ph", "Philippines"], ["pl", "Poland"], ["pt", "Portugal"],
    ["qa", "Qatar"], ["ro", "Romania"], ["ru", "Russia"], ["rw", "Rwanda"],
    ["st", "São Tomé and Príncipe"], ["sa", "Saudi Arabia"],
    ["sn", "Senegal"], ["rs", "Serbia"], ["sc", "Seychelles"],
    ["sl", "Sierra Leone"], ["sg", "Singapore"], ["sk", "Slovakia"],
    ["si", "Slovenia"], ["sb", "Solomon Islands"], ["za", "South Africa"],
    ["kr", "South Korea"], ["es", "Spain"], ["lk", "Sri Lanka"],
    ["kn", "St. Kitts and Nevis"], ["lc", "St. Lucia"],
    ["vc", "St. Vincent and the Grenadines"], ["sr", "Suriname"],
    ["se", "Sweden"], ["ch", "Switzerland"], ["tw", "Taiwan"],
    ["tj", "Tajikistan"], ["tz", "Tanzania"], ["th", "Thailand"],
    ["to", "Tonga"], ["tt", "Trinidad and Tobago"], ["tn", "Tunisia"],
    ["tr", "Türkiye"], ["tm", "Turkmenistan"], ["tc", "Turks and Caicos"],
    ["ug", "Uganda"], ["ua", "Ukraine"], ["ae", "United Arab Emirates"],
    ["gb", "United Kingdom"], ["us", "United States"], ["uy", "Uruguay"],
    ["uz", "Uzbekistan"], ["vu", "Vanuatu"], ["ve", "Venezuela"],
    ["vn", "Vietnam"], ["ye", "Yemen"], ["zm", "Zambia"], ["zw", "Zimbabwe"]
];

// Apple's top-level podcast genres, as [genre id, name], by name. Each has a
// chart of its own; the v2 chart feed has no genre filter, so these come
// from the older store feed (see browseGenre).
const GENRES = [
    ["1301", "Arts"], ["1321", "Business"], ["1303", "Comedy"],
    ["1304", "Education"], ["1483", "Fiction"], ["1511", "Government"],
    ["1512", "Health & Fitness"], ["1487", "History"],
    ["1305", "Kids & Family"], ["1502", "Leisure"], ["1310", "Music"],
    ["1489", "News"], ["1314", "Religion & Spirituality"],
    ["1533", "Science"], ["1324", "Society & Culture"], ["1545", "Sports"],
    ["1318", "Technology"], ["1488", "True Crime"], ["1309", "TV & Film"]
];

function countryName(cc) {
    const hit = COUNTRIES.find(c => c[0] === cc);
    return hit ? hit[1] : cc.toUpperCase();
}
const MAX_EPISODES = 200;
// Shows a search lists, at most (Apple's search serves up to 200).
const SEARCH_LIMIT = 200;

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

function browseRoot() {
    return [
	showFolder("top", "Most popular",
		   "Top " + CHART_SIZE + " shows in " + countryName(DEFAULT_COUNTRY)),
	showFolder("countries", "By country",
		   "Top " + CHART_SIZE + " shows of " + COUNTRIES.length + " countries"),
	showFolder("genres", "By genre",
		   "Top " + CHART_SIZE + " shows of " + GENRES.length + " genres")
    ];
}

function browseGenres() {
    return GENRES.map(g => showFolder("genre:" + g[0], g[1], "Top " + CHART_SIZE + " shows"));
}

// The chart's small art comes larger by asking for it.
function largeArt(url) {
    return String(url || "").replace(/\/\d+x\d+bb\./, "/600x600bb.");
}

// A genre's chart, from the store feed (the v2 feed has no genre filter).
// Its rows are { id: { attributes: { "im:id" } }, "im:name": { label },
// "im:artist": { label }, "im:image": [{ label }, ...] } -- and a single row
// rather than a list when the chart has only one.
function browseGenre(genre) {
    const cc = DEFAULT_COUNTRY;
    return cached("genre:" + cc + ":" + genre, 60, () => {
	const res = http.get("https://itunes.apple.com/" + cc + "/rss/toppodcasts/limit=" +
			     CHART_SIZE + "/genre=" + encodeURIComponent(genre) + "/json",
			     { timeout: 20 });
	if (!res.ok)
	    throw new Error("HTTP " + res.status + " from the Apple Podcasts genre chart");
	const feed = JSON.parse(res.body).feed || {};
	const rows = Array.isArray(feed.entry) ? feed.entry : (feed.entry ? [feed.entry] : []);
	const label = (v) => (v && v.label) || "";
	const out = [];
	rows.forEach(r => {
	    const id = r && r.id && r.id.attributes && r.id.attributes["im:id"];
	    if (!id)
		return;
	    const images = r["im:image"] || [];
	    const artist = label(r["im:artist"]);
	    out.push(showFolder(
		"apple:" + cc + ":" + id,
		label(r["im:name"]) || "(untitled show)",
		"#" + (out.length + 1) + (artist ? "  ·  " + artist : ""),
		largeArt(label(images[images.length - 1]))));
	});
	if (!out.length)
	    throw new Error("the chart for this genre came back empty");
	return out;
    });
}

function browseCountries() {
    return COUNTRIES.map(c => showFolder("chart:" + c[0], c[1], "Top " + CHART_SIZE + " shows"));
}

// A country's chart. Apple has none for some of the smaller stores.
function browseChart(cc) {
    return cached("chart:" + cc, 60, () => {
	const res = http.get("https://rss.applemarketingtools.com/api/v2/" + cc +
			     "/podcasts/top/" + CHART_SIZE + "/podcasts.json", { timeout: 20 });
	if (!res.ok) {
	    console.warn("podcasts: chart of " + cc + ": HTTP " + res.status);
	    throw new Error(res.status === 404 || res.status === 500
			    ? "Apple has no podcast chart for " + countryName(cc)
			    : "HTTP " + res.status + " from the Apple Podcasts chart");
	}
	const data = JSON.parse(res.body);
	const results = (data.feed && data.feed.results) || [];
	if (!results.length)
	    throw new Error("the chart for " + countryName(cc) + " came back empty");
	return results.map((r, i) => showFolder(
	    "apple:" + cc + ":" + r.id,
	    r.name || "(untitled show)",
	    "#" + (i + 1) + (r.artistName ? "  ·  " + r.artistName : ""),
	    largeArt(r.artworkUrl100)));
    });
}

// Shows matching 'query' in all of Apple Podcasts in country <cc>'s store,
// as the chart lists them. Not cached: the app keeps the results it is
// showing, and a refresh should ask again.
function searchShows(query, cc) {
    const data = getJson("https://itunes.apple.com/search?media=podcast&entity=podcast" +
			 "&country=" + cc + "&limit=" + SEARCH_LIMIT +
			 "&term=" + encodeURIComponent(query));
    return (data.results || [])
	.filter(r => r.collectionId)
	.map(r => showFolder(
	    "apple:" + cc + ":" + r.collectionId,
	    r.collectionName || r.trackName || "(untitled show)",
	    [r.artistName, r.primaryGenreName].filter(s => s).join("  ·  "),
	    String(r.artworkUrl600 || r.artworkUrl100 || "")));
}

function feedOfApple(cc, id) {
    return cached("apple:" + cc + ":" + id, 24 * 60, () => {
	const data = getJson("https://itunes.apple.com/lookup?id=" + encodeURIComponent(id) +
			     "&country=" + cc);
	const hit = (data.results || []).find(r => r.feedUrl);
	return hit ? hit.feedUrl : "";
    });
}

// Apple's own episode list for a show, for when its feed is unusable.
function appleEpisodes(cc, id) {
    return cached("apple-episodes:" + cc + ":" + id, 15, () => {
	const data = getJson("https://itunes.apple.com/lookup?id=" + encodeURIComponent(id) +
			     "&country=" + cc + "&entity=podcastEpisode&limit=" + MAX_EPISODES);
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

// "apple:<cc>:<id>", or "apple:<id>" for a show in the default store (what
// earlier versions made): [cc, id].
function parseApple(rest) {
    const colon = rest.indexOf(":");
    return colon < 0 ? [DEFAULT_COUNTRY, rest]
		     : [rest.slice(0, colon), rest.slice(colon + 1)];
}

// The country a folder belongs to, for searching from it.
function countryOf(folderId) {
    const id = String(folderId || "");
    if (id.startsWith("chart:"))
	return id.slice(6);
    if (id.startsWith("apple:"))
	return parseApple(id.slice(6))[0];
    return DEFAULT_COUNTRY;
}

function browseApple(cc, id) {
    let reason = "this show has no public feed";
    const url = feedOfApple(cc, id);
    if (url) {
	try {
	    return readFeed(url).episodes;
	} catch (e) {
	    reason = e.message;
	    console.warn("podcasts: " + url + ": " + reason + "; trying Apple's episode list");
	}
    }
    const episodes = appleEpisodes(cc, id);
    if (!episodes.length)
	throw new Error(reason);
    return episodes;
}

return {
    name: "Podcasts",
    discover() {
	return [{
	    name: "Podcasts",
	    detail: "Apple Podcasts charts by country and genre",
	    icon: "🎙",
	    root: "/",
	    // In the store of the country of the folder on screen.
	    search(query, folderId) {
		return searchShows(query, countryOf(folderId));
	    },
	    browse(id) {
		if (id === "/")
		    return browseRoot();
		if (id === "top")
		    return browseChart(DEFAULT_COUNTRY);
		if (id === "countries")
		    return browseCountries();
		if (id === "genres")
		    return browseGenres();
		if (id.startsWith("chart:"))
		    return browseChart(id.slice(6));
		if (id.startsWith("genre:"))
		    return browseGenre(id.slice(6));
		if (id.startsWith("feed:"))
		    return readFeed(id.slice(5)).episodes;
		if (id.startsWith("apple:")) {
		    const show = parseApple(id.slice(6));
		    return browseApple(show[0], show[1]);
		}
		throw new Error("unknown folder " + id);
	    }
	}];
    }
};
