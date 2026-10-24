// SPDX-License-Identifier: GPL-3.0-or-later
//
// SVT Play, the video service of Swedish public service television.
//
// Listings come from SVT's Contento GraphQL endpoint, which only accepts
// *persisted* queries: the client sends the sha256 of a query document the
// server already knows, never the document itself. The hashes below are the
// ones svtplay.se uses; when SVT retires one the API answers with
// "PersistedQueryNotFound" and that listing stops working until the hash is
// refreshed. Playback goes through the separate video API, which mints
// short-lived manifest URLs, hence resolve() rather than a uri per entry.

const VIDEO_API = "https://api.svt.se/video/";
const QUERY_API = "https://api.svt.se/contento/graphql";
const IMAGE_API = "https://www.svtstatic.se/image";
const CLIENT_UA = "svtplaywebb-play-render-prod-client";

// [operationName, sha256 of the query document]. Several listings share an
// operation name with different documents, so the hash travels with the call
// instead of being looked up by name.
const OP = {
    CHANNELS:  ["ChannelsQuery",
		"210be4b72f03223b990f031d9a2e3501ff9284f8d2c66b01b255a807775f0b19"],
    PROGRAMS:  ["ProgramsListing",
		"17252e11da632f5c0d1b924b32be9191f6854723a0f50fb2adb35f72bb670efa"],
    GENRES:    ["MainGenres",
		"65b3d9bccd1adf175d2ad6b1aaa482bb36f382f7bad6c555750f33322bc2b489"],
    CATEGORY:  ["CategoryPageQuery",
		"00be06320342614f4b186e9c7710c29a7fc235a1936bde08a6ab0f427131bfaf"],
    GRID:      ["GridPage",
		"a8248fc130da34208aba94c4d5cc7bd44187b5f36476d8d05e03724321aafb40"],
    GRID_LIVE: ["GridPage",
		"1e2d15ff7ffa578d33ebf1287d3f7af7fd47125552b564e96fd277a744345a69"],
    DETAILS:   ["DetailsPageQuery",
		"e240d515657bbb54f33cf158cea581f6303b8f01f3022ea3f9419fbe3a5614b0"],
    VIDEO_ID:  ["DetailsPageQuery",
		"5be42eb4028ed8f2680ce2302f6887df3fed2dcb6f61ac091ff5a37a3d0bf477"]
};

// The fixed rows of the start page: [id, name, description, selectionId].
const START_ROWS = [
    ["/live",       "Live nu",       "Sänds just nu",      "live_start"],
    ["/popular",    "Populärt",      "Mest sedda",         "popular_start"],
    ["/latest",     "Senaste",       "Nyss tillagt",       "latest_start"],
    ["/lastchance", "Sista chansen", "Försvinner snart",   "lastchance_start"]
];

// __typename of the objects the API hands out. Anything else is dropped
// rather than guessed at, so a new content type shows up as a missing row in
// the log instead of an unplayable entry.
const SHOW_TYPES = ["TvShow", "KidsTvShow", "TvSeries"];
const VIDEO_TYPES = ["Episode", "Clip", "Single", "Trailer", "Variant"];

// Subheadings that only repeat what the row already says ("Ikväll", "42 min")
// and are not worth appending to a title.
const NOISE = /^(Idag|Ikväll|Igår|I morgon)\b|\b(sek|min|tim)$/i;

// Video formats worth handing to ffmpeg, best first. HLS is listed above
// DASH because ffmpeg's hls demuxer copes with SVT's manifests better than
// its dash one; the "-full" variants carry 5.1 audio and HEVC.
const FORMATS = [
    "hls", "hls-ts-avc", "hls-cmaf-full", "hls-cmaf-live", "hls-cmaf",
    "hls-ts-avc-51", "hls-ts-full",
    "dash", "dash-avc-51", "dash-full", "dash-hbbtv-avc", "dashhbbtv"
];

