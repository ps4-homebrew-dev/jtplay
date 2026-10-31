// SPDX-License-Identifier: GPL-3.0-or-later
//
// Application shell, structured after ps5-payload-dev/tvhp: all RmlUi access
// happens on the main thread; anything that touches the network or the disk
// (discovery, browsing, opening a stream) runs on a single worker thread
// whose results are polled once per frame in Update().
//
// The app itself is source-agnostic: it browses browse::Source objects that
// browse::Providers discover (DLNA servers, local directories, ...).
//
// The UI is driven by a TV remote or game controller only; every action is
// a key press (see gamepad.cpp for how buttons map to keys).
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <RmlUi/Core.h>

#include "browse/source.h"
#include "library.h"
#include "player.h"
#include "visualizer.h"

class App : public Rml::EventListener {
public:
  App();
  ~App() override;

  struct Options {
    std::string assets_dir = "assets";   // directory containing main.rml
    std::string cache_dir;             // artwork cache; "" = XDG default
    std::string plugins_dir;           // JS plugin scripts; "" = none
    std::string fonts_dir;             // also used for subtitles
  };

  // Creates the data model and loads <assets_dir>/main.rml. Must be called
  // before the first context update.
  bool Initialize(Rml::Context* context, const Options& options, std::string& error);
  void Shutdown();

  // Per-frame housekeeping; call before context->Update().
  void Update();

  // Hook for the player to draw video beneath the UI; call between
  // Backend::BeginFrame() and context->Render().
  void RenderVideo(int width, int height);

  // Rml::EventListener (document-level keydown, capture phase).
  void ProcessEvent(Rml::Event& event) override;

private:
  enum class View { Home, Browse };

  // Browse lists draw as rows or as a wall of artwork tiles.
  enum class ViewMode { List, Grid };

  // How a folder listing is ordered. Default keeps the source's own order
  // (a DLNA album in track order, a plugin's curated list); folders always
  // come before items regardless.
  enum class SortMode { Default, Name, Date, Size, Count };

  // One level of the browse tree; kept on a stack so backing out is
  // instant and restores the selection.
  struct BrowseLevel {
    std::string id;
    std::string name;
    browse::Listing raw;       // as the source returned it
    browse::Listing entries;   // 'raw', sorted for display
    int selection = 0;
    // Not empty: this level lists the results of a search for it, not a
    // folder (see RequestSearch).
    std::string query;
    // What the list is narrowed to (see OpenFilter); 'entries' holds only
    // the entries of 'raw' that match it. "" = all of them.
    std::string filter;
  };

  // Rows exposed to the RmlUi data model.
  struct RailRow {      // home: the menu down the left edge
    Rml::String icon;
    Rml::String label;
    Rml::String detail;
  };
  struct CardRow {      // home: one tile of a shelf
    Rml::String title;
    Rml::String subtitle;
    Rml::String glyph;  // shown while there is no artwork
    Rml::String kind;   // "video", "audio", ...: tints the glyph tile
    // Decorator for the artwork; "none" while there is none (never empty:
    // a data-style binding is evaluated even where its data-if hides it).
    Rml::String art = "none";
    Rml::String progress = "0%"; // never empty, see EntryRow
    bool watched = false;
  };
  struct ShelfRow {
    Rml::String title;
    std::vector<CardRow> cards;
  };
  struct EntryRow {
    Rml::String icon;   // glyph for the type of entry
    Rml::String kind;   // "video", "audio", ...: tints the glyph tile
    Rml::String art = "none"; // decorator for the thumbnail, see CardRow
    Rml::String name;
    Rml::String meta;   // right-hand column: follows the sort key
    // Resume bar width ("37%"); "0%" = none. Never empty: a data-style
    // binding is evaluated even on a row its data-if hides.
    Rml::String progress = "0%";
    bool folder = false;
    bool favorite = false;
    bool watched = false;
  };
  struct PropRow {      // one "label: value" line in the info panel
    Rml::String label;
    Rml::String value;
  };
  struct MenuRow {
    Rml::String label;
    Rml::String value;  // current setting, "" for plain actions
  };

