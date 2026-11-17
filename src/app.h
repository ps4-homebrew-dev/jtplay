// SPDX-License-Identifier: GPL-3.0-or-later
//
// Application shell, structured after ps5-payload-dev/tvhp: all RmlUi access
// happens on the main thread; anything that touches the network or the disk
// (discovery, browsing, opening a stream) runs on a worker thread, one per
// kind of work (see Lane), whose results are polled once per frame in
// Update().
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
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <RmlUi/Core.h>

#include "browse/plugin_settings.h"
#include "browse/source.h"
#include "library.h"
#include "net/cancel.h"
#include "player.h"
#include "util/worker.h"
#include "visualizer.h"

namespace browse {
class JsPlugin;
class JsProvider;
}

class App : public Rml::EventListener {
public:
  App();
  ~App() override;

  struct Options {
    std::string assets_dir = "assets";   // directory containing main.rml
    std::string cache_dir;             // artwork cache; "" = XDG default
    std::string plugins_dir;           // bundled JS plugin scripts; "" = none
    // The user's own plugin scripts, looked at after the bundled ones (a
    // file of the same name replaces the bundled one); "" = none.
    std::string user_plugins_dir;
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
    // Not empty: the folder could not be listed, and this is why. The level
    // is on the stack all the same, so the screen says where the trouble
    // is, select tries again and back leaves the usual way.
    std::string error;
    // A folder above the one a source was reopened in (see RememberPlace):
    // known by name, listed only when back reaches it, on the entry 'keep'.
    bool stale = false;
    std::string keep;
    // A folder that held nothing but one folder, which was opened in its
    // place: back passes over it.
    bool passthrough = false;
  };

  // Rows exposed to the RmlUi data model.
  struct RailRow {      // home: a tab across the top
    Rml::String icon;
    Rml::String label;
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
    int count = 0;        // what the title counts; 0 shows no count
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
    bool playing = false; // the item in the player right now
  };
  struct ChildRow {     // the info panel: one entry of the focused folder
    Rml::String icon;
    Rml::String name;
  };
  struct PropRow {      // one "label: value" line in the info panel
    Rml::String label;
    Rml::String value;
  };
  struct MenuRow {
    Rml::String section; // a heading above the first row of a group
    Rml::String label;
    Rml::String value;  // current setting, "" for plain actions
  };
  struct ControlRow {   // a button of the playback controls (see below)
    Rml::String icon;
    bool on = false;    // a mode that is on: shuffle, repeat, subtitles
    bool dim = false;   // nothing to do right now: no previous item
    bool gap = false;   // the first of a group: set apart from the one before
    bool operator==(const ControlRow& o) const {
      return icon == o.icon && on == o.on && dim == o.dim && gap == o.gap;
    }
    bool operator!=(const ControlRow& o) const { return !(*this == o); }
  };

  // An entry of the options menu. 'value' reports the current setting;
  // 'change' is called with -1/+1 for left/right and 0 for select. Actions
  // without a value close the menu when selected. 'section', on the first
  // entry of a group, is the heading over it (see MenuSection).
  struct MenuItem {
    std::string label;
    std::function<std::string()> value;
    std::function<void(int)> change;
    std::string section;
  };
  // A row of the Settings page: a setting (left / right change it, select
  // steps it on) or an action (select runs it; 'value' is then what it
  // would act on, such as how many, and may be empty).
  struct SettingRow {
    Rml::String section;  // a heading above the first row of a section
    Rml::String label;
    Rml::String value;
    bool action = false;
  };
  struct SettingItem {
    std::string section;
    std::string label;
    std::string about;
    std::function<std::string()> value;
    std::function<void(int)> change; // -1 / +1 left / right, 0 select
    bool action = false;
  };

  bool SetupDataModel(Rml::Context* context, std::string& error);

