// SPDX-License-Identifier: GPL-3.0-or-later
//
// TV4 Play (tv4play.se): TV4, Sjuan, TV12 and the rest of TV4's catalogue:
// series, films, news, sport, clips.
//
// Browsing needs no account. Playing does: TV4 Play wants a signed-in
// account even for what is free to watch (a free account will do). There
// is no way to sign in from a TV here yet, so the account comes in as the
// sign-in token the website keeps in the cookie 'tv4-refresh-token', the
// way svtplay-dl and Kodi's Retrospect do it. The Sign-in token setting
// says how to get it. It lasts for months; a new one is needed when TV4
// says it is no longer accepted.
//
// What is protected with DRM (most films, live sport, some series) cannot
// be played here; such titles say so in their description, and resolve()
// says so before trying.
//
// The tree:
//   /                    Populärt, Senaste, Nyheter, Kategorier, Program A-Ö
//   panel:<id>           a row of the website: series, films, clips, episodes
//   live:<id>            a row of live channels and broadcasts
//   pagelist:<id>        a list of pages (the categories)
//   page:<id>            one page: its rows
//   index                every series and film, A to Z
//   series:<id>          a series: its seasons (or, with one, its episodes)
//   season:<id>          a season's episodes
//   v:<id>               something to play (an episode, film, clip, event)
//
// Search (options menu) searches series, films, clips, sport and pages.
//
// Sources, both of them maintained against the live service:
//   - The GraphQL queries are Retrospect's (github.com/retrospect-addon/
//     plugin.video.retrospect, channels/channel.se/tv4se, GPL-3.0-or-later),
//     generated from its source and kept as they are, since the gateway
//     answers the queries its own clients send.
//   - Sign-in and playback follow svtplay-dl (github.com/spaam/svtplay-dl,
//     lib/svtplay_dl/service/tv4play.py, MIT).
//
// Endpoints:
//   POST https://client-gateway.tv4.a2d.tv/graphql?operationName=<op>
//        { operationName, query, variables }   -> { data } | { errors }
//   POST https://auth.tv4.a2d.tv/v2/auth/token
//        { grant_type: "refresh_token", refresh_token, ... } -> { access_token }
//   GET  https://playback2.a2d.tv/play/<id>?service=tv4play&...
//        -> { metadata: { isDrmProtected, isLive, ... },
//             playbackItem: { type: "hls" | "dash", manifestUrl } }
//   GET  https://imageproxy.a2d.tv/?source=<url encoded>&width=<px>

const GATEWAY = "https://client-gateway.tv4.a2d.tv/graphql";
const AUTH = "https://auth.tv4.a2d.tv/v2/auth/token";
const PLAYBACK = "https://playback2.a2d.tv/play/";
const IMAGES = "https://imageproxy.a2d.tv/";
const CLIENT = { "Client-Name": "tv4-web", "Client-Version": "5.2.0" };

// Rows of the website's start page (from Retrospect, January 2026). If TV4
// replaces a row, its folder here comes up empty or fails; Kategorier and
// Program A-Ö do not depend on these.
const PANELS = [
    ["4nNp00Z12bjEiNboaW2uxB", "Populärt", "What most people are watching"],
    ["3DkAXJqTwajM7RytxM0ju7", "Senaste", "Recently added"],
    ["4OsLKff6znSeWNeOfewYNw", "Nyheter", "The latest news broadcasts"]
];

const PAGE_SIZE = 100;
const MAX_ITEMS = 1500;
const CACHE_SEC = 10 * 60;
const IMAGE_WIDTH = 640;

