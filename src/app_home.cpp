// SPDX-License-Identifier: GPL-3.0-or-later
//
// The home screen: one page of rows, the focus on its first card, so that
// what was last watched or listened to is a press of select away.
//
//   Now playing         while something does: the way back to it
//   Continue watching   unfinished items; select resumes, at once
//   Recent              live streams as themselves, and the folders other
//                       things were played from (where the next one is)
//   Favorites           anything pinned, most recently first
//   Sources             every source there is, local, network or plugin,
//                       most used first
//
// The sources are remembered from one run to the next (library.h), so the
// row is there from the start, as it was; the providers look for them again
// in the background, and a tile not found yet is looked for when selected.
// Above the rows are two tabs, Home and Settings (a gear): up from the
// first row reaches them, back or down returns. Settings is a list of its
// own, under its tab (see BuildSettings).
//
// The banner shows the card in focus, large -- its artwork across the top
// of the screen, its title, what is left of it, its description -- and so
// follows the focus along the rows, as the big streaming apps do. Whatever
// a card has no data for is left out rather than shown empty.
//
// The internals still say "rail" for the tabs: they were a rail down the
// left once.
#include <algorithm>
#include <set>

#include "app.h"
#include "app_internal.h"
#include "browse/js_source.h"
#include "gamepad.h"
#include "icons.h"

using namespace appdetail;

namespace {

// Of Recent, this many cards at most.
constexpr size_t kRecentShown = 10;

} // namespace