  // --- Workers -----------------------------------------------------------
  // Blocking work runs on background threads, one per lane; within a lane,
  // tasks run strictly in the order they were posted. The lanes keep one
  // kind of waiting from holding up another: a search for servers does not
  // delay a folder, and neither delays stopping what plays.
  //
  //   Discovery  Provider::Discover()
  //   Browse     Source::Browse() and Search()
  //   Playback   Source::Resolve(), then player_->Open(); player_->Stop()
  //              (the only lane that may call those two)
  //
  // A task hands its result back with Reply(), whose callback runs on the
  // main thread in the next Update() -- the only place the UI and the data
  // model are touched. With 'busy', the task counts towards the activity
  // indicator in the top bar from the moment it is posted.
  // (Preview: what is in the focused folder, see RequestPreview; apart, so
  // it never holds up opening a folder.)
  enum class Lane { Discovery, Browse, Playback, Preview, Count };
  void PostTask(Lane lane, std::function<void()> task, bool busy = true);
  void Reply(std::function<void()> apply);
  void RunReplies();

  // The browse (or search) the browse view is waiting for. Starting a new
  // one, or walking away, gives up on the one in flight: its result is
  // dropped when it arrives (the ticket no longer matches), and the token
  // tells the lane to stop waiting for it (see net/cancel.h).
  struct BrowseTicket {
    uint32_t id = 0;
    net::CancelToken cancel;
  };
  BrowseTicket NewBrowseRequest();
  void AbandonBrowse();
  // The result of the request 'ticket' was for has arrived (and is the one
  // being waited for).
  void BrowseArrived();

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
  // The same, for an entry listed by 'source' rather than the current one.
  bool IdentifyIn(const browse::Entry& entry, const browse::SourcePtr& source,
                  library::Ref& out) const;
  void ToggleFavorite(const browse::Entry& entry);
  // A source as a favorite: its top folder, kept against the source's key.
  library::Ref SourceRef(const browse::SourcePtr& source) const;
  void ToggleSourceFavorite(const browse::SourcePtr& source);
  // Snapshots of where a video was stopped (see RecordProgress) live in the
  // cache directory and go when their resume point does.
  bool IsSnapshot(const std::string& image) const;
  void DropSnapshot(const std::string& image) const;
  void SetWatched(const browse::Entry& entry, bool watched);
  // Saves the playing item's position as a resume point, or marks it
  // watched once it is (nearly) over. 'final': playback is ending;
  // 'ended': it played to the end.
  void RecordProgress(bool final, bool ended = false);
  // Puts what played among the library's recent items as it ends (which
  // Search all sources looks through first, see library.h).
  void RecordRecent();
  // Artwork from a listing of 'source', for the list of sources when it
  // has no picture of its own yet.
  void NoteSourceArt(const browse::SourcePtr& source, const browse::Listing& entries);
  // Library changed: refresh what shows it (rows, counts, library views).
  void OnLibraryChanged();

