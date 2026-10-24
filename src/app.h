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
  void SetWatched(const browse::Entry& entry, bool watched);
  // Saves the playing item's position as a resume point, or marks it
  // watched once it is (nearly) over. 'final': playback is ending;
  // 'ended': it played to the end.
  void RecordProgress(bool final, bool ended = false);
  // Library changed: refresh what shows it (rows, counts, library views).
  void OnLibraryChanged();

  // --- Home view -----------------------------------------------------------
  // A rail of places on the left (Home, every source, Rescan) and, on the
  // right, shelves of cards from the library (Continue watching,
  // Favorites) for the place the rail is on. Kodi's home, more or less.
  enum class RailKind { Home, Source, Rescan };
  struct RailItem {
    RailKind kind;
    browse::SourcePtr source;
  };
  struct Shelf {
    std::string title;
    std::shared_ptr<library::LibrarySource> source; // what its cards open in
    browse::Listing entries;                        // ids encoded
  };
  void StartDiscovery();
  void RebuildHome();          // rail, then shelves for its selection
  void RebuildShelves();
  void RebuildCards();         // card rows from shelves_ (artwork, progress)
  void MoveRail(int delta);
  void ActivateRail();
  void ActivateCard();
  void HandleKeyHome(Rml::Event& event, int key);
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
  // playing (or being launched), or -1.
  int NeighborIndex(int direction) const;
  bool PlayNeighbor(int direction);      // next/previous playable item
  void CycleAudioTrack(int direction = +1);
  void CycleVideoTrack(int direction = +1);
  void HandleKeyWatch(Rml::Event& event, int key);

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
  Rml::String bind_status_;       // discovery status line
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
  Rml::String bind_watch_buffered_ = "0%"; // buffered-ahead bar width
  Rml::String bind_watch_speed_;   // "1.5×", "" at normal speed
  Rml::String bind_watch_next_;    // name of the item that plays next
  Rml::String bind_watch_seek_;    // seek bubble over the knob, "" = none
  double seek_bubble_until_ = 0.0;
  int64_t seek_accum_us_ = 0;      // jump of the current run of presses
  // Where the seek bubble sits: the knob, kept far enough from the screen
  // edges that the whole bubble shows.
  Rml::String bind_watch_seek_at_ = "50%";
  Rml::String bind_watch_progress_ = "0%"; // data-style-width; never empty
  // Codec lines shown in the watch bar: the label of the video and audio
  // track being decoded ("h264 1920x1080", "eng · ac3 5.1(side)"), or ""
  // when the file has no stream of that kind.
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
  std::vector<Shelf> shelves_;
  std::vector<int> shelf_card_sel_;    // remembered column per shelf

  std::vector<EntryRow> entry_rows_;
  int sel_entry_ = 0;
  int entry_count_ = 0;

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
  bool scroll_sources_pending_ = false;
};
