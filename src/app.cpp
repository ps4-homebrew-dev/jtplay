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

#include <RmlUi/Core/Elements/ElementFormControlInput.h>

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
  std::srand((unsigned)std::time(nullptr));
  shuffle_seed_ = (uint32_t)std::rand() | 1u;

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

  // Cache directory: artwork, plus the settings and the library. If it
  // can't be used, remote art is simply skipped and nothing is remembered.
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
  cache_dir_ = cache_dir;
  LoadSettings();
  ApplyLanguages();
  library_.Load(cache_dir + "/library.xml");
  library_seen_ = library_.Generation();
  favorites_source_ = std::make_shared<library::LibrarySource>(
    library::LibrarySource::Kind::Favorites, library_, registry_);
  continue_source_ = std::make_shared<library::LibrarySource>(
    library::LibrarySource::Kind::ContinueWatching, library_, registry_);

  providers_.push_back(std::make_unique<browse::FsProvider>());
  providers_.push_back(std::make_unique<browse::DlnaProvider>(kDiscoveryWaitMs));
  if (!options.plugins_dir.empty())
    browse::LoadJsProviders(options.plugins_dir, providers_);

  worker_running_ = true;
  worker_ = std::thread(&App::WorkerMain, this);
  {
    std::lock_guard<std::mutex> lock(art_mutex_);
    art_running_ = true;
  }
  art_thread_ = std::thread(&App::ArtworkMain, this);

  Set("view_mode", bind_view_mode_,
      Rml::String(view_mode_ == ViewMode::Grid ? "grid" : "list"));
  RebuildHome();
  // Only the local drives up front: they are found at once. Network
  // servers and plugins are searched for when the rail enters their group.
  StartDiscovery(browse::Source::Group::Local);
  return true;
}

void App::Shutdown() {
  // Stop the workers first; their tasks capture 'this'.
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
    std::lock_guard<std::mutex> lock(art_mutex_);
    art_running_ = false;
    art_queue_.clear();
  }
  art_cv_.notify_all();
  if (art_thread_.joinable())
    art_thread_.join();
  {
    // Nothing is left to apply them to.
    std::lock_guard<std::mutex> lock(replies_mutex_);
    replies_.clear();
  }

  artcache::Shutdown(); // after both threads are joined; they call Fetch()

  if (bind_watching_)
    RecordProgress(true);
  library_.Save();

  visualizer_.Shutdown(); // GL context still current
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
    } else if (key == "view") {
      view_mode_ = value == "grid" ? ViewMode::Grid : ViewMode::List;
    } else if (key == "photo_interval") {
      photo_interval_ = std::clamp(std::atoi(value.c_str()), 2, 120);
    } else if (key == "photo_effect") {
      const int effect = std::atoi(value.c_str());
      if (effect >= 0 && effect < (int)PhotoEffect::Count)
        photo_effect_ = (PhotoEffect)effect;
    } else if (key == "photo_shuffle") {
      photo_shuffle_ = value == "1";
    } else if (key == "repeat") {
      const int mode = std::atoi(value.c_str());
      if (mode >= 0 && mode < (int)Repeat::Count)
        repeat_ = (Repeat)mode;
    } else if (key == "shuffle") {
      shuffle_ = value == "1";
    } else if (key == "autoplay") {
      autoplay_ = value != "0";
    } else if (key == "night_mode") {
      player_->SetNightMode(value == "1");
    } else if (key == "fit") {
      const int fit = std::atoi(value.c_str());
      if (fit >= 0 && fit < (int)Player::Fit::Count)
        player_->SetFit((Player::Fit)fit);
    } else if (key == "audio_delay") {
      player_->SetAudioDelay(std::clamp(std::atof(value.c_str()), -5.0, 5.0));
    } else if (key == "audio_lang") {
      audio_lang_ = value;
    } else if (key == "sub_lang") {
      sub_lang_ = value;
    } else if (key == "screensaver") {
      screensaver_sec_ = std::max(0.0, std::atof(value.c_str()));
    } else if (key == "sub_scale") {
      player_->SetSubtitleScale(std::clamp(std::atof(value.c_str()), 0.5, 2.5));
    }
  }
}