const QUERIES = {
    Panel:
        "query Panel($panelId: ID!, $offset: Int!, $limit: Int!) { panel(id: $panelId) { ... on ContinueWatchingPanel { id } ... on PagePanel { id content(input: {offset: $offset, limit: $limit}) { pageInfo { ...PageInfoFields } items { ... on PagePanelPageItem { page { ...PageListFields } } } } } ... on SportEventPanel { id content(input: {offset: $offset, limit: $limit}) { pageInfo { ...PageInfoFields } items { sportEvent { ...SportEventFieldsLight } } } } ... on ClipsPanel { id title content(input: {offset: $offset, limit: $limit}) { pageInfo { ...PageInfoFields } items { __typename clip { __typename ...ClipFieldsLight } } } } ... on EpisodesPanel { id title content(input: {offset: $offset, limit: $limit}) { pageInfo { ...PageInfoFields } items { episode { ...EpisodeFields } labelText } } } ... on MediaPanel { __typename id slug title content(input: {offset: $offset, limit: $limit}) { __typename pageInfo { ...PageInfoFields } items { ... on MediaPanelMovieItem { __typename movie { ...MovieFieldsLight __typename } } ... on MediaPanelSeriesItem { __typename series { ...SeriesFieldsLight __typename} } } } } ... on ChannelPanel { id title type content(input: {offset: $offset, limit: $limit}) { pageInfo { ...PageInfoFields } items { channel { ...ChannelFields } } } } } } fragment ImageFieldsFull on Image { sourceEncoded meta { muteBgColor { hex } } } fragment ParentalRatingFields on ParentalRating { finland { ageRestriction reason } sweden { ageRecommendation suitableForChildren } } fragment ImageFieldsLight on Image { sourceEncoded isFallback } fragment VideoFields on Video { id duration { readableShort readableMinutes seconds } isLiveContent access { hasAccess } isDrmProtected } fragment EpisodeFields on Episode { id slug title isLiveContent isStartOverEnabled series { id title slug } images { main16x9 { ...ImageFieldsLight } } liveEventEnd { isoString } playableUntil { readableDistance(type: DAYS_LEFT) humanDateTime } playableFrom { humanDateTime isoString } video { ...VideoFields } upsell { tierId } } fragment LabelFields on Label { id airtime announcement recurringBroadcast } fragment MovieFieldsLight on Movie { id slug title genres productionYear isLiveContent productionCountries { name } parentalRating { ...ParentalRatingFields } liveEventEnd { isoString } label { ...LabelFields } images { cover2x3 { ...ImageFieldsFull } main16x9Annotated { ...ImageFieldsLight } } playableUntil { readableDistance(type: DAYS_LEFT) } playableFrom { isoString humanDateTime readableDate } trailers { mp4 webm } upsell { tierId } } fragment SportEventFieldsLight on SportEvent { title slug id league round images { main16x9 { ...ImageFieldsLight } brandLogo { ...ImageFieldsLight } } playableFrom { humanDateTime isoString } liveEventEnd { isoString } isLiveContent upsell { tierId } }  fragment SeriesFieldsLight on Series { id slug title genres mediaClassification numberOfAvailableSeasons label { ...LabelFields } images { cover2x3 { ...ImageFieldsFull } main16x9Annotated { ...ImageFieldsLight } } parentalRating { ...ParentalRatingFields } trailers { mp4 webm } upsell { tierId } } fragment ClipFieldsLight on Clip { id slug title clipVideo: video { ...VideoFields } images { main16x9 { ...ImageFieldsLight } } playableFrom { readableDistance } mediaClassification parent { ... on ClipParentSeriesLink { id title } ... on ClipParentMovieLink { id title } } } fragment ChannelFields on Channel { __typename id title description tagline slug type images { logo { ...ImageFieldsLight } main16x9 { ...ImageFieldsLight } } access { hasAccess } epg { end start title } }  fragment PageListFields on PageReference { id title images { image16x9 { ...ImageFieldsFull } image4x3 { ...ImageFieldsFull } logo { ...ImageFieldsLight isFallback } } } fragment PageInfoFields on PageInfo { hasNextPage nextPageOffset totalCount }",
    LivePanel:
        "query LivePanel($panelId: ID!, $offset: Int!, $limit: Int!) { panel(id: $panelId) { __typename ... on LivePanel { __typename id title playOutOfView content(input: {offset: $offset, limit: $limit}) { pageInfo { ...PageInfoFields } items { ... on LivePanelChannelItem { __typename channel { __typename ...ChannelVideoFields } } ... on LivePanelEpisodeItem { __typename episode { __typename ...EpisodeVideoFields } } ... on LivePanelMovieItem { __typename movie { __typename ...MovieVideoFields } } ... on LivePanelSportEventItem { __typename sportEvent { __typename ...SportEventVideoFields } } } } } } } fragment PageInfoFields on PageInfo { hasNextPage nextPageOffset totalCount } fragment ChannelVideoFields on Channel { title id slug isDrmProtected access { hasAccess } epg { end start title type images { main16x9 { isFallback id source sourceEncoded } } } images { logo { ...ImageFieldsLight } main16x9 { ...ImageFieldsLight source } } } fragment EpisodeVideoFields on Episode { id slug title extendedTitle isPollFeatureEnabled isLiveContent liveEventEnd { isoString } series { title id slug } synopsis { medium } images { main16x9 { ...ImageFieldsLight source } } parentalRating { ...ParentalRatingFields } video { ...VideoFields } playableFrom { isoString humanDateTime } playableUntil { isoString humanDateTime } } fragment MovieVideoFields on Movie { id slug title isPollFeatureEnabled liveEventEnd { isoString } isLiveContent synopsis { medium } images { main16x9 { ...ImageFieldsLight source } brandLogo { ...ImageFieldsLight } } parentalRating { ...ParentalRatingFields } video { ...VideoFields } playableFrom { isoString humanDateTime } playableUntil { isoString humanDateTime } } fragment SportEventVideoFields on SportEvent { title id slug isLiveContent isDrmProtected access { hasAccess } synopsis { medium } images { logo { ...ImageFieldsLight } main16x9 { ...ImageFieldsLight source } brandLogo { ...ImageFieldsLight } } playableUntil { isoString humanDateTime } playableFrom { humanDateTime isoString readableDistance } liveEventEnd { isoString } } fragment ImageFieldsLight on Image { sourceEncoded isFallback } fragment ParentalRatingFields on ParentalRating { finland { ageRestriction reason } sweden { ageRecommendation suitableForChildren } } fragment VideoFields on Video { id duration { readableShort readableMinutes seconds } isLiveContent access { hasAccess } isDrmProtected }",
    PageList:
        "query PageList($pageListId: ID!) { pageList(id: $pageListId) { id content { ... on PageReferenceItem { pageReference { ...PageListFields __typename } __typename } ... on StaticPageItem { staticPage { id title type images { image4x3 { ...ImageFieldsFull __typename } __typename } __typename } __typename } __typename } __typename }} fragment PageListFields on PageReference { id title images { image16x9 { ...ImageFieldsFull } image4x3 { ...ImageFieldsFull } logo { ...ImageFieldsLight isFallback } } } fragment ImageFieldsFull on Image { sourceEncoded meta { muteBgColor { hex } } } fragment ImageFieldsLight on Image { sourceEncoded isFallback }",
    Page:
        "query Page($pageId: ID!, $input: PageContentInput!) { page(id: $pageId) { id title content(input: $input) { pageInfo { ...PageInfoFields } panels { __typename ... on ContinueWatchingPanel { __typename id title } ... on MediaPanel { __typename id slug title displayHint { mediaPanelImageRatio } } ... on SportEventPanel { __typename id title } ... on ClipsPanel { __typename id title } ... on EpisodesPanel { __typename id title } ... on LivePanel { __typename id title } ... on PagePanel { __typename id title } ... on ChannelPanel { __typename id title type } ... on ThemePanel { __typename ...ThemePanelFields } ... on SinglePanel { __typename ...SinglePanelFields } } } } } fragment PageInfoFields on PageInfo { hasNextPage nextPageOffset totalCount } fragment ThemePanelFields on ThemePanel { id title pitch hexColor images { image16x9 { ...ImageFieldsFull } } link { ... on ThemePanelSeriesLink { series { id slug genres numberOfAvailableSeasons parentalRating { ...ParentalRatingFields } images { brandLogo { ...ImageFieldsLight } } upsell { tierId } } } ... on ThemePanelMovieLink { movie { id slug genres productionCountries { countryCode name } productionYear parentalRating { ...ParentalRatingFields } images { brandLogo { ...ImageFieldsLight } } upsell { tierId } } } ... on ThemePanelEpisodeLink { episode { id slug upsell { tierId } } } ... on ThemePanelClipLink { clip { id slug } } ... on ThemePanelPageLink { page { id } } ... on ThemePanelUrlsLink { webUrl } ... on ThemePanelSportEventLink { sportEvent { id slug arena league round playableFrom { humanDateTime isoString readableDate } images { brandLogo { ...ImageFieldsLight } } upsell { tierId } } } } themePanelLinkText: linkText showMetadataForLink subtitle trailers { ...TrailerFields } showUpsellLabel } fragment SinglePanelFields on SinglePanel { id secondaryLinkText images { image16x9 { ...ImageFieldsFull } image2x3 { ...ImageFieldsFull } brandLogo { ...ImageFieldsLight } } link { __typename ... on SinglePanelPageLink { page { id } } ... on SinglePanelSportEventLink { sportEvent { ...SportEventFieldsLight } } ... on SinglePanelSeriesLink { series { ...SeriesFieldsLight } } ... on SinglePanelMovieLink { movie { ...MovieFieldsLight } } ... on SinglePanelEpisodeLink { episode { ...EpisodeFields } } ... on SinglePanelClipLink { clip { ...ClipFieldsLight } } ... on SinglePanelChannelLink { channel { ...ChannelFields } } } trailers { ...TrailerFields } linkText title pitch shortPitch } fragment ImageFieldsFull on Image { sourceEncoded meta { muteBgColor { hex } } } fragment ParentalRatingFields on ParentalRating { finland { ageRestriction reason } sweden { ageRecommendation suitableForChildren } } fragment ImageFieldsLight on Image { sourceEncoded isFallback } fragment TrailerFields on Trailers { mp4 webm } fragment SportEventFieldsLight on SportEvent { title slug id league round images { main16x9 { ...ImageFieldsLight } brandLogo { ...ImageFieldsLight } } playableFrom { humanDateTime isoString } liveEventEnd { isoString } isLiveContent upsell { tierId } }  fragment SeriesFieldsLight on Series { id slug title genres mediaClassification numberOfAvailableSeasons label { ...LabelFields } images { cover2x3 { ...ImageFieldsFull } main16x9Annotated { ...ImageFieldsLight } } parentalRating { ...ParentalRatingFields } trailers { mp4 webm } upsell { tierId } } fragment MovieFieldsLight on Movie { id slug title genres productionYear isLiveContent productionCountries { name } parentalRating { ...ParentalRatingFields } liveEventEnd { isoString } label { ...LabelFields } images { cover2x3 { ...ImageFieldsFull } main16x9Annotated { ...ImageFieldsLight } } playableUntil { readableDistance(type: DAYS_LEFT) } playableFrom { isoString humanDateTime readableDate } trailers { mp4 webm } upsell { tierId } } fragment EpisodeFields on Episode { id slug title isLiveContent isStartOverEnabled series { id title slug } images { main16x9 { ...ImageFieldsLight } } liveEventEnd { isoString } playableUntil { readableDistance(type: DAYS_LEFT) humanDateTime } playableFrom { humanDateTime isoString } video { ...VideoFields } upsell { tierId } } fragment ClipFieldsLight on Clip { id slug title clipVideo: video { ...VideoFields } images { main16x9 { ...ImageFieldsLight } } playableFrom { readableDistance } mediaClassification parent { ... on ClipParentSeriesLink { id title } ... on ClipParentMovieLink { id title } } } fragment ChannelFields on Channel { __typename id title description tagline slug type images { logo { ...ImageFieldsLight } main16x9 { ...ImageFieldsLight } } access { hasAccess } epg { end start title } }  fragment LabelFields on Label { id airtime announcement recurringBroadcast } fragment VideoFields on Video { id duration { readableShort readableMinutes seconds } isLiveContent access { hasAccess } isDrmProtected }",
    MediaIndex:
        "query MediaIndex($input: MediaIndexListInput!, $genres: [String!]) { mediaIndex(genres: $genres) { overview { letterIndex { letter totalCount } } contentList(input: $input) { __typename items { ... on MediaIndexSeriesItem { __typename letter series { ...SeriesFieldsLight __typename } } ... on MediaIndexMovieItem { __typename letter movie { ...MovieFieldsLight __typename } } } pageInfo { hasMoreForLastLetter totalCount lastLetter nextPageOffset } } } } fragment SeriesFieldsLight on Series { id slug title genres mediaClassification numberOfAvailableSeasons label { ...LabelFields } images { cover2x3 { ...ImageFieldsFull } main16x9Annotated { ...ImageFieldsLight } } parentalRating { ...ParentalRatingFields } trailers { mp4 webm } upsell { tierId } } fragment MovieFieldsLight on Movie { id slug title genres productionYear isLiveContent productionCountries { name } parentalRating { ...ParentalRatingFields } liveEventEnd { isoString } label { ...LabelFields } images { cover2x3 { ...ImageFieldsFull } main16x9Annotated { ...ImageFieldsLight } } playableUntil { readableDistance(type: DAYS_LEFT) } playableFrom { isoString humanDateTime readableDate } trailers { mp4 webm } upsell { tierId } } fragment LabelFields on Label { id airtime announcement recurringBroadcast } fragment ImageFieldsFull on Image { sourceEncoded meta { muteBgColor { hex } } } fragment ImageFieldsLight on Image { sourceEncoded isFallback } fragment ParentalRatingFields on ParentalRating { finland { ageRestriction reason } sweden { ageRecommendation suitableForChildren } }",
    ContentDetailsPage:
        "query ContentDetailsPage($mediaId: ID!, $panelsInput: CdpPanelsInput!) { media(id: $mediaId) { ... on SportEvent { __typename id slug title league arena commentators country round season inStudio isLiveContent isStartOverEnabled humanCallToAction editorialInfoText synopsis { brief long } trailers { ...TrailerFields } playableFrom { humanDateTime isoString readableDate } playableUntil { readableDate } liveEventEnd { isoString } images { poster2x3 { ...ImageFieldsFull } main16x9 { ...ImageFieldsFull } logo { ...ImageFieldsLight } brandLogo { ...ImageFieldsLight } } upsell { tierId } } ... on Movie { __typename id slug title genres humanCallToAction isPollFeatureEnabled productionYear isLiveContent isStartOverEnabled liveEventEnd { isoString } productionCountries { countryCode name } playableFrom { isoString readableDate humanDateTime readableDistance } playableUntil { isoString readableDate } video { ...VideoFields } parentalRating { ...ParentalRatingFields } credits { ...MovieCreditsFields } images { poster2x3 { ...ImageFieldsFull } main16x9 { ...ImageFieldsFull } logo { ...ImageFieldsLight } brandLogo { ...ImageFieldsLight } } synopsis { brief long } trailers { ...TrailerFields } label { ...LabelFields } panels(input: $panelsInput) { ...CdpPanelsFields } hasPanels editorialInfoText upsell { tierId } } ... on Series { __typename id slug title numberOfAvailableSeasons genres isPollFeatureEnabled upcomingEpisode { ...UpcomingEpisodeFields } trailers { ...TrailerFields } parentalRating { ...ParentalRatingFields } credits { ...SeriesCreditsFields } images { poster2x3 { ...ImageFieldsFull } main16x9 { ...ImageFieldsFull } logo { ...ImageFieldsLight } brandLogo { ...ImageFieldsLight } } synopsis { brief long } allSeasonLinks { __typename seasonId title numberOfEpisodes } label { ...LabelFields } panels(input: $panelsInput) { ...CdpPanelsFields } hasPanels editorialInfoText upsell { tierId } } } } fragment TrailerFields on Trailers { mp4 webm } fragment ImageFieldsFull on Image { sourceEncoded meta { muteBgColor { hex } } } fragment ImageFieldsLight on Image { sourceEncoded isFallback } fragment VideoFields on Video { id duration { readableShort readableMinutes seconds } isLiveContent access { hasAccess } isDrmProtected } fragment ParentalRatingFields on ParentalRating { finland { ageRestriction reason } sweden { ageRecommendation suitableForChildren } } fragment MovieCreditsFields on MovieCredits { actors { characterName name type } directors { name type } } fragment LabelFields on Label { id airtime announcement recurringBroadcast } fragment CdpPanelsFields on CdpPanels { items { ... on ClipsPanel { id title } } } fragment UpcomingEpisodeFields on UpcomingEpisode { id title seasonTitle playableFrom { humanDateTime isoString readableDateShort } image { main16x9 { ...ImageFieldsLight } } upsell { tierId } }  fragment SeriesCreditsFields on SeriesCredits { directors { name type } hosts { name type } actors { characterName name type } }",
    SeasonEpisodes:
        "query SeasonEpisodes($input: SeasonEpisodesInput!, $seasonId: ID!) { season(id: $seasonId) { id numberOfEpisodes episodes(input: $input) { initialSortOrder pageInfo { ...PageInfoFields } items { __typename ...EpisodeFieldsFull } } } } fragment PageInfoFields on PageInfo { hasNextPage nextPageOffset totalCount } fragment EpisodeFieldsFull on Episode { ...EpisodeFields synopsis { medium } parentalRating { ...ParentalRatingFields } upsell { tierId } } fragment EpisodeFields on Episode { id slug title isLiveContent isStartOverEnabled series { id title slug } images { main16x9 { ...ImageFieldsLight } } liveEventEnd { isoString } playableUntil { readableDistance(type: DAYS_LEFT) humanDateTime } playableFrom { humanDateTime isoString } video { ...VideoFields } upsell { tierId } } fragment ParentalRatingFields on ParentalRating { finland { ageRestriction reason } sweden { ageRecommendation suitableForChildren } } fragment ImageFieldsLight on Image { sourceEncoded isFallback } fragment VideoFields on Video { id duration { readableShort readableMinutes seconds } isLiveContent access { hasAccess } isDrmProtected }",
    PanelSearch:
        "query PanelSearch($input: PanelSearchInput!, $limit: Int!, $offset: Int!, $shouldFetchMovieSeries: Boolean!, $shouldFetchMovieSeriesUpsell: Boolean!, $shouldFetchClip: Boolean!, $shouldFetchPage: Boolean!, $shouldFetchSportEvent: Boolean!, $shouldFetchSportEventUpsell: Boolean!) { panelSearch(input: $input) { data { movieSeries @include(if: $shouldFetchMovieSeries) { id title content(input: {limit: $limit, offset: $offset}) { items { ... on MediaPanelSeriesItem { __typename series { __typename ...SeriesFieldsLight } } ... on MediaPanelMovieItem { __typename movie { __typename ...MovieFieldsLight } } } pageInfo { ...PageInfoFields } } } movieSeriesUpsell @include(if: $shouldFetchMovieSeriesUpsell) { id title content(input: {limit: $limit, offset: $offset}) { items { ... on MediaPanelSeriesItem { __typename series { __typename ...SeriesFieldsLight } } ... on MediaPanelMovieItem { __typename movie { __typename ...MovieFieldsLight } } } pageInfo { ...PageInfoFields } } } clip @include(if: $shouldFetchClip) { id title content(input: {limit: $limit, offset: $offset}) { items { __typename clip { __typename ...ClipFieldsLight } } pageInfo { ...PageInfoFields } } } page @include(if: $shouldFetchPage) { id title content(input: {limit: $limit, offset: $offset}) { items { __typename ... on PagePanelPageItem { __typename page { __typename ...PageListFields } } } pageInfo { ...PageInfoFields } } } sportEvent @include(if: $shouldFetchSportEvent) { id title content(input: {limit: $limit, offset: $offset}) { items { __typename sportEvent { __typename ...SportEventFieldsLight } } pageInfo { ...PageInfoFields } } } sportEventUpsell @include(if: $shouldFetchSportEventUpsell) { id title content(input: {limit: $limit, offset: $offset}) { items { __typename sportEvent { __typename ...SportEventFieldsLight } } pageInfo { ...PageInfoFields } } } } pageInfo { totalCountAll { clips movies pages series sportEvents } } order } } fragment SeriesFieldsLight on Series { id slug title genres mediaClassification numberOfAvailableSeasons label { ...LabelFields } images { cover2x3 { ...ImageFieldsFull } main16x9Annotated { ...ImageFieldsLight } } parentalRating { ...ParentalRatingFields } trailers { mp4 webm } upsell { tierId } } fragment MovieFieldsLight on Movie { id slug title genres productionYear isLiveContent productionCountries { name } parentalRating { ...ParentalRatingFields } liveEventEnd { isoString } label { ...LabelFields } images { cover2x3 { ...ImageFieldsFull } main16x9Annotated { ...ImageFieldsLight } } playableUntil { readableDistance(type: DAYS_LEFT) } playableFrom { isoString humanDateTime readableDate } trailers { mp4 webm } upsell { tierId } } fragment PageInfoFields on PageInfo { hasNextPage nextPageOffset totalCount } fragment ClipFieldsLight on Clip { id slug title clipVideo: video { ...VideoFields } images { main16x9 { ...ImageFieldsLight } } playableFrom { readableDistance } mediaClassification parent { ... on ClipParentSeriesLink { id title } ... on ClipParentMovieLink { id title } } } fragment PageListFields on PageReference { id title images { image16x9 { ...ImageFieldsFull } image4x3 { ...ImageFieldsFull } logo { ...ImageFieldsLight isFallback } } } fragment SportEventFieldsLight on SportEvent { title slug id league round images { main16x9 { ...ImageFieldsLight } brandLogo { ...ImageFieldsLight } } playableFrom { humanDateTime isoString } liveEventEnd { isoString } isLiveContent upsell { tierId } } fragment LabelFields on Label { id airtime announcement recurringBroadcast } fragment ImageFieldsFull on Image { sourceEncoded meta { muteBgColor { hex } } } fragment ImageFieldsLight on Image { sourceEncoded isFallback } fragment ParentalRatingFields on ParentalRating { finland { ageRestriction reason } sweden { ageRecommendation suitableForChildren } } fragment VideoFields on Video { id duration { readableShort readableMinutes seconds } isLiveContent access { hasAccess } isDrmProtected }",
};