  // --- Home view -----------------------------------------------------------
  // Tabs across the top, Home, Sources and Settings (see app_home.cpp).
  // Home's page is rows of cards: what plays, Continue watching and
  // Favorites. Sources' page is the list of sources, in the browse view
  // (see OpenSourcesList); Settings', a list of what can be set.
  enum class RailKind { Home, Sources, Settings };
  struct RailItem {
    RailKind kind;
  };
  struct Shelf {
    std::string title;
    std::shared_ptr<library::LibrarySource> source; // what its cards open in
    browse::Listing entries;                        // ids encoded
    // The row of what is playing (Home's first, while something does):
    // one card, the way back to it.
    bool now_playing = false;
  };
  // Runs the providers of one group of sources on the worker and replaces
  // that group's sources with what they find; 'done' runs on the main
  // thread afterwards. A search already under way for the group is joined.
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
  // Every group of sources looked for again (square on Home, Refresh on
  // the list of sources).
  void SearchAllSources();
  // A source by its key: opened if it has been found, looked for (and then
  // opened) if it has not been yet.
  void OpenKnownSource(const std::string& key, const std::string& name);
  // The focus onto the card whose entry has 'id', if Home has one.
  void FocusHomeCard(const std::string& id);
  bool NowPlayingShelf(int shelf) const {
    return shelf >= 0 && shelf < (int)shelves_.size() && shelves_[shelf].now_playing;
  }
  static std::string RailKey(const RailItem& item); // what RebuildHome keeps
  void ActivateCard();
  void OpenCard(const browse::Entry& entry, const browse::SourcePtr& shelf_source);
  void HandleKeyHome(Rml::Event& event, int key);
  // Settings: the page under the Settings tab, a list of what can be set
  // (see BuildSettings), down from the tab as rows are anywhere else.
  void BuildSettings();
  void RebuildSettingRows();          // values -> the page
  void EnterSettings();               // focus from the tab into the list
  void HandleKeySettings(Rml::Event& event, int key);
  void ConfirmForgetContinue();       // asks, then empties Continue watching
  // A setting with more choices than anyone wants to step through one by
  // one: the menu lists them all, on the current one, and 'pick' is called
  // with the index of the one chosen.
  void OpenChoiceMenu(const std::string& title, const std::vector<std::string>& choices,
                      int current, std::function<void(int)> pick);
  // Asks for a line of text in the search dialog (on a console, the system
  // keyboard), from wherever the user is. 'done' gets what was typed;
  // nothing at all when the dialog is closed without typing.
  // With 'secret', what is typed shows as dots.
  void OpenTextEntry(const std::string& title, const std::string& initial, size_t max_length,
                     bool secret, std::function<void(const std::string&)> done);
  void ApplyLanguages();              // preferred languages -> player
  const browse::Entry* SelectedCard() const;
  // The card the banner shows: the focused one, or with the focus on the
  // tabs, the first of Home's shelves (other places show themselves).
  // False when there is none; 'shelf' and 'card' index shelves_.
  bool FeaturedIndex(int& shelf, int& card) const;
  const browse::Entry* FeaturedCard() const;
  void UpdateFeature();               // the banner's text, from FeaturedIndex
  void GoHome();

