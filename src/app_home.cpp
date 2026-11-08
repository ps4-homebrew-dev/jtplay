// SPDX-License-Identifier: GPL-3.0-or-later
//
// The home screen: tabs of places across the top (Home, the groups of
// sources -- local, network, plugins -- and Settings), always those five
// however many sources there are, a banner on Home, and rows of cards.
//
// On Home the rows are the library's: Continue watching and Favorites. On
// a group they are first its sources, as a grid of tiles (rows of four,
// as many rows as it takes: fifty plugins are thirteen rows), and then
// what the library has from that group. A group is searched as soon as
// its tab is reached. Selecting a source opens it in the browse view, and
// selecting a card plays it, or opens a favorite folder, without going
// through the source's own tree. Settings is a list of its own, under
// its tab like the rows (see BuildSettings).
//
// The banner, on Home only, shows the card in focus, large -- its artwork across the top
// of the screen, its title, what is left of it, its description -- and so
// follows the focus along the rows, as the big streaming apps do. With the
// focus on the tabs it shows what Home has first. Whatever a card has no
// data for is left out rather than shown empty. Elsewhere the tab says
// where we are, and the page begins right under it.
//
// The internals still say "rail" for the places: they were a rail down the
// left once, and are the same list.
#include <algorithm>

#include "app.h"
#include "app_internal.h"
#include "icons.h"

using namespace appdetail;

