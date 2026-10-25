// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internet radio from radio-browser.info, the free, community-run directory
// of some 50 000 stations. No account or key is needed.
//
// The tree:
//   /                      Most popular, Trending, By country, By genre
//   top:votes / top:clicks the 100 best voted / most listened stations
//   countries              countries with stations, most stations first
//   country:SE             that country's stations, most popular first
//   tags                   the 60 biggest genres
//   tag:jazz               that genre's stations
//
// Playback goes through resolve(), which asks the directory for the stream
// URL; that request is also how radio-browser counts listeners, which is
// what "Trending" is made of.

// Any server of the cluster serves the whole directory; the first that
// answers is used for the rest of the session.
const SERVERS = [
    "https://de1.api.radio-browser.info",
    "https://de2.api.radio-browser.info",
    "https://fi1.api.radio-browser.info",
    "https://nl1.api.radio-browser.info",
    "https://at1.api.radio-browser.info"
];
// radio-browser asks clients to identify themselves.
const HEADERS = { "User-Agent": "jtplay/1.0", Accept: "application/json" };
const LIMIT = 200;

let server = null;

function api(path) {
    const order = server ? [server].concat(SERVERS.filter(s => s !== server)) : SERVERS;
    let last = null;
    for (const base of order) {
	try {
	    const res = http.get(base + path, { headers: HEADERS, timeout: 15 });
	    if (res.ok) {
		server = base;
		return JSON.parse(res.body);
	    }
	    last = new Error("HTTP " + res.status);
	} catch (e) {
	    last = e;
	}
    }
    throw last || new Error("radio-browser.info is unreachable");
}

// Listings are browsed back and forth; keep them for a few minutes.
const cache = {};
function cached(key, produce) {
    const hit = cache[key];
    if (hit && Date.now() - hit.time < 5 * 60 * 1000)
	return hit.value;
    const value = produce();
    cache[key] = { time: Date.now(), value: value };
    return value;
}

function folder(id, name, description, image) {
    return { id: id, type: "folder", name: name, description: description || "", image: image || "" };
}

// "rock, classic rock,80s" -> "Rock · Classic rock · 80s"
function prettyTags(tags) {
    return String(tags || "").split(",").map(t => t.trim()).filter(t => t)
	.slice(0, 4).map(t => t.charAt(0).toUpperCase() + t.slice(1)).join(" · ");
}

function station(s) {
    const facts = [];
    if (s.country)
	facts.push(s.country);
    const tags = prettyTags(s.tags);
    if (tags)
	facts.push(tags);
    if (s.codec)
	facts.push(s.codec + (s.bitrate ? " " + s.bitrate + " kbps" : ""));
    return {
	id: s.stationuuid,
	type: "audio",
	name: String(s.name || "").trim() || "(unnamed station)",
	description: facts.join("  ·  "),
	image: s.favicon || "",
	uri: s.url_resolved || s.url
    };
}

function stations(path) {
    const sep = path.indexOf("?") < 0 ? "?" : "&";
    return cached(path, () =>
	api(path + sep + "hidebroken=true&limit=" + LIMIT).map(station));
}

function browseRoot() {
    return [
	folder("top:votes", "Most popular", "The best voted stations everywhere"),
	folder("top:clicks", "Trending", "What people are listening to right now"),
	folder("countries", "By country", "Stations from every country"),
	folder("tags", "By genre", "Jazz, news, classical, ...")
    ];
}

function browseCountries() {
    return cached("countries", () =>
	api("/json/countries?order=stationcount&reverse=true&hidebroken=true")
	    .filter(c => c.stationcount >= 5 && c.iso_3166_1)
	    .map(c => folder("country:" + c.iso_3166_1,
			     c.name,
			     c.stationcount + " stations")));
}

function browseTags() {
    return cached("tags", () =>
	api("/json/tags?order=stationcount&reverse=true&hidebroken=true&limit=60")
	    .filter(t => t.name)
	    .map(t => folder("tag:" + t.name,
			     t.name.charAt(0).toUpperCase() + t.name.slice(1),
			     t.stationcount + " stations")));
}

return {
    name: "Radio",
    discover() {
	return [{
	    name: "Internet radio",
	    detail: "50 000 stations from radio-browser.info",
	    icon: "📻",
	    root: "/",
	    browse(id) {
		if (id === "/")
		    return browseRoot();
		if (id === "top:votes")
		    return stations("/json/stations/topvote/" + LIMIT);
		if (id === "top:clicks")
		    return stations("/json/stations/topclick/" + LIMIT);
		if (id === "countries")
		    return browseCountries();
		if (id === "tags")
		    return browseTags();
		if (id.startsWith("country:"))
		    return stations("/json/stations/bycountrycodeexact/" +
				    encodeURIComponent(id.slice(8)) + "?order=votes&reverse=true");
		if (id.startsWith("tag:"))
		    return stations("/json/stations/bytagexact/" +
				    encodeURIComponent(id.slice(4)) + "?order=votes&reverse=true");
		throw new Error("unknown folder " + id);
	    },
	    // Counts the listen and hands back the current stream URL; the one
	    // from the listing is the fallback if the directory is down.
	    resolve(id, entry) {
		try {
		    const r = api("/json/url/" + encodeURIComponent(id));
		    if (r && r.url)
			return r.url;
		} catch (e) {
		    console.warn("radio: " + e.message);
		}
		if (entry && entry.uri)
		    return entry.uri;
		throw new Error("this station has no stream");
	    }
	}];
    }
};