  // --- Browse view -------------------------------------------------------
  void OpenSource(const browse::SourcePtr& source); // browse its root
  // The folders on screen, down to the last one listed (not results of a
  // search), each with the entry in focus.
  std::vector<library::PlaceLevel> CurrentPlace() const;
  // Continue watching: back to the folder the item was played from (as
  // the library remembers it, see launch_place_), the item resumed from
  // there. False when there is no such place (or its source is not
  // found): the caller plays the card as it is.
  bool ResumeInPlace(const browse::Entry& card);
  // The card ResumeInPlace is opening a folder for, and that request.
  browse::Entry pending_resume_card_;
  std::string pending_resume_id_;
  uint32_t pending_resume_request_ = 0;
  void EnterContainer(const browse::Entry& entry);
  // Back one level; from the top of a source, to the list of sources (or
  // home); from that list, up to its tab.
  void LeaveContainer();
  void RefreshContainer();               // re-list the current folder
  // 'keep': the entry to put the cursor on (by id), if it is there.
  void RequestBrowse(const std::string& id, const std::string& name,
                     bool replace, const std::string& keep = std::string());
  // Where the source on screen was left, remembered for the next time it
  // is opened (OpenSource reopens it there). Not for the library's views.
  void RememberPlace();
  bool LibraryView(const browse::SourcePtr& source) const;
  // The focused folder's first entries, in the info panel: listed in the
  // background a moment after the focus stops on it, and kept.
  void RequestPreview();
  void ShowPreview();
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
  // The list of sources (the Sources tab), in the browse view as any
  // folder: every source there is, remembered or found, most used first,
  // on 'select_key' (or the one last opened from it). Select opens one,
  // back from its top returns here; Search in its options menu searches
  // the library and every source that can.
  void OpenSourcesList(const std::string& select_key = std::string());
  // The list again, as the sources found now are, on the same one.
  void RefreshSourcesList();
  browse::Listing SourcesListing();
  // On the list itself (not results searched from it).
  bool OnSourcesList() const {
    return view_ == View::Browse && current_source_ &&
           current_source_ == sources_source_ && path_.size() == 1;
  }
  // A source as listed there: its key, and the source if it is found.
  struct ListedSource {
    std::string key, name;
    std::string icon;
    browse::SourcePtr source;
  };
  std::map<std::string, ListedSource> listed_sources_; // by entry id
  const ListedSource* ListedSourceOf(const browse::Entry& e) const;
  // The glyph for an entry: a source's own, on the list of them.
  const char* EntryIcon(const browse::Entry& e) const;
  std::string sources_list_key_;      // last opened from the list
  bool from_sources_list_ = false;    // the source on screen was
  // The list of sources is the Sources tab's page: on screen, it has the
  // tabs above it, and the focus may be up on them (moving along them
  // leaves it for the others).
  bool bind_sources_tab_ = false;
  bool bind_browse_tabs_ = false;
  void SetBrowseTabs(bool on);
  // From the list's tabs to another tab ("home", "settings"), the focus
  // staying on the tabs; or down into Home's rows ('rows').
  void LeaveSourcesTab(const std::string& tab, bool rows);
  void HandleKeySourcesTabs(Rml::Event& event, int key);
  // The search dialog asking for something else (OpenTextEntry): who gets
  // the text. Empty while it asks for a search.
  std::function<void(const std::string&)> text_entry_;
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
  // The menus are lists of groups, each under a heading and always in the
  // same order: what the item in focus can do, its folder, finding things
  // in it, how it is shown, the plugin's settings and, last, what plays
  // behind the menus.
  void OpenMenu();
  void AddEntryActions(const browse::Entry& entry);
  // Entries added to menu_items_ from here on belong under 'title': the
  // next one added gets the heading (none, if no entry follows).
  void MenuSection(const std::string& title);
  void AddMenuItem(MenuItem item);
  void ShowMenu(int selection = 0);  // menu_items_ -> the screen
  void CloseMenu();
  // A plugin's settings (what the Settings page lists for it) as a menu of
  // their own, from the options menu of anything that is the plugin's.
  // PluginOf() is null for a source that is not a plugin's.
  std::shared_ptr<browse::JsPlugin> PluginOf(const std::string& source_key) const;
  void AddPluginSettingsEntry(const std::string& source_key);
  void OpenPluginMenu(const std::shared_ptr<browse::JsPlugin>& plugin, int selection = 0);
  void ClosePluginMenu();
  // The settings one plugin declares, as rows for the Settings page or for
  // the plugin's menu. 'menu' (re)opens that menu, for a setting whose
  // list of choices or text dialog took its place; empty on the Settings
  // page, whose rows stay where they are.
  void AddPluginSettings(const std::shared_ptr<browse::JsPlugin>& plugin,
                         std::vector<SettingItem>& out, std::function<void()> menu);
  void RebuildMenuRows();
  void HandleKeyMenu(Rml::Event& event, int key);