void App::SaveSettings() const {
  if (settings_path_.empty())
    return;
  std::ofstream out(settings_path_, std::ios::trunc);
  out << "sort=" << (int)sort_mode_ << "\n"
      << "sort_desc=" << (sort_desc_ ? 1 : 0) << "\n"
      << "view=" << (view_mode_ == ViewMode::Grid ? "grid" : "list") << "\n"
      << "photo_interval=" << photo_interval_ << "\n"
      << "photo_effect=" << (int)photo_effect_ << "\n"
      << "photo_shuffle=" << (photo_shuffle_ ? 1 : 0) << "\n"
      << "repeat=" << (int)repeat_ << "\n"
      << "shuffle=" << (shuffle_ ? 1 : 0) << "\n"
      << "autoplay=" << (autoplay_ ? 1 : 0) << "\n"
      << "night_mode=" << (player_->NightMode() ? 1 : 0) << "\n"
      << "fit=" << (int)player_->GetFit() << "\n"
      << "audio_delay=" << player_->AudioDelay() << "\n"
      << "sub_scale=" << player_->SubtitleScale() << "\n"
      << "audio_lang=" << audio_lang_ << "\n"
      << "sub_lang=" << sub_lang_ << "\n"
      << "screensaver=" << screensaver_sec_ << "\n";
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

  if (auto row = ctor.RegisterStruct<RailRow>()) {
    row.RegisterMember("icon", &RailRow::icon);
    row.RegisterMember("label", &RailRow::label);
    row.RegisterMember("detail", &RailRow::detail);
  }
  ctor.RegisterArray<std::vector<RailRow>>();

  if (auto row = ctor.RegisterStruct<CardRow>()) {
    row.RegisterMember("title", &CardRow::title);
    row.RegisterMember("subtitle", &CardRow::subtitle);
    row.RegisterMember("glyph", &CardRow::glyph);
    row.RegisterMember("kind", &CardRow::kind);
    row.RegisterMember("art", &CardRow::art);
    row.RegisterMember("progress", &CardRow::progress);
    row.RegisterMember("watched", &CardRow::watched);
  }
  ctor.RegisterArray<std::vector<CardRow>>();

  if (auto row = ctor.RegisterStruct<ShelfRow>()) {
    row.RegisterMember("title", &ShelfRow::title);
    row.RegisterMember("cards", &ShelfRow::cards);
  }
  ctor.RegisterArray<std::vector<ShelfRow>>();

  if (auto row = ctor.RegisterStruct<EntryRow>()) {
    row.RegisterMember("icon", &EntryRow::icon);
    row.RegisterMember("kind", &EntryRow::kind);
    row.RegisterMember("art", &EntryRow::art);
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
  if (auto row = ctor.RegisterStruct<MarkRow>())
    row.RegisterMember("at", &MarkRow::at);
  ctor.RegisterArray<std::vector<MarkRow>>();

  ctor.Bind("view", &bind_view_);
  ctor.Bind("view_mode", &bind_view_mode_);
  ctor.Bind("toast", &bind_toast_);
  ctor.Bind("toast_icon", &bind_toast_icon_);
  ctor.Bind("backdrop", &bind_backdrop_);
  ctor.Bind("crumb", &bind_crumb_);
  ctor.Bind("source_name", &bind_source_name_);
  ctor.Bind("clock", &bind_clock_);
  ctor.Bind("date", &bind_date_);
  ctor.Bind("busy", &bind_busy_);
  ctor.Bind("launch_status", &bind_launch_status_);

  ctor.Bind("rail", &rail_rows_);
  ctor.Bind("sel_rail", &sel_rail_);
  ctor.Bind("shelves", &shelf_rows_);
  ctor.Bind("sel_shelf", &sel_shelf_);
  ctor.Bind("sel_card", &sel_card_);
  ctor.Bind("home_shelves", &bind_home_shelves_);
  ctor.Bind("hero_icon", &bind_hero_icon_);
  ctor.Bind("hero_title", &bind_hero_title_);
  ctor.Bind("hero_detail", &bind_hero_detail_);
  ctor.Bind("hero_note", &bind_hero_note_);

  ctor.Bind("entries", &entry_rows_);
  ctor.Bind("entry_count", &entry_count_);
  ctor.Bind("entry_total", &entry_total_);
  ctor.Bind("sel_entry", &sel_entry_);
  ctor.Bind("sort_label", &bind_sort_label_);
  ctor.Bind("jump_letter", &bind_jump_letter_);

  ctor.Bind("detail_name", &bind_detail_name_);
  ctor.Bind("detail_icon", &bind_detail_icon_);
  ctor.Bind("detail_kind", &bind_detail_kind_);
  ctor.Bind("detail_description", &bind_detail_description_);
  ctor.Bind("detail_props", &detail_props_);
  ctor.Bind("detail_image", &bind_detail_image_);
  ctor.Bind("detail_favorite", &bind_detail_favorite_);

  ctor.Bind("saver", &bind_saver_);
  ctor.Bind("saver_x", &bind_saver_x_);
  ctor.Bind("saver_y", &bind_saver_y_);
  ctor.Bind("saver_now", &bind_saver_now_);

  ctor.Bind("filter_open", &bind_filter_open_);
  ctor.Bind("filter_text", &bind_filter_text_);
  ctor.Bind("search_open", &bind_search_open_);
  ctor.Bind("search_title", &bind_search_title_);

  ctor.Bind("menu_open", &bind_menu_open_);
  ctor.Bind("menu_dialog", &bind_menu_dialog_);
  ctor.Bind("menu_title", &bind_menu_title_);
  ctor.Bind("menu", &menu_rows_);
  ctor.Bind("sel_menu", &sel_menu_);

  ctor.Bind("photo", &bind_photo_);
  ctor.Bind("photo_info", &bind_photo_info_);
  ctor.Bind("photo_playing", &bind_photo_playing_);
  ctor.Bind("photo_loading", &bind_photo_loading_);
  ctor.Bind("photo_fade", &bind_photo_fade_);
  ctor.Bind("photo_front", &bind_photo_front_);
  ctor.Bind("photo_art0", &bind_photo_art_[0]);
  ctor.Bind("photo_art1", &bind_photo_art_[1]);
  ctor.Bind("photo_anim0", &bind_photo_anim_[0]);
  ctor.Bind("photo_anim1", &bind_photo_anim_[1]);
  ctor.Bind("photo_rotate", &bind_photo_rotate_);
  ctor.Bind("photo_sideways", &bind_photo_sideways_);
  ctor.Bind("photo_name", &bind_photo_name_);
  ctor.Bind("photo_meta", &bind_photo_meta_);

  ctor.Bind("watching", &bind_watching_);
  ctor.Bind("info_visible", &bind_info_visible_);
  ctor.Bind("watch_audio", &bind_watch_audio_);
  ctor.Bind("watch_paused", &bind_watch_paused_);
  ctor.Bind("watch_seekable", &bind_watch_seekable_);
  ctor.Bind("watch_name", &bind_watch_name_);
  ctor.Bind("watch_description", &bind_watch_description_);
  ctor.Bind("watch_pos", &bind_watch_pos_);
  ctor.Bind("watch_dur", &bind_watch_dur_);
  ctor.Bind("watch_ends", &bind_watch_ends_);
  ctor.Bind("watch_buffered", &bind_watch_buffered_);
  ctor.Bind("watch_speed", &bind_watch_speed_);
  ctor.Bind("watch_next", &bind_watch_next_);
  ctor.Bind("watch_seek", &bind_watch_seek_);
  ctor.Bind("watch_seek_at", &bind_watch_seek_at_);
  ctor.Bind("watch_progress", &bind_watch_progress_);
  ctor.Bind("watch_chapter", &bind_watch_chapter_);
  ctor.Bind("chapter_marks", &chapter_marks_);
  ctor.Bind("watch_modes", &bind_watch_modes_);
  ctor.Bind("upnext", &bind_upnext_);
  ctor.Bind("upnext_name", &bind_upnext_name_);
  ctor.Bind("upnext_art", &bind_upnext_art_);
  ctor.Bind("upnext_count", &bind_upnext_count_);
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
  UpdatePhotos(now);
  UpdateSleepTimer(now);

  if (image_refresh_pending_) {
    image_refresh_pending_ = false;
    art_scan_pending_ = true;
    RefreshImageBindings();
  }

  if (bind_watching_) {
    UpdateWatchOverlay();
    if (player_->IsPlaying() && player_->AtEnd())
      HandlePlaybackEnd();
    if (bind_info_visible_ && !bind_watch_audio_ && !bind_menu_open_ &&
        now > info_deadline_)
      Set("info_visible", bind_info_visible_, false);
    if (!bind_watch_seek_.empty() && now > seek_bubble_until_)
      Set("watch_seek", bind_watch_seek_, Rml::String());
  }

  if (!bind_jump_letter_.empty() && now > jump_letter_until_)
    Set("jump_letter", bind_jump_letter_, Rml::String());
  if (!bind_toast_.empty() && now > toast_deadline_)
    Set("toast", bind_toast_, Rml::String());

  // Deferred scrolling: the rows generated by data bindings exist only
  // after the context update that follows the change, so scroll one frame
  // later using the fixed row pitch.
  if (scroll_entries_pending_) {
    if (view_mode_ == ViewMode::Grid)
      EnsureRowVisible("entry-list", sel_entry_ / kGridColumns, kGridRowPitch);
    else
      EnsureRowVisible("entry-list", sel_entry_, kEntryRowPitch);
    scroll_entries_pending_ = false;
    art_scan_pending_ = true;
  }
  if (scroll_menu_pending_) {
    EnsureRowVisible("menu-rows", sel_menu_, kMenuRowPitch);
    scroll_menu_pending_ = false;
  }
  // Focusing a text field is what brings up the system keyboard. It waits
  // until the field is on screen: a data-if only hides its element
  // (display: none), and shows it in the context update after the change.
  if (search_focus_pending_) {
    Rml::Element* dialog = document_->GetElementById("search");
    Rml::Element* input = document_->GetElementById("search-input");
    if (!bind_search_open_)
      search_focus_pending_ = false;
    else if (dialog && input && dialog->IsVisible() && input->Focus())
      search_focus_pending_ = false;
  }
  if (filter_focus_pending_) {
    Rml::Element* bar = document_->GetElementById("filter-bar");
    Rml::Element* input = document_->GetElementById("filter-input");
    if (!bind_filter_open_)
      filter_focus_pending_ = false;
    else if (bar && input && bar->IsVisible() && input->Focus()) {
      filter_focus_pending_ = false;
      scroll_entries_pending_ = true; // the list got shorter
      // The filter put back in the field is there to be added to: the caret
      // goes after it (this only holds once the field has the focus).
      const int end = (int)static_cast<Rml::ElementFormControlInput*>(input)->GetValue().size();
      static_cast<Rml::ElementFormControlInput*>(input)->SetSelectionRange(end, end);
    }
  }
  // The list follows what is typed into the filter bar, a key at a time.
  // (Polled: the field has no change event worth depending on, and the
  // system keyboard hands over its text all at once anyway.)
  if (bind_filter_open_ && !filter_focus_pending_)
    ApplyFilter(FilterFieldText());
  if (scroll_sources_pending_) {
    EnsureRowVisible("rail", sel_rail_, kRailRowPitch);
    if (Rml::Element* shelves = document_->GetElementById("shelves")) {
      // The focused shelf in view, then the focused card in its row.
      const float pitch = kShelfPitch;
      const float view_h = shelves->GetClientHeight();
      float top = shelves->GetScrollTop();
      const float row_top = sel_shelf_ * pitch;
      if (row_top < top)
        top = row_top;
      else if (row_top + pitch > top + view_h)
        top = row_top + pitch - view_h;
      shelves->SetScrollTop(std::max(0.0f, top));
      const std::string id = "cards-" + std::to_string(sel_shelf_);
      if (Rml::Element* row = document_->GetElementById(id)) {
        const float view_w = row->GetClientWidth();
        float left = row->GetScrollLeft();
        const float card_left = sel_card_ * kCardPitch;
        if (card_left < left + kCardPitch * 0.5f)
          left = card_left - kCardPitch * 0.5f;
        else if (card_left + kCardPitch * 1.5f > left + view_w)
          left = card_left + kCardPitch * 1.5f - view_w;
        row->SetScrollLeft(std::max(0.0f, left));
      }
    }
    scroll_sources_pending_ = false;
  }

  if (art_scan_pending_) {
    art_scan_pending_ = false;
    UpdateVisibleArt();
  }
}

void App::RenderVideo(int width, int height) {
  // Audio-only: the visualizer is the picture. It also covers the moment
  // before the player is up, so the screen never flashes black.
  if (bind_watching_ && bind_watch_audio_) {
    visualizer_.Render(*player_, width, height, player_->IsPaused());
    return;
  }
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

void App::ShowToast(const std::string& text, const char* icon) {
  toast_deadline_ = Now() + kToastSec;
  Set("toast", bind_toast_, text);
  Set("toast_icon", bind_toast_icon_, Rml::String(icon ? icon : "ℹ"));
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

  // From a folder: Ctrl+F filters it, Ctrl+S searches the source. Not
  // buttons of the controller; there the options menu has both.
  if ((key == Rml::Input::KI_F || key == Rml::Input::KI_S) &&
      (event.GetParameter<int>("ctrl_key", 0) != 0) &&
      view_ == View::Browse && !bind_filter_open_ && !bind_search_open_ &&
      !bind_menu_open_ && !bind_photo_ && !bind_watching_) {
    if (key == Rml::Input::KI_F)
      OpenFilter();
    else
      OpenSearch();
    event.StopPropagation();
    return;
  }

  // The text fields come first: they have to see the typing.
  if (bind_filter_open_) {
    HandleKeyFilter(event, key);
    return;
  }
  if (bind_search_open_) {
    HandleKeySearch(event, key);
    return;
  }
  if (bind_menu_open_) {
    HandleKeyMenu(event, key);
    return;
  }
  if (bind_photo_) {
    HandleKeyPhoto(event, key);
    return;
  }
  if (bind_watching_) {
    HandleKeyWatch(event, key);
    return;
  }
  if (view_ == View::Home) {
    HandleKeyHome(event, key);
    return;
  }
  HandleKeyBrowse(event, key);
}

// ---------------------------------------------------------------------------
// Artwork
// ---------------------------------------------------------------------------

std::string App::CoverDecorator(const std::string& path) {
  if (path.empty())
    return "none";
  // Quoted, so a path with spaces or parentheses stays one argument.
  return "image(\"" + path + "\" cover)";
}

std::string App::ImagePathFor(const std::string& uri, bool thumb) {
  // Anything shown large (details, backdrop, now playing, the photo viewer)
  // uses the screen-sized copy, so a camera original never becomes a
  // texture. artcache answers for an already-made copy without any I/O;
  // everything else goes to the artwork thread, because RmlUi wants a path
  // now and the download is a blocking request.
  std::string path;
  const artcache::Status status =
    thumb ? artcache::LookupThumb(uri, path) : artcache::LookupScreen(uri, path);
  switch (status) {
  case artcache::Status::Ready:
    return path;
  case artcache::Status::Failed:
    return {};
  case artcache::Status::Missing:
    break;
  }

  const std::string key = (thumb ? "t:" : "f:") + uri;
  if (!image_inflight_.insert(key).second)
    return {}; // already asked for; the next refresh picks it up

  {
    std::lock_guard<std::mutex> lock(art_mutex_);
    art_queue_.emplace_front(uri, thumb);
    // Scrolling fast through a big wall asks for far more than it shows;
    // what fell off the end is asked for again if it comes back on screen.
    while (art_queue_.size() > kArtQueueMax) {
      const auto& dropped = art_queue_.back();
      image_inflight_.erase((dropped.second ? "t:" : "f:") + dropped.first);
      art_queue_.pop_back();
    }
  }
  art_cv_.notify_one();
  return {};
}

void App::ArtworkMain() {
  for (;;) {
    std::pair<std::string, bool> job;
    {
      std::unique_lock<std::mutex> lock(art_mutex_);
      art_cv_.wait(lock, [&] { return !art_queue_.empty() || !art_running_; });
      if (!art_running_)
        return;
      job = std::move(art_queue_.front());
      art_queue_.pop_front();
    }
    // The cache holds the result; the main thread only needs the signal.
    if (job.second)
      artcache::FetchThumb(job.first);
    else
      artcache::FetchScreen(job.first);
    const std::string key = (job.second ? "t:" : "f:") + job.first;
    Reply([this, key] {
      image_inflight_.erase(key);
      image_refresh_pending_ = true; // coalesced: one refresh per frame
    });
  }
}

void App::RefreshImageBindings() {
  // Details pane follows the browse selection.
  Rml::String detail;
  if (view_ == View::Browse) {
    if (const browse::Entry* entry = SelectedEntry())
      detail = ImagePathFor(ArtUri(*entry));
  }
  Set("detail_image", bind_detail_image_, detail);

  // Now-playing card / transport bar follows the playing item.
  Rml::String watch;
  if (bind_watching_)
    watch = ImagePathFor(playing_.image);
  Set("watch_image", bind_watch_image_, watch);

  UpdateBackdrop();
}

// The selected item's artwork, large and dim behind everything (Kodi's
// fanart): the focused card at home, the selection while browsing.
void App::UpdateBackdrop() {
  std::string uri;
  if (view_ == View::Browse) {
    if (const browse::Entry* entry = SelectedEntry())
      uri = ArtUri(*entry);
  } else if (bind_home_shelves_) {
    if (const browse::Entry* card = SelectedCard())
      uri = ArtUri(*card);
  }
  // Keep the last picture while the next one loads, rather than flash to
  // nothing between two items that both have art.
  const std::string path = uri.empty() ? std::string() : ImagePathFor(uri);
  if (!path.empty() || uri.empty())
    Set("backdrop", bind_backdrop_, CoverDecorator(path));
}
