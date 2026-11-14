// SPDX-License-Identifier: GPL-3.0-or-later
//
// The part of the plugin host that is written in JavaScript: evaluated in
// every plugin's runtime after the natives are installed and before the
// plugin's own script (see JsPlugin). It turns the few natives handed over
// in '__native' into the globals docs/plugins.md describes, and returns the
// helper the host waits for promises with.
//
// Kept to what every QuickJS understands (ES2020, no modules, no Intl).
#ifndef BROWSE_JS_PRELUDE_H
#define BROWSE_JS_PRELUDE_H

namespace browse {

inline constexpr const char kJsPrelude[] = R"JS((function (global) {
"use strict";

const native = global.__native;
delete global.__native;

// ---------------------------------------------------------------- base64

const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Binary string (one char per byte) -> base64, as in a browser.
function btoa(input) {
    const s = String(input);
    let out = "";
    for (let i = 0; i < s.length; i += 3) {
        const a = s.charCodeAt(i);
        const b = i + 1 < s.length ? s.charCodeAt(i + 1) : NaN;
        const c = i + 2 < s.length ? s.charCodeAt(i + 2) : NaN;
        if (a > 255 || b > 255 || c > 255)
            throw new RangeError("btoa: character out of the Latin-1 range");
        out += B64[a >> 2];
        out += B64[((a & 3) << 4) | (isNaN(b) ? 0 : b >> 4)];
        out += isNaN(b) ? "=" : B64[((b & 15) << 2) | (isNaN(c) ? 0 : c >> 6)];
        out += isNaN(c) ? "=" : B64[c & 63];
    }
    return out;
}

// base64 (or its URL-safe alphabet, padded or not) -> binary string.
function atob(input) {
    const s = String(input).replace(/-/g, "+").replace(/_/g, "/").replace(/[\s=]/g, "");
    let bits = 0, nbits = 0, out = "";
    for (let i = 0; i < s.length; i++) {
        const v = B64.indexOf(s[i]);
        if (v < 0)
            throw new SyntaxError("atob: not base64");
        bits = (bits << 6) | v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out += String.fromCharCode((bits >> nbits) & 255);
            bits &= (1 << nbits) - 1;
        }
    }
    return out;
}

function utf8Encode(text) {
    return encodeURIComponent(String(text)).replace(/%([0-9A-F]{2})/g,
        (m, hex) => String.fromCharCode(parseInt(hex, 16)));
}

function utf8Decode(bytes) {
    try {
        return decodeURIComponent(bytes.replace(/[\s\S]/g,
            c => "%" + ("0" + c.charCodeAt(0).toString(16)).slice(-2)));
    } catch (e) {
        return bytes; // not UTF-8: as it came
    }
}

// ------------------------------------------------------------------ util

const NAMED = {
    amp: "&", lt: "<", gt: ">", quot: '"', apos: "'", nbsp: " ",
    lsquo: "‘", rsquo: "’", ldquo: "“", rdquo: "”",
    ndash: "–", mdash: "—", hellip: "…", bull: "•",
    middot: "·", copy: "©", reg: "®", trade: "™",
    laquo: "«", raquo: "»", deg: "°", euro: "€",
    pound: "£", shy: "", zwj: "", zwnj: "",
    aring: "å", auml: "ä", ouml: "ö", Aring: "Å",
    Auml: "Ä", Ouml: "Ö", eacute: "é", Eacute: "É",
    egrave: "è", agrave: "à", aacute: "á", uuml: "ü",
    Uuml: "Ü", szlig: "ß", ntilde: "ñ", ccedil: "ç",
    oslash: "ø", Oslash: "Ø", aelig: "æ", AElig: "Æ"
};

function codePoint(n) {
    return n > 0 && n <= 0x10ffff && !(n >= 0xd800 && n <= 0xdfff)
        ? String.fromCodePoint(n) : "�";
}

// One pass, so "&amp;lt;" stays "&lt;".
function decodeEntities(s) {
    return String(s == null ? "" : s).replace(/&(#x[0-9a-f]+|#\d+|[a-z]+\d*);/gi, (m, e) => {
        if (e[0] === "#")
            return codePoint(e[1] === "x" || e[1] === "X"
                             ? parseInt(e.slice(2), 16) : Number(e.slice(1)));
        const v = Object.prototype.hasOwnProperty.call(NAMED, e) ? NAMED[e]
            : NAMED[e.toLowerCase()];
        return v === undefined ? m : v;
    });
}

function stripTags(s) {
    return String(s == null ? "" : s)
        .replace(/<(script|style)\b[\s\S]*?<\/\1\s*>/gi, " ")
        .replace(/<br\s*\/?>|<\/(p|div|li|h[1-6])\s*>/gi, "\n")
        .replace(/<[^>]*>/g, " ");
}