  // --- Playback ----------------------------------------------------------
  // Playback starts full screen. Back leaves full screen without stopping:
  // the menus come back, drawn over the video (or, for audio, over their
  // usual background) while it plays on, Kodi style. Triangle (or Tab), or
  // selecting the item that is playing, returns to full screen; stopping
  // is the stop button of the controls, hold back, the X key, a remote's
  // stop key or the options menu.
  //
  // bind_watching_ says a playback session is on; bind_fullscreen_ says it
  // has the screen to itself.
  //
  // Plays 'entry', first offering to resume where it was left off.
  // 'resume': from where it was left, without asking (a Continue watching
  // card); otherwise an item with a resume point asks.
  void PlayEntry(const browse::Entry& entry, bool resume = false);
  // 'from_queue': the item comes from the play queue (next, previous,
  // auto-advance, repeat), and resolves through the queue's source rather
  // than whatever is being browsed now.
  void Launch(const browse::Entry& entry, int64_t start_seconds, bool from_queue = false);
  void CycleSubtitleTrack(int direction, bool toast = false);
  // The playback controls: a row of buttons in the info bar (video) and on
  // the now-playing card (audio). Select puts the focus on them: left and
  // right then go along them, select presses one. Without the focus the
  // keys steer playback directly (left, right, up and down seek).
  enum class Control {
    Previous, PlayPause, Next, Stop, Audio, Subtitles, Shuffle, Repeat
  };
  void UpdateControls();              // the buttons there are, and their state
  void FocusControls(bool on);
  void ActivateControl(Control control);
  std::string ControlLabel(Control control) const;
  bool HandleKeyControls(int key);    // true if 'key' was one of theirs
  // Abandons an in-flight PlayEntry: the result that eventually arrives is
  // discarded (and the stream torn down again if it did open).
  void CancelLaunch();
  // CancelLaunch() for moving around the menus: a launch the menus started
  // would otherwise open over a folder it no longer belongs to. One that
  // moves the play queue on behind the menus stays.
  void CancelForegroundLaunch();
  void StopPlayback();                   // posts the stop; exits watch UI
  void EnterWatch(const browse::Entry& entry, bool fullscreen);
  void ExitWatch();
  // Full screen, or the menus over what plays (see above).
  void SetFullscreen(bool on);
  bool FullscreenPlayback() const { return bind_watching_ && bind_fullscreen_; }
  // Keys that work on playback while the menus are up: back to full
  // screen, stop, and a remote's media keys. True if 'key' was one.
  bool HandleKeyBehind(int key);
  // A remote's (or keyboard's) media keys, wherever playback is. True if
  // 'key' was one.
  bool HandleMediaKey(int key);
  bool HandleTransportKey(int key);     // the transport keys among them
  void HandleKeyUp(int key);
  // The video info bar along the bottom (see app_watch.cpp): up with the
  // focus on its buttons, until no key has come for a while; up without
  // it, after a pause or a track picked with a remote's key; away.
  void ShowControls();
  void PeekWatchInfo();
  void HideWatchInfo();
  // A seek: shown on the bottom bar if that is up, otherwise on the thin
  // bar along the top (which also shows an item as it starts), which goes
  // again a moment after the last.
  void ShowSeek();
  void SeekStep(int sign, bool large = false);
  void UpdateWatchOverlay();
  // AtEnd(): audio advances to the next track in the folder, video exits.
  void HandlePlaybackEnd();
  // Index of the next (+1) / previous (-1) playable item after the one
  // playing (or being launched), or -1. Follows the play order: shuffled
  // or not, wrapping around with repeat-all.
  int NeighborIndex(int direction) const;
  bool PlayNeighbor(int direction);      // next/previous playable item
  // Indices into queue_.entries of its playable items, in the order they
  // play (shuffled when shuffle is on); cached per queue.
  const std::vector<int>& PlayOrder() const;
  // The current folder becomes the play queue.
  void QueueCurrentFolder();
  // Playable items in the folder on screen (not the queue).
  size_t FolderPlayableCount() const;
  void SetShuffle(bool on);
  // Plays the folder from its first playable item (or, shuffled, from a
  // random one).
  void PlayFolder(bool shuffled);
  // Previous/next chapter, or item at either end of the chapters.
  void SkipChapter(int direction);
  // Up next: the card that counts down to the next episode over the end
  // of a video. Dismissing it means the player stops at the end instead.
  void UpdateUpNext(int64_t pos, int64_t dur);
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
  bool upnext_dismissed_ = false;
  // Language codes ("en", "sv", ...; "" = the file's own default).
  std::string audio_lang_;
  std::string sub_lang_;
  double screensaver_sec_ = 180.0;    // 0 = never
  // Settings > Show input: a toast (and a log line) for every input event,
  // also those nothing reacts to, to find out what a remote sends.
  bool input_trace_ = false;
  void SetInputTrace(bool on);
  Rml::String bind_jump_letter_;      // letter flashed by JumpLetter()
  double jump_letter_until_ = 0.0;

  // --- Screensaver -------------------------------------------------------
  // Dims the screen with a drifting clock after a while without input,
  // unless a video is playing.
  void UpdateScreensaver(double now);