// Listings are browsed back and forth (a show, back to the letter, the next
// show), and A-O is one 1500 item response, so keep answers around briefly.
const CACHE_TTL_MS = 10 * 60 * 1000;
const cache = {};

function cached(key, produce) {
    const hit = cache[key];
    const now = Date.now();
    if (hit && now - hit.time < CACHE_TTL_MS) {
	return hit.value;
    }
    const value = produce();
    cache[key] = {time: now, value: value};
    return value;
}

function query(op, variables) {
    const extensions = {
	persistedQuery: {version: 1, sha256Hash: op[1]}
    };
    const url = QUERY_API +
	  "?ua=" + CLIENT_UA +
	  "&operationName=" + op[0] +
	  "&variables=" + encodeURIComponent(JSON.stringify(variables || {})) +
	  "&extensions=" + encodeURIComponent(JSON.stringify(extensions));

    const res = http.get(url, {headers: {Accept: "application/json"}});
    if (!res.ok) {
	throw new Error(op[0] + ": HTTP " + res.status);
    }

    const json = JSON.parse(res.body);
    if (json.errors && json.errors.length) {
	// A stale hash lands here as "PersistedQueryNotFound".
	throw new Error(op[0] + ": " + json.errors[0].message);
    }
    if (!json.data) {
	throw new Error(op[0] + ": no data in response");
    }
    return json.data;
}