// ------------------------------------------------------------------ sign-in

// The refresh token to sign in with: the one the user set, or the one TV4
// handed back in its place the last time it was used (should TV4 ever
// rotate them), as long as it was handed back for the one set now.
function refreshToken() {
    const set = String(settings.get("token") || "").trim();
    if (!set)
        return "";
    const rotated = storage.get("rotated");
    if (rotated && rotated.from === set && typeof rotated.token === "string")
        return rotated.token;
    return set;
}

// The expiry of a JWT, in milliseconds since the epoch; 0 if it has none
// or is not a JWT.
function jwtExpiry(token) {
    try {
        const payload = JSON.parse(util.base64Decode(String(token).split(".")[1]));
        return Number(payload.exp) > 0 ? Number(payload.exp) * 1000 : 0;
    } catch (e) {
        return 0;
    }
}

let access = null; // { token, until, from }

// A bearer token for the gateway and playback; null when there is no
// account. Throws when there is one and TV4 turns it down.
function accessToken() {
    const refresh = refreshToken();
    if (!refresh)
        return null;
    if (access && access.from === refresh && Date.now() < access.until)
        return access.token;

    const expires = jwtExpiry(refresh);
    if (expires && expires < Date.now())
        throw new Error("The TV4 Play sign-in token expired on " +
                              new Date(expires).toISOString().slice(0, 10) +
                              ". Get a new one (Settings, TV4 Play).");

    const res = http.post(AUTH, JSON.stringify({
        grant_type: "refresh_token",
        refresh_token: refresh,
        profile_id: "default",
        is_child: false
    }), { headers: { "Content-Type": "application/json", "client-name": "tv4-web" },
          timeout: 20 });
    let json = null;
    try {
        json = res.json();
    } catch (e) {
        // Not JSON: said below.
    }
    if (!res.ok || !json || !json.access_token) {
        if (res.status >= 400 && res.status < 500)
            throw new Error("TV4 Play did not accept the sign-in token. It may have " +
                                  "expired or been signed out; get a new one (Settings, TV4 Play).");
        throw new Error("TV4 Play's sign-in did not answer (HTTP " + res.status + ")");
    }

    if (json.refresh_token && json.refresh_token !== refresh) {
        try {
            storage.set("rotated", { from: String(settings.get("token")).trim(),
                                     token: json.refresh_token });
        } catch (e) {
            console.warn("the new refresh token was not kept: " + e.message);
        }
    }
    const until = jwtExpiry(json.access_token) ||
        Date.now() + (Number(json.expires_in) > 0 ? Number(json.expires_in) * 1000 : 600000);
    access = { token: json.access_token, from: refresh, until: until - 60000 };
    return access.token;
}