  void EnsureRowVisible(const char* list_id, int index, float row_pitch);
  // The same for rows that are not all of one height: 'top' to 'bottom' is
  // where the row is in the list, 'row_pitch' what a row of context is.
  void EnsureSpanVisible(const char* list_id, float top, float bottom, float row_pitch);
  bool ScrollRows();
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
  void ArtworkMain();             // an artwork thread
  // The artwork URI for an entry: its own, or else a snapshot the library
  // remembers for it.
  std::string ArtUri(const browse::Entry& entry) const;
  void UpdateBackdrop();          // fanart behind home/browse
  void RefreshImageBindings();     // re-resolve detail/now-playing artwork
  // RmlUi keeps every image it has loaded as a texture until told to let
  // it go, whether anything still shows it or not. Every path handed to
  // the UI is noted here, and the ones nothing refers to any more are
  // released (see app.cpp).
  void TrackTexture(const std::string& path);
  void ReleaseUnusedTextures(double now);

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
  std::vector<ChildRow> detail_children_; // a folder's, see RequestPreview
  Rml::String bind_detail_more_;          // "and 12 more", "Empty", ""
  bool bind_detail_preview_ = false;      // the panel shows a folder's entries
  struct Preview {
    std::vector<ChildRow> children;
    size_t count = 0;
  };
  std::map<std::string, Preview> previews_; // by source key and folder id
  std::string preview_key_;                 // the one the panel waits for
  double preview_at_ = 0.0;                 // when to ask for it; 0 = not
  bool preview_running_ = false;
  // The one sign of waiting, the dots by the clock: a task of a lane (see
  // PostTask) or a picture on its way.
  bool bind_busy_ = false;
  // A listing is on its way (the list says nothing about being empty
  // meanwhile); and what went wrong with the folder on screen.
  bool bind_browse_loading_ = false;
  Rml::String bind_browse_error_;
  bool bind_watching_ = false;     // a playback session is on
  bool bind_fullscreen_ = false;   // ... and has the screen (chrome hidden)
  // Video plays behind the menus: their background turns see-through.
  bool bind_video_behind_ = false;
  // Back held in full screen stops (see HandleKeyUp); a short press leaves
  // full screen instead, so the press is only acted on when it ends.
  bool back_held_ = false;
  double back_down_at_ = 0.0;
  // Playback was stopped by holding back: what the keyboard repeats of it
  // until it is let go is not more presses of back.
  bool back_swallow_ = false;
  bool bind_info_visible_ = false; // the bottom bar shown
  // The playback controls (see UpdateControls): the buttons shown, which
  // of them has the focus and what that one is, in words.
  std::vector<ControlRow> control_rows_;
  std::vector<Control> controls_;
  Control control_sel_ = Control::PlayPause;
  int bind_sel_control_ = 0;
  bool bind_control_focus_ = false;
  Rml::String bind_control_label_;
  bool bind_watch_audio_ = false;  // audio-only: persistent now-playing card
  bool bind_watch_paused_ = false;
  bool bind_watch_seekable_ = false;
  Rml::String bind_watch_name_;
  Rml::String bind_watch_description_;
  // What the stream says is on (Player::StreamTitle), when that is more
  // than the name already says: the song on a radio station.
  Rml::String bind_watch_stream_title_;
  Rml::String bind_watch_pos_;     // "12:34"
  Rml::String bind_watch_dur_;     // "45:00", "" for live streams
  Rml::String bind_watch_buffered_ = "0%"; // buffered-ahead bar width
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
  bool bind_upnext_ = false;       // the up-next card is up
  Rml::String bind_upnext_name_;
  Rml::String bind_upnext_art_ = "none";
  Rml::String bind_upnext_count_;  // "Playing in 8"
  // The tracks being decoded, as the watch bar shows them ("h264
  // 1920x1080", "eng · ac3 5.1(side)"); "" when there is none of the kind.
  Rml::String bind_watch_vtrack_;
  Rml::String bind_watch_atrack_;
  Rml::String bind_detail_image_;  // local image path, "" = none
  Rml::String bind_detail_cover_ = "none"; // the same as a decorator
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
  bool bind_photo_loading_ = false;    // (not bound: one of bind_busy_'s)
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
  // Under a dialog's title: "7:42 of 14:48 · 7:06 left" and how far that
  // is, for the resume dialog; "" for none.
  Rml::String bind_menu_meta_;
  Rml::String bind_menu_progress_ = "0%"; // data-style-width; never empty
  std::vector<MenuRow> menu_rows_;
  int sel_menu_ = 0;
  std::vector<MenuItem> menu_items_;
  std::string menu_section_;       // heading the next entry added gets
  // A menu opened from another: back returns to that one (this reopens
  // it) rather than closing them all.
  std::function<void()> menu_parent_;
  // A plugin's settings menu is up, over a folder of that plugin: what
  // the settings were when it opened. The folder is listed again when the
  // menu closes on other settings than these.
  bool plugin_menu_open_ = false;
  int sel_plugin_setting_ = 0;     // the row of it last changed
  uint64_t plugin_menu_revision_ = 0;