  // An entry of the options menu. 'value' reports the current setting;
  // 'change' is called with -1/+1 for left/right and 0 for select. Actions
  // without a value close the menu when selected.
  struct MenuItem {
    std::string label;
    std::function<std::string()> value;
    std::function<void(int)> change;
  };

  bool SetupDataModel(Rml::Context* context, std::string& error);

  // --- Worker ------------------------------------------------------------
  // Tasks run strictly in order on one background thread; that thread is
  // the only one allowed to call Source::Browse()/Provider::Discover() or
  // player_->Open()/Stop(). A task hands its result back with Reply(),
  // whose callback runs on the main thread in the next Update() -- the
  // only place the UI and the data model are touched.
  void PostTask(std::function<void()> task);
  void Reply(std::function<void()> apply);
  void WorkerMain();
  void RunReplies();

  // Assigns a bound value and marks it dirty, but only if it changed.
  template <typename T, typename U>
  void Set(const char* name, T& slot, U&& value) {
    if (slot == value)
      return;
    slot = std::forward<U>(value);
    model_.DirtyVariable(name);
  }

  // --- Settings ----------------------------------------------------------
  void LoadSettings();
  void SaveSettings() const;

  // --- Library (favorites, resume points) ----------------------------------
  // The source and source-native id 'entry' belongs to, also for entries
  // listed under Favorites / Continue watching. False when unknown.
  bool Identify(const browse::Entry& entry, library::Ref& out) const;
  void ToggleFavorite(const browse::Entry& entry);
  // Snapshots of where a video was stopped (see RecordProgress) live in the
  // cache directory and go when their resume point does.
  bool IsSnapshot(const std::string& image) const;
  void DropSnapshot(const std::string& image) const;
  void SetWatched(const browse::Entry& entry, bool watched);
  // Saves the playing item's position as a resume point, or marks it
  // watched once it is (nearly) over. 'final': playback is ending;
  // 'ended': it played to the end.
  void RecordProgress(bool final, bool ended = false);
  // Library changed: refresh what shows it (rows, counts, library views).
  void OnLibraryChanged();

  // --- Home view -----------------------------------------------------------
  // A rail of places on the left and, on the right, shelves of cards from
  // the library (Continue watching, Favorites) for the place the rail is
  // on. Kodi's home, more or less. The top level is Home, one row per
  // group of sources (local, network, plugins) and Settings; selecting a
  // group searches it and lists its sources in the rail (back leaves).
  enum class RailKind { Home, Group, Source, Settings };
  struct RailItem {
    RailKind kind;
    browse::SourcePtr source;                                   // Source
    browse::Source::Group group = browse::Source::Group::Local; // Group
  };
  struct Shelf {
    std::string title;
    std::shared_ptr<library::LibrarySource> source; // what its cards open in
    browse::Listing entries;                        // ids encoded
  };
  // Runs the providers of one rail group on the worker and replaces that
  // group's sources with what they find; 'done' runs on the main thread
  // afterwards. A search already under way for the group is joined.
  void StartDiscovery(browse::Source::Group group,
                      std::function<void()> done = nullptr);
  bool SourceKnown(const std::string& key) const;
  // Rail, then shelves for its selection. The rail stays on the place it
  // was on, or moves to the one keyed 'select' (see RailKey) if given.
  void RebuildHome(const std::string& select = std::string());
  void RebuildShelves();
  void RebuildCards();         // card rows from shelves_ (artwork, progress)
  void MoveRail(int delta);
  void ActivateRail();
  void EnterRailGroup(browse::Source::Group group);
  void LeaveRailGroup();
  static std::string RailKey(const RailItem& item); // what RebuildHome keeps
  void ActivateCard();
  void OpenCard(const browse::Entry& entry, const browse::SourcePtr& shelf_source);
  void HandleKeyHome(Rml::Event& event, int key);
  void OpenSettings();                // the settings menu, from the rail
  void ApplyLanguages();              // preferred languages -> player
  const browse::Entry* SelectedCard() const;
  void GoHome();

