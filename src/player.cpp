// SPDX-License-Identifier: GPL-3.0-or-later
//
// See player.h.
#include "player.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <utility>

#include <SDL.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>

#include "net/http_client.h"

namespace {

// A stalled server is mpv's to notice (network-timeout: no data for that
// long); a slow but live one may take as long as it takes, and the user can
// back out with AbortOpen(). This only bounds an mpv that never answers.
constexpr auto kOpenTimeout = std::chrono::minutes(5);

// A reported duration that moves by more than this once playback has started
// is an estimate, not the length of the file (see Player::DurationReliable).
constexpr int64_t kDurationDriftUs = 2 * 1000000;

// How far ahead of playback mpv reads a live stream, in seconds. An audio
// track picked on a live stream (another language: an HLS rendition with a
// playlist of its own) joins where FFmpeg's HLS demuxer is reading, not
// where playback is: mpv cannot seek a live demuxer back to fetch it from
// the play position, as it does for files and on-demand streams. Whatever
// is read ahead is how far in the future the new track starts. With mpv's
// default that is the whole live window, ten seconds and more, and the
// sound either stays silent that long or runs that far ahead of the
// picture. At one second the new language is heard at once and the two
// are back in step within about a second. The price is less buffer
// against a network hiccup; raise this if live channels start to stall.
constexpr const char* kLiveReadaheadSecs = "1";

// FFmpeg's http options for plain files: DLNA servers recycle sockets
// aggressively, and a dropped transfer should pick up where it was.
constexpr const char* kReconnectOptions =
  "reconnect=1,reconnect_streamed=1,reconnect_delay_max=5";

// True for an HLS or DASH playlist (".m3u8", ".mpd", in the path or the
// query). Its segments are fetched with the options of the playlist, and
// FFmpeg's reconnect on a segment a CDN keeps cutting short is a busy loop
// ("Will reconnect at N in 0 second(s)", thousands a second) that never
// gives up; without it the segment is skipped and playback goes on.
bool IsAdaptivePlaylist(const std::string& url) {
  std::string lower = url;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return lower.find(".m3u8") != std::string::npos ||
         lower.find(".mpd") != std::string::npos;
}

// True for a list of clips, an mpv EDL whose parts are HLS playlists carried
// inline as data: URIs (see plugins/plutotv.js): one title in several clips
// that play as a single timeline. It has no file name to probe, so it is
// only ever told apart by its scheme.
bool IsClipList(const std::string& url) {
  return url.compare(0, 6, "edl://") == 0;
}

// 'ref' (a URI from inside a playlist) made absolute against 'base' (the
// playlist's own URL, after redirects).
std::string ResolveUrl(const std::string& base, const std::string& ref) {
  if (ref.find("://") != std::string::npos)
    return ref;
  const size_t scheme_end = base.find("://");
  if (scheme_end == std::string::npos)
    return ref;
  if (ref.compare(0, 2, "//") == 0)
    return base.substr(0, scheme_end + 1) + ref;
  const size_t host_end = base.find('/', scheme_end + 3);
  if (!ref.empty() && ref[0] == '/')
    return base.substr(0, host_end) + ref;
  const std::string path = base.substr(0, base.find_first_of("?#"));
  const size_t dir_end = path.rfind('/');
  return (dir_end != std::string::npos && dir_end >= scheme_end + 3
            ? path.substr(0, dir_end + 1) : path + "/") + ref;
}

// Whether the HLS or DASH stream at 'url' is live (a playlist that keeps
// growing) rather than on demand. Looks at the manifest itself: an HLS
// media playlist is on demand once it has #EXT-X-ENDLIST (a master playlist
// says nothing, so its first variant is asked), a DASH manifest is live when
// its type is "dynamic". Anything that cannot be fetched or read counts as
// live, which is what every adaptive stream was taken for before.
bool IsLiveStream(const std::string& url) {
  net::HttpClient http;
  net::HttpClient::Request req;
  req.stall_timeout_ms = 4000;
  req.deadline_ms = 5000;
  req.max_bytes = 4u << 20; // a long title, short segments, signed URLs
  req.truncate = true;
  net::HttpClient::Response res;
  std::string error;

  req.url = url;
  if (!http.Perform(req, res, error) || res.status != 200)
    return true;
  if (res.body.find("<MPD") != std::string::npos)
    return res.body.find("\"dynamic\"") != std::string::npos ||
           res.body.find("'dynamic'") != std::string::npos;
  if (res.body.compare(0, 7, "#EXTM3U") != 0)
    return true;

  // A master playlist: the variants are all live or all on demand, so the
  // first one speaks for them.
  const size_t inf = res.body.find("#EXT-X-STREAM-INF");
  if (inf != std::string::npos) {
    size_t at = res.body.find('\n', inf);
    std::string variant;
    while (at != std::string::npos && variant.empty()) {
      const size_t end = res.body.find('\n', at + 1);
      std::string line = res.body.substr(at + 1, end == std::string::npos
                                                   ? std::string::npos : end - at - 1);
      while (!line.empty() && std::isspace((unsigned char)line.back()))
        line.pop_back();
      if (!line.empty() && line[0] != '#')
        variant = line;
      at = end;
    }
    if (variant.empty())
      return true;
    req.url = ResolveUrl(res.url.empty() ? url : res.url, variant);
    res = {};
    if (!http.Perform(req, res, error) || res.status != 200 ||
        res.body.compare(0, 7, "#EXTM3U") != 0)
      return true;
  }
  return res.body.find("#EXT-X-ENDLIST") == std::string::npos &&
         res.body.find("#EXT-X-PLAYLIST-TYPE:VOD") == std::string::npos;
}

// The spectrum shown for audio-only files, fed from track 'aid'. It is a
// lavfi graph: the audio goes to the output untouched ([ao]), a copy is
// drawn as a tiny white-on-black bar spectrum ([vo]). That picture is not
// for looking at: the visualizer has mpv render it into an offscreen
// framebuffer, reads the bar heights back and draws its own scene from
// them. mpv presents the frames against its audio clock, so the data
// arrives in step with what is audible, with no PCM tap needed.
// No alpha channel: mpv would draw its transparency checkerboard behind
// the bars, and every column would read as full.
std::string VisualizerGraph(int aid) {
  char graph[512];
  std::snprintf(graph, sizeof(graph),
                "[aid%d] asplit [ao] [viz];"
                "[viz] showfreqs=s=%dx%d:mode=bar:ascale=log:fscale=log:"
                "win_size=2048:overlap=0.5:averaging=1:cmode=combined:"
                "colors=white,format=rgb24 [vo]",
                aid, Player::kSpectrumWidth, Player::kSpectrumHeight);
  return graph;
}

// Property ids for mpv_observe_property's reply_userdata.
enum : uint64_t {
  kPropTimePos = 1,
  kPropDuration,
  kPropPause,
  kPropSeekable,
  kPropVid,
  kPropAid,
  kPropTrackList,
  kPropSid,
  kPropCacheTime,
  kPropSpeed,
  kPropChapterList,
  kPropChapter,
};

const mpv_node* MapGet(const mpv_node* map, const char* key) {
  if (!map || map->format != MPV_FORMAT_NODE_MAP || !map->u.list)
    return nullptr;
  for (int i = 0; i < map->u.list->num; i++)
    if (!std::strcmp(map->u.list->keys[i], key))
      return &map->u.list->values[i];
  return nullptr;
}

std::string NodeString(const mpv_node* node) {
  return (node && node->format == MPV_FORMAT_STRING && node->u.string)
           ? std::string(node->u.string) : std::string();
}

int64_t NodeInt(const mpv_node* node, int64_t fallback) {
  if (!node)
    return fallback;
  if (node->format == MPV_FORMAT_INT64)
    return node->u.int64;
  if (node->format == MPV_FORMAT_DOUBLE)
    return (int64_t)node->u.double_;
  return fallback;
}

bool NodeFlag(const mpv_node* node) {
  return node && node->format == MPV_FORMAT_FLAG && node->u.flag;
}

// Seconds (an mpv double property) to microseconds, -1 if unknown.
int64_t PropertyUs(const mpv_event_property* prop) {
  if (prop->format != MPV_FORMAT_DOUBLE || !prop->data)
    return -1;
  const double s = *static_cast<const double*>(prop->data);
  return std::isfinite(s) && s >= 0 ? (int64_t)(s * 1e6) : -1;
}

int64_t PropertyInt(const mpv_event_property* prop, int64_t fallback) {
  if (prop->format != MPV_FORMAT_INT64 || !prop->data)
    return fallback;
  return *static_cast<const int64_t*>(prop->data);
}

bool PropertyFlag(const mpv_event_property* prop) {
  return prop->format == MPV_FORMAT_FLAG && prop->data &&
         *static_cast<const int*>(prop->data);
}

void* GetProcAddress(void*, const char* name) {
  return SDL_GL_GetProcAddress(name);
}

} // namespace