  // Home.
  std::vector<RailRow> rail_rows_;
  std::vector<ShelfRow> shelf_rows_;
  int sel_rail_ = 0;
  int sel_shelf_ = 0;
  int sel_card_ = 0;
  bool bind_home_shelves_ = false;     // focus is on the shelves
  // Settings: the tab is the place (its page shows), and the focus is down
  // in its list.
  bool bind_settings_page_ = false;
  bool bind_home_settings_ = false;
  std::vector<SettingItem> setting_items_;
  std::vector<SettingRow> setting_rows_;
  int sel_setting_ = 0;
  Rml::String bind_setting_title_;    // the focused one, beside the list
  Rml::String bind_setting_about_;
  Rml::String bind_hero_icon_;
  Rml::String bind_hero_title_;
  Rml::String bind_hero_detail_;
  Rml::String bind_hero_note_;
  // The banner (see FeaturedIndex): what the featured card is, large. An
  // empty title hides it; any other part that is empty is left out.
  Rml::String bind_feat_title_;
  Rml::String bind_feat_eyebrow_;     // the shelf it is on: "Continue watching"
  Rml::String bind_feat_meta_;        // "17:40 left  ·  SVT Play"
  Rml::String bind_feat_desc_;
  Rml::String bind_feat_progress_ = "0%"; // never empty, see EntryRow
  bool bind_feat_live_ = false;  // the banner shows what plays: live time
  // On the Settings page: no banner, so that the list starts right under
  // the tabs (the tab already says where we are).
  bool bind_home_compact_ = false;
  std::vector<RailItem> rail_;
  // Per group of sources (indexed by browse::Source::Group): looked for at
  // least once, a search under way and what to do when it ends, and what
  // the providers had to say about the last one ("" when all went well).
  struct GroupScan {
    bool scanned = false;
    bool running = false;
    std::vector<std::function<void()>> waiting;
    Rml::String errors;
  };
  GroupScan scans_[3];
  std::vector<Shelf> shelves_;
  std::vector<int> shelf_card_sel_;    // remembered column per shelf

  std::vector<EntryRow> entry_rows_;
  // The rows the document has: those around the selection. A folder of a
  // thousand entries as a thousand rows is a thousand rows to lay out and
  // draw every frame, which a console does at a few frames a second; the
  // rows above and below the window are stood in for by two spacers of
  // their height, so scrolling and the scroll bar are as before.
  std::vector<EntryRow> window_rows_;
  int window_first_ = 0;
  int window_mode_ = -1;              // ViewMode the window was made for
  bool entry_rows_dirty_ = false;     // entry_rows_ changed; the window follows
  int bind_sel_window_ = 0;           // sel_entry_ within the window
  Rml::String bind_spacer_top_ = "0px"; // "<n>px"; "0px" for none
  Rml::String bind_spacer_bottom_ = "0px";
  void SyncEntryWindow();
  int sel_entry_ = 0;
  int entry_count_ = 0;                // rows shown
  int entry_total_ = 0;                // entries of the folder, filtered or not