// For browsing, which works without an account: an account that does not
// work is remarked on and browsed without, rather than failing the folder.
function optionalToken() {
    try {
        return accessToken();
    } catch (e) {
        console.warn(e.message);
        return null;
    }
}

// ------------------------------------------------------------------ gateway

function gql(operation, variables, extraHeaders) {
    const headers = Object.assign({ "Content-Type": "application/json" }, CLIENT,
                                  extraHeaders || {});
    const token = optionalToken();
    if (token)
        headers.Authorization = "Bearer " + token;

    const res = http.post(GATEWAY + "?operationName=" + operation,
                          JSON.stringify({ operationName: operation, query: QUERIES[operation],
                                           variables: variables }),
                          { headers: headers, timeout: 30 });
    let json = null;
    try {
        json = res.json();
    } catch (e) {
        throw new Error("TV4 Play answered HTTP " + res.status);
    }
    // GraphQL answers with what it could and a list of what it could not.
    // A part missing is not worth failing over; nothing at all is.
    const errors = Array.isArray(json.errors) ? json.errors : [];
    if (errors.length)
        console.warn(operation + ": " + errors.map(e => e && e.message).join("; "));
    if (!json.data) {
        const why = errors.length && errors[0].message ? errors[0].message : "HTTP " + res.status;
        throw new Error("TV4 Play: " + why);
    }
    return json.data;
}

