// SPDX-License-Identifier: GPL-3.0-or-later
//
// Art gallery: paintings, prints and photographs from the Art Institute of
// Chicago, whose collection is open (api.artic.edu; public domain works are
// CC0). Pictures open in the photo viewer, so a room of the gallery is also
// a slideshow: select starts it, and the TV is a picture frame.
//
// The tree:
//   /               Essentials, then rooms by theme
//   essentials      the works the museum itself puts first
//   room:<query>    public domain works matching a theme
//
// Search (options menu) searches the public domain works: an artist, a
// subject, a place, a century.
//
// The API is Elasticsearch behind one GET:
//   /api/v1/artworks/search?q=...&query[term][is_public_domain]=true
//       &fields=id,title,image_id,...&limit=100&page=N
//   -> { pagination: { total_pages }, data: [{ id, title, image_id,
//        artist_display, date_display, medium_display }],
//        config: { iiif_url } }
// and a picture is <iiif_url>/<image_id>/full/<width>,/0/default.jpg. A
// room is two pages of a hundred, asked for together (http.all).

const API = "https://api.artic.edu/api/v1/artworks/search";
const IIIF = "https://www.artic.edu/iiif/2";
const FIELDS = "id,title,image_id,artist_display,date_display,medium_display,is_public_domain";
const PAGES = 2;
const PAGE_SIZE = 100;
const CACHE_SEC = 60 * 60;
// The museum asks who is calling.
const HEADERS = { "AIC-User-Agent": "jtplay media player (github.com/ps4-homebrew-dev/jtplay)" };

// [query, name, what hangs there].
const ROOMS = [
    ["impressionism", "Impressionism", "Monet, Renoir, Pissarro and the light they chased"],
    ["post-impressionism", "After Impressionism", "Seurat, Cézanne, Van Gogh, Gauguin"],
    ["landscape painting", "Landscapes", "Fields, coasts, mountains and weather"],
    ["portrait painting", "Portraits", "Faces across five centuries"],
    ["still life", "Still lifes", "Fruit, flowers and things on tables"],
    ["ukiyo-e woodblock print", "Japanese prints", "Hokusai, Hiroshige and the floating world"],
    ["photograph", "Photographs", "From the first decades of the camera on"],
    ["modern art", "Modern art", "The early twentieth century, taken apart and rebuilt"],
    ["seascape", "The sea", "Harbours, storms and open water"],
    ["winter snow", "Winter", "Snow, ice and low sun"],
    ["architecture drawing", "Architecture", "Buildings, drawn and dreamed"],
    ["textile", "Textiles", "Woven, printed and embroidered"]
];

function url(params, page) {
    return API + "?" + params + "&fields=" + FIELDS + "&limit=" + PAGE_SIZE + "&page=" + page;
}

// The works a query finds, 'pages' pages of them fetched side by side.
function works(params, pages) {
    const requests = [];
    for (let page = 1; page <= pages; page++)
        requests.push({ url: url(params, page), headers: HEADERS, timeout: 30 });
    const responses = http.all(requests);

    let base = IIIF;
    const out = [];
    const seen = {};
    let failure = null;
    responses.forEach(res => {
        if (!res.ok) {
            // A second page past the end of the results is not a failure.
            failure = failure || res.error || "HTTP " + res.status;
            return;
        }
        const json = res.json();
        if (json.config && json.config.iiif_url)
            base = json.config.iiif_url;
        (json.data || []).forEach(w => {
            if (!w || !w.image_id || seen[w.id])
                return;
            seen[w.id] = true;
            out.push(w);
        });
    });
    if (!out.length && failure)
        throw new Error("The museum did not answer (" + failure + ")");
    return out.map(w => entry(w, base));
}

function entry(w, base) {
    const artist = util.text(w.artist_display).split("\n")[0];
    const facts = [artist, util.text(w.date_display), util.text(w.medium_display)]
        .filter(s => s);
    // Public domain works are served at full size; the rest up to 843 wide.
    const width = w.is_public_domain ? 1686 : 843;
    const iiif = base + "/" + w.image_id + "/full/";
    return {
        id: "w:" + w.id,
        type: "image",
        name: util.text(w.title) || "Untitled",
        description: facts.join("  ·  "),
        image: iiif + "400,/0/default.jpg",
        uri: iiif + width + ",/0/default.jpg"
    };
}

const PUBLIC = "query%5Bterm%5D%5Bis_public_domain%5D=true";

return {
    name: "Art gallery",
    discover() {
        return [{
            name: "Art gallery",
            detail: "The Art Institute of Chicago, as a picture frame",
            icon: "🖼",
            root: "/",

            browse(id) {
                if (id === "/")
                    return [{ id: "essentials", type: "folder", name: "Essentials",
                              description: "The works the museum puts first" }]
                        .concat(ROOMS.map(r => ({
                            id: "room:" + r[0], type: "folder", name: r[1], description: r[2]
                        })));
                if (id === "essentials")
                    return util.memo(id, CACHE_SEC, () =>
                        works("query%5Bterm%5D%5Bis_boosted%5D=true", PAGES));
                if (id.startsWith("room:"))
                    return util.memo(id, CACHE_SEC, () =>
                        works("q=" + encodeURIComponent(id.slice(5)) + "&" + PUBLIC, PAGES));
                throw new Error("unknown folder " + id);
            },

            search(query) {
                return works("q=" + encodeURIComponent(query) + "&" + PUBLIC, 1);
            }
        }];
    }
};