  // --- Browse view -------------------------------------------------------
  void OpenSource(const browse::SourcePtr& source); // browse its root
  void EnterContainer(const browse::Entry& entry);
  void LeaveContainer();                 // back one level (or to sources)
  void RefreshContainer();               // re-list the current folder
  void RequestBrowse(const std::string& id, const std::string& name,
                     bool replace);
  void RebuildEntryRows();
  void RebuildDetail();
  void RebuildCrumb();
  // Moves the selection; single steps wrap around the ends of the list,
  // larger jumps stop at them.
  void MoveSelection(int delta);
  // List sorted by name: the first entry whose name starts with the
  // next (+1) or previous (-1) letter, flashed large on screen.
  void JumpLetter(int direction);
  void ActivateSelection();
  void HandleKeyBrowse(Rml::Event& event, int key);
  void SetViewMode(ViewMode mode);
  // Asks for the artwork of the rows (or cards) on screen and around them,
  // and fills in what has arrived. Cheap when nothing changed.
  void UpdateVisibleArt();
  // "image(\"path\" cover)" for a decorator, "none" for no path.
  static std::string CoverDecorator(const std::string& path);
  const browse::Entry* SelectedEntry() const;
  BrowseLevel* CurrentLevel();

  // --- Filter ------------------------------------------------------------
  // Options menu > Filter (or Ctrl+F) narrows the list on screen to the
  // entries whose name or description hold every word typed. A bar above
  // the list takes the typing and the list follows it as it goes; on a
  // console, focusing the field brings up the system keyboard. Return keeps
  // the filter and goes back to the list, Escape drops it. The filter
  // belongs to the folder (the level), and works in every source, since it
  // only looks at what is listed.
  void OpenFilter();
  void CloseFilter();                         // keeps the filter
  void ClearFilter();
  void ApplyFilter(const std::string& text);  // the current level's filter
  // What is in the field, as typed; "" if nothing.
  std::string FilterFieldText() const;
  void HandleKeyFilter(Rml::Event& event, int key);

  // --- Search ------------------------------------------------------------
  // Options menu > Search (or Ctrl+S), for sources that can ask their
  // backend (Source::CanSearch): a dialog with a text field, and what the
  // backend finds is listed as a level of its own on the browse stack, so
  // back returns to the folder, selection and all. Unlike the filter it
  // reaches content that is not listed.
  void OpenSearch();
  void CloseSearch();
  // What has been typed, trimmed; "" if nothing.
  std::string SearchText() const;
  void HandleKeySearch(Rml::Event& event, int key);
  void RequestSearch(const std::string& query);
  // Index in path_ of the folder a search starts from: the current level, or
  // the one below it when the current level is itself a list of results.
  size_t SearchScope() const;

  // --- Sorting -----------------------------------------------------------
  void SortLevel(BrowseLevel& level) const;   // raw -> entries
  void SetSort(SortMode mode, bool descending);
  void CycleSortMode(int direction);
  std::string SortLabel() const;
  std::string EntryMeta(const browse::Entry& entry) const;

  // --- Options menu ------------------------------------------------------
  void OpenMenu();
  void AddEntryActions(const browse::Entry& entry);
  void CloseMenu();
  void RebuildMenuRows();
  void HandleKeyMenu(Rml::Event& event, int key);