// Every item of a paged list. 'fetch(offset, limit)' returns the content
// object, { pageInfo: { totalCount, hasNextPage, nextPageOffset }, items };
// the first page says how many there are, and the rest are fetched side
// by side.
function allPages(operation, variablesFor, contentOf, limit) {
    limit = limit || PAGE_SIZE;
    const first = contentOf(gql(operation, variablesFor(0, limit)));
    if (!first)
        return [];
    let items = (first.items || []).slice();
    const info = first.pageInfo || {};
    const total = Math.min(Number(info.totalCount) || 0, MAX_ITEMS);
    if (!info.hasNextPage && !(total > items.length))
        return items;

    const requests = [];
    for (let offset = Number(info.nextPageOffset) || limit; offset < total; offset += limit)
        requests.push(offset);
    const token = optionalToken();
    const headers = Object.assign({ "Content-Type": "application/json" }, CLIENT);
    if (token)
        headers.Authorization = "Bearer " + token;
    const responses = http.all(requests.map(offset => ({
        url: GATEWAY + "?operationName=" + operation,
        method: "POST",
        headers: headers,
        timeout: 30,
        body: JSON.stringify({ operationName: operation, query: QUERIES[operation],
                               variables: variablesFor(offset, limit) })
    })));
    responses.forEach((res, i) => {
        try {
            const content = res.ok && contentOf(res.json().data || {});
            if (content && content.items)
                items = items.concat(content.items);
        } catch (e) {
            console.warn(operation + " at " + requests[i] + ": " + e.message);
        }
    });
    return items;
}