Player::Player() = default;

Player::~Player() {
  Shutdown();
}

bool Player::Initialize(std::string& error, const std::string& fonts_dir) {
  if (mpv_)
    return true;

  mpv_ = mpv_create();
  if (!mpv_) {
    error = "mpv_create failed";
    return false;
  }

  // The window is the app's; mpv draws through the render API and takes no
  // input. Everything else is tuned for playing from a network server.
  // 'quiet' options are left out silently by an mpv that doesn't know them
  // (the built-in Lua scripts differ between versions; none is wanted).
  struct Option { const char* name; const char* value; bool quiet; };
  static const Option options[] = {
    // Bilinear scaling, no dithering, no LUT-based scalers: what the PS4's
    // GPU affords at 1080p next to the UI. (The LUT scalers also came up
    // black on the first file with some drivers.)
    {"profile", "fast", false},
    {"vo", "libmpv", false},
    {"idle", "yes", false},
    {"keep-open", "no", false},
    {"terminal", "no", false},
    {"config", "no", false},
    {"load-scripts", "no", false},
    {"osc", "no", true},
    {"load-stats-overlay", "no", true},
    {"load-console", "no", true},
    {"load-osd-console", "no", true},
    {"load-auto-profiles", "no", true},
    {"load-select", "no", true},
    {"load-commands", "no", true},
    {"load-context-menu", "no", true},
    {"load-positioning", "no", true},
    {"ytdl", "no", true},
    {"input-default-bindings", "no", false},
    {"input-vo-keyboard", "no", false},
    {"osd-level", "0", false},
    {"hwdec", "no", false},
    // Cover art is shown by the UI, from the server's metadata.
    {"audio-display", "no", false},
    {"network-timeout", "30", false},
    // HLS and DASH: the highest-bitrate variant, for the whole stream (there
    // is no switching between video tracks; see player.h).
    {"hls-bitrate", "max", false},
    // Set again for every file by Open(); see kReconnectOptions.
    {"stream-lavf-o", kReconnectOptions, false},
#if defined(__ORBIS__)
    // The demuxer cache comes out of the same pool as the GPU's memory.
    {"demuxer-max-bytes", "64MiB", false},
    {"demuxer-max-back-bytes", "16MiB", false},
#endif
  };
  for (const Option& o : options) {
    const int rc = mpv_set_option_string(mpv_, o.name, o.value);
    if (rc < 0 && !o.quiet)
      std::fprintf(stderr, "mpv: option %s=%s: %s\n", o.name, o.value,
                   mpv_error_string(rc));
  }
  if (const char* ca = std::getenv("SSL_CERT_FILE"))
    mpv_set_option_string(mpv_, "tls-ca-file", ca);

  // Subtitles in the UI's typeface, from the app's own font directory, so
  // text subtitles render the same with and without a system font setup.
  if (!fonts_dir.empty()) {
    mpv_set_option_string(mpv_, "sub-fonts-dir", fonts_dir.c_str());
    mpv_set_option_string(mpv_, "osd-fonts-dir", fonts_dir.c_str());
    mpv_set_option_string(mpv_, "sub-font", "LatoLatin");
    mpv_set_option_string(mpv_, "sub-bold", "yes");
  }

  // Extra mpv options for testing and tuning, as "name=value" words:
  //   JTPLAY_MPV="hwdec=auto-safe ao=pulse" ./jtplay
  if (const char* extra = std::getenv("JTPLAY_MPV")) {
    std::string words = extra;
    size_t at = 0;
    while (at < words.size()) {
      const size_t end = std::min(words.find(' ', at), words.size());
      const std::string word = words.substr(at, end - at);
      const size_t eq = word.find('=');
      if (eq != std::string::npos) {
        const int rc = mpv_set_option_string(mpv_, word.substr(0, eq).c_str(),
                                             word.substr(eq + 1).c_str());
        if (rc < 0)
          std::fprintf(stderr, "mpv: option %s: %s\n", word.c_str(), mpv_error_string(rc));
      }
      at = end + 1;
    }
  }

  int rc = mpv_initialize(mpv_);
  if (rc < 0) {
    error = std::string("mpv_initialize: ") + mpv_error_string(rc);
    mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
    return false;
  }

  // The read-ahead files and on-demand streams get, JTPLAY_MPV included;
  // Open() shortens it for live streams and puts this back for the rest.
  for (auto [name, into] : {std::pair{"cache-secs", &cache_secs_},
                            std::pair{"demuxer-readahead-secs", &readahead_secs_}}) {
    if (char* value = mpv_get_property_string(mpv_, name)) {
      *into = value;
      mpv_free(value);
    }
  }

  mpv_opengl_init_params gl_init = {};
  gl_init.get_proc_address = GetProcAddress;
  mpv_render_param params[] = {
    {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
    {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
    {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  rc = mpv_render_context_create(&render_, mpv_, params);
  if (rc < 0) {
    error = std::string("mpv's OpenGL renderer: ") + mpv_error_string(rc);
    mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
    render_ = nullptr;
    return false;
  }

  // mpv's messages go to stderr; JTPLAY_MPV_LOG=v (or debug) says more.
  const char* log_level = std::getenv("JTPLAY_MPV_LOG");
  mpv_request_log_messages(mpv_, log_level && *log_level ? log_level : "warn");
  mpv_observe_property(mpv_, kPropTimePos, "time-pos", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, kPropDuration, "duration", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, kPropPause, "pause", MPV_FORMAT_FLAG);
  mpv_observe_property(mpv_, kPropSeekable, "seekable", MPV_FORMAT_FLAG);
  mpv_observe_property(mpv_, kPropVid, "vid", MPV_FORMAT_INT64);
  mpv_observe_property(mpv_, kPropAid, "aid", MPV_FORMAT_INT64);
  mpv_observe_property(mpv_, kPropTrackList, "track-list", MPV_FORMAT_NODE);
  mpv_observe_property(mpv_, kPropSid, "sid", MPV_FORMAT_INT64);
  mpv_observe_property(mpv_, kPropCacheTime, "demuxer-cache-time", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, kPropSpeed, "speed", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, kPropChapterList, "chapter-list", MPV_FORMAT_NODE);
  mpv_observe_property(mpv_, kPropChapter, "chapter", MPV_FORMAT_INT64);

  quitting_ = false;
  event_thread_ = std::thread(&Player::EventThread, this);
  return true;
}

void Player::Shutdown() {
  if (!mpv_)
    return;
  active_ = false;
  quitting_ = true;
  mpv_wakeup(mpv_);
  if (event_thread_.joinable())
    event_thread_.join();
  // The renderer goes first, while the GL context is still current.
  if (render_)
    mpv_render_context_free(render_);
  render_ = nullptr;
  mpv_terminate_destroy(mpv_);
  mpv_ = nullptr;
}

void Player::Command(const char* const* args) {
  if (mpv_)
    mpv_command_async(mpv_, 0, const_cast<const char**>(args));
}

void Player::ResetFileState() {
  active_ = false;
  playing_id_ = -1;
  seekable_ = false;
  paused_ = false;
  position_us_ = -1;
  duration_us_ = -1;
  video_id_ = -1;
  audio_id_ = -1;
  sub_id_ = -1;
  buffered_us_ = -1;
  chapter_ = -1;
  {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    chapters_.clear();
    video_tracks_.clear();
    audio_tracks_.clear();
    sub_tracks_.clear();
  }
  vtrack_count_ = 0;
  atrack_count_ = 0;
}

// ---------------------------------------------------------------------------
// Opening and stopping (app worker thread)
// ---------------------------------------------------------------------------

bool Player::Open(const std::string& url, std::string& error, int64_t start_seconds) {
  if (!mpv_) {
    error = "the player is not available";
    return false;
  }

  Stop();

  // Whatever the last file left set is undone before the next one starts.
  mpv_set_property_string(mpv_, "pause", "no");
  mpv_set_property_string(mpv_, "speed", "1");
  speed_ = 1.0;
  mpv_set_property_string(mpv_, "sub-delay", "0");
  sub_delay_ = 0.0;
  // "start" applies to every file loaded after it is set, so it is set
  // (or reset) for each one.
  const std::string start = start_seconds > 0 ? std::to_string(start_seconds) : "none";
  mpv_set_property_string(mpv_, "start", start.c_str());
  // Also per file: reconnecting suits plain files, not HLS/DASH segments.
  const bool clips = IsClipList(url);
  const bool adaptive = clips || IsAdaptivePlaylist(url);
  mpv_set_property_string(mpv_, "stream-lavf-o",
                          adaptive ? "" : kReconnectOptions);
  const bool live = adaptive && !clips && IsLiveStream(url);
  // A live stream reads only a little ahead, so an audio switch is quick;
  // see kLiveReadaheadSecs.
  //
  // mpv's timestamp linearizing (demuxer-lavf-linearize-timestamps) is
  // left off on purpose. On a live stream with ads spliced in (Pluto TV)
  // the timestamps start again at every ad break; without it, the shown
  // position jumps back there and the picture holds for half a second.
  // With it, mpv corrects only the tracks it is reading at the time: an
  // audio track picked after the first ad break is never heard, and on an
  // on-demand stream an audio switch left the picture minutes behind.
  for (auto [name, normal] : {std::pair{"cache-secs", &cache_secs_},
                              std::pair{"demuxer-readahead-secs", &readahead_secs_}}) {
    const char* value = live ? kLiveReadaheadSecs : normal->c_str();
    if (*value)
      mpv_set_property_string(mpv_, name, value);
  }
  // The playlists of a clip list are data: URIs, which say nothing about
  // being HLS, so the demuxer is told.
  mpv_set_property_string(mpv_, "demuxer-lavf-format", clips ? "hls" : "");
  // mpv opens the parts of an EDL with the origin of the EDL itself, and
  // some builds count a list handed over as a playlist and so refuse the
  // ffmpeg:// names in it ("Refusing to load potentially unsafe URL from a
  // playlist"). The list is made by a plugin, not read from a server, so it
  // is trusted; other files keep the default.
  mpv_set_property_string(mpv_, "load-unsafe-playlists", clips ? "yes" : "no");

  int64_t starts_before;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    starts_before = started_count_;
    // Together, so an AbortOpen() sees either no open or this one.
    opening_ = true;
    abort_open_ = false;
    end_error_.clear();
    last_log_error_.clear();
  }

  // loadfile answers with the playlist entry it made, whose start, load and
  // end the event thread then reports.
  const char* cmd[] = {"loadfile", url.c_str(), "replace", nullptr};
  mpv_node result = {};
  int rc = mpv_command_ret(mpv_, cmd, &result);
  if (rc < 0) {
    opening_ = false;
    error = std::string("unable to open the stream: ") + mpv_error_string(rc);
    return false;
  }
  int64_t id = NodeInt(MapGet(&result, "playlist_entry_id"), -1);
  mpv_free_node_contents(&result);

  std::unique_lock<std::mutex> lock(mutex_);
  const auto deadline = std::chrono::steady_clock::now() + kOpenTimeout;
  const bool settled = cv_.wait_until(lock, deadline, [&] {
    if (quitting_ || abort_open_)
      return true;
    // An mpv too old to name the entry: the first file started after the
    // command is the one.
    if (id < 0 && started_count_ > starts_before)
      id = started_id_;
    return id >= 0 && (loaded_id_ == id || ended_id_ == id);
  });

  // A file that loaded and is over already (a very short clip) counts as
  // opened; AtEnd() then moves on.
  if (!settled || quitting_ || abort_open_ || loaded_id_ != id) {
    std::string why = !settled ? "timed out" : end_error_;
    if (!last_log_error_.empty())
      why = last_log_error_;
    if (abort_open_)
      why = "cancelled";
    lock.unlock();
    opening_ = false;
    const char* stop[] = {"stop", nullptr};
    mpv_command(mpv_, stop);
    error = "unable to open the stream" + (why.empty() ? "" : ": " + why);
    return false;
  }
  lock.unlock();

  playing_id_ = id;

  // The tracks are known once the file is loaded; ask for them now rather
  // than wait for the property event, so the UI has them from the start.
  mpv_node tracks = {};
  if (mpv_get_property(mpv_, "track-list", MPV_FORMAT_NODE, &tracks) >= 0) {
    UpdateTracks(&tracks);
    mpv_free_node_contents(&tracks);
  }
  int seekable = 0;
  if (mpv_get_property(mpv_, "seekable", MPV_FORMAT_FLAG, &seekable) >= 0)
    seekable_ = seekable != 0;
  mpv_node chapters = {};
  if (mpv_get_property(mpv_, "chapter-list", MPV_FORMAT_NODE, &chapters) >= 0) {
    UpdateChapters(&chapters);
    mpv_free_node_contents(&chapters);
  }

  // Nothing to look at: draw the sound instead.
  if (vtrack_count_ == 0 && atrack_count_ > 0)
    SetVisualizer(audio_id_ >= 0 ? audio_id_.load() : AudioTracks().front().stream_index);

  active_ = true;
  opening_ = false;
  return true;
}

void Player::AbortOpen() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opening_)
    return;
  abort_open_ = true;
  cv_.notify_all();
}

void Player::Stop() {
  if (!mpv_)
    return;
  active_ = false;
  // Synchronous, so a stop the app asks for is done when this returns.
  const char* stop[] = {"stop", nullptr};
  mpv_command(mpv_, stop);
  if (vis_aid_ >= 0)
    SetVisualizer(-1);
  ResetFileState();
}

bool Player::AtEnd() const {
  if (!active_)
    return false;
  std::lock_guard<std::mutex> lock(mutex_);
  return ended_id_ >= 0 && ended_id_ == playing_id_;
}

void Player::SetVisualizer(int aid) {
  const std::string graph = aid >= 0 ? VisualizerGraph(aid) : std::string();
  const int rc = mpv_set_property_string(mpv_, "lavfi-complex", graph.c_str());
  if (rc < 0) {
    std::fprintf(stderr, "mpv: no spectrum: %s\n", mpv_error_string(rc));
    vis_aid_ = -1;
    return;
  }
  vis_aid_ = aid;
}

// ---------------------------------------------------------------------------
// Events (event thread)
// ---------------------------------------------------------------------------

void Player::EventThread() {
  while (!quitting_) {
    mpv_event* event = mpv_wait_event(mpv_, -1);
    switch (event->event_id) {
    case MPV_EVENT_SHUTDOWN:
      quitting_ = true;
      cv_.notify_all();
      return;

    case MPV_EVENT_LOG_MESSAGE: {
      const auto* msg = static_cast<const mpv_event_log_message*>(event->data);
      // A message repeated back to back is counted, not printed again: a
      // flood of them (a network retry loop) would otherwise keep this
      // thread busy writing to a slow console while events pile up.
      if (log_repeat_text_ == msg->text) {
        log_repeats_++;
      } else {
        if (log_repeats_ > 0)
          std::fprintf(stderr, "mpv: last message repeated %d more times\n",
                       log_repeats_);
        log_repeats_ = 0;
        log_repeat_text_ = msg->text;
        std::fprintf(stderr, "mpv[%s] %s: %s", msg->prefix, msg->level, msg->text);
      }
      if (msg->log_level <= MPV_LOG_LEVEL_ERROR) {
        std::string text = msg->text;
        while (!text.empty() && (text.back() == '\n' || text.back() == '.'))
          text.pop_back();
        std::lock_guard<std::mutex> lock(mutex_);
        last_log_error_ = text;
      }
      // mpv's EDL demuxer warns "EDL: source file <url> has unknown duration"
      // and then makes the timeline as long as what it has seen so far.
      if (std::strstr(msg->text, "has unknown duration"))
        duration_unreliable_ = true;
      break;
    }

    case MPV_EVENT_START_FILE: {
      const auto* start = static_cast<const mpv_event_start_file*>(event->data);
      std::lock_guard<std::mutex> lock(mutex_);
      started_count_++;
      started_id_ = start ? start->playlist_entry_id : -1;
      duration_unreliable_ = false; // per file; the log may set it again
      cv_.notify_all();
      break;
    }

    case MPV_EVENT_FILE_LOADED: {
      std::lock_guard<std::mutex> lock(mutex_);
      loaded_id_ = started_id_;
      cv_.notify_all();
      break;
    }

    case MPV_EVENT_END_FILE: {
      const auto* end = static_cast<const mpv_event_end_file*>(event->data);
      std::lock_guard<std::mutex> lock(mutex_);
      ended_id_ = end->playlist_entry_id;
      if (end->reason != MPV_END_FILE_REASON_STOP && end->reason != MPV_END_FILE_REASON_REDIRECT)
        std::fprintf(stderr, "mpv: entry %lld ended (%s)\n", (long long)end->playlist_entry_id,
                     end->reason == MPV_END_FILE_REASON_EOF ? "eof"
                     : end->reason == MPV_END_FILE_REASON_ERROR ? mpv_error_string(end->error)
                     : "quit");
      if (end->reason == MPV_END_FILE_REASON_ERROR)
        end_error_ = mpv_error_string(end->error);
      cv_.notify_all();
      break;
    }

    case MPV_EVENT_PROPERTY_CHANGE: {
      const auto* prop = static_cast<const mpv_event_property*>(event->data);
      switch (event->reply_userdata) {
      case kPropTimePos:
        position_us_ = PropertyUs(prop);
        break;
      case kPropDuration: {
        // A real duration is fixed once playback has started. One that keeps
        // moving (a live playlist that grows, a bitrate estimate) is not
        // something to compare the position against.
        const int64_t now = PropertyUs(prop);
        const int64_t before = duration_us_.load();
        if (now >= 0 && before >= 0 && position_us_.load() >= 0 &&
            std::llabs(now - before) > kDurationDriftUs)
          duration_unreliable_ = true;
        duration_us_ = now;
        break;
      }
      case kPropPause:
        paused_ = PropertyFlag(prop);
        break;
      case kPropSeekable:
        seekable_ = PropertyFlag(prop);
        break;
      case kPropVid:
        video_id_ = (int)PropertyInt(prop, -1);
        break;
      case kPropAid:
        audio_id_ = (int)PropertyInt(prop, -1);
        break;
      case kPropSid:
        sub_id_ = (int)PropertyInt(prop, -1); // "no" arrives as no int
        break;
      case kPropCacheTime:
        buffered_us_ = PropertyUs(prop);
        break;
      case kPropSpeed:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
          speed_ = *static_cast<const double*>(prop->data);
        break;
      case kPropTrackList:
        if (prop->format == MPV_FORMAT_NODE && prop->data)
          UpdateTracks(static_cast<const mpv_node*>(prop->data));
        break;
      case kPropChapterList:
        if (prop->format == MPV_FORMAT_NODE && prop->data)
          UpdateChapters(static_cast<const mpv_node*>(prop->data));
        break;
      case kPropChapter:
        chapter_ = (int)PropertyInt(prop, -1);
        break;
      }
      break;
    }

    case MPV_EVENT_COMMAND_REPLY:
    case MPV_EVENT_SET_PROPERTY_REPLY:
      if (event->error < 0)
        std::fprintf(stderr, "mpv: %s\n", mpv_error_string(event->error));
      break;

    default:
      break;
    }
  }
}

// The tracks from mpv's track-list. The label is built from the metadata
// (language, title) plus the codec and, depending on the kind, the channel
// layout or the frame size: "eng · ac3 5.1(side)", "Commentary · aac
// stereo", "h264 1920x1080".
//
// An HLS or DASH stream with its audio inside each variant (Pluto TV) has
// as many audio tracks as variants, all the same sound at another bitrate.
// Only the one of the variant playing is listed: picking another would
// not change the language, only have FFmpeg fetch a second variant, whose
// audio joins seconds away from the picture. FFmpeg marks such a track
// with its variant's bitrate ("hls-bitrate"); a separate audio rendition
// (another language, SVT Play) belongs to every variant and has none, so
// those are all listed.
void Player::UpdateTracks(const mpv_node* list) {
  std::vector<VideoTrackInfo> video;
  std::vector<AudioTrackInfo> audio;
  std::vector<SubtitleTrackInfo> subs;

  // The variant playing: that of the video track shown, or for a stream
  // without video, of the audio track heard. Unknown (-1) for plain files.
  int64_t program = -1, program_bitrate = 0;
  if (list && list->format == MPV_FORMAT_NODE_ARRAY && list->u.list) {
    for (const char* kind : {"video", "audio"}) {
      for (int i = 0; i < list->u.list->num && program < 0; i++) {
        const mpv_node* track = &list->u.list->values[i];
        if (NodeString(MapGet(track, "type")) != kind ||
            !NodeFlag(MapGet(track, "selected")) ||
            NodeFlag(MapGet(track, "albumart")) ||
            NodeInt(MapGet(track, "hls-bitrate"), 0) <= 0)
          continue;
        program = NodeInt(MapGet(track, "program-id"), -1);
        program_bitrate = NodeInt(MapGet(track, "hls-bitrate"), 0);
      }
    }
  }

  if (list && list->format == MPV_FORMAT_NODE_ARRAY && list->u.list) {
    for (int i = 0; i < list->u.list->num; i++) {
      const mpv_node* track = &list->u.list->values[i];
      const std::string type = NodeString(MapGet(track, "type"));
      const bool is_video = type == "video";
      const bool is_sub = type == "sub";
      if (!is_video && !is_sub && type != "audio")
        continue;
      // Cover art is a video track in name only; not selectable.
      if (is_video && NodeFlag(MapGet(track, "albumart")))
        continue;
      // Another variant's copy of the sound; see above.
      if (type == "audio" && program >= 0 && !NodeFlag(MapGet(track, "selected"))) {
        const int64_t bitrate = NodeInt(MapGet(track, "hls-bitrate"), 0);
        if (bitrate > 0 && bitrate != program_bitrate &&
            NodeInt(MapGet(track, "program-id"), -1) != program)
          continue;
      }

      std::string label;
      auto append = [&label](const std::string& part) {
        if (!part.empty())
          label += (label.empty() ? "" : " · ") + part;
      };
      const std::string lang = NodeString(MapGet(track, "lang"));
      if (lang != "und")
        append(lang);
      append(NodeString(MapGet(track, "title")));

      std::string codec = NodeString(MapGet(track, "codec"));
      if (is_sub) {
        if (NodeFlag(MapGet(track, "forced")))
          append("Forced");
        if (NodeFlag(MapGet(track, "external")))
          append("External");
      } else if (is_video) {
        const int64_t w = NodeInt(MapGet(track, "demux-w"), 0);
        const int64_t h = NodeInt(MapGet(track, "demux-h"), 0);
        if (w > 0 && h > 0)
          codec += " " + std::to_string(w) + "x" + std::to_string(h);
      } else {
        // A layout the container doesn't name comes out as "unknownN".
        std::string channels = NodeString(MapGet(track, "demux-channels"));
        if (channels.empty() || channels.compare(0, 7, "unknown") == 0) {
          const int64_t n = NodeInt(MapGet(track, "demux-channel-count"), 0);
          channels = n == 1 ? "mono" : n == 2 ? "stereo" : n > 0 ? std::to_string(n) + "ch" : "";
        }
        if (!channels.empty())
          codec += " " + channels;
      }
      append(codec);

      std::vector<TrackInfo>& into = is_sub ? subs : is_video ? video : audio;
      TrackInfo t;
      t.stream_index = (int)NodeInt(MapGet(track, "id"), -1);
      t.label = label.empty() ? "Track " + std::to_string(into.size() + 1) : label;
      into.push_back(std::move(t));
    }
  }

  std::lock_guard<std::mutex> lock(tracks_mutex_);
  video_tracks_ = std::move(video);
  audio_tracks_ = std::move(audio);
  sub_tracks_ = std::move(subs);
  vtrack_count_ = (int)video_tracks_.size();
  atrack_count_ = (int)audio_tracks_.size();
}

void Player::UpdateChapters(const mpv_node* list) {
  std::vector<ChapterInfo> chapters;
  if (list && list->format == MPV_FORMAT_NODE_ARRAY && list->u.list) {
    for (int i = 0; i < list->u.list->num; i++) {
      const mpv_node* item = &list->u.list->values[i];
      const mpv_node* time = MapGet(item, "time");
      ChapterInfo c;
      if (time && time->format == MPV_FORMAT_DOUBLE)
        c.start_us = (int64_t)(time->u.double_ * 1e6);
      else
        c.start_us = NodeInt(time, 0) * 1000000;
      c.title = NodeString(MapGet(item, "title"));
      chapters.push_back(std::move(c));
    }
  }
  std::lock_guard<std::mutex> lock(tracks_mutex_);
  chapters_ = std::move(chapters);
}

// ---------------------------------------------------------------------------
// Main thread
// ---------------------------------------------------------------------------

void Player::RenderVideo(int width, int height) {
  // Audio-only: the "video" is spectrum data for the visualizer, which
  // fetches it itself (RenderSpectrum) and draws the screen.
  if (HasSpectrum())
    return;
  RenderTo(0, width, height);
}

bool Player::RenderSpectrum(unsigned fbo) {
  if (!HasSpectrum())
    return false;
  return RenderTo(fbo, kSpectrumWidth, kSpectrumHeight);
}

bool Player::RenderTo(unsigned target, int width, int height) {
  // Rendering starts with the open, not after it: mpv hands the first frame
  // over before Open() returns, and gives up on a renderer that doesn't
  // take it within 200 ms.
  if (!render_ || !(active_ || opening_))
    return false;

  mpv_render_context_update(render_);

  mpv_opengl_fbo fbo = {};
  fbo.fbo = (int)target;
  fbo.w = width;
  fbo.h = height;
  int flip_y = 1;
  // The UI renders every frame anyway; mpv is not to hold it up waiting
  // for the next video frame's time.
  int block = 0;
  mpv_render_param params[] = {
    {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
    {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
    {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
    {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  return mpv_render_context_render(render_, params) >= 0;
}

void Player::SeekRelative(int64_t delta_us) {
  if (!active_ || !CanSeek())
    return;

  // Seeking past the end would end the file, and with it go on to the
  // next item; stop a second short instead.
  const int64_t pos = PositionUs();
  const int64_t dur = DurationUs();
  char amount[32];
  if (delta_us > 0 && pos >= 0 && dur > 0 && pos + delta_us > dur - 1000000) {
    std::snprintf(amount, sizeof(amount), "%.3f",
                  std::max<int64_t>(0, dur - 1000000) / 1e6);
    const char* cmd[] = {"seek", amount, "absolute", nullptr};
    Command(cmd);
    return;
  }
  std::snprintf(amount, sizeof(amount), "%.3f", delta_us / 1e6);
  const char* cmd[] = {"seek", amount, "relative", nullptr};
  Command(cmd);
}

void Player::SetPaused(bool paused) {
  if (!mpv_)
    return;
  paused_ = paused; // at once, for the UI; the property event confirms it
  int flag = paused ? 1 : 0;
  mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &flag);
}

bool Player::TogglePause() {
  const bool paused = !paused_;
  SetPaused(paused);
  return paused;
}

std::vector<VideoTrackInfo> Player::VideoTracks() const {
  std::lock_guard<std::mutex> lock(tracks_mutex_);
  return video_tracks_;
}

std::vector<AudioTrackInfo> Player::AudioTracks() const {
  std::lock_guard<std::mutex> lock(tracks_mutex_);
  return audio_tracks_;
}

int Player::CurrentAudioStream() const {
  // The spectrum's track is the filter's, not mpv's "aid".
  const int vis = vis_aid_;
  return vis >= 0 ? vis : audio_id_.load();
}

std::string Player::CurrentVideoLabel() const {
  const int id = video_id_;
  std::lock_guard<std::mutex> lock(tracks_mutex_);
  for (const TrackInfo& t : video_tracks_)
    if (t.stream_index == id)
      return t.label;
  return {};
}

std::string Player::CurrentAudioLabel() const {
  const int id = CurrentAudioStream();
  std::lock_guard<std::mutex> lock(tracks_mutex_);
  for (const TrackInfo& t : audio_tracks_)
    if (t.stream_index == id)
      return t.label;
  return {};
}

bool Player::SelectAudioTrack(int stream_index) {
  if (!active_ || stream_index == CurrentAudioStream())
    return false;
  bool known = false;
  for (const TrackInfo& t : AudioTracks())
    known |= t.stream_index == stream_index;
  if (!known)
    return false;
  if (vis_aid_ >= 0) {
    // The spectrum's graph names its track; build it again for this one.
    SetVisualizer(stream_index);
    return vis_aid_ == stream_index;
  }
  int64_t id = stream_index;
  mpv_set_property_async(mpv_, 0, "aid", MPV_FORMAT_INT64, &id);
  audio_id_ = stream_index;
  return true;
}

void Player::SetSpeed(double speed) {
  if (!mpv_ || !active_)
    return;
  speed_ = speed; // at once, for the UI; the property event confirms it
  mpv_set_property_async(mpv_, 0, "speed", MPV_FORMAT_DOUBLE, &speed);
}

std::vector<SubtitleTrackInfo> Player::SubtitleTracks() const {
  std::lock_guard<std::mutex> lock(tracks_mutex_);
  return sub_tracks_;
}

bool Player::SelectSubtitleTrack(int stream_index) {
  if (!active_ || stream_index == CurrentSubtitleStream())
    return false;
  if (stream_index >= 0) {
    bool known = false;
    for (const TrackInfo& t : SubtitleTracks())
      known |= t.stream_index == stream_index;
    if (!known)
      return false;
  }
  const std::string value = stream_index >= 0 ? std::to_string(stream_index) : "no";
  mpv_set_property_string(mpv_, "sid", value.c_str());
  sub_id_ = stream_index;
  return true;
}

bool Player::Snapshot(const std::string& path) {
  if (!mpv_ || !active_ || video_id_ < 0)
    return false;
  const char* cmd[] = {"screenshot-to-file", path.c_str(), "video", nullptr};
  return mpv_command(mpv_, cmd) >= 0;
}

// ---------------------------------------------------------------------------
// Chapters and adjustments
// ---------------------------------------------------------------------------

std::vector<ChapterInfo> Player::Chapters() const {
  std::lock_guard<std::mutex> lock(tracks_mutex_);
  return chapters_;
}

bool Player::SeekChapter(int index) {
  if (!mpv_ || !active_)
    return false;
  {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    if (index < 0 || index >= (int)chapters_.size())
      return false;
  }
  int64_t value = index;
  mpv_set_property_async(mpv_, 0, "chapter", MPV_FORMAT_INT64, &value);
  chapter_ = index; // at once, for the UI
  return true;
}

void Player::SetDouble(const char* name, double value) {
  if (mpv_)
    mpv_set_property_async(mpv_, 0, name, MPV_FORMAT_DOUBLE, &value);
}

void Player::SetFit(Fit fit) {
  fit_ = fit;
  if (!mpv_)
    return;
  // Fill crops to the screen (panscan), Stretch ignores the aspect ratio.
  mpv_set_property_string(mpv_, "keepaspect", fit == Fit::Stretch ? "no" : "yes");
  SetDouble("panscan", fit == Fit::Fill ? 1.0 : 0.0);
}

void Player::SetAudioDelay(double seconds) {
  audio_delay_ = seconds;
  SetDouble("audio-delay", seconds);
}

void Player::SetSubtitleDelay(double seconds) {
  sub_delay_ = seconds;
  SetDouble("sub-delay", seconds);
}

void Player::SetSubtitleScale(double scale) {
  sub_scale_ = scale;
  SetDouble("sub-scale", scale);
}

void Player::SetNightMode(bool on) {
  night_mode_ = on;
  if (!mpv_)
    return;
  // dynaudnorm lifts what is quiet and holds back what is loud, over a
  // short window, which is what a film's dialogue against its effects
  // needs; it works on any channel layout.
  const int rc = mpv_set_property_string(mpv_, "af",
                                         on ? "lavfi=[dynaudnorm=f=150:g=15:p=0.9:m=20]" : "");
  if (rc < 0)
    std::fprintf(stderr, "mpv: night mode: %s\n", mpv_error_string(rc));
}

void Player::SetPreferredLanguages(const std::string& audio, const std::string& subs) {
  if (!mpv_)
    return;
  mpv_set_property_string(mpv_, "alang", audio.c_str());
  mpv_set_property_string(mpv_, "slang", subs.c_str());
}