  // --- Playback ----------------------------------------------------------
  // Plays 'entry', first offering to resume where it was left off.
  void PlayEntry(const browse::Entry& entry);
  void Launch(const browse::Entry& entry, int64_t start_seconds);
  void OpenWatchMenu();
  void CycleSubtitleTrack(int direction);
  // Abandons an in-flight PlayEntry: the result that eventually arrives is
  // discarded (and the stream torn down again if it did open).
  void CancelLaunch();
  void StopPlayback();                   // posts the stop; exits watch UI
  void EnterWatch(const browse::Entry& entry);
  void ExitWatch();
  void ShowWatchInfo(double seconds);
  void UpdateWatchOverlay();
  // AtEnd(): audio advances to the next track in the folder, video exits.
  void HandlePlaybackEnd();
  // Index of the next (+1) / previous (-1) playable item after the one
  // playing (or being launched), or -1. Follows the play order: shuffled
  // or not, wrapping around with repeat-all.
  int NeighborIndex(int direction) const;
  bool PlayNeighbor(int direction);      // next/previous playable item
  // Indices of the playable items of the current folder, in the order
  // they play (shuffled when shuffle is on); cached per folder.
  const std::vector<int>& PlayOrder() const;
  void SetShuffle(bool on);
  // Plays the folder from its first playable item (or, shuffled, from a
  // random one).
  void PlayFolder(bool shuffled);
  // Previous/next chapter, or item at either end of the chapters.
  void SkipChapter(int direction);
  // Up next: the card that counts down to the next episode over the end
  // of a video. Dismissing it means the player stops at the end instead.
  void UpdateUpNext(int64_t pos, int64_t dur);
  void UpdateSleepTimer(double now);
  std::string SleepLabel() const;
  void CycleAudioTrack(int direction = +1);
  void HandleKeyWatch(Rml::Event& event, int key);

  // --- Photo viewer --------------------------------------------------------
  // Pictures open full screen rather than in the player: one at a time with
  // left/right, or as a slideshow that cross-fades from one to the next and
  // slowly pans and zooms across each (Ken Burns). Pictures come from the
  // artwork cache's screen-sized copies, turned upright from their EXIF.
  enum class PhotoEffect { KenBurns, Fade, None, Count };
  // 'photos' are the pictures to step through (the folder's, in display
  // order), 'index' the one to start at.
  void OpenPhotos(const browse::Listing& photos, int index, bool slideshow);
  void ClosePhotos();
  void ShowPhoto(int index);
  void StepPhoto(int direction);
  void RotatePhoto();
  void SetSlideshow(bool playing);
  void UpdatePhotos(double now);
  void ShowPhotoInfo(double seconds);
  void OpenPhotoMenu();
  void HandleKeyPhoto(Rml::Event& event, int key);
  // The pictures of the folder on screen, and where 'entry' is among them.
  browse::Listing FolderPhotos(const browse::Entry& entry, int& index) const;
  static std::string PhotoUri(const browse::Entry& entry);

  // --- Playback preferences ----------------------------------------------
  enum class Repeat { Off, All, One, Count };
  Repeat repeat_ = Repeat::Off;
  bool shuffle_ = false;
  uint32_t shuffle_seed_ = 1;
  bool autoplay_ = true;              // video: go on to the next episode
  // Cached PlayOrder(), and what it was computed for.
  mutable std::vector<int> play_order_;
  mutable std::string play_order_key_;
  int launch_failures_ = 0;           // skipped in a row; bounds the skipping
  // Sleep timer: stop at 'sleep_at_' (Now() based), or at the end of what
  // is playing.
  double sleep_at_ = 0.0;
  bool sleep_end_of_item_ = false;
  bool upnext_dismissed_ = false;
  // Language codes ("en", "sv", ...; "" = the file's own default).
  std::string audio_lang_;
  std::string sub_lang_;
  double screensaver_sec_ = 180.0;    // 0 = never
  Rml::String bind_jump_letter_;      // letter flashed by JumpLetter()
  double jump_letter_until_ = 0.0;

  // --- Screensaver -------------------------------------------------------
  // Dims the screen with a drifting clock after a while without input,
  // unless a video is playing.
  void UpdateScreensaver(double now);

  void EnsureRowVisible(const char* list_id, int index, float row_pitch);
  // A notification in the corner; 'icon' is a glyph ("⭐", "⚠", ...).
  void ShowToast(const std::string& text, const char* icon = nullptr);

