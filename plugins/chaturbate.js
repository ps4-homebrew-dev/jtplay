// SPDX-License-Identifier: GPL-3.0-or-later

const API_DISCOVER = "https://chaturbate.com/api/ts/discover/carousels/";
const API_ROOMS = "https://chaturbate.com/api/ts/roomlist/room-list/";
const API_TAGS = "https://chaturbate.com/api/ts/hashtags/tag-table-data/";
const API_MODEL = "https://chaturbate.com/api/chatvideocontext/";


function getURLSearchParams(params) {
    return Object.entries(params).map(([k, v]) =>
        `${encodeURIComponent(k)}=${encodeURIComponent(v)}`)
	.join("&");
}

function fetchRooms(params={}) {
    const res = http.get(API_ROOMS + '?' + getURLSearchParams(params));
    if (!res.ok)
	throw new Error(res.status);

    const json = JSON.parse(res.body);
    const rooms = json.rooms || [];
    return rooms.map((c) => ({
	id: c.username,
	type: "video",
	name: c.username,
	image: c.img,
	description: c.room_subject
    }));
}


function fetchDiscover(category, params={}) {
    const res = http.get(API_DISCOVER + category + '/?' + getURLSearchParams(params));
    if (!res.ok)
	throw new Error(res.status);

    const json = JSON.parse(res.body);
    const rooms = json.rooms || [];
    return rooms.map((c) => ({
	id: c.room,
	type: "video",
	name: c.room,
	image: c.img,
	description: c.room_subject
    }));
}

function fetchTags(params={}) {
    const res = http.get(API_TAGS + '?' + getURLSearchParams(params));
    if (!res.ok)
	throw new Error(res.status);

    const json = JSON.parse(res.body);
    const tags = json.tags || [];
    return tags.map((t) => ({
	id: "/tag/" + t.hashtag,
	type: "folder",
	name: t.hashtag
    }));
}

function absUrl(base, ref) {
    if (/^https?:\/\//.test(ref))
	return ref;
    if (ref.startsWith("/"))
	return base.match(/^https?:\/\/[^\/]+/)[0] + ref;
    return base.split("?")[0].replace(/[^\/]*$/, "") + ref;
}

// Attribute value from an #EXT-X-... tag line, quotes stripped.
function attr(line, name) {
    const m = line.match(new RegExp('[:,]' + name + '=("[^"]*"|[^,]*)'));
    return m ? m[1].replace(/^"|"$/g, "") : null;
}

// An mpv EDL URL that plays 'video' and 'audio' as one file, the way mpv's
// own ytdl hook merges separate audio and video streams.
function mergeStreams(video, audio) {
    const part = (u) => "!new_stream;!no_clip;!no_chapters;%" + u.length + "%" + u;
    return "edl://" + part(video) + ";" + part(audio);
}

function resolveStream(username) {
    const res = http.get(API_MODEL + username + '/');
    if (!res.ok)
	throw new Error(res.status);

    const master = JSON.parse(res.body).hls_source;
    if (!master)
	throw new Error("Room is offline");

    // mpv fetches the URL it is given twice, and the master playlist URL
    // only works once. Fetch it here and hand mpv the variant instead.
    const pl = http.get(master);
    if (!pl.ok)
	throw new Error(pl.status);

    const lines = pl.body.split(/\r?\n/);
    const audioGroups = {};  // GROUP-ID -> audio playlist url
    let best = null, bestBw = -1, bestAudio = null;
    for (let i = 0; i < lines.length; i++) {
	const line = lines[i];
	if (line.startsWith("#EXT-X-MEDIA:") && attr(line, "TYPE") == "AUDIO") {
	    const group = attr(line, "GROUP-ID"), uri = attr(line, "URI");
	    // Prefer the DEFAULT rendition of a group.
	    if (group && uri && (!(group in audioGroups) || attr(line, "DEFAULT") == "YES"))
		audioGroups[group] = absUrl(master, uri);
	    continue;
	}
	if (!line.startsWith("#EXT-X-STREAM-INF:"))
	    continue;
	let j = i + 1;
	while (j < lines.length && (!lines[j] || lines[j].startsWith("#")))
	    j++;
	const bw = parseInt(attr(line, "BANDWIDTH") || "0", 10);
	if (j < lines.length && bw > bestBw) {
	    bestBw = bw;
	    best = absUrl(master, lines[j].trim());
	    bestAudio = attr(line, "AUDIO");
	}
    }
    if (!best)
	return master;

    // The variant may carry video only, with the sound in a separate audio
    // playlist (#EXT-X-MEDIA); then both are handed to mpv together.
    const audio = bestAudio ? audioGroups[bestAudio] : null;
    return audio && audio != best ? mergeStreams(best, audio) : best;
}

return {
    name: "Chaturbate",

    discover() {
	return [{
	    name: "Chaturbate",
	    detail: "Pornographic live-streamed webcam performances",
	    icon: "💋",
	    root: "",

	    browse(id) {
		if(!id) {
		    return [{
			id: "/home",
			type: "folder",
			name: "Home"
		    }, {
			id: "/discover",
			type: "folder",
			name: "Discover"
		    }, {
			id: "/tags",
			type: "folder",
			name: "Tags"
		    }];
		}

		if(id == "/home") {
		    return fetchRooms({limit: 100})
		}
		if(id == "/discover") {
		    return [
			{
			    id: "/discover/most_popular",
			    type: "folder",
			    name: "Most popular"			    
			},
			{
			    id: "/discover/new_cams",
			    type: "folder",
			    name: "New cams"
			},
			{
			    id: "/discover/smaller_communities",
			    type: "folder",
			    name: "Smaller communities"
			},
			{
			    id: "/discover/most_viewed_in_region",
			    type: "folder",
			    name: "Most viewed in region"
			},
			{
			    id: "/discover/top-rated",
			    type: "folder",
			    name: "Top-rated"
			},
			{
			    id: "/discover/recently_started",
			    type: "folder",
			    name: "Recently started"
			}
		    ];
		}
		if(id == "/tags") {
		    return fetchTags({limit: 100});
		}
		
		if(id.startsWith("/tag/")) {
		    return fetchRooms({limit: 100, hashtags: id.substring(5)})
		}
		if(id.startsWith("/discover/")) {
		    return fetchDiscover(id.substring(10), {limit: 100})
		}
		
	    },
	    resolve(id) {
		return resolveStream(id);
	    }    
	}];
    }
}