// Descriptions and search headings arrive with markup in them now and then.
function text(s) {
    if (!s) {
	return "";
    }
    return String(s)
	.replace(/<[^>]*>/g, "")
	.replace(/&amp;/g, "&")
	.replace(/&quot;/g, '"')
	.replace(/&#39;/g, "'")
	.replace(/&lt;/g, "<")
	.replace(/&gt;/g, ">")
	.replace(/\s+/g, " ")
	.trim();
}

// Artwork is addressed by an id and the timestamp of its last change; the
// service scales on the fly, so ask for something a TV can use.
function imageUrl(img, width) {
    if (!img || !img.id) {
	return "";
    }
    return IMAGE_API + "/wide/" + (width || 720) + "/" + img.id + "/" +
	(img.changed || 0) + "?quality=70";
}

// Picks the best artwork an object carries, whatever shape it came in.
function pickImage(obj, fallback) {
    const images = obj && obj.images;
    if (images) {
	const img = images.wide || images.cleanWide || images.landscape;
	if (img) {
	    return imageUrl(img);
	}
    }
    if (obj && obj.image) {
	return imageUrl(obj.image);
    }
    return fallback || "";
}

// QuickJS is built without Intl, so localeCompare() is a code unit compare:
// "Zorro" would sort before "abc" and "Åsa" would land next to "A". Fold the
// case and push the three Swedish vowels past Z instead.
const COLLATE = {"Å": "Z1", "Ä": "Z2", "Ö": "Z3", "Æ": "Z2", "Ø": "Z3"};

function sortKey(name) {
    return name.toUpperCase().replace(/[ÅÄÖÆØ]/g, (c) => COLLATE[c]);
}

function isShow(typename) {
    return SHOW_TYPES.indexOf(typename) >= 0;
}

function isVideo(typename) {
    return VIDEO_TYPES.indexOf(typename) >= 0;
}

// The id of a playable item: an svt id when the listing carries one (the
// video API takes it directly), otherwise the svtplay path, which resolve()
// trades for an svt id when playback starts.
function videoId(item) {
    const svtId = item.videoSvtId || item.svtId;
    if (svtId) {
	return "video:" + svtId;
    }
    const path = item.urls && item.urls.svtplay;
    return path ? "path:" + path : "";
}

// Converts one Teaser (or a bare item, as the search and A-O listings hand
// out) into an entry. Returns null for anything unplayable or unbrowsable.
function toEntry(node, parentImage) {
    if (!node) {
	return null;
    }

    // A Teaser wraps the real object and adds the display strings the site
    // shows for it; prefer those, they are the ones with episode numbers.
    const teaser = node.__typename === "Teaser" ? node : null;
    const item = teaser ? teaser.item : (node.item || node);
    if (!item) {
	return null;
    }

    const typename = item.__typename;
    let name = text(teaser ? teaser.heading : "") || text(item.name);
    if (!name) {
	return null;
    }

    // Episodes are listed under their show, so the heading alone ("Avsnitt
    // 3") is ambiguous once it turns up in a genre or a live row.
    const sub = teaser ? text(teaser.subHeading) : "";
    if (sub && sub !== name && !NOISE.test(sub)) {
	name = name + " - " + sub;
    }

    const description =
	  text(teaser ? teaser.description : "") ||
	  text(item.longDescription) ||
	  text(item.description);
    const image = pickImage(teaser, "") || pickImage(item, parentImage);

    if (isShow(typename)) {
	const path = item.urls && item.urls.svtplay;
	if (!path) {
	    return null;
	}
	return {
	    id: "show:" + path,
	    type: "folder",
	    name: name,
	    description: description,
	    image: image
	};
    }

    if (isVideo(typename)) {
	const id = videoId(item);
	if (!id) {
	    return null;
	}
	return {
	    id: id,
	    type: "video",
	    name: name,
	    description: description,
	    image: image
	};
    }

    if (typename === "Genre") {
	return {
	    id: "genre:" + item.id,
	    type: "folder",
	    name: name,
	    description: description,
	    image: image
	};
    }

    console.log("skipping unsupported type: " + typename);
    return null;
}

function toEntries(nodes, parentImage) {
    const out = [];
    (nodes || []).forEach((node) => {
	const entry = toEntry(node, parentImage);
	if (entry) {
	    out.push(entry);
	}
    });
    return out;
}

function fetchChannels() {
    const res = query(OP.CHANNELS);
    const channels = (res.channels && res.channels.channels) || [];

    return channels.map((ch) => {
	const running = ch.running || {};
	return {
	    id: "video:" + ch.id,
	    type: "video",
	    name: ch.name,
	    description: running.name
		? text(running.name) + " - " + text(running.description)
		: "Live",
	    image: pickImage(running, "")
	};
    });
}

// One of the curated rows of the start page ("Populärt", "Live nu", ...).
function fetchSelection(selectionId) {
    const op = selectionId === "live_start" ? OP.GRID_LIVE : OP.GRID;
    const res = query(op, {
	selectionId: selectionId,
	includeFullOppetArkiv: true
    });
    const selection = res.selectionById || {};
    return toEntries(selection.items);
}

// The whole A-O catalogue in one request; the letters are our own grouping
// so that a folder holds tens of entries rather than a thousand.
function allPrograms() {
    return cached("programs", () => {
	const res = query(OP.PROGRAMS);
	const selections = (res.programAtillO && res.programAtillO.selections) || [];
	const entries = [];
	selections.forEach((selection) => {
	    toEntries(selection.items).forEach((entry) => entries.push(entry));
	});
	entries.sort((a, b) => {
	    const ka = sortKey(a.name);
	    const kb = sortKey(b.name);
	    return ka < kb ? -1 : (ka > kb ? 1 : 0);
	});
	return entries;
    });
}

// Everything that is not a Swedish letter goes into one bucket at the end,
// which is where digits and the odd punctuated title end up.
function programLetter(name) {
    const first = name.charAt(0).toUpperCase();
    return /^[A-ZÅÄÖ]$/.test(first) ? first : "#";
}

function fetchProgramLetters() {
    const seen = {};
    const letters = [];
    allPrograms().forEach((entry) => {
	const letter = programLetter(entry.name);
	if (!seen[letter]) {
	    seen[letter] = 0;
	    letters.push(letter);
	}
	seen[letter]++;
    });

    // "#" sorts before "A" by code point but belongs last.
    letters.sort((a, b) => {
	if (a === "#") return 1;
	if (b === "#") return -1;
	return sortKey(a) < sortKey(b) ? -1 : 1;
    });

    return letters.map((letter) => ({
	id: "programs:" + letter,
	type: "folder",
	name: letter === "#" ? "0-9 och övrigt" : letter,
	description: seen[letter] + " program"
    }));
}

function fetchProgramsByLetter(letter) {
    return allPrograms().filter((entry) => programLetter(entry.name) === letter);
}

function fetchGenres() {
    const res = query(OP.GENRES);
    const genres = (res.genresInMain && res.genresInMain.genres) || [];
    return genres.map((g) => ({
	id: "genre:" + g.id,
	type: "folder",
	name: g.name,
	description: "",
	image: pickImage(g, "")
    }));
}

function fetchGenre(id) {
    const res = query(OP.CATEGORY, {
	id: id,
	tab: "all",
	includeFullOppetArkiv: true
    });

    const page = res.categoryPage || {};
    const tabs = page.lazyLoadedTabs || [];
    let items = [];

    tabs.forEach((tab) => {
	(tab.selections || []).forEach((selection) => {
	    // Every tab carries a few themed rows; "all" is the full listing
	    // and the only one that is not a subset of it.
	    if (selection.selectionType === "all" || selection.id === "all") {
		items = items.concat(selection.items || []);
	    }
	});
    });

    // Older documents only expose the rows, in which case take them all and
    // let the id dedup below sort out the overlap.
    if (!items.length) {
	tabs.forEach((tab) => {
	    (tab.selections || []).forEach((selection) => {
		items = items.concat(selection.items || []);
	    });
	});
    }

    const seen = {};
    return toEntries(items).filter((entry) => {
	if (seen[entry.id]) {
	    return false;
	}
	seen[entry.id] = true;
	return true;
    });
}

// A show: its seasons, its clips, and whatever else SVT groups with it.
// 'folderId' picks one of those groups once the user has opened it.
function fetchShow(path, folderId) {
    const res = cached("show:" + path, () => query(OP.DETAILS, {
	path: path,
	includeFullOppetArkiv: true
    }));

    const page = res.detailsPageByPath;
    if (!page) {
	throw new Error("no such show: " + path);
    }

    const showImage = pickImage(page, "");
    const groups = (page.associatedContent || []).filter((group) => {
	// "upcoming" is unplayable and "related" is a different show.
	return group.id !== "upcoming" && group.id !== "related" &&
	    (group.items || []).length > 0;
    });

    if (folderId) {
	const group = groups.filter((g) => g.id === folderId)[0];
	if (!group) {
	    throw new Error("no such section: " + folderId);
	}
	return toEntries(group.items, showImage);
    }

    // One group is not worth a folder of its own; show the episodes.
    if (groups.length === 1) {
	return toEntries(groups[0].items, showImage);
    }

    return groups.map((group) => ({
	id: "show:" + path + "|" + group.id,
	type: "folder",
	name: text(group.name) || group.id.replace(/-/g, " "),
	description: group.items.length + " videor",
	image: showImage
    }));
}

// Trades an svtplay path for the svt id of the video on that page, for the
// listings that give a path and no id.
function svtIdForPath(path) {
    return cached("svtid:" + path, () => {
	const res = query(OP.VIDEO_ID, {path: path});
	const page = res.detailsPageByPath;
	const svtId = page && page.video && page.video.svtId;
	if (!svtId) {
	    throw new Error("no video on " + path);
	}
	return svtId;
    });
}

// videoReferences carry either a ready manifest url or a 'resolve' endpoint
// that redirects to one; the latter is what live channels use.
function referenceUrl(ref) {
    if (ref.resolve) {
	const res = http.get(ref.resolve, {headers: {Accept: "application/json"}});
	if (res.ok) {
	    const location = JSON.parse(res.body).location;
	    if (location) {
		return location;
	    }
	}
	console.warn("could not resolve " + ref.format + ", using its url");
    }
    return ref.url || "";
}

function resolveStream(svtId) {
    const res = http.get(VIDEO_API + svtId, {
	headers: {Accept: "application/json"},
	timeout: 20
    });
    if (!res.ok) {
	// 403 is what a geo blocked title abroad looks like.
	throw new Error(res.status === 403
			? "not available from here"
			: "video API: HTTP " + res.status);
    }

    const video = JSON.parse(res.body);
    let refs = video.videoReferences || [];
    if (!refs.length && video.variants && video.variants.default) {
	refs = video.variants.default.videoReferences || [];
    }
    if (!refs.length) {
	throw new Error("nothing to play in " + svtId);
    }

    const drm = video.rights && video.rights.drmCopyProtection;

    // Best supported format wins; DRM rules out the DASH streams, which are
    // the only ones SVT protects.
    let best = null;
    let bestRank = FORMATS.length;
    refs.forEach((ref) => {
	const format = String(ref.format || ref.playerType || "").toLowerCase();
	if (drm && format.indexOf("dash") === 0) {
	    return;
	}
	const rank = FORMATS.indexOf(format);
	if (rank >= 0 && rank < bestRank) {
	    best = ref;
	    bestRank = rank;
	}
    });

    if (!best) {
	// SVT renames formats now and then, so before giving up take anything
	// that looks like an HLS manifest.
	best = refs.filter((ref) => {
	    const format = String(ref.format || "").toLowerCase();
	    return String(ref.url || "").indexOf(".m3u8") > 0 &&
		!(drm && format.indexOf("dash") === 0);
	})[0];
    }

    if (!best) {
	throw new Error(drm
			? "copy protected"
			: "no supported stream format for " + svtId);
    }

    const url = referenceUrl(best);
    if (!url) {
	throw new Error("empty stream url for " + svtId);
    }
    console.log("playing " + best.format + ": " + url);
    return url;
}

return {
    name: "SVT Play",

    discover() {
	return [{
	    name: "SVT Play",
	    detail: "TV from Swedish public service",
	    icon: "📺",
	    root: "",

	    browse(id) {
		if (!id) {
		    const rows = START_ROWS.map((row) => ({
			id: row[0],
			type: "folder",
			name: row[1],
			description: row[2]
		    }));
		    return [{
			id: "channels",
			type: "folder",
			name: "Kanaler",
			description: "Direktsändning"
		    }].concat(rows, [{
			id: "programs",
			type: "folder",
			name: "Program A-Ö",
			description: "Hela utbudet"
		    }, {
			id: "genres",
			type: "folder",
			name: "Genrer",
			description: "Bläddra efter kategori"
		    }]);
		}

		if (id === "channels") {
		    return fetchChannels();
		}
		if (id === "programs") {
		    return fetchProgramLetters();
		}
		if (id === "genres") {
		    return fetchGenres();
		}

		const row = START_ROWS.filter((r) => r[0] === id)[0];
		if (row) {
		    return fetchSelection(row[3]);
		}

		if (id.indexOf("programs:") === 0) {
		    return fetchProgramsByLetter(id.substring("programs:".length));
		}

		if (id.indexOf("genre:") === 0) {
		    return fetchGenre(id.substring("genre:".length));
		}

		if (id.indexOf("show:") === 0) {
		    const rest = id.substring("show:".length);
		    const bar = rest.indexOf("|");
		    return bar < 0
			? fetchShow(rest)
			: fetchShow(rest.substring(0, bar), rest.substring(bar + 1));
		}

		console.warn("cannot browse " + id);
		return [];
	    },

	    resolve(id) {
		if (id.indexOf("video:") === 0) {
		    return resolveStream(id.substring("video:".length));
		}
		if (id.indexOf("path:") === 0) {
		    return resolveStream(svtIdForPath(id.substring("path:".length)));
		}
		return resolveStream(id);
	    }
	}];
    }
};