  // --- Artwork -----------------------------------------------------------
  // Entry::image is a URI: file:// (or a bare path) for local artwork,
  // http(s):// for remote. RmlUi only loads textures through the file
  // interface, so everything has to end up as a local path; artcache is
  // where that happens, for every source alike.
  // Returns the displayable path, or "" while a download is (or has been
  // scheduled to be) fetched in the background, or if it failed.
  // 'thumb': the small copy, for tiles and rows.
  std::string ImagePathFor(const std::string& uri, bool thumb = false);
  void ArtworkMain();             // the artwork thread
  // The artwork URI for an entry: its own, or else a snapshot the library
  // remembers for it.
  std::string ArtUri(const browse::Entry& entry) const;
  void UpdateBackdrop();          // fanart behind home/browse
  void RefreshImageBindings();     // re-resolve detail/now-playing artwork

  // Data model bound state (main thread only).
  Rml::DataModelHandle model_;
  Rml::String bind_view_ = "home";
  Rml::String bind_view_mode_ = "list"; // browse: "list" / "grid"
  Rml::String bind_toast_;
  Rml::String bind_toast_icon_;
  Rml::String bind_backdrop_ = "none"; // fanart decorator
  Rml::String bind_crumb_;        // breadcrumb of the browse path
  Rml::String bind_source_name_;  // pane title: connected source
  Rml::String bind_clock_;
  Rml::String bind_date_;
  Rml::String bind_sort_label_;   // "Name ▲" in the list header
  Rml::String bind_detail_name_;
  Rml::String bind_detail_icon_;  // type glyph when there is no artwork
  Rml::String bind_detail_kind_;  // "video", ...: tints the art box
  Rml::String bind_detail_description_;
  std::vector<PropRow> detail_props_;
  bool bind_busy_ = false;         // a browse/discovery is in flight
  // Phase of an in-flight launch ("Opening stream..."), or "" when idle.
  // The browse view's own feedback is the topbar busy indicator, which
  // keeps running because the shell is not hidden until the player is up.
  Rml::String bind_launch_status_;
  bool bind_watching_ = false;     // full-screen playback, chrome hidden
  bool bind_info_visible_ = false; // watch info bar shown (auto-hides)
  bool bind_watch_audio_ = false;  // audio-only: persistent now-playing card
  bool bind_watch_paused_ = false;
  bool bind_watch_seekable_ = false;
  Rml::String bind_watch_name_;
  Rml::String bind_watch_description_;
  Rml::String bind_watch_pos_;     // "12:34"
  Rml::String bind_watch_dur_;     // "45:00", "" for live streams
  Rml::String bind_watch_ends_;    // "Ends at 21:40", "" when unknown
  Rml::String bind_watch_buffered_ = "0%"; // buffered-ahead bar width
  Rml::String bind_watch_speed_;   // "1.5×", "" at normal speed
  Rml::String bind_watch_next_;    // name of the item that plays next
  Rml::String bind_watch_seek_;    // seek bubble over the knob, "" = none
  double seek_bubble_until_ = 0.0;
  int64_t seek_accum_us_ = 0;      // jump of the current run of presses
  int seek_run_ = 0;               // presses in the current run
  // Where the seek bubble sits: the knob, kept far enough from the screen
  // edges that the whole bubble shows.
  Rml::String bind_watch_seek_at_ = "50%";
  Rml::String bind_watch_progress_ = "0%"; // data-style-width; never empty
  Rml::String bind_watch_chapter_; // "Chapter 2/5 · The Middle", "" = none
  struct MarkRow {      // a chapter tick on the timeline
    Rml::String at;     // "37.50%"
    bool operator==(const MarkRow& o) const { return at == o.at; }
    bool operator!=(const MarkRow& o) const { return at != o.at; }
  };
  std::vector<MarkRow> chapter_marks_;
  Rml::String bind_watch_modes_;   // "🔀 🔁" badges: shuffle, repeat
  bool bind_upnext_ = false;       // the up-next card is up
  Rml::String bind_upnext_name_;
  Rml::String bind_upnext_art_ = "none";
  Rml::String bind_upnext_count_;  // "Playing in 8"
  // The tracks being decoded, as the watch bar shows them ("h264
  // 1920x1080", "eng · ac3 5.1(side)"); "" when there is none of the kind.
  Rml::String bind_watch_vtrack_;
  Rml::String bind_watch_atrack_;
  Rml::String bind_detail_image_;  // local image path, "" = none
  bool bind_detail_favorite_ = false;