  // Backing data (main thread).
  std::vector<std::unique_ptr<browse::Provider>> providers_;
  // The plugins' provider (one of providers_), for what only plugins have:
  // settings of their own. Null without a plugin directory.
  browse::JsProvider* js_provider_ = nullptr;
  // What the user chose for those; shared with the plugins, which read it
  // from their worker threads.
  std::shared_ptr<browse::PluginSettings> plugin_settings_ =
    std::make_shared<browse::PluginSettings>();
  uint64_t saved_plugin_settings_ = 0; // plugin_settings_->Revision() last written
  browse::SourcePtr current_source_;   // source being browsed, or null
  std::vector<BrowseLevel> path_;      // root first
  browse::Entry playing_;              // item in the player right now

  // What playback moves through (next, previous, auto-advance): the folder
  // an item was started from, as it was listed then, and the source it
  // resolves through. Copied, because the menus may be anywhere else by
  // the time the next item is due.
  struct PlayQueue {
    browse::SourcePtr source;
    std::string id;              // the folder's id; "" for a single item
    browse::Listing entries;
  };
  PlayQueue queue_;
  PlayQueue launch_queue_;             // what a launch from the menus brings
  bool launch_from_queue_ = false;     // the launch in flight is queue_'s
  bool launch_behind_ = false;         // ... and stays behind the menus

  library::Library library_;
  library::SourceRegistry registry_;
  std::shared_ptr<library::LibrarySource> favorites_source_;
  std::shared_ptr<library::LibrarySource> continue_source_;
  std::shared_ptr<library::LibrarySource> sources_source_;
  std::vector<browse::SourcePtr> discovered_;   // from the providers
  uint64_t library_seen_ = 0;          // generation the UI reflects

  // Where an item was started from, in the browse view: the source on
  // screen (its key) and the folders down to the one it is in, so that
  // Continue watching resumes it there, with that folder as the queue and
  // back leading where it led (see ResumeInPlace). Empty for an item
  // started from Home, or from results of a search.
  std::string launch_place_source_, playing_place_source_;
  std::vector<library::PlaceLevel> launch_place_, playing_place_;
  // The item playing, as the library knows it; 'identified': it does (a
  // file played from outside every source is not).
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
  util::Worker lanes_[(int)Lane::Count];
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
  // Image paths given to the UI that may be loaded as textures, with how
  // many sweeps in a row found nothing referring to them.
  std::unordered_map<std::string, int> textures_;
  double textures_swept_at_ = 0.0;
  std::vector<std::thread> art_threads_;
  std::mutex art_mutex_;
  std::condition_variable art_cv_;
  std::deque<std::pair<std::string, bool>> art_queue_; // (uri, thumb)
  bool art_running_ = false;          // guarded by art_mutex_
  uint32_t browse_request_ = 0;     // id of the browse we are waiting for
  net::CancelToken browse_cancel_;  // ... and the way to give up on it
  bool browse_pending_ = false;     // ... while it has not arrived
  std::atomic<int> busy_ops_{0};

  // In-flight PlayEntry. The main thread stops caring as soon as
  // launch_request_ no longer matches the result that comes back.
  uint32_t launch_seq_ = 0;         // ever-increasing ticket source
  uint32_t launch_request_ = 0;     // ticket we are waiting for; 0 = idle
  net::CancelToken launch_cancel_;  // gives up on its resolve()
  browse::Entry launch_entry_;      // item being opened
  // Direction of a next/previous (or auto-advance) launch, so an item that
  // fails to open is skipped the same way; 0 for an explicit choice.
  int launch_direction_ = 0;

  double toast_deadline_ = 0.0;
  double info_deadline_ = 0.0;      // watch info bar auto-hide
  bool info_stay_ = false;          // ... not: select brought it up
  bool bind_seekbar_visible_ = false; // the top bar
  double seekbar_deadline_ = 0.0;
  bool scroll_entries_pending_ = false;
  bool scroll_menu_pending_ = false;
  bool scroll_settings_pending_ = false;
  bool scroll_rows_pending_ = false;    // Home's rows, onto the focus
  // Home's focus belongs on its rows (it goes back there with Home),
  // unless it was taken up to the tabs. At the start it is on the tabs,
  // on Home.
  bool home_rows_wanted_ = false;
  // The card to focus when Home is back: the one a folder was opened from.
  std::string home_return_id_;
};