// Markup of any kind -> the text a person would read: no CDATA wrapper, no
// tags, entities decoded (also where HTML was escaped into the text, as
// feeds do), whitespace tidied. Paragraph breaks survive as single newlines.
function text(s) {
    let t = String(s == null ? "" : s).replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, "$1");
    t = decodeEntities(stripTags(t));
    if (/<[a-z\/][^>]*>/i.test(t))
        t = decodeEntities(stripTags(t));
    return t.replace(/[ \t\r\f\v ]+/g, " ").replace(/ ?\n ?/g, "\n")
        .replace(/\n{2,}/g, "\n").trim();
}

// At most 'n' characters, cut at a word where there is one, with an
// ellipsis to say so.
function clip(s, n) {
    s = String(s == null ? "" : s);
    if (s.length <= n)
        return s;
    const cut = s.slice(0, Math.max(1, n - 1));
    // Ends on a whole word already, or goes back to the last one that does.
    const space = /\s/.test(s[cut.length]) ? cut.length : cut.lastIndexOf(" ");
    return (space > n * 0.6 ? cut.slice(0, space) : cut).replace(/[\s.,;:!?-]+$/, "") + "…";
}

// { a: 1, b: ["x", "y"], c: null } -> "a=1&b=x&b=y"
function query(params) {
    const parts = [];
    Object.keys(params || {}).forEach(key => {
        const value = params[key];
        if (value === null || value === undefined)
            return;
        (Array.isArray(value) ? value : [value]).forEach(v => {
            parts.push(encodeURIComponent(key) + "=" + encodeURIComponent(String(v)));
        });
    });
    return parts.join("&");
}

function uuid() {
    return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, c => {
        const r = Math.floor(Math.random() * 16);
        return (c === "x" ? r : (r & 3) | 8).toString(16);
    });
}