  bool bind_saver_ = false;        // screensaver up
  Rml::String bind_saver_x_ = "0px";
  Rml::String bind_saver_y_ = "0px";
  Rml::String bind_saver_now_;     // what is playing, under the clock
  double last_input_ = 0.0;
  double saver_moved_at_ = 0.0;
  Rml::String bind_watch_image_;   // now-playing artwork path, "" = none

  // Photo viewer (see OpenPhotos). Two layers take turns: the new picture
  // fades in on one while the old one stays on the other.
  bool bind_photo_ = false;
  bool bind_photo_info_ = false;       // caption along the bottom
  bool bind_photo_playing_ = false;    // slideshow running
  bool bind_photo_loading_ = false;
  bool bind_photo_fade_ = true;        // cross-fade between pictures
  int bind_photo_front_ = 0;           // layer showing the current picture
  Rml::String bind_photo_art_[2] = {"none", "none"};  // decorators
  Rml::String bind_photo_anim_[2] = {"none", "none"}; // Ken Burns moves
  Rml::String bind_photo_rotate_ = "none"; // manual turn, a transform
  bool bind_photo_sideways_ = false;   // turned 90 or 270 degrees
  Rml::String bind_photo_name_;
  Rml::String bind_photo_meta_;        // "3 / 24  ·  2024-03-01 14:22"
  browse::Listing photo_list_;
  int photo_index_ = 0;
  bool photo_pending_ = false;         // waiting for the current picture
  double photo_next_at_ = 0.0;         // slideshow: when to move on
  double photo_info_until_ = 0.0;
  int photo_interval_ = 6;             // seconds per picture
  PhotoEffect photo_effect_ = PhotoEffect::KenBurns;
  bool photo_shuffle_ = false;
  int photo_moves_ = 0;                // picks the next Ken Burns move
  std::vector<int> photo_rotation_;    // manual quarter turns, per picture

  bool bind_filter_open_ = false;  // the filter bar is up
  Rml::String bind_filter_text_;   // the current level's filter, for the head
  bool filter_focus_pending_ = false; // focus the field once the bar shows
  bool bind_search_open_ = false;  // the search dialog is up
  Rml::String bind_search_title_;  // "Search in Podcasts"
  bool search_focus_pending_ = false; // focus the field once the dialog shows

  bool bind_menu_open_ = false;
  bool bind_menu_dialog_ = false;  // centered dialog rather than side blade
  Rml::String bind_menu_title_;
  std::vector<MenuRow> menu_rows_;
  int sel_menu_ = 0;
  std::vector<MenuItem> menu_items_;

  // Home.
  std::vector<RailRow> rail_rows_;
  std::vector<ShelfRow> shelf_rows_;
  int sel_rail_ = 0;
  int sel_shelf_ = 0;
  int sel_card_ = 0;
  bool bind_home_shelves_ = false;     // focus is on the shelves
  Rml::String bind_hero_icon_;
  Rml::String bind_hero_title_;
  Rml::String bind_hero_detail_;
  Rml::String bind_hero_note_;
  std::vector<RailItem> rail_;
  // Per rail group (indexed by browse::Source::Group): searched at least
  // once, a search under way and what to do when it ends, and how the
  // last one went ("3 sources found", errors).
  struct GroupScan {
    bool scanned = false;
    bool running = false;
    std::vector<std::function<void()>> waiting;
    Rml::String status;
  };
  GroupScan scans_[3];
  bool rail_in_group_ = false;         // the rail lists one group's sources
  browse::Source::Group rail_group_ = browse::Source::Group::Local;
  std::vector<Shelf> shelves_;
  std::vector<int> shelf_card_sel_;    // remembered column per shelf

