// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>

#include <sys/stat.h>

#include <SDL.h>

#include "app.h"
#include "app_internal.h"
#include "artcache.h"
#include "browse/dlna_source.h"
#include "browse/fs_source.h"
#include "browse/js_source.h"

using namespace appdetail;

App::App() : player_(std::make_unique<Player>()) {}

App::~App() = default;

bool App::Initialize(Rml::Context* context, const Options& options, std::string& error) {
  context_ = context;

  if (!SetupDataModel(context, error))
    return false;

  const std::string doc_path = options.assets_dir + "/main.rml";
  if (!(document_ = context->LoadDocument(doc_path))) {
    error = "failed to load " + doc_path;
    return false;
  }

  document_->Show();
  document_->AddEventListener(Rml::EventId::Keydown, this, true);

  // The player draws with the window's GL context, current here. Without it
  // the app still browses; playing says why it can't.
  std::string player_error;
  if (!player_->Initialize(player_error, options.fonts_dir))
    Rml::Log::Message(Rml::Log::LT_WARNING, "No playback: %s", player_error.c_str());

  // Cache directory: artwork, plus the small settings file. If it can't be
  // used, remote art is simply skipped and settings are not remembered.
  std::string cache_dir = options.cache_dir;
  if (cache_dir.empty()) {
    const char* base = std::getenv("XDG_CACHE_HOME");
    cache_dir = (base && *base) ? std::string(base)
      : (std::getenv("HOME") ? std::string(std::getenv("HOME")) + "/.cache"
                             : std::string("/tmp"));
    ::mkdir(cache_dir.c_str(), 0755); // the parent; artcache makes the leaf
    cache_dir += "/jtplay";
  }
  if (!artcache::Initialize(cache_dir))
    Rml::Log::Message(Rml::Log::LT_WARNING,
      "Cannot use cache directory '%s' (%s); remote artwork disabled",
      cache_dir.c_str(), std::strerror(errno));
  settings_path_ = cache_dir + "/settings.ini";
  LoadSettings();
  library_.Load(cache_dir + "/library.xml");
  library_seen_ = library_.Generation();
  favorites_source_ = std::make_shared<library::LibrarySource>(
    library::LibrarySource::Kind::Favorites, library_, registry_);
  continue_source_ = std::make_shared<library::LibrarySource>(
    library::LibrarySource::Kind::ContinueWatching, library_, registry_);
  RebuildSources();

  providers_.push_back(std::make_unique<browse::FsProvider>());
  providers_.push_back(std::make_unique<browse::DlnaProvider>(kDiscoveryWaitMs));
  if (!options.plugins_dir.empty())
    browse::LoadJsProviders(options.plugins_dir, providers_);

  worker_running_ = true;
  worker_ = std::thread(&App::WorkerMain, this);

  StartDiscovery();
  return true;
}

void App::Shutdown() {
  // Stop the worker first; its tasks capture 'this'.
  if (worker_running_) {
    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      worker_running_ = false;
      tasks_.clear();
    }
    tasks_cv_.notify_all();
    if (worker_.joinable())
      worker_.join();
  }
  {
    // Nothing is left to apply them to.
    std::lock_guard<std::mutex> lock(replies_mutex_);
    replies_.clear();
  }

  artcache::Shutdown(); // after the worker is joined; it calls Fetch()

  if (bind_watching_)
    RecordProgress(true);
  library_.Save();

  if (player_) {
    player_->Stop();
    player_->Shutdown();
  }

  if (document_) {
    document_->RemoveEventListener(Rml::EventId::Keydown, this, true);
    document_->Close();
    document_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

// A handful of "key=value" lines; unknown keys and bad values are ignored,
// so the file never stops the app from starting.
void App::LoadSettings() {
  std::ifstream in(settings_path_);
  std::string line;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == "sort") {
      const int mode = std::atoi(value.c_str());
      if (mode >= 0 && mode < (int)SortMode::Count)
        sort_mode_ = (SortMode)mode;
    } else if (key == "sort_desc") {
      sort_desc_ = value == "1";
    }
  }
}