void App::GoHome() {
  CancelForegroundLaunch();
  AbandonBrowse(); // any browse that is still in flight
  path_.clear();
  current_source_.reset();
  view_ = View::Home;
  Set("view", bind_view_, Rml::String("home"));
  // Back on Home: on its rows, on the card it was left from.
  home_rows_wanted_ = true;
  RebuildHome("home");
  if (!home_return_id_.empty()) {
    FocusHomeCard(home_return_id_);
    home_return_id_.clear();
  }
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

namespace {

using Group = browse::Source::Group;
constexpr Group kGroups[] = {Group::Local, Group::Plugin, Group::Network};

// What a group is called, in a toast while one of its sources is looked for.
const char* GroupIcon(Group group) {
  switch (group) {
  case Group::Local:   return icon::kDrive;
  case Group::Network: return icon::kServer;
  default:             return icon::kPlugin;
  }
}

// The group a source belongs to, from its key alone, so a server that is
// offline right now is still looked for in the right place. Follows the
// Key() of FsSource ("fs:"), DlnaSource ("dlna:") and JsSource ("js:").
Group GroupOfKey(const std::string& key) {
  if (key.compare(0, 3, "fs:") == 0)
    return Group::Local;
  if (key.compare(0, 5, "dlna:") == 0)
    return Group::Network;
  return Group::Plugin;
}

std::string StandInId(const std::string& key) {
  return "source:" + key;
}

} // namespace

void App::StartDiscovery(browse::Source::Group group,
                         std::function<void()> done) {
  GroupScan& scan = scans_[(int)group];
  if (done)
    scan.waiting.push_back(std::move(done));
  if (scan.running)
    return;
  scan.running = true;
  if (view_ == View::Home)
    RebuildHome();

  // The providers stay put for the app's lifetime, so the worker can take
  // plain pointers to the ones it needs.
  std::vector<browse::Provider*> providers;
  for (const std::unique_ptr<browse::Provider>& p : providers_)
    if (p->RailGroup() == group)
      providers.push_back(p.get());

  PostTask(Lane::Discovery, [this, group, providers] {
    std::vector<browse::SourcePtr> sources;
    std::string errors;
    for (browse::Provider* p : providers) {
      std::string error;
      if (!p->Discover(sources, error) && !error.empty()) {
        if (!errors.empty())
          errors += "; ";
        errors += *p->Name() ? std::string(p->Name()) + ": " + error : error;
      }
    }

    Reply([this, group, sources = std::move(sources), errors]() mutable {
      // This group's sources make way for what was found now.
      discovered_.erase(std::remove_if(discovered_.begin(), discovered_.end(),
        [group](const browse::SourcePtr& s) { return s->RailGroup() == group; }),
        discovered_.end());
      for (browse::SourcePtr& s : sources)
        if (s->RailGroup() == group)
          discovered_.push_back(std::move(s));
      registry_.Set(discovered_);

      // Remembered, for Home's row of sources the next time round (and the
      // name a library item's source goes by while it is not found).
      for (const browse::SourcePtr& s : discovered_) {
        if (s->RailGroup() != group)
          continue;
        library::KnownSource known;
        known.key = s->Key();
        known.name = s->Name();
        known.detail = s->Detail();
        known.icon = s->Icon();
        library_.RememberSource(known);
      }
      library_.Save();

      // The plugins may have been loaded (or loaded afresh) by this: what
      // they want on the Settings page is known only now.
      if (group == browse::Source::Group::Plugin) {
        setting_items_.clear();
        if (bind_settings_page_)
          RebuildSettingRows();
        // A plugins' settings.ini may have brought new values.
        if (plugin_settings_->Revision() != saved_plugin_settings_) {
          saved_plugin_settings_ = plugin_settings_->Revision();
          SaveSettings();
        }
      }

      GroupScan& scan = scans_[(int)group];
      scan.errors = errors;
      scan.scanned = true;
      scan.running = false;
      std::vector<std::function<void()>> waiting;
      waiting.swap(scan.waiting);

      if (view_ == View::Home)
        RebuildHome(); // keeps the focus where it was
      for (const std::function<void()>& then : waiting)
        then();
    });
  });
}

bool App::SourceKnown(const std::string& key) const {
  return registry_.Find(key) != nullptr;
}

// Local drives and plugins come first: both are found at once (a plugin's
// discover() only describes its sources). Network servers take seconds of
// listening for answers, and share the lane: queued first, they would hold
// the others up.
void App::SearchAllSources() {
  for (Group group : kGroups)
    StartDiscovery(group);
}

void App::OpenKnownSource(const std::string& key, const std::string& name) {
  if (const browse::SourcePtr source = registry_.Find(key)) {
    OpenSource(source);
    return;
  }
  // Not found (yet): looked for, and opened when it is, unless the user
  // has moved on meanwhile.
  const Group group = GroupOfKey(key);
  ShowToast("Looking for " + name + "...", GroupIcon(group));
  StartDiscovery(group, [this, key, name] {
    if (view_ != View::Home || FullscreenPlayback() || bind_menu_open_)
      return;
    if (const browse::SourcePtr source = registry_.Find(key))
      OpenSource(source);
    else
      ShowToast(name + " is not available right now", icon::kWarning);
  });
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

std::string App::RailKey(const RailItem& item) {
  return item.kind == RailKind::Settings ? "settings" : "home";
}

void App::RebuildHome(const std::string& select) {
  const std::string selected = !select.empty() ? select
    : sel_rail_ < (int)rail_.size() ? RailKey(rail_[sel_rail_])
    : std::string("home");

  rail_.clear();
  rail_rows_.clear();
  rail_.push_back({RailKind::Home});
  rail_rows_.push_back({icon::kHome, "Home", ""});
  rail_.push_back({RailKind::Settings});
  rail_rows_.push_back({icon::kSettings, "", ""}); // a gear, no word

  sel_rail_ = 0;
  for (size_t i = 0; i < rail_.size(); i++) {
    if (RailKey(rail_[i]) == selected) {
      sel_rail_ = (int)i;
      break;
    }
  }
  model_.DirtyVariable("rail");
  model_.DirtyVariable("sel_rail");
  RebuildShelves();
  scroll_sources_pending_ = true;
}

void App::MoveRail(int delta) {
  const int n = (int)rail_.size();
  if (n == 0)
    return;
  // Tabs stop at either end, rather than wrap round behind the screen edge.
  const int to = std::clamp(sel_rail_ + delta, 0, n - 1);
  if (to == sel_rail_)
    return;
  sel_rail_ = to;
  model_.DirtyVariable("sel_rail");
  RebuildShelves();
  scroll_sources_pending_ = true;
}

void App::ActivateRail() {
  if (sel_rail_ >= (int)rail_.size())
    return;
  if (rail_[sel_rail_].kind == RailKind::Settings) {
    EnterSettings();
    return;
  }
  home_rows_wanted_ = true;
  if (!shelves_.empty())
    Set("home_shelves", bind_home_shelves_, true);
  RefreshImageBindings();
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

void App::RebuildShelves() {
  const bool settings =
    sel_rail_ < (int)rail_.size() && rail_[sel_rail_].kind == RailKind::Settings;

  // What the rows were, to keep the focus on the same row when the one of
  // what is playing comes or goes above it.
  const bool had_now_playing = NowPlayingShelf(0);
  shelves_.clear();
  Set("home_compact", bind_home_compact_, settings);
  Set("settings_page", bind_settings_page_, settings);
  if (settings)
    RebuildSettingRows();
  else
    Set("home_settings", bind_home_settings_, false);

  bool searching = false;
  Rml::String errors;
  for (Group group : kGroups) {
    const GroupScan& scan = scans_[(int)group];
    searching = searching || scan.running;
    if (!scan.errors.empty())
      errors += (errors.empty() ? "" : "; ") + scan.errors;
  }

  if (!settings) {
    if (bind_watching_) {
      Shelf now{"Now playing", nullptr, {playing_}, {}, {}};
      now.now_playing = true;
      shelves_.push_back(std::move(now));
    }

    std::set<std::string> resuming; // what Continue watching has already
    Shelf resume{"Continue watching", continue_source_, {}, {}, {}};
    for (const auto& item : library_.InProgress()) {
      resuming.insert(item.first.source_key + '\n' + item.first.entry.id);
      browse::Entry e = item.first.entry;
      e.id = library::EncodeId(item.first.source_key, item.first.entry.id);
      resume.entries.push_back(std::move(e));
    }
    if (!resume.entries.empty())
      shelves_.push_back(std::move(resume));

    // Opened through the library's own source, which hands every browse
    // and resolve to the source the id names.
    Shelf recent{"Recent", favorites_source_, {}, {}, {}};
    recent.recent = true;
    for (const library::Recent& r : library_.RecentItems()) {
      if (recent.entries.size() >= kRecentShown)
        break;
      if (resuming.count(r.ref.source_key + '\n' + r.ref.entry.id))
        continue;
      browse::Entry e = r.ref.entry;
      e.id = library::EncodeId(r.ref.source_key, r.ref.entry.id);
      recent.entries.push_back(std::move(e));
      recent.live.push_back(r.live);
    }
    if (!recent.entries.empty())
      shelves_.push_back(std::move(recent));

    Shelf favs{"Favorites", favorites_source_, {}, {}, {}};
    for (const library::Ref& ref : library_.Favorites()) {
      browse::Entry e = ref.entry;
      e.id = library::EncodeId(ref.source_key, ref.entry.id);
      favs.entries.push_back(std::move(e));
    }
    // Most recently added first: what was just pinned is what is wanted.
    std::reverse(favs.entries.begin(), favs.entries.end());
    if (!favs.entries.empty())
      shelves_.push_back(std::move(favs));

    Shelf row{"Sources", nullptr, {}, {}, {}};
    for (const library::KnownSource& known : library_.KnownSources()) {
      browse::Entry stand_in;
      stand_in.id = StandInId(known.key);
      stand_in.name = known.name;
      stand_in.type = browse::Entry::Type::Folder;
      row.entries.push_back(std::move(stand_in));
      row.sources.push_back(registry_.Find(known.key));
      row.known.push_back(known);
    }
    if (!row.entries.empty())
      shelves_.push_back(std::move(row));
  }

  // Nothing at all to show: the very first start, before any source is
  // found. Says what is going on in its place.
  const bool empty = !settings && shelves_.empty();
  Set("hero_icon", bind_hero_icon_, Rml::String(empty ? icon::kHome : ""));
  Set("hero_title", bind_hero_title_, Rml::String(empty ? "Welcome" : ""));
  Set("hero_detail", bind_hero_detail_, Rml::String(!empty ? ""
    : searching ? "Looking for your drives, servers and plugins..."
    : "No sources found."));
  Set("hero_note", bind_hero_note_, Rml::String(empty && !searching
    ? "Square looks again." : ""));
  // Beside the head of the row of sources: a search under way, or what
  // went wrong with the last one.
  Set("group_status", bind_group_status_, searching ? Rml::String("Searching...") : errors);

  if (NowPlayingShelf(0) != had_now_playing && !shelf_card_sel_.empty()) {
    if (had_now_playing) {
      shelf_card_sel_.erase(shelf_card_sel_.begin());
      sel_shelf_--;
    } else {
      shelf_card_sel_.insert(shelf_card_sel_.begin(), 0);
      sel_shelf_++;
    }
  }
  shelf_card_sel_.resize(shelves_.size(), 0);
  sel_shelf_ = std::clamp(sel_shelf_, 0, std::max(0, (int)shelves_.size() - 1));
  // The focus on the rows, as soon as there are any (at the start, once
  // the first sources are found), unless it was taken up to the tabs.
  if (shelves_.empty())
    Set("home_shelves", bind_home_shelves_, false);
  else if (home_rows_wanted_ && !settings && view_ == View::Home)
    Set("home_shelves", bind_home_shelves_, true);
  RebuildCards();
}

void App::RebuildCards() {
  shelf_rows_.clear();
  for (const Shelf& shelf : shelves_) {
    ShelfRow row;
    row.title = shelf.title;
    row.sources = !shelf.known.empty();
    if (row.sources) {
      // A source: its own icon and what it says it is. One that has not
      // been found again says so once its group has been looked through.
      row.count = (int)shelf.known.size();
      for (size_t i = 0; i < shelf.known.size(); i++) {
        const library::KnownSource& known = shelf.known[i];
        const browse::SourcePtr& s = shelf.sources[i];
        const GroupScan& scan = scans_[(int)GroupOfKey(known.key)];
        CardRow card;
        card.title = s ? s->Name() : known.name;
        card.subtitle = s ? s->Detail()
          : scan.scanned && !scan.running ? std::string("Not found") : known.detail;
        card.glyph = s ? s->Icon() : !known.icon.empty() ? known.icon.c_str() : icon::kFolder;
        card.kind = "source"; // drawn as a launcher tile (main.rml)
        card.watched = !s && scan.scanned && !scan.running; // dimmed (main.rml)
        row.cards.push_back(std::move(card));
      }
      shelf_rows_.push_back(std::move(row));
      continue;
    }
    if (shelf.now_playing) {
      // Its time and progress are the player's, live (main.rml).
      CardRow card;
      card.title = DisplayName(playing_);
      card.glyph = IconFor(playing_);
      card.kind = "playing";
      row.cards.push_back(std::move(card));
      shelf_rows_.push_back(std::move(row));
      continue;
    }
    row.count = (int)shelf.entries.size();
    for (size_t i = 0; i < shelf.entries.size(); i++) {
      const browse::Entry& e = shelf.entries[i];
      CardRow card;
      card.title = DisplayName(e);
      card.glyph = IconFor(e);
      card.kind = browse::TypeName(e.type);
      library::Ref ref;
      std::string where;
      if (Identify(e, ref)) {
        where = ref.source_name;
        library::Progress p;
        if (library_.GetProgress(ref.source_key, ref.entry.id, p)) {
          card.watched = p.watched;
          if (!p.watched)
            card.progress = ProgressWidth(p.position, p.duration);
          if (!p.watched && p.position > 0 && p.duration > p.position)
            card.subtitle = FormatTime((p.duration - p.position) * 1000000) + " left";
        }
      }
      if (shelf.recent && i < shelf.live.size() && shelf.live[i])
        card.subtitle = "Live";
      // Time left, and where it is from; what kind of entry it is only
      // when there is nothing else to say.
      if (!where.empty())
        card.subtitle += (card.subtitle.empty() ? "" : "  ·  ") + where;
      if (card.subtitle.empty())
        card.subtitle = TypeLabel(e);
      row.cards.push_back(std::move(card));
    }
    shelf_rows_.push_back(std::move(row));
  }
  // Keep each shelf's column, within its new length.
  for (size_t i = 0; i < shelves_.size(); i++)
    shelf_card_sel_[i] = std::clamp(shelf_card_sel_[i], 0,
                                    std::max(0, (int)shelves_[i].entries.size() - 1));
  sel_card_ = shelves_.empty() ? 0 : shelf_card_sel_[sel_shelf_];

  model_.DirtyVariable("shelves");
  model_.DirtyVariable("sel_shelf");
  model_.DirtyVariable("sel_card");
  art_scan_pending_ = true;
  UpdateBackdrop();
}

void App::FocusHomeCard(const std::string& id) {
  for (size_t i = 0; i < shelves_.size(); i++) {
    for (size_t j = 0; j < shelves_[i].entries.size(); j++) {
      if (shelves_[i].entries[j].id != id)
        continue;
      sel_shelf_ = (int)i;
      sel_card_ = (int)j;
      shelf_card_sel_[i] = (int)j;
      model_.DirtyVariable("sel_shelf");
      model_.DirtyVariable("sel_card");
      scroll_sources_pending_ = true;
      UpdateBackdrop();
      return;
    }
  }
}

bool App::FeaturedIndex(int& shelf, int& card) const {
  if (view_ != View::Home || shelves_.empty())
    return false;
  if (sel_rail_ >= (int)rail_.size() || rail_[sel_rail_].kind != RailKind::Home)
    return false;
  if (bind_home_shelves_) {
    shelf = sel_shelf_;
    card = sel_card_;
  } else {
    // On the tabs: Home shows what it has first.
    shelf = 0;
    card = shelf_card_sel_.empty() ? 0 : shelf_card_sel_[0];
  }
  return shelf >= 0 && shelf < (int)shelves_.size() && card >= 0 &&
         card < (int)shelves_[shelf].entries.size() && shelf < (int)shelf_rows_.size() &&
         card < (int)shelf_rows_[shelf].cards.size();
}

const browse::Entry* App::FeaturedCard() const {
  int shelf = 0, card = 0;
  return FeaturedIndex(shelf, card) ? &shelves_[shelf].entries[card] : nullptr;
}

void App::UpdateFeature() {
  int shelf = 0, card = 0;
  Rml::String title, eyebrow, meta, desc, progress = "0%";
  if (FeaturedIndex(shelf, card)) {
    const CardRow& row = shelf_rows_[shelf].cards[card];
    title = row.title;
    eyebrow = shelf_rows_[shelf].title;
    meta = row.subtitle;
    desc = shelves_[shelf].entries[card].description;
    progress = row.progress;
  }
  // What is playing: its time and progress are the player's, live.
  Set("feat_live", bind_feat_live_, NowPlayingShelf(shelf) && !title.empty());
  Set("feat_title", bind_feat_title_, title);
  Set("feat_eyebrow", bind_feat_eyebrow_, eyebrow);
  Set("feat_meta", bind_feat_meta_, meta);
  Set("feat_desc", bind_feat_desc_, desc);
  Set("feat_progress", bind_feat_progress_, progress);
}

const browse::Entry* App::SelectedCard() const {
  if (sel_shelf_ < 0 || sel_shelf_ >= (int)shelves_.size())
    return nullptr;
  const browse::Listing& cards = shelves_[sel_shelf_].entries;
  if (sel_card_ < 0 || sel_card_ >= (int)cards.size())
    return nullptr;
  return &cards[sel_card_];
}

void App::ActivateCard() {
  const browse::Entry* card = SelectedCard();
  if (!card)
    return;
  if (SourceShelf(sel_shelf_)) {
    const library::KnownSource& known = shelves_[sel_shelf_].known[sel_card_];
    OpenKnownSource(known.key, known.name);
    return;
  }
  if (NowPlayingShelf(sel_shelf_)) {
    SetFullscreen(true); // back to it
    return;
  }
  const browse::Entry entry = *card;
  const browse::SourcePtr shelf_source = shelves_[sel_shelf_].source;

  // The source a card came from may not have been found yet; look for it
  // first. A plugin's source may well be needed to play at all (resolve()
  // hands out fresh URIs).
  std::string key, id;
  if (library::DecodeId(entry.id, key, id) && !SourceKnown(key)) {
    const Group group = GroupOfKey(key);
    if (!scans_[(int)group].scanned) {
      const std::string name = library_.SourceName(key);
      ShowToast("Looking for " + (name.empty() ? std::string("its source") : name) + "...",
                GroupIcon(group));
      StartDiscovery(group, [this, entry, shelf_source] {
        if (view_ == View::Home && !FullscreenPlayback())
          OpenCard(entry, shelf_source);
      });
      return;
    }
  }
  OpenCard(entry, shelf_source);
}

void App::OpenCard(const browse::Entry& entry,
                   const browse::SourcePtr& shelf_source) {
  // The card's own library source resolves and browses through the source
  // the entry came from.
  current_source_ = shelf_source;
  if (entry.IsFolder()) {
    home_return_id_ = entry.id; // back from it: to this card
    path_.clear();
    sel_entry_ = 0;
    view_ = View::Browse;
    Set("view", bind_view_, Rml::String("browse"));
    // Named after the source it is from, not the library row it was on.
    library::Ref ref;
    Set("source_name", bind_source_name_,
        Identify(entry, ref) && !ref.source_name.empty() ? Rml::String(ref.source_name)
                                                         : current_source_->Name());
    RebuildEntryRows();
    RebuildCrumb();
    RebuildDetail();
    RequestBrowse(entry.id, entry.name, false);
  } else {
    // Continue watching: from where it was left, without asking (the
    // options menu has "Play from beginning").
    PlayEntry(entry, shelf_source == continue_source_);
  }
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

void App::HandleKeyHome(Rml::Event& event, int key) {
  if (bind_settings_page_ && bind_home_settings_) {
    HandleKeySettings(event, key);
    return;
  }
  const bool on_shelves = bind_home_shelves_ && !shelves_.empty();

  if (!on_shelves) {
    switch (key) {
    case Rml::Input::KI_LEFT:  MoveRail(-1); break;
    case Rml::Input::KI_RIGHT: MoveRail(+1); break;
    case Rml::Input::KI_HOME:  MoveRail(-sel_rail_); break;
    case Rml::Input::KI_END:   MoveRail((int)rail_.size() - 1 - sel_rail_); break;
    case Rml::Input::KI_DOWN:
      if (bind_settings_page_)
        EnterSettings();
      else
        ActivateRail();
      break;
    case Rml::Input::KI_UP:
      break; // nothing above the tabs
    case Rml::Input::KI_SPACE:
      SearchAllSources();
      break;
    case gamepad::kKeyOptions: OpenMenu(); break;
    case Rml::Input::KI_RETURN:
    case Rml::Input::KI_NUMPADENTER:
      ActivateRail();
      break;
    case Rml::Input::KI_BACK:
    case Rml::Input::KI_ESCAPE:
      // What plays behind the menus, as on Kodi's home screen; otherwise
      // back down to the rows.
      if (bind_watching_) {
        SetFullscreen(true);
      } else if (!bind_settings_page_) {
        ActivateRail();
      } else {
        MoveRail(-sel_rail_);
        ActivateRail();
      }
      break;
    default:
      return;
    }
    event.StopPropagation();
    return;
  }

  const int cards = (int)shelves_[sel_shelf_].entries.size();
  auto select = [this](int shelf, int card) {
    sel_shelf_ = shelf;
    sel_card_ = card;
    shelf_card_sel_[shelf] = card;
    model_.DirtyVariable("sel_shelf");
    model_.DirtyVariable("sel_card");
    scroll_sources_pending_ = true;
    UpdateBackdrop();
  };

  switch (key) {
  case Rml::Input::KI_LEFT:
    if (sel_card_ > 0)
      select(sel_shelf_, sel_card_ - 1);
    break;
  case Rml::Input::KI_RIGHT:
    if (sel_card_ + 1 < cards)
      select(sel_shelf_, sel_card_ + 1);
    break;
  case Rml::Input::KI_UP:
    if (sel_shelf_ > 0) {
      select(sel_shelf_ - 1, shelf_card_sel_[sel_shelf_ - 1]);
    } else { // up out of the first row: the tabs
      home_rows_wanted_ = false;
      Set("home_shelves", bind_home_shelves_, false);
      scroll_sources_pending_ = true;
      UpdateBackdrop();
    }
    break;
  case Rml::Input::KI_DOWN:
    if (sel_shelf_ + 1 < (int)shelves_.size())
      select(sel_shelf_ + 1, shelf_card_sel_[sel_shelf_ + 1]);
    break;
  case Rml::Input::KI_SPACE:
    SearchAllSources();
    break;
  case Rml::Input::KI_HOME:  select(sel_shelf_, 0); break;
  case Rml::Input::KI_END:   select(sel_shelf_, cards - 1); break;
  case Rml::Input::KI_PRIOR:
  case Rml::Input::KI_NEXT: {
    const int to = std::clamp(sel_shelf_ + (key == Rml::Input::KI_PRIOR ? -kPageShelves : +kPageShelves),
                              0, (int)shelves_.size() - 1);
    if (to != sel_shelf_)
      select(to, shelf_card_sel_[to]);
    break;
  }
  case gamepad::kKeyOptions: OpenMenu(); break;
  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER:
    ActivateCard();
    break;
  case Rml::Input::KI_BACK:
  case Rml::Input::KI_ESCAPE:
    // What plays behind the menus; otherwise the start of the page.
    if (bind_watching_)
      SetFullscreen(true);
    else
      select(0, 0);
    break;
  default:
    return;
  }
  event.StopPropagation();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

namespace {

// Languages offered for audio and subtitles: a code we store, the name we
// show, and mpv's alang/slang list matching both tag styles files use.
struct Language {
  const char* code;
  const char* name;
  const char* mpv;
};
constexpr Language kLanguages[] = {
  {"",   "Default",    ""},
  {"en", "English",    "eng,en"},
  {"sv", "Swedish",    "swe,sv,sve"},
  {"no", "Norwegian",  "nor,no,nob,nb,nno,nn"},
  {"da", "Danish",     "dan,da"},
  {"fi", "Finnish",    "fin,fi"},
  {"de", "German",     "deu,ger,de"},
  {"fr", "French",     "fra,fre,fr"},
  {"es", "Spanish",    "spa,es"},
  {"it", "Italian",    "ita,it"},
  {"nl", "Dutch",      "nld,dut,nl"},
  {"pl", "Polish",     "pol,pl"},
  {"pt", "Portuguese", "por,pt"},
  {"ja", "Japanese",   "jpn,ja"},
};
constexpr int kLanguageCount = (int)(sizeof(kLanguages) / sizeof(kLanguages[0]));

int LanguageIndex(const std::string& code) {
  for (int i = 0; i < kLanguageCount; i++)
    if (code == kLanguages[i].code)
      return i;
  return 0;
}

constexpr int kSaverMinutes[] = {1, 3, 5, 10, 30, 0}; // 0 = never
constexpr int kSaverCount = (int)(sizeof(kSaverMinutes) / sizeof(kSaverMinutes[0]));

} // namespace

void App::ApplyLanguages() {
  player_->SetPreferredLanguages(kLanguages[LanguageIndex(audio_lang_)].mpv,
                                 kLanguages[LanguageIndex(sub_lang_)].mpv);
}

// The page under the Settings tab. Built once: the values are read back
// from the app each time the rows are made, so a setting changed elsewhere
// (night mode in the playback options) shows as it is.
void App::BuildSettings() {
  setting_items_.clear();
  auto step = [](int index, int d, int count) {
    return (index + (d < 0 ? count - 1 : 1)) % count;
  };
  auto language_names = [] {
    std::vector<std::string> names;
    for (int i = 0; i < kLanguageCount; i++)
      names.push_back(kLanguages[i].name);
    return names;
  };

  setting_items_.push_back({"Playback", "Next episode",
    "When an episode ends, the next one in its folder starts by itself, "
    "after a countdown over the last seconds. Music always plays on.",
    [this] { return std::string(autoplay_ ? "Plays by itself" : "Stop at the end"); },
    [this](int) { autoplay_ = !autoplay_; SaveSettings(); }});
  setting_items_.push_back({"", "Audio language",
    "The audio track to pick when a file has several. Default leaves it "
    "to the file.",
    [this] { return std::string(kLanguages[LanguageIndex(audio_lang_)].name); },
    [this, step, language_names](int d) {
      auto choose = [this](int i) {
        audio_lang_ = kLanguages[i].code;
        ApplyLanguages();
        SaveSettings();
      };
      if (d == 0)
        OpenChoiceMenu("Audio language", language_names(), LanguageIndex(audio_lang_), choose);
      else
        choose(step(LanguageIndex(audio_lang_), d, kLanguageCount));
    }});
  setting_items_.push_back({"", "Subtitle language",
    "Subtitles in this language are turned on when a file has them. "
    "Default leaves it to the file.",
    [this] { return std::string(kLanguages[LanguageIndex(sub_lang_)].name); },
    [this, step, language_names](int d) {
      auto choose = [this](int i) {
        sub_lang_ = kLanguages[i].code;
        ApplyLanguages();
        SaveSettings();
      };
      if (d == 0)
        OpenChoiceMenu("Subtitle language", language_names(), LanguageIndex(sub_lang_), choose);
      else
        choose(step(LanguageIndex(sub_lang_), d, kLanguageCount));
    }});
  setting_items_.push_back({"", "Night mode",
    "Evens out the volume: quiet dialogue is lifted and loud effects are "
    "held back, for when the others are asleep.",
    [this] { return std::string(player_->NightMode() ? "On" : "Off"); },
    [this](int) { player_->SetNightMode(!player_->NightMode()); SaveSettings(); }});
  setting_items_.push_back({"Screen", "Screensaver",
    "Dims the screen to a clock after this long without a button pressed. "
    "Never while something plays.",
    [this] {
      return screensaver_sec_ <= 0 ? std::string("Never")
        : "After " + std::to_string((int)(screensaver_sec_ / 60)) + " min";
    },
    [this, step](int d) {
      // Where the setting is among the choices: "Never" is the last; a
      // delay from an edited settings file counts as the next one up.
      int i = kSaverCount - 1;
      if (screensaver_sec_ > 0) {
        i = 0;
        while (i < kSaverCount - 2 && kSaverMinutes[i] * 60.0 < screensaver_sec_)
          i++;
      }
      screensaver_sec_ = kSaverMinutes[step(i, d, kSaverCount)] * 60.0;
      SaveSettings();
    }});
  setting_items_.push_back({"Library", "Forget Continue watching",
    "Empties the Continue watching row: what is in it starts from the "
    "beginning next time. Favorites stay. Asks first.",
    [this] {
      const size_t n = library_.InProgress().size();
      return n ? std::to_string(n) + (n == 1 ? " item" : " items") : std::string("Empty");
    },
    [this](int) { ConfirmForgetContinue(); }, true});
  // What the plugins ask for (a provider's 'settings', see docs/plugins.md):
  // a section each, under the plugin's name.
  if (js_provider_) {
    for (const browse::JsPluginPtr& plugin : js_provider_->Plugins()) {
      const size_t first = setting_items_.size();
      AddPluginSettings(plugin, setting_items_, nullptr);
      if (setting_items_.size() > first)
        setting_items_[first].section = plugin->ProviderName();
    }
  }
  // For finding out what a remote's buttons arrive as; not remembered.
  setting_items_.push_back({"Diagnostics", "Show input",
    "Shows every button press as a notification, with what it arrives as. "
    "For finding out what a remote's buttons do; off again at the next start.",
    [this] { return std::string(input_trace_ ? "On" : "Off"); },
    [this](int) { SetInputTrace(!input_trace_); }});
}

// One plugin's settings as rows: of the Settings page, or of the plugin's
// own menu ('menu' opens it, see OpenPluginMenu). The values live in
// plugin_settings_, where the scripts read them.
void App::AddPluginSettings(const std::shared_ptr<browse::JsPlugin>& plugin,
                            std::vector<SettingItem>& out, std::function<void()> menu) {
  auto step = [](int index, int d, int count) {
    return (index + (d < 0 ? count - 1 : 1)) % count;
  };
  const std::string id = plugin->Id();
  for (const browse::PluginSetting& spec : plugin->Settings()) {
    auto index = [this, id, spec] {
      const std::string value = plugin_settings_->Get(id, spec.key, spec.fallback);
      for (size_t i = 0; i < spec.options.size(); i++)
        if (spec.options[i].first == value)
          return (int)i;
      return 0;
    };
    if (spec.text) {
      out.push_back({std::string(), spec.label, spec.about,
        [this, id, spec] {
          const std::string value = plugin_settings_->Get(id, spec.key, spec.fallback);
          if (value.empty())
            return std::string("Not set");
          return spec.secret ? std::string("Set") : value;
        },
        [this, id, spec, menu, name = plugin->ProviderName()](int d) {
          // Left or right forgets it (signing out, for a token).
          if (d != 0) {
            if (plugin_settings_->Get(id, spec.key).empty())
              return;
            plugin_settings_->Set(id, spec.key, "");
            SaveSettings();
            ShowToast(name + ": " + spec.label + " cleared", icon::kClear);
            return;
          }
          // A secret is typed afresh; it is not shown back.
          const std::string initial =
            spec.secret ? std::string() : plugin_settings_->Get(id, spec.key, spec.fallback);
          OpenTextEntry(spec.label, initial, kTextSettingMaxLength, spec.secret,
                        [this, id, spec, menu](const std::string& text) {
                          plugin_settings_->Set(id, spec.key, text);
                          SaveSettings();
                          if (menu)
                            menu(); // its rows, with the new value
                          else
                            RebuildSettingRows();
                        });
        }});
      continue;
    }
    out.push_back({std::string(), spec.label, spec.about,
      [spec, index] { return spec.options[index()].second; },
      [this, id, spec, index, step, menu](int d) {
        auto choose = [this, id, spec](int i) {
          plugin_settings_->Set(id, spec.key, spec.options[i].first);
          SaveSettings();
        };
        // Select on a long list of choices (every country there is)
        // lists them, rather than stepping to the next.
        if (d == 0 && spec.options.size() > kChoiceMenuFrom) {
          std::vector<std::string> labels;
          for (const auto& option : spec.options)
            labels.push_back(option.second);
          OpenChoiceMenu(spec.label, labels, index(), choose);
          menu_parent_ = menu; // chosen or not: back to where it was asked
          return;
        }
        choose(step(index(), d, (int)spec.options.size()));
      }});
  }
}

void App::RebuildSettingRows() {
  if (setting_items_.empty())
    BuildSettings();
  setting_rows_.clear();
  for (const SettingItem& item : setting_items_)
    setting_rows_.push_back({item.section, item.label, item.value ? item.value() : "",
                             item.action});
  sel_setting_ = std::clamp(sel_setting_, 0, (int)setting_rows_.size() - 1);
  model_.DirtyVariable("settings");
  model_.DirtyVariable("sel_setting");
  const SettingItem& focused = setting_items_[sel_setting_];
  Set("setting_title", bind_setting_title_, Rml::String(focused.label));
  Set("setting_about", bind_setting_about_, Rml::String(focused.about));
}

void App::EnterSettings() {
  RebuildSettingRows();
  Set("home_settings", bind_home_settings_, true);
  scroll_settings_pending_ = true;
}

void App::ConfirmForgetContinue() {
  const size_t n = library_.InProgress().size();
  if (n == 0) {
    ShowToast("Continue watching is already empty", icon::kClear);
    return;
  }
  menu_items_.clear();
  menu_section_.clear();
  menu_parent_ = nullptr;
  menu_items_.push_back({"Forget " + std::to_string(n) + (n == 1 ? " item" : " items"),
    nullptr, [this](int) {
      const auto items = library_.InProgress();
      for (const auto& item : items) {
        DropSnapshot(item.first.entry.image);
        library_.SetWatched(item.first, false);
      }
      library_.Save();
      RebuildSettingRows();
      ShowToast("Cleared " + std::to_string(items.size()) + " in progress", icon::kClear);
    }});
  menu_items_.push_back({"Cancel", nullptr, [](int) {}});
  Set("menu_title", bind_menu_title_, Rml::String("Forget Continue watching?"));
  Set("menu_meta", bind_menu_meta_, Rml::String());
  Set("menu_dialog", bind_menu_dialog_, true);
  sel_menu_ = 1; // the safe one: a stray press of select changes nothing
  RebuildMenuRows();
  Set("menu_open", bind_menu_open_, true);
}

void App::OpenChoiceMenu(const std::string& title, const std::vector<std::string>& choices,
                         int current, std::function<void(int)> pick) {
  menu_items_.clear();
  menu_section_.clear();
  menu_parent_ = nullptr;
  for (size_t i = 0; i < choices.size(); i++)
    menu_items_.push_back({choices[i], nullptr, [this, pick, i](int) {
      pick((int)i);
      RebuildSettingRows();
    }});
  Set("menu_title", bind_menu_title_, Rml::String(title));
  Set("menu_meta", bind_menu_meta_, Rml::String());
  Set("menu_dialog", bind_menu_dialog_, false);
  sel_menu_ = std::clamp(current, 0, std::max(0, (int)choices.size() - 1));
  RebuildMenuRows();
  Set("menu_open", bind_menu_open_, true);
  scroll_menu_pending_ = true;
}

void App::HandleKeySettings(Rml::Event& event, int key) {
  const int count = (int)setting_items_.size();
  auto select = [this](int index) {
    sel_setting_ = index;
    model_.DirtyVariable("sel_setting");
    Set("setting_title", bind_setting_title_, Rml::String(setting_items_[index].label));
    Set("setting_about", bind_setting_about_, Rml::String(setting_items_[index].about));
    scroll_settings_pending_ = true;
  };
  switch (key) {
  case Rml::Input::KI_UP:
    if (sel_setting_ > 0)
      select(sel_setting_ - 1);
    else // up out of the first row: the tabs
      Set("home_settings", bind_home_settings_, false);
    break;
  case Rml::Input::KI_DOWN:
    if (sel_setting_ + 1 < count)
      select(sel_setting_ + 1);
    break;
  case Rml::Input::KI_HOME: select(0); break;
  case Rml::Input::KI_END:  select(count - 1); break;
  case Rml::Input::KI_PRIOR:
  case Rml::Input::KI_NEXT:
    if (count > 0)
      select(std::clamp(sel_setting_ + (key == Rml::Input::KI_PRIOR ? -kPageSettings : +kPageSettings),
                        0, count - 1));
    break;
  case Rml::Input::KI_LEFT:
  case Rml::Input::KI_RIGHT:
    if (sel_setting_ < count && !setting_items_[sel_setting_].action) {
      setting_items_[sel_setting_].change(key == Rml::Input::KI_LEFT ? -1 : +1);
      RebuildSettingRows();
    }
    break;
  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER:
    if (sel_setting_ < count) {
      setting_items_[sel_setting_].change(0);
      RebuildSettingRows();
    }
    break;
  case Rml::Input::KI_BACK:
  case Rml::Input::KI_ESCAPE:
    Set("home_settings", bind_home_settings_, false);
    break;
  case gamepad::kKeyOptions:
    OpenMenu(); // what plays, if anything
    break;
  default:
    return;
  }
  event.StopPropagation();
}