// ------------------------------------------------------------------ entries

// The best picture of an object's 'images', through TV4's image proxy.
function image(images) {
    if (!images)
        return "";
    for (const kind of ["main16x9Annotated", "main16x9", "image16x9", "cover2x3",
                        "poster2x3", "image4x3", "image2x3"]) {
        const img = images[kind];
        if (!img || img.isFallback)
            continue;
        const encoded = img.sourceEncoded || (img.source && encodeURIComponent(img.source));
        if (encoded)
            return IMAGES + "?source=" + encoded + "&width=" + IMAGE_WIDTH;
    }
    return "";
}

function iso(value) {
    return value && value.isoString ? value.isoString : undefined;
}

function later(value) {
    const t = value && value.isoString ? Date.parse(value.isoString) : NaN;
    return isFinite(t) && t > Date.now();
}

// Lines of facts, the ones there are, one per line.
function lines() {
    return Array.prototype.slice.call(arguments).filter(s => s).join("\n");
}

const PAID = "Needs a TV4 Play subscription";
const DRM = "Protected with DRM: cannot be played here";

function video(obj, extra) {
    const v = obj.video || obj.clipVideo || {};
    const notes = [];
    if (obj.isLiveContent || v.isLiveContent)
        notes.push("Live");
    if (obj.upsell)
        notes.push(PAID);
    if (v.isDrmProtected || obj.isDrmProtected)
        notes.push(DRM);
    const until = obj.playableUntil && obj.playableUntil.readableDistance;
    return {
        id: "v:" + obj.id,
        type: "video",
        name: util.text(obj.title) || "(untitled)",
        description: lines(notes.join("  ·  "), extra,
                           util.text(obj.synopsis && (obj.synopsis.medium || obj.synopsis.long ||
                                                      obj.synopsis.brief || obj.synopsis.short)),
                           until ? "Available: " + until : ""),
        image: image(obj.images),
        duration: Number(v.duration && v.duration.seconds) || 0,
        date: iso(obj.playableFrom)
    };
}

function series(obj) {
    const facts = [];
    if (Array.isArray(obj.genres) && obj.genres.length)
        facts.push(obj.genres.slice(0, 3).join(", "));
    const seasons = Number(obj.numberOfAvailableSeasons);
    if (seasons > 1)
        facts.push(seasons + " seasons");
    if (obj.label && obj.label.airtime)
        facts.push(util.text(obj.label.airtime));
    return {
        id: "series:" + obj.id,
        type: "folder",
        name: util.text(obj.title) || "(untitled)",
        description: lines(obj.upsell ? PAID : "", facts.join("  ·  "),
                           util.text(obj.synopsis && (obj.synopsis.brief || obj.synopsis.long))),
        image: image(obj.images)
    };
}