namespace {

std::string Plural(size_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

} // namespace

void App::GoHome() {
  CancelForegroundLaunch();
  browse_request_++; // invalidate any browse that is still in flight
  path_.clear();
  current_source_.reset();
  view_ = View::Home;
  Set("view", bind_view_, Rml::String("home"));
  RebuildHome();
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

namespace {

using Group = browse::Source::Group;
constexpr Group kGroups[] = {Group::Local, Group::Network, Group::Plugin};

// What a group's page says while it has no sources to show: that they are
// being looked for, or that none were found and what it looks for.
struct GroupInfo {
  const char* icon;
  const char* name;
  const char* blurb;
  const char* looking;
  const char* none;
};

const GroupInfo& InfoFor(Group group) {
  static const GroupInfo kLocal{icon::kDrive, "Local filesystem",
                                "Drives and folders on this machine.",
                                "Looking for drives...", "No drives found"};
  static const GroupInfo kNetwork{icon::kServer, "Local network",
                                  "Media servers (UPnP/DLNA) on the network.",
                                  "Looking for media servers...",
                                  "No media servers found"};
  static const GroupInfo kPlugin{icon::kPlugin, "Plugins",
                                 "Sources added by JavaScript plugins.",
                                 "Looking for plugins...", "No plugins found"};
  switch (group) {
  case Group::Local:   return kLocal;
  case Group::Network: return kNetwork;
  default:             return kPlugin;
  }
}

// The group a library item's source belongs to, from its key alone, so a
// server that is offline right now still has its favorites in its group.
// Follows the Key() of FsSource ("fs:"), DlnaSource ("dlna:") and
// JsSource ("js:").
Group GroupOfKey(const std::string& key) {
  if (key.compare(0, 3, "fs:") == 0)
    return Group::Local;
  if (key.compare(0, 5, "dlna:") == 0)
    return Group::Network;
  return Group::Plugin;
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

  busy_ops_++;
  PostTask([this, group, providers] {
    std::vector<browse::SourcePtr> sources;
    std::string errors;
    for (browse::Provider* p : providers) {
      std::string error;
      if (!p->Discover(sources, error) && !error.empty()) {
        if (!errors.empty())
          errors += "; ";
        errors += std::string(p->Name()) + ": " + error;
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

      GroupScan& scan = scans_[(int)group];
      scan.errors = errors;
      scan.scanned = true;
      scan.running = false;
      std::vector<std::function<void()>> waiting;
      waiting.swap(scan.waiting);

      if (view_ == View::Home)
        RebuildHome(); // keeps the rail on the same place
      for (const std::function<void()>& then : waiting)
        then();
    });
    busy_ops_--;
  });
}

bool App::SourceKnown(const std::string& key) const {
  return registry_.Find(key) != nullptr;
}

// ---------------------------------------------------------------------------
// Rail
// ---------------------------------------------------------------------------

std::string App::RailKey(const RailItem& item) {
  switch (item.kind) {
  case RailKind::Home:     return "home";
  case RailKind::Settings: return "settings";
  case RailKind::Group:    return "group:" + std::to_string((int)item.group);
  default:                 return "source:" + item.source->Key();
  }
}

void App::RebuildHome(const std::string& select) {
  const std::string selected = !select.empty() ? select
    : sel_rail_ < (int)rail_.size() ? RailKey(rail_[sel_rail_])
    : std::string("home");

  rail_.clear();
  rail_rows_.clear();
  {
    const size_t in_progress = library_.InProgress().size();
    const size_t favorites = library_.Favorites().size();
    rail_.push_back({RailKind::Home, nullptr});
    std::string home_detail;
    if (in_progress)
      home_detail = Plural(in_progress, "in progress", "in progress");
    if (favorites)
      home_detail += (home_detail.empty() ? "" : "  ·  ") +
                     Plural(favorites, "favorite", "favorites");
    rail_rows_.push_back({icon::kHome, "Home", home_detail});
    for (Group group : kGroups) {
      const GroupScan& scan = scans_[(int)group];
      const size_t n = std::count_if(discovered_.begin(), discovered_.end(),
        [group](const browse::SourcePtr& s) { return s->RailGroup() == group; });
      std::string detail; // nothing to say before the first search
      if (scan.running)
        detail = "Searching...";
      else if (scan.scanned)
        detail = n ? Plural(n, "source", "sources") : "None found";
      const GroupInfo& info = InfoFor(group);
      rail_.push_back({RailKind::Group, nullptr, group});
      rail_rows_.push_back({info.icon, info.name, detail});
    }
    rail_.push_back({RailKind::Settings, nullptr});
    rail_rows_.push_back({icon::kSettings, "Settings", ""});
  }

  // Stay where we were.
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

bool App::PlaceGroup(browse::Source::Group& group) const {
  if (sel_rail_ >= (int)rail_.size() || rail_[sel_rail_].kind != RailKind::Group)
    return false;
  group = rail_[sel_rail_].group;
  return true;
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
  sel_shelf_ = 0;
  shelf_card_sel_.clear();
  RebuildShelves();
  scroll_sources_pending_ = true;
  // A group's sources are looked for as soon as its tab is reached (once;
  // square looks again).
  Group group;
  if (PlaceGroup(group) && !scans_[(int)group].scanned && !scans_[(int)group].running)
    StartDiscovery(group);
}

void App::ActivateRail() {
  if (sel_rail_ >= (int)rail_.size())
    return;
  const RailItem item = rail_[sel_rail_]; // a copy: the rail is rebuilt below
  switch (item.kind) {
  case RailKind::Home:
  case RailKind::Group:
    // Into the rows; a group with nothing found yet is looked for again.
    if (!shelves_.empty())
      Set("home_shelves", bind_home_shelves_, true);
    else if (item.kind == RailKind::Group)
      StartDiscovery(item.group);
    RefreshImageBindings();
    break;
  case RailKind::Source:
    OpenSource(item.source);
    break;
  case RailKind::Settings:
    EnterSettings();
    break;
  }
}

// ---------------------------------------------------------------------------
// Shelves
// ---------------------------------------------------------------------------

void App::RebuildShelves() {
  // Which place the shelves are for: everything, one source, or nothing.
  // An empty group has no rows at all; the place is still the group.
  RailItem place{RailKind::Home, nullptr};
  if (sel_rail_ < (int)rail_.size())
    place = rail_[sel_rail_];
  const RailKind kind = place.kind;
  const RailItem* item = &place;
  auto wanted = [&](const std::string& source_key) {
    switch (kind) {
    case RailKind::Source:
      return source_key == item->source->Key();
    case RailKind::Group:
      return GroupOfKey(source_key) == item->group;
    default:
      return true;
    }
  };

  // What the rows were, to keep the focus on the same row when the one of
  // what is playing comes or goes above it.
  const bool had_now_playing = NowPlayingShelf(0);
  shelves_.clear();
  // Home, while something plays: that first, the way back to it.
  if (kind == RailKind::Home && bind_watching_) {
    Shelf now{"Now playing", nullptr, {playing_}, {}};
    now.now_playing = true;
    shelves_.push_back(std::move(now));
  }
  // A group: its sources first, as rows of tiles that make a grid; the
  // first row says what they are and how many.
  size_t group_sources = 0;
  if (kind == RailKind::Group) {
    std::vector<browse::SourcePtr> sources;
    for (const browse::SourcePtr& s : discovered_)
      if (s->RailGroup() == item->group)
        sources.push_back(s);
    group_sources = sources.size();
    for (size_t i = 0; i < sources.size(); i += kSourcesPerRow) {
      Shelf row{i == 0 ? "Sources" : "", nullptr, {}, {}};
      for (size_t j = i; j < std::min(sources.size(), i + kSourcesPerRow); j++) {
        browse::Entry stand_in;
        stand_in.id = "source:" + sources[j]->Key();
        stand_in.name = sources[j]->Name();
        stand_in.type = browse::Entry::Type::Folder;
        row.entries.push_back(std::move(stand_in));
        row.sources.push_back(sources[j]);
      }
      shelves_.push_back(std::move(row));
    }
  }
  Set("home_compact", bind_home_compact_,
      kind == RailKind::Group || kind == RailKind::Settings);
  Set("settings_page", bind_settings_page_, kind == RailKind::Settings);
  if (kind == RailKind::Settings)
    RebuildSettingRows();
  else
    Set("home_settings", bind_home_settings_, false);
  if (kind != RailKind::Settings) {
    Shelf resume{"Continue watching", continue_source_, {}, {}};
    for (const auto& item : library_.InProgress()) {
      if (!wanted(item.first.source_key))
        continue;
      browse::Entry e = item.first.entry;
      e.id = library::EncodeId(item.first.source_key, item.first.entry.id);
      resume.entries.push_back(std::move(e));
    }
    if (!resume.entries.empty())
      shelves_.push_back(std::move(resume));

    Shelf favs{"Favorites", favorites_source_, {}, {}};
    for (const library::Ref& ref : library_.Favorites()) {
      if (!wanted(ref.source_key))
        continue;
      browse::Entry e = ref.entry;
      e.id = library::EncodeId(ref.source_key, ref.entry.id);
      favs.entries.push_back(std::move(e));
    }
    // Most recently added first: what was just pinned is what is wanted.
    std::reverse(favs.entries.begin(), favs.entries.end());
    if (!favs.entries.empty())
      shelves_.push_back(std::move(favs));
  }

  // The hero: what the rail is on, in large -- only where there is nothing
  // else to show (an empty Home, a group with no sources yet). The tabs
  // already say where we are.
  Rml::String group_status;
  switch (kind) {
  case RailKind::Home:
    Set("hero_icon", bind_hero_icon_, Rml::String(shelves_.empty() ? icon::kHome : ""));
    Set("hero_title", bind_hero_title_,
        Rml::String(shelves_.empty() ? "Welcome" : ""));
    Set("hero_detail", bind_hero_detail_, Rml::String(shelves_.empty()
      ? "Pick a place from the tabs above to start browsing." : ""));
    Set("hero_note", bind_hero_note_, Rml::String(shelves_.empty()
      ? "Favorites and anything left unfinished will gather here." : ""));
    break;
  case RailKind::Source: {
    const browse::SourcePtr& s = item->source;
    Set("hero_icon", bind_hero_icon_, Rml::String(s->Icon()));
    Set("hero_title", bind_hero_title_, s->Name());
    Set("hero_detail", bind_hero_detail_, s->Detail());
    Set("hero_note", bind_hero_note_, Rml::String(shelves_.empty()
      ? "Nothing from here in your library yet." : ""));
    break;
  }
  case RailKind::Settings:
    // The settings themselves are the page (see RebuildSettings).
    Set("hero_icon", bind_hero_icon_, Rml::String());
    Set("hero_title", bind_hero_title_, Rml::String());
    Set("hero_detail", bind_hero_detail_, Rml::String());
    Set("hero_note", bind_hero_note_, Rml::String());
    break;
  case RailKind::Group: {
    const GroupInfo& info = InfoFor(item->group);
    const GroupScan& scan = scans_[(int)item->group];
    // It is looked for as soon as the tab is reached, so there is hardly
    // a moment before the first search.
    const bool looking = scan.running || !scan.scanned;
    if (group_sources > 0) {
      // The grid says what was found; a new search, or what went wrong,
      // goes beside its count.
      group_status = looking ? Rml::String("Searching...") : scan.errors;
      Set("hero_icon", bind_hero_icon_, Rml::String());
      Set("hero_title", bind_hero_title_, Rml::String());
      Set("hero_detail", bind_hero_detail_, Rml::String());
      Set("hero_note", bind_hero_note_, Rml::String());
    } else {
      Set("hero_icon", bind_hero_icon_, Rml::String(info.icon));
      Set("hero_title", bind_hero_title_, Rml::String(looking ? info.looking : info.none));
      Set("hero_detail", bind_hero_detail_,
          looking || scan.errors.empty() ? Rml::String(info.blurb) : scan.errors);
      Set("hero_note", bind_hero_note_,
          Rml::String(looking ? "" : "Square searches again."));
    }
    break;
  }
  }
  Set("group_status", bind_group_status_, group_status);

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
  if (shelves_.empty())
    Set("home_shelves", bind_home_shelves_, false);
  RebuildCards();
}

void App::RebuildCards() {
  shelf_rows_.clear();
  size_t source_total = 0;
  for (const Shelf& shelf : shelves_)
    source_total += shelf.sources.size();
  for (const Shelf& shelf : shelves_) {
    ShelfRow row;
    row.title = shelf.title;
    row.sources = !shelf.sources.empty();
    row.gap = !row.sources && !shelf_rows_.empty() && shelf_rows_.back().sources;
    if (row.sources) {
      // A source: its own icon and what it says it is.
      row.count = shelf.title.empty() ? 0 : (int)source_total;
      for (const browse::SourcePtr& s : shelf.sources) {
        CardRow card;
        card.title = s->Name();
        card.subtitle = s->Detail();
        card.glyph = s->Icon();
        card.kind = "source"; // drawn as a launcher tile (main.rml)
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
    for (const browse::Entry& e : shelf.entries) {
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

bool App::FeaturedIndex(int& shelf, int& card) const {
  if (view_ != View::Home || shelves_.empty())
    return false;
  // Only Home has the banner: on a group's page the rows start right under
  // the tabs, and the cards say what they are themselves.
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
  return shelf >= 0 && shelf < (int)shelves_.size() && !SourceShelf(shelf) && card >= 0 &&
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
    if (eyebrow.empty() && SourceShelf(shelf))
      eyebrow = "Sources";
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
    OpenSource(shelves_[sel_shelf_].sources[sel_card_]);
    return;
  }
  if (NowPlayingShelf(sel_shelf_)) {
    SetFullscreen(true); // back to it
    return;
  }
  const browse::Entry entry = *card;
  const browse::SourcePtr shelf_source = shelves_[sel_shelf_].source;

  // Groups are only searched when entered, so the source a card came from
  // may not be known yet; look for it first. A plugin's source may well be
  // needed to play at all (resolve() hands out fresh URIs).
  std::string key, id;
  if (library::DecodeId(entry.id, key, id) && !SourceKnown(key)) {
    const Group group = GroupOfKey(key);
    if (!scans_[(int)group].scanned) {
      ShowToast(std::string("Looking for ") + InfoFor(group).name + "...",
                InfoFor(group).icon);
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
    path_.clear();
    sel_entry_ = 0;
    view_ = View::Browse;
    Set("view", bind_view_, Rml::String("browse"));
    Set("source_name", bind_source_name_, current_source_->Name());
    RebuildEntryRows();
    RebuildCrumb();
    RebuildDetail();
    RequestBrowse(entry.id, entry.name, false);
  } else {
    PlayEntry(entry);
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
      if (bind_settings_page_) {
        EnterSettings();
      } else if (!shelves_.empty()) {
        Set("home_shelves", bind_home_shelves_, true);
        RefreshImageBindings();
      }
      break;
    case Rml::Input::KI_UP:
      break; // nothing above the tabs
    case Rml::Input::KI_SPACE: {
      Group group;
      if (!PlaceGroup(group))
        return;
      StartDiscovery(group); // search the group again
      break;
    }
    case Rml::Input::KI_F1:    OpenMenu(); break;
    case Rml::Input::KI_RETURN:
    case Rml::Input::KI_NUMPADENTER:
      ActivateRail();
      break;
    case Rml::Input::KI_BACK:
    case Rml::Input::KI_ESCAPE:
      // Nothing above the tabs but what plays behind them, as on Kodi's
      // home screen: back leads out of it and, step by step, back to it.
      if (!bind_watching_)
        return;
      SetFullscreen(true);
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
  // The card to land on in another row: in the grid of a group's sources,
  // the same column (or the last tile, on a short last row); elsewhere,
  // wherever that row was left.
  auto column = [this](int shelf) {
    if (SourceShelf(sel_shelf_) && SourceShelf(shelf))
      return std::min(sel_card_, (int)shelves_[shelf].entries.size() - 1);
    return shelf_card_sel_[shelf];
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
      select(sel_shelf_ - 1, column(sel_shelf_ - 1));
    } else { // up out of the first row: the tabs
      Set("home_shelves", bind_home_shelves_, false);
      scroll_sources_pending_ = true;
      UpdateBackdrop();
    }
    break;
  case Rml::Input::KI_DOWN:
    if (sel_shelf_ + 1 < (int)shelves_.size())
      select(sel_shelf_ + 1, column(sel_shelf_ + 1));
    break;
  case Rml::Input::KI_SPACE: {
    Group group;
    if (!PlaceGroup(group))
      return;
    StartDiscovery(group); // search the group again
    break;
  }
  case Rml::Input::KI_HOME:  select(sel_shelf_, 0); break;
  case Rml::Input::KI_END:   select(sel_shelf_, cards - 1); break;
  case Rml::Input::KI_F1:    OpenMenu(); break;
  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER:
    ActivateCard();
    break;
  case Rml::Input::KI_BACK:
  case Rml::Input::KI_ESCAPE:
    Set("home_shelves", bind_home_shelves_, false);
    UpdateBackdrop();
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
// (night mode in the playback menu) shows as it is.
void App::BuildSettings() {
  setting_items_.clear();
  auto step = [](int index, int d, int count) {
    return (index + (d < 0 ? count - 1 : 1)) % count;
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
    [this, step](int d) {
      audio_lang_ = kLanguages[step(LanguageIndex(audio_lang_), d, kLanguageCount)].code;
      ApplyLanguages();
      SaveSettings();
    }});
  setting_items_.push_back({"", "Subtitle language",
    "Subtitles in this language are turned on when a file has them. "
    "Default leaves it to the file.",
    [this] { return std::string(kLanguages[LanguageIndex(sub_lang_)].name); },
    [this, step](int d) {
      sub_lang_ = kLanguages[step(LanguageIndex(sub_lang_), d, kLanguageCount)].code;
      ApplyLanguages();
      SaveSettings();
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
  // For finding out what a remote's buttons arrive as; not remembered.
  setting_items_.push_back({"Diagnostics", "Show input",
    "Shows every button press as a notification, with what it arrives as. "
    "For finding out what a remote's buttons do; off again at the next start.",
    [this] { return std::string(input_trace_ ? "On" : "Off"); },
    [this](int) { SetInputTrace(!input_trace_); }});
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
  Set("menu_dialog", bind_menu_dialog_, true);
  sel_menu_ = 1; // the safe one: a stray press of select changes nothing
  RebuildMenuRows();
  Set("menu_open", bind_menu_open_, true);
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
  case Rml::Input::KI_F1:
    OpenMenu(); // what plays, if anything
    break;
  default:
    return;
  }
  event.StopPropagation();
}