  std::vector<EntryRow> entry_rows_;
  int sel_entry_ = 0;
  int entry_count_ = 0;                // rows shown
  int entry_total_ = 0;                // entries of the folder, filtered or not

  // Backing data (main thread).
  std::vector<std::unique_ptr<browse::Provider>> providers_;
  browse::SourcePtr current_source_;   // source being browsed, or null
  std::vector<BrowseLevel> path_;      // root first
  browse::Entry playing_;              // item in the player right now

  library::Library library_;
  library::SourceRegistry registry_;
  std::shared_ptr<library::LibrarySource> favorites_source_;
  std::shared_ptr<library::LibrarySource> continue_source_;
  std::vector<browse::SourcePtr> discovered_;   // from the providers
  uint64_t library_seen_ = 0;          // generation the UI reflects

  // The item playing, as the library knows it; 'resumable' says whether
  // its progress is worth remembering (seekable, long enough).
  library::Ref playing_ref_;
  bool playing_identified_ = false;
  double progress_saved_at_ = 0.0;  // last RecordProgress() while playing
  double library_saved_at_ = 0.0;   // last write of the library file
  // The last position/duration the player reported for the item playing:
  // once a file has ended, mpv reports neither.
  int64_t watch_last_pos_us_ = -1;
  int64_t watch_last_dur_us_ = -1;
  bool watch_last_seekable_ = false;

  SortMode sort_mode_ = SortMode::Default;
  bool sort_desc_ = false;
  ViewMode view_mode_ = ViewMode::List;
  std::string settings_path_;          // "" = settings are not persisted
  std::string cache_dir_;              // snapshots of where videos stopped

  std::unique_ptr<Player> player_;
  Visualizer visualizer_;          // background for audio-only playback
  Rml::Context* context_ = nullptr;
  Rml::ElementDocument* document_ = nullptr;
  View view_ = View::Home;

  // Worker plumbing: jobs going out, results coming back.
  std::thread worker_;
  std::mutex tasks_mutex_;
  std::condition_variable tasks_cv_;
  std::deque<std::function<void()>> tasks_;
  std::atomic<bool> worker_running_{false};
  std::mutex replies_mutex_;
  std::deque<std::function<void()>> replies_;

  // Artwork. The cache itself is artcache. Downloads and thumbnailing run
  // on a thread of their own, so a wall of posters never holds up a
  // browse, and newest request first, so what is on screen now comes
  // before what was scrolled past. image_inflight_ (main thread) is what
  // has been asked for, so the UI asks once.
  std::set<std::string> image_inflight_;
  bool image_refresh_pending_ = false;
  bool art_scan_pending_ = false;     // UpdateVisibleArt() has work
  std::thread art_thread_;
  std::mutex art_mutex_;
  std::condition_variable art_cv_;
  std::deque<std::pair<std::string, bool>> art_queue_; // (uri, thumb)
  bool art_running_ = false;          // guarded by art_mutex_
  uint32_t browse_request_ = 0;     // id of the browse we are waiting for
  std::atomic<int> busy_ops_{0};

  // In-flight PlayEntry. The worker moves through the phases; the main
  // thread only reads them, and stops caring as soon as launch_request_ no
  // longer matches the result that comes back.
  enum LaunchPhase { kLaunchIdle = 0, kLaunchResolving, kLaunchOpening };
  std::atomic<int> launch_phase_{kLaunchIdle};
  uint32_t launch_seq_ = 0;         // ever-increasing ticket source
  uint32_t launch_request_ = 0;     // ticket we are waiting for; 0 = idle
  browse::Entry launch_entry_;      // item being opened
  // Direction of a next/previous (or auto-advance) launch, so an item that
  // fails to open is skipped the same way; 0 for an explicit choice.
  int launch_direction_ = 0;
  double launch_visible_at_ = 0.0;  // suppress the phase line before this

  double toast_deadline_ = 0.0;
  double info_deadline_ = 0.0;      // watch info bar auto-hide
  bool scroll_entries_pending_ = false;
  bool scroll_menu_pending_ = false;
  bool scroll_sources_pending_ = false;
};
