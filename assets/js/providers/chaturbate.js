// SPDX-License-Identifier: GPL-3.0-or-later

export default function init(ctx) {
    const API_DISCOVER = "https://chaturbate.com/api/ts/discover/carousels/";
    const API_ROOMS = "https://chaturbate.com/api/ts/roomlist/room-list/";
    const API_TAGS = "https://chaturbate.com/api/ts/hashtags/tag-table-data/";
    const API_MODEL = "https://chaturbate.com/api/chatvideocontext/";


    function getURLSearchParams(params) {
	return Object.entries(params).map(([k, v]) =>
            `${encodeURIComponent(k)}=${encodeURIComponent(v)}`)
	    .join("&");
    }

    async function fetchProxy(url) {
	return await fetch('/http?url=' + encodeURI(url));
    }
    
    async function fetchRooms(params={}) {
	const res = await fetchProxy(API_ROOMS + '?' + getURLSearchParams(params));
	if (!res.ok)
	    throw new Error(res.status);

	const rooms = (await res.json()).rooms || [];
	return rooms.map((c) => ({
	    id: c.username,
	    type: "video",
	    name: c.username,
	    image: c.img,
	    description: c.room_subject
	}));
    }


    async function fetchDiscover(category, params={}) {
	const res = await fetchProxy(API_DISCOVER + category + '/?' + getURLSearchParams(params));
	if (!res.ok)
	    throw new Error(res.status);

	const rooms = (await res.json()).rooms || [];
	return rooms.map((c) => ({
	    id: c.room,
	    type: "video",
	    name: c.room,
	    image: c.img,
	    description: c.room_subject
	}));
    }

    async function fetchTags(params={}) {
	const res = await fetchProxy(API_TAGS + '?' + getURLSearchParams(params));
	if (!res.ok)
	    throw new Error(res.status);

	const tags = (await res.json()).hashtags || [];
	return tags.map((t) => ({
	    id: "/tag/" + t.hashtag,
	    type: "folder",
	    name: t.hashtag
	}));
    }

    async function resolveStream(username) {
	const res = await fetchProxy(API_MODEL + username + '/');
	if (!res.ok)
	    throw new Error(res.status);

	return (await res.json()).hls_source;
    }

    return async function discover() {
	return [{
	    id: "https://chaturbate.com",
	    name: "Chaturbate",
	    detail: "Pornographic live-streamed webcam performances",
	    icon: "💋",
	    browse: async function(id) {
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
	    resolve: async function(id) {
		return resolveStream(id);
	    }    
	}];
    }
}
