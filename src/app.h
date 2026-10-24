// SPDX-License-Identifier: GPL-3.0-or-later
//
// Application shell, structured after ps5-payload-dev/tvhp: all RmlUi access
// happens on the main thread; anything that touches the network or the disk
// (discovery, browsing, opening a stream) runs on a single worker thread
// whose results are polled once per frame in Update().
//
// The app itself is source-agnostic: it browses browse::Source objects that
// browse::Providers discover (DLNA servers, local directories, ...).
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
#include <vector>

#include <RmlUi/Core.h>

#include "browse/source.h"
#include "player.h"

class App : public Rml::EventListener {
public:
  App();
  ~App() override;

  struct Options {
    std::string assets_dir = "assets";   // directory containing main.rml
    std::string cache_dir;             // artwork cache; "" = XDG default
    std::string plugins_dir;           // JS plugin scripts; "" = none
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
  enum class View { Sources, Browse };

  // One level of the browse tree; kept on a stack so backing out is
  // instant and restores the selection.
  struct BrowseLevel {
    std::string id;
    std::string name;
    browse::Listing entries;
    int selection = 0;
  };

  // Rows exposed to the RmlUi data model.
  struct SourceRow {
    Rml::String icon;
    Rml::String name;
    Rml::String detail;
  };
  struct EntryRow {
    Rml::String icon;   // glyph for the type of entry
    Rml::String name;
    Rml::String description;
    bool folder = false;
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

  // --- Sources view ------------------------------------------------------
  void StartDiscovery();
  void RebuildSourceRows();
  void HandleKeySources(Rml::Event& event, int key);

  // --- Browse view -------------------------------------------------------
  void OpenSource(int index);            // browse the root of a source
  void EnterContainer(const browse::Entry& entry);
  void LeaveContainer();                 // back one level (or to sources)
  void RequestBrowse(const std::string& id, const std::string& name);
  void RebuildEntryRows();
  void RebuildDetail();
  void RebuildCrumb();
  void MoveSelection(int delta);
  void ActivateSelection();
  void HandleKeyBrowse(Rml::Event& event, int key);
  const browse::Entry* SelectedEntry() const;
  BrowseLevel* CurrentLevel();

  // --- Playback ----------------------------------------------------------
  void PlayEntry(const browse::Entry& entry);
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
  bool PlayNeighbor(int direction);      // next/previous playable item
  void CycleAudioTrack();                // triangle: next audio track
  void CycleVideoTrack();                // square: next video track
  void HandleKeyWatch(Rml::Event& event, int key);

  void EnsureRowVisible(const char* list_id, int index, float row_pitch);
  void ShowToast(const std::string& text);

  // --- Artwork -----------------------------------------------------------
  // Entry::image is a URI: file:// (or a bare path) for local artwork,
  // http(s):// for remote. RmlUi only loads textures through the file
  // interface, so everything has to end up as a local path; artcache is
  // where that happens, for every source alike.
  // Returns the displayable path, or "" while a download is (or has been
  // scheduled to be) fetched in the background, or if it failed.
  std::string ImagePathFor(const std::string& uri);
  void RefreshImageBindings();     // re-resolve detail/now-playing artwork

  // Data model bound state (main thread only).
  Rml::DataModelHandle model_;
  Rml::String bind_view_ = "sources";
  Rml::String bind_status_;       // sources view status line
  Rml::String bind_toast_;
  Rml::String bind_crumb_;        // breadcrumb of the browse path
  Rml::String bind_source_name_;  // topbar: connected source
  Rml::String bind_clock_;
  Rml::String bind_detail_name_;
  Rml::String bind_detail_description_;
  Rml::String bind_player_status_;
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
  Rml::String bind_watch_time_;
  Rml::String bind_watch_progress_ = "0%"; // data-style-width; never empty
  // Codec lines shown in the watch bar: the label of the video and audio
  // track being decoded ("h264 1920x1080", "eng · ac3 5.1(side)"), or ""
  // when the file has no stream of that kind. The multi_ flags say whether
  // there is anything to switch to, and gate the button hints.
  Rml::String bind_watch_vtrack_;
  Rml::String bind_watch_atrack_;
  bool bind_watch_multi_video_ = false;
  bool bind_watch_multi_audio_ = false;
  Rml::String bind_detail_image_;  // local image path, "" = none
  Rml::String bind_watch_image_;   // now-playing artwork path, "" = none

  std::vector<SourceRow> source_rows_;
  std::vector<EntryRow> entry_rows_;
  int sel_source_ = 0;
  int source_count_ = 0;
  int sel_entry_ = 0;
  int entry_count_ = 0;

  // Backing data (main thread).
  std::vector<std::unique_ptr<browse::Provider>> providers_;
  std::vector<browse::SourcePtr> sources_;
  browse::SourcePtr current_source_;   // source being browsed, or null
  std::vector<BrowseLevel> path_;      // root first
  browse::Entry playing_;              // item in the player right now

  std::unique_ptr<Player> player_;
  Rml::Context* context_ = nullptr;
  Rml::ElementDocument* document_ = nullptr;
  View view_ = View::Sources;

  // Worker plumbing: jobs going out, results coming back.
  std::thread worker_;
  std::mutex tasks_mutex_;
  std::condition_variable tasks_cv_;
  std::deque<std::function<void()>> tasks_;
  std::atomic<bool> worker_running_{false};
  std::mutex replies_mutex_;
  std::deque<std::function<void()>> replies_;

  // Artwork (main thread). The cache itself is artcache; this is only the
  // set of URIs a worker task is already fetching, so the UI asks once.
  std::set<std::string> image_inflight_;
  bool image_refresh_pending_ = false;
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
  double launch_visible_at_ = 0.0;  // suppress the phase line before this

  double toast_deadline_ = 0.0;
  double info_deadline_ = 0.0;      // watch info bar auto-hide
  bool scroll_entries_pending_ = false;
  bool scroll_sources_pending_ = false;
};