void App::SaveSettings() const {
  if (settings_path_.empty())
    return;
  std::ofstream out(settings_path_, std::ios::trunc);
  out << "sort=" << (int)sort_mode_ << "\n"
      << "sort_desc=" << (sort_desc_ ? 1 : 0) << "\n";
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

void App::PostTask(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    if (!worker_running_)
      return;
    tasks_.push_back(std::move(task));
  }
  tasks_cv_.notify_one();
}

// Worker thread: hands a result to the main thread. Replies are applied in
// the order they were posted, so a result that has since been superseded
// still arrives -- and is dropped by its own request-id check.
void App::Reply(std::function<void()> apply) {
  std::lock_guard<std::mutex> lock(replies_mutex_);
  replies_.push_back(std::move(apply));
}

void App::RunReplies() {
  std::deque<std::function<void()>> ready;
  {
    std::lock_guard<std::mutex> lock(replies_mutex_);
    ready.swap(replies_);
  }
  for (std::function<void()>& apply : ready)
    apply();
}

void App::WorkerMain() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(tasks_mutex_);
      tasks_cv_.wait(lock, [&] { return !tasks_.empty() || !worker_running_; });
      if (!worker_running_)
        return;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    task();
  }
}

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

bool App::SetupDataModel(Rml::Context* context, std::string& error) {
  Rml::DataModelConstructor ctor = context->CreateDataModel("jtplay");
  if (!ctor) {
    error = "failed to create data model";
    return false;
  }

  if (auto row = ctor.RegisterStruct<SourceRow>()) {
    row.RegisterMember("icon", &SourceRow::icon);
    row.RegisterMember("name", &SourceRow::name);
    row.RegisterMember("detail", &SourceRow::detail);
  }
  ctor.RegisterArray<std::vector<SourceRow>>();

  if (auto row = ctor.RegisterStruct<EntryRow>()) {
    row.RegisterMember("icon", &EntryRow::icon);
    row.RegisterMember("name", &EntryRow::name);
    row.RegisterMember("meta", &EntryRow::meta);
    row.RegisterMember("progress", &EntryRow::progress);
    row.RegisterMember("folder", &EntryRow::folder);
    row.RegisterMember("favorite", &EntryRow::favorite);
    row.RegisterMember("watched", &EntryRow::watched);
  }
  ctor.RegisterArray<std::vector<EntryRow>>();

  if (auto row = ctor.RegisterStruct<PropRow>()) {
    row.RegisterMember("label", &PropRow::label);
    row.RegisterMember("value", &PropRow::value);
  }
  ctor.RegisterArray<std::vector<PropRow>>();

  if (auto row = ctor.RegisterStruct<MenuRow>()) {
    row.RegisterMember("label", &MenuRow::label);
    row.RegisterMember("value", &MenuRow::value);
  }
  ctor.RegisterArray<std::vector<MenuRow>>();

  ctor.Bind("view", &bind_view_);
  ctor.Bind("status", &bind_status_);
  ctor.Bind("toast", &bind_toast_);
  ctor.Bind("crumb", &bind_crumb_);
  ctor.Bind("source_name", &bind_source_name_);
  ctor.Bind("clock", &bind_clock_);
  ctor.Bind("date", &bind_date_);
  ctor.Bind("busy", &bind_busy_);
  ctor.Bind("launch_status", &bind_launch_status_);

  ctor.Bind("sources", &source_rows_);
  ctor.Bind("source_count", &source_count_);
  ctor.Bind("sel_source", &sel_source_);

  ctor.Bind("entries", &entry_rows_);
  ctor.Bind("entry_count", &entry_count_);
  ctor.Bind("sel_entry", &sel_entry_);
  ctor.Bind("sort_label", &bind_sort_label_);

  ctor.Bind("detail_name", &bind_detail_name_);
  ctor.Bind("detail_icon", &bind_detail_icon_);
  ctor.Bind("detail_description", &bind_detail_description_);
  ctor.Bind("detail_props", &detail_props_);
  ctor.Bind("detail_image", &bind_detail_image_);
  ctor.Bind("detail_favorite", &bind_detail_favorite_);

  ctor.Bind("saver", &bind_saver_);
  ctor.Bind("saver_x", &bind_saver_x_);
  ctor.Bind("saver_y", &bind_saver_y_);
  ctor.Bind("saver_now", &bind_saver_now_);

  ctor.Bind("menu_open", &bind_menu_open_);
  ctor.Bind("menu_title", &bind_menu_title_);
  ctor.Bind("menu", &menu_rows_);
  ctor.Bind("sel_menu", &sel_menu_);

  ctor.Bind("watching", &bind_watching_);
  ctor.Bind("info_visible", &bind_info_visible_);
  ctor.Bind("watch_audio", &bind_watch_audio_);
  ctor.Bind("watch_paused", &bind_watch_paused_);
  ctor.Bind("watch_seekable", &bind_watch_seekable_);
  ctor.Bind("watch_name", &bind_watch_name_);
  ctor.Bind("watch_description", &bind_watch_description_);
  ctor.Bind("watch_pos", &bind_watch_pos_);
  ctor.Bind("watch_dur", &bind_watch_dur_);
  ctor.Bind("watch_buffered", &bind_watch_buffered_);
  ctor.Bind("watch_speed", &bind_watch_speed_);
  ctor.Bind("watch_ends", &bind_watch_ends_);
  ctor.Bind("watch_next", &bind_watch_next_);
  ctor.Bind("watch_progress", &bind_watch_progress_);
  ctor.Bind("watch_vtrack", &bind_watch_vtrack_);
  ctor.Bind("watch_atrack", &bind_watch_atrack_);
  ctor.Bind("watch_image", &bind_watch_image_);

  model_ = ctor.GetModelHandle();
  return true;
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------

void App::Update() {
  const double now = Now();

  // Clock and date in the top bar.
  {
    const std::time_t t = std::time(nullptr);
    std::tm tm = {};
    localtime_r(&t, &tm);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    Set("clock", bind_clock_, Rml::String(buf));
    std::strftime(buf, sizeof(buf), "%A, %d %B", &tm);
    Set("date", bind_date_, Rml::String(buf));
  }

  Set("busy", bind_busy_, busy_ops_.load() > 0);

  // How far an in-flight launch has got. Only shown once the open has been
  // slow enough to be worth mentioning.
  {
    Rml::String status;
    if (launch_request_ && now >= launch_visible_at_) {
      switch (launch_phase_.load()) {
      case kLaunchResolving: status = "Resolving..."; break;
      case kLaunchOpening:   status = "Opening stream..."; break;
      default:               break;
      }
    }
    Set("launch_status", bind_launch_status_, status);
  }

  RunReplies();

  // Favorites or progress changed (here or in a worker reply): bring the
  // views that show them up to date, and save now and then.
  if (library_.Generation() != library_seen_) {
    library_seen_ = library_.Generation();
    OnLibraryChanged();
  }
  if (bind_watching_ && player_->IsPlaying() && now - progress_saved_at_ > 15.0) {
    progress_saved_at_ = now;
    RecordProgress(false);
  }

  UpdateScreensaver(now);

  if (image_refresh_pending_) {
    image_refresh_pending_ = false;
    RefreshImageBindings();
  }

  if (bind_watching_) {
    UpdateWatchOverlay();
    if (player_->IsPlaying() && player_->AtEnd())
      HandlePlaybackEnd();
    if (bind_info_visible_ && !bind_watch_audio_ && !bind_menu_open_ &&
        now > info_deadline_)
      Set("info_visible", bind_info_visible_, false);
  }

  if (!bind_toast_.empty() && now > toast_deadline_)
    Set("toast", bind_toast_, Rml::String());

  // Deferred scrolling: the rows generated by data bindings exist only
  // after the context update that follows the change, so scroll one frame
  // later using the fixed row pitch.
  if (scroll_entries_pending_) {
    EnsureRowVisible("entry-list", sel_entry_, kEntryRowPitch);
    scroll_entries_pending_ = false;
  }
  if (scroll_sources_pending_) {
    EnsureRowVisible("source-list", sel_source_, kSourceRowPitch);
    scroll_sources_pending_ = false;
  }
}

void App::RenderVideo(int width, int height) {
  player_->RenderVideo(width, height);
}

void App::EnsureRowVisible(const char* list_id, int index, float row_pitch) {
  if (!document_)
    return;
  Rml::Element* list = document_->GetElementById(list_id);
  if (!list)
    return;

  // Keep a few rows of context around the selection (fewer if the list is
  // too short for that), so the next item is visible before it is reached.
  const float view_h = list->GetClientHeight();
  const int fit = (int)(view_h / row_pitch);
  const int margin = std::max(0, std::min(kScrollMarginRows, (fit - 1) / 2));
  const float row_top = (index - margin) * row_pitch;
  const float row_bottom = (index + 1 + margin) * row_pitch;
  float scroll = list->GetScrollTop();

  if (row_top < scroll)
    scroll = row_top;
  else if (row_bottom > scroll + view_h)
    scroll = row_bottom - view_h;
  list->SetScrollTop(std::max(0.0f, scroll));
}

void App::ShowToast(const std::string& text) {
  toast_deadline_ = Now() + kToastSec;
  Set("toast", bind_toast_, text);
}

// ---------------------------------------------------------------------------
// Input routing
// ---------------------------------------------------------------------------

void App::ProcessEvent(Rml::Event& event) {
  if (event.GetId() != Rml::EventId::Keydown)
    return;

  const int key = event.GetParameter<int>("key_identifier", 0);

  // The first key after the screensaver came up only wakes the screen.
  last_input_ = Now();
  if (bind_saver_) {
    Set("saver", bind_saver_, false);
    event.StopPropagation();
    return;
  }

  // Global quit (keyboard only; controllers use the system's own menu).
  if (key == Rml::Input::KI_Q &&
      (event.GetParameter<int>("ctrl_key", 0) != 0)) {
    SDL_Event quit = {};
    quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
    event.StopPropagation();
    return;
  }

  if (bind_menu_open_) {
    HandleKeyMenu(event, key);
    return;
  }
  if (bind_watching_) {
    HandleKeyWatch(event, key);
    return;
  }
  if (view_ == View::Sources) {
    HandleKeySources(event, key);
    return;
  }
  HandleKeyBrowse(event, key);
}

// ---------------------------------------------------------------------------
// Artwork
// ---------------------------------------------------------------------------

std::string App::ImagePathFor(const std::string& uri) {
  // artcache answers for a local file or an already-cached download without
  // any I/O; everything else has to go to the worker, because RmlUi wants a
  // path now and the download is a blocking request.
  std::string path;
  switch (artcache::Lookup(uri, path)) {
  case artcache::Status::Ready:
    return path;
  case artcache::Status::Failed:
    return {};
  case artcache::Status::Missing:
    break;
  }

  if (!image_inflight_.insert(uri).second)
    return {}; // already being fetched; the next Refresh picks it up

  PostTask([this, uri] {
    artcache::Fetch(uri); // the cache holds the result; we only signal
    Reply([this, uri] {
      image_inflight_.erase(uri);
      image_refresh_pending_ = true; // coalesced: one refresh per frame
    });
  });

  return {};
}

void App::RefreshImageBindings() {
  // Details pane follows the browse selection.
  Rml::String detail;
  if (view_ == View::Browse) {
    if (const browse::Entry* entry = SelectedEntry())
      detail = ImagePathFor(entry->image);
  }
  Set("detail_image", bind_detail_image_, detail);

  // Now-playing card / transport bar follows the playing item.
  Rml::String watch;
  if (bind_watching_)
    watch = ImagePathFor(playing_.image);
  Set("watch_image", bind_watch_image_, watch);
}