function movie(obj) {
    const facts = [];
    if (obj.productionYear)
        facts.push(String(obj.productionYear));
    if (Array.isArray(obj.genres) && obj.genres.length)
        facts.push(obj.genres.slice(0, 3).join(", "));
    return video(obj, facts.join("  ·  "));
}

function folder(id, title, description, images) {
    return { id: id, type: "folder", name: util.text(title) || "(untitled)",
             description: description || "", image: image(images) };
}

// What a listing's objects are, by __typename, when they are the thing
// itself rather than a panel item wrapped around it.
const KINDS = {
    Series: o => series(o),
    Movie: o => movie(o),
    Episode: o => later(o.playableFrom) ? null
        : video(o, o.series && o.series.title ? util.text(o.series.title) : ""),
    Clip: o => video(o, o.parent && o.parent.title ? util.text(o.parent.title) : ""),
    SportEvent: o => video(o, [o.league, o.round].filter(s => s).join("  ·  ")),
    Channel: o => {
        const entry = video(o, util.text(o.description || o.tagline));
        entry.description = lines("Live channel", entry.description);
        return entry;
    },
    PageReference: o => folder("page:" + o.id, o.title, "", o.images),
    Page: o => folder("page:" + o.id, o.title, "", o.images),
    MediaPanel: o => panel(o),
    ClipsPanel: o => panel(o),
    EpisodesPanel: o => panel(o),
    SportEventPanel: o => panel(o),
    PagePanel: o => panel(o),
    ChannelPanel: o => panel(o),
    LivePanel: o => o.title ? folder("live:" + o.id, o.title, "Live now") : null,
    SinglePanel: o => {
        const link = o.link || {};
        if (link.page && link.page.id)
            return folder("page:" + link.page.id, o.title, util.text(o.pitch), o.images);
        return entryOf(link);
    },
    ThemePanel: o => {
        const target = o.link && entryOf(o.link);
        if (!target)
            return null;
        target.name = util.text(o.title) || target.name;
        target.description = target.description || util.text(o.pitch || o.subtitle);
        target.image = target.image || image(o.images);
        return target;
    }
};

function panel(o) {
    return o.title ? folder("panel:" + o.id, o.title) : null;
}

// Panel items name what they hold by key ({ series }, { movie }, ...),
// with or without a __typename of their own.
const WRAPPED = [["series", "Series"], ["movie", "Movie"], ["episode", "Episode"],
                 ["clip", "Clip"], ["sportEvent", "SportEvent"], ["channel", "Channel"],
                 ["pageReference", "PageReference"], ["page", "PageReference"]];

// Any object of a listing as an entry; null for what is not for here
// (polls, promotions, continue-watching, an episode not out yet).
function entryOf(obj) {
    if (!obj || typeof obj !== "object")
        return null;
    const kind = KINDS[obj.__typename];
    if (kind)
        return kind(obj);
    for (const [key, typename] of WRAPPED) {
        const inner = obj[key];
        // The key says what it is, whatever the inner object says (an
        // episode's own 'series' is where it belongs, not what it is).
        if (inner && typeof inner === "object")
            return entryOf(Object.assign({}, inner, { __typename: typename }));
    }
    if (obj.staticPage)
        return obj.staticPage.id === "alphabetical"
            ? folder("index", obj.staticPage.title || "Program A-Ö", "", obj.staticPage.images)
            : null;
    return null;
}

function entries(items) {
    const out = [];
    const seen = {};
    (items || []).forEach(item => {
        const entry = entryOf(item);
        if (entry && !seen[entry.id]) {
            seen[entry.id] = true;
            out.push(entry);
        }
    });
    return out;
}

// ------------------------------------------------------------------ folders

function browsePanel(id, operation) {
    return util.memo(id, CACHE_SEC, () => entries(allPages(
        operation,
        (offset, limit) => ({ panelId: id.slice(id.indexOf(":") + 1), offset: offset,
                              limit: limit }),
        data => data.panel && data.panel.content)));
}

function browsePageList(id) {
    return util.memo(id, CACHE_SEC, () => {
        const data = gql("PageList", { pageListId: id.slice("pagelist:".length) });
        return entries(data.pageList && data.pageList.content);
    });
}

function browsePage(id) {
    return util.memo(id, CACHE_SEC, () => entries(allPages(
        "Page",
        (offset, limit) => ({ pageId: id.slice("page:".length),
                              input: { offset: offset, limit: limit } }),
        data => data.page && data.page.content &&
            { pageInfo: data.page.content.pageInfo, items: data.page.content.panels })));
}

function browseIndex() {
    return util.memo("index", CACHE_SEC, () => {
        const letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ".split("");
        const items = allPages(
            "MediaIndex",
            (offset, limit) => ({ input: { letterFilters: letters, limit: limit,
                                           offset: offset } }),
            data => data.mediaIndex && data.mediaIndex.contentList);
        return entries(items);
    });
}

function browseSeason(seasonId) {
    return util.memo("season:" + seasonId, CACHE_SEC, () => entries(allPages(
        "SeasonEpisodes",
        (offset, limit) => ({ seasonId: seasonId, input: { offset: offset, limit: limit } }),
        data => data.season && data.season.episodes)));
}

function browseSeries(id) {
    return util.memo(id, CACHE_SEC, () => {
        let data;
        try {
            data = gql("ContentDetailsPage",
                       { mediaId: id.slice("series:".length),
                         panelsInput: { offset: 0, limit: 20 } },
                       { feature_flag_enable_season_upsell_on_cdp: "true" });
        } catch (e) {
            if (!refreshToken())
                throw new Error("TV4 Play lists a series' episodes only to a signed-in " +
                                "account (Settings, TV4 Play)");
            throw e;
        }
        const media = data.media || {};
        if (media.__typename === "Movie")
            return [movie(media)];
        const seasons = (media.allSeasonLinks || []).filter(s => s && s.seasonId);
        // One season: its episodes, without a folder in between.
        if (seasons.length === 1)
            return browseSeason(seasons[0].seasonId);
        return seasons.map(s => folder(
            "season:" + s.seasonId,
            s.title || "Season",
            Number(s.numberOfEpisodes) > 0 ? s.numberOfEpisodes + " episodes" : "",
            media.images));
    });
}

