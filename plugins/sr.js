// SPDX-License-Identifier: GPL-3.0-or-later

const API_URL = "https://api.sr.se/api/v2";

function fetchChannels() {
    const res = http.get(API_URL + '/channels?format=json&pagination=false&audioquality=hi');
    if (!res.ok)
	throw new Error(res.status);

    const channels = JSON.parse(res.body).channels || [];

    return channels.map((ch) => ({
	id: String(ch.id),
	type: "audio",
	name: ch.name,
	description: ch.tagline,
	image: ch.image,
	uri: ch.liveaudio.url
    }));
}


return {
    discover() {
	return [{
	    name: "Sveriges Radio",
	    detail: "Live radio from Swedish public service",
	    icon: "📻",
	    browse(id) {
		return fetchChannels();
	    }
	}];
    }
};