// 'ref', as found in the document at 'base', made absolute.
function resolveUrl(base, ref) {
    base = String(base == null ? "" : base);
    ref = String(ref == null ? "" : ref).trim();
    if (!ref)
        return base;
    if (/^[a-z][a-z0-9+.-]*:/i.test(ref))
        return ref;
    const m = /^([a-z][a-z0-9+.-]*:)\/\/([^\/?#]*)([^?#]*)(\?[^#]*)?/i.exec(base);
    if (!m)
        return ref;
    const origin = m[1] + "//" + m[2];
    if (ref.startsWith("//"))
        return m[1] + ref;
    if (ref[0] === "#")
        return origin + (m[3] || "/") + (m[4] || "") + ref;
    if (ref[0] === "?")
        return origin + (m[3] || "/") + ref;

    const cut = ref.search(/[?#]/);
    const refPath = cut < 0 ? ref : ref.slice(0, cut);
    const tail = cut < 0 ? "" : ref.slice(cut);
    const basePath = m[3] || "/";
    const joined = refPath[0] === "/" ? refPath
        : basePath.slice(0, basePath.lastIndexOf("/") + 1) + refPath;
    const out = [];
    joined.split("/").forEach((seg, i, all) => {
        if (seg === "..") {
            if (out.length > 1)
                out.pop();
            if (i === all.length - 1)
                out.push("");
        } else if (seg === ".") {
            if (i === all.length - 1)
                out.push("");
        } else {
            out.push(seg);
        }
    });
    return origin + (out.join("/") || "/") + tail;
}

// "1:02:03", "62:03", "3723", "PT1H2M3S" -> seconds; 0 when it is none.
function duration(s) {
    if (typeof s === "number")
        return isFinite(s) && s > 0 ? Math.round(s) : 0;
    s = String(s == null ? "" : s).trim();
    const iso = /^P(?:(\d+)D)?T?(?:(\d+)H)?(?:(\d+)M)?(?:(\d+(?:\.\d+)?)S)?$/i.exec(s);
    if (iso && s.length > 1)
        return Math.round((+iso[1] || 0) * 86400 + (+iso[2] || 0) * 3600 +
                          (+iso[3] || 0) * 60 + (+iso[4] || 0));
    if (!/^\d+(?::\d+){0,2}(?:\.\d+)?$/.test(s))
        return 0;
    return Math.round(s.split(":").reduce((total, part) => total * 60 + Number(part), 0));
}

// Results worth keeping for a while: listings are browsed back and forth.
// Bounded, and emptied when the user changes one of the plugin's settings,
// since what was listed may depend on them.
const MEMO_MAX = 256;
let memoEntries = new Map();
let memoRevision = native.settingsRevision();

function memo(key, seconds, produce) {
    const revision = native.settingsRevision();
    if (revision !== memoRevision) {
        memoEntries = new Map();
        memoRevision = revision;
    }
    key = String(key);
    const now = Date.now();
    const hit = memoEntries.get(key);
    if (hit && now < hit.until)
        return hit.value;
    const value = produce();
    // An async producer: a promise that failed is not worth keeping.
    if (value && typeof value.then === "function")
        value.then(null, () => { memoEntries.delete(key); });
    memoEntries.delete(key);
    memoEntries.set(key, { until: now + seconds * 1000, value: value });
    while (memoEntries.size > MEMO_MAX)
        memoEntries.delete(memoEntries.keys().next().value);
    return value;
}

function forget(key) {
    if (key === undefined)
        memoEntries = new Map();
    else
        memoEntries.delete(String(key));
}

global.util = Object.freeze({
    query, uuid, text, clip, decodeEntities, stripTags, resolveUrl, duration,
    memo, forget,
    base64Encode: s => btoa(utf8Encode(s)),
    base64Decode: s => utf8Decode(atob(s))
});
global.atob = atob;
global.btoa = btoa;

// --------------------------------------------------------------- storage

// A small JSON file per plugin, for what should outlive the session: a
// device id, a token, what the user last picked. Values are copied in and
// out, so changing what get() returned changes nothing until it is set().
let stored = null;

function load() {
    if (stored)
        return stored;
    try {
        stored = JSON.parse(native.storageLoad() || "{}");
    } catch (e) {
        stored = null;
    }
    if (typeof stored !== "object" || stored === null || Array.isArray(stored))
        stored = {};
    return stored;
}

function save(data, undo) {
    try {
        native.storageSave(JSON.stringify(data));
    } catch (e) {
        undo();
        throw e;
    }
}

const copy = v => v === undefined ? undefined : JSON.parse(JSON.stringify(v));
const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);

global.storage = Object.freeze({
    get(key, fallback) {
        const data = load();
        key = String(key);
        return has(data, key) ? copy(data[key]) : fallback;
    },
    set(key, value) {
        if (value === undefined)
            return this.remove(key);
        const data = load();
        key = String(key);
        const had = has(data, key), before = data[key];
        data[key] = copy(value);
        save(data, () => { if (had) data[key] = before; else delete data[key]; });
    },
    remove(key) {
        const data = load();
        key = String(key);
        if (!has(data, key))
            return;
        const before = data[key];
        delete data[key];
        save(data, () => { data[key] = before; });
    },
    keys() {
        return Object.keys(load());
    }
});

// -------------------------------------------------------------- settings

global.settings = Object.freeze({
    get(key) {
        return native.setting(String(key));
    }
});

// ------------------------------------------------------------------- xml

// xml.parse() gives the root element as a plain tree of
//   { name, attrs: {name: value}, children: [element], text }
// where 'text' is the element's own text (and CDATA), without its
// children's. The accessors take a missing node in their stride, so a chain
// of them needs no checks in between.
const xml = {
    parse(source) {
        return native.xmlParse(String(source));
    },
    children(node, name) {
        if (!node || !node.children)
            return [];
        return name === undefined ? node.children
            : node.children.filter(c => c.name === name);
    },
    child(node, name) {
        if (!node || !node.children)
            return null;
        for (let i = 0; i < node.children.length; i++)
            if (node.children[i].name === name)
                return node.children[i];
        return null;
    },
    // The text of the node, or of its first child called 'name'.
    text(node, name) {
        const n = name === undefined ? node : xml.child(node, name);
        return n ? String(n.text || "").trim() : "";
    },
    attr(node, name) {
        return node && node.attrs && has(node.attrs, name) ? node.attrs[name] : "";
    },
    // Every descendant called 'name', in document order.
    find(node, name) {
        const out = [];
        (function walk(n) {
            if (!n || !n.children)
                return;
            n.children.forEach(c => {
                if (c.name === name)
                    out.push(c);
                walk(c);
            });
        })(node);
        return out;
    }
};
global.xml = Object.freeze(xml);

// ------------------------------------------------------------------ http

// The natives hand back plain objects; these add what nearly every caller
// does with one next.
const responseProto = {
    json() {
        return JSON.parse(this.body);
    },
    xml() {
        return xml.parse(this.body);
    }
};
const dress = res => Object.setPrototypeOf(res, responseProto);
const raw = global.http;

function hostOf(url) {
    const m = /^[a-z][a-z0-9+.-]*:\/\/([^\/?#]*)/i.exec(String(url));
    return m ? m[1] : String(url);
}

global.http = Object.freeze({
    get: (url, options) => dress(raw.get(url, options)),
    post: (url, body, options) => dress(raw.post(url, body, options)),
    request: options => dress(raw.request(options)),
    // Several requests at once; see docs/plugins.md.
    all: requests => raw.all(requests).map(dress),
    // GET and parse, for the common case: anything but a 2xx answer with
    // JSON in it is an Error.
    json(url, options) {
        const res = raw.get(url, options);
        if (!res.ok)
            throw new Error("HTTP " + res.status + " from " + hostOf(res.url || url));
        return JSON.parse(res.body);
    }
});

// ---------------------------------------------------------------- jtplay

global.jtplay = Object.freeze({
    api: native.api,
    version: native.version,
    platform: native.platform
});

// What the host waits for a promise with: 'box' is filled in when (if) it
// settles, as the job queue is run.
return function settle(promise, box) {
    promise.then(
        value => { box.done = true; box.value = value; },
        error => { box.done = true; box.failed = true; box.error = error; });
};
})(this);
)JS";

} // namespace browse

#endif