function root() {
    const out = [];
    if (!refreshToken())
        out.push({
            id: "signin",
            type: "other",
            name: "Sign in to play",
            description: "TV4 Play plays only for a signed-in account (a free one will do). " +
                "Everything can be browsed without one. Settings, TV4 Play, Sign-in token " +
                "says how."
        });
    PANELS.forEach(p => out.push(folder("panel:" + p[0], p[1], p[2])));
    out.push(folder("pagelist:categories", "Kategorier", "Drama, news, sport, children, ..."));
    out.push(folder("index", "Program A-Ö", "Every series and film"));
    return out;
}

// ------------------------------------------------------------------ search

const SEARCH_GROUPS = ["movieSeries", "movieSeriesUpsell", "sportEvent", "sportEventUpsell",
                       "clip", "page"];

function search(query) {
    const data = gql("PanelSearch", {
        input: { query: query }, limit: 40, offset: 0,
        shouldFetchMovieSeries: true, shouldFetchMovieSeriesUpsell: true,
        shouldFetchClip: true, shouldFetchPage: true,
        shouldFetchSportEvent: true, shouldFetchSportEventUpsell: true
    });
    const groups = (data.panelSearch && data.panelSearch.data) || {};
    let items = [];
    SEARCH_GROUPS.forEach(name => {
        const group = groups[name];
        if (group && group.content && Array.isArray(group.content.items))
            items = items.concat(group.content.items);
    });
    return entries(items);
}

// ------------------------------------------------------------------ playback

function playbackUrl(id) {
    return PLAYBACK + encodeURIComponent(id) + "?" + util.query({
        service: "tv4play",
        device: "browser",
        protocol: "hls,dash",
        drm: "widevine",
        browser: "GoogleChrome",
        capabilities: "live-drm-adstitch-2,yospace3"
    });
}

function resolve(id) {
    if (id === "signin")
        throw new Error("Set the sign-in token under Settings, TV4 Play; it says how to " +
                        "get one");
    if (!id.startsWith("v:"))
        throw new Error("nothing to play here");
    const token = accessToken();
    if (!token)
        throw new Error("TV4 Play plays only for a signed-in account. Set the sign-in token " +
                        "under Settings, TV4 Play.");

    const res = http.get(playbackUrl(id.slice(2)), {
        headers: { Authorization: "Bearer " + token, "x-jwt": "Bearer " + token },
        timeout: 20
    });
    let json = null;
    try {
        json = res.json();
    } catch (e) {
        // Said below.
    }
    if (!res.ok) {
        const said = json && (json.message || (json.error && json.error.message) ||
                              json.errorCode);
        if (res.status === 401)
            throw new Error("TV4 Play did not accept the sign-in (HTTP 401). Set a new " +
                            "sign-in token under Settings, TV4 Play.");
        if (res.status === 403 || res.status === 451)
            throw new Error("Not available to this account here" +
                            (said ? ": " + said : " (it may need a subscription, or be " +
                             "blocked outside Sweden)"));
        throw new Error("TV4 Play could not start this" + (said ? ": " + said
                                                           : " (HTTP " + res.status + ")"));
    }

    const meta = (json && json.metadata) || {};
    if (meta.isDrmProtected)
        throw new Error("This is protected with DRM, which cannot be played here");
    const item = (json && json.playbackItem) || {};
    if (!item.manifestUrl)
        throw new Error("TV4 Play sent nothing to play");
    // Foreign titles carry a dubbed track, "qas", that the stream marks as
    // its default; svtplay-dl starts the other one instead, and so does
    // this (unless the preferred audio language asks for the dub).
    return { uri: item.manifestUrl, avoidAudio: ["qas"] };
}

// ------------------------------------------------------------------ provider

return {
    name: "TV4 Play",
    settings: [{
        key: "token",
        type: "text",
        secret: true,
        label: "Sign-in token",
        about: "TV4 Play plays only for a signed-in account; a free one will do. Sign in " +
            "at tv4play.se in a computer's browser, open the developer tools (F12), and " +
            "under Application (Storage in Firefox), Cookies, copy the value of " +
            "tv4-refresh-token: a long text starting with ey. Paste it here. On a " +
            "console, put the line tv4play.token=ey... in a file settings.ini in the " +
            "folder of your own plugins over FTP (the log names it at start), then " +
            "search the Plugins tab. Left or right " +
            "signs out."
    }],
    discover() {
        return [{
            name: "TV4 Play",
            detail: "TV4, Sjuan, TV12: series, films, news",
            icon: "📺",
            root: "/",
            browse(id) {
                if (id === "/")
                    return root();
                if (id === "signin")
                    return [];
                if (id.startsWith("panel:"))
                    return browsePanel(id, "Panel");
                if (id.startsWith("live:"))
                    return browsePanel(id, "LivePanel");
                if (id.startsWith("pagelist:"))
                    return browsePageList(id);
                if (id.startsWith("page:"))
                    return browsePage(id);
                if (id === "index")
                    return browseIndex();
                if (id.startsWith("series:"))
                    return browseSeries(id);
                if (id.startsWith("season:"))
                    return browseSeason(id.slice("season:".length));
                throw new Error("unknown folder " + id);
            },
            search(query) {
                return search(query);
            },
            resolve(id) {
                return resolve(id);
            }
        }];
    }
};
