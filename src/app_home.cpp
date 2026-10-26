// SPDX-License-Identifier: GPL-3.0-or-later
//
// The home screen: a rail of places down the left (Home, the groups of
// sources -- local, network, plugins -- and Settings) and shelves of cards
// on the right, drawn from the library -- Continue watching and Favorites,
// for everything when the rail is on Home and for one group or source when
// it is on that. Selecting a group lists its sources in the rail, selecting
// a source opens it in the browse view, and selecting a card plays it, or
// opens a favorite folder, without going through the source's own tree.
#include <algorithm>

#include "app.h"
#include "app_internal.h"

using namespace appdetail;

namespace {

const char* KindName(const browse::Entry& e) {
  switch (e.type) {
  case browse::Entry::Type::Folder: return "folder";
  case browse::Entry::Type::Audio:  return "audio";
  case browse::Entry::Type::Video:  return "video";
  case browse::Entry::Type::Image:  return "image";
  default:                          return "other";
  }
}

std::string Plural(size_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

} // namespace

void App::GoHome() {
  CancelLaunch();
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

void App::StartDiscovery() {
  Set("status", bind_status_, Rml::String("Searching for media sources..."));
  discovering_ = true;
  if (view_ == View::Home)
    RebuildHome();

  busy_ops_++;
  PostTask([this] {
    std::vector<browse::SourcePtr> sources;
    std::string errors;
    for (const std::unique_ptr<browse::Provider>& p : providers_) {
      std::string error;
      if (!p->Discover(sources, error) && !error.empty()) {
        if (!errors.empty())
          errors += "; ";
        errors += std::string(p->Name()) + ": " + error;
      }
    }

    Reply([this, sources = std::move(sources), errors]() mutable {
      discovered_ = std::move(sources);
      discovering_ = false;
      registry_.Set(discovered_);

      Rml::String status;
      if (discovered_.empty()) {
        status = errors.empty() ? "No media sources found." : errors;
      } else {
        status = Plural(discovered_.size(), "source", "sources") + " found";
        if (!errors.empty())
          status += "  ·  " + errors;
      }
      Set("status", bind_status_, status);
      RebuildHome(); // keeps the rail on the same place
    });
    busy_ops_--;
  });
}

// ---------------------------------------------------------------------------
// Rail
// ---------------------------------------------------------------------------

namespace {

using Group = browse::Source::Group;
constexpr Group kGroups[] = {Group::Local, Group::Network, Group::Plugin};

struct GroupInfo {
  const char* icon;
  const char* name;
  const char* blurb;
};

const GroupInfo& InfoFor(Group group) {
  static const GroupInfo kLocal{"💾", "Local filesystem",
                                "Drives and folders on this machine."};
  static const GroupInfo kNetwork{"🖥", "Network sources",
                                  "Media servers (UPnP/DLNA) on the network."};
  static const GroupInfo kPlugin{"🧩", "Plugins",
                                 "Sources added by JavaScript plugins."};
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

std::string App::RailKey(const RailItem& item) {
  switch (item.kind) {
  case RailKind::Home:     return "home";
  case RailKind::Settings: return "settings";
  case RailKind::Group:    return "group:" + std::to_string((int)item.group);
  case RailKind::Up:       return "up";
  default:                 return "source:" + item.source->Key();
  }
}

void App::RebuildHome(const std::string& select) {
  const std::string selected = !select.empty() ? select
    : sel_rail_ < (int)rail_.size() ? RailKey(rail_[sel_rail_])
    : std::string("home");

  auto count = [this](Group group) {
    return std::count_if(discovered_.begin(), discovered_.end(),
      [group](const browse::SourcePtr& s) { return s->RailGroup() == group; });
  };

  rail_.clear();
  rail_rows_.clear();
  if (rail_in_group_) {
    // One group's sources, under a row that leads back to the top.
    rail_.push_back({RailKind::Up, nullptr, rail_group_});
    rail_rows_.push_back({"‹", InfoFor(rail_group_).name, "Back"});
    for (const browse::SourcePtr& s : discovered_) {
      if (s->RailGroup() != rail_group_)
        continue;
      rail_.push_back({RailKind::Source, s});
      rail_rows_.push_back({s->Icon(), s->Name(), s->Detail()});
    }
  } else {
    const size_t in_progress = library_.InProgress().size();
    const size_t favorites = library_.Favorites().size();
    rail_.push_back({RailKind::Home, nullptr});
    std::string home_detail;
    if (in_progress)
      home_detail = Plural(in_progress, "in progress", "in progress");
    if (favorites)
      home_detail += (home_detail.empty() ? "" : "  ·  ") +
                     Plural(favorites, "favorite", "favorites");
    rail_rows_.push_back({"🏠", "Home", home_detail});
    for (Group group : kGroups) {
      const size_t n = count(group);
      const GroupInfo& info = InfoFor(group);
      rail_.push_back({RailKind::Group, nullptr, group});
      rail_rows_.push_back({info.icon, info.name,
                            n ? Plural(n, "source", "sources")
                            : discovering_ ? "Searching..." : "None found"});
    }
    rail_.push_back({RailKind::Settings, nullptr});
    rail_rows_.push_back({"⚙", "Settings", ""});
  }

  // Stay where we were; just inside a group, that is its first source.
  sel_rail_ = rail_in_group_ && rail_.size() > 1 ? 1 : 0;
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

void App::EnterRailGroup(browse::Source::Group group) {
  rail_in_group_ = true;
  rail_group_ = group;
  sel_shelf_ = 0;
  shelf_card_sel_.clear();
  RebuildHome("-"); // no such key: lands on the group's first source
}

void App::LeaveRailGroup() {
  rail_in_group_ = false;
  sel_shelf_ = 0;
  shelf_card_sel_.clear();
  RailItem item{RailKind::Group, nullptr, rail_group_};
  RebuildHome(RailKey(item));
}

void App::MoveRail(int delta) {
  const int n = (int)rail_.size();
  if (n == 0)
    return;
  sel_rail_ = (sel_rail_ + delta + n) % n;
  model_.DirtyVariable("sel_rail");
  sel_shelf_ = 0;
  shelf_card_sel_.clear();
  RebuildShelves();
  scroll_sources_pending_ = true;
}

void App::ActivateRail() {
  if (sel_rail_ >= (int)rail_.size())
    return;
  const RailItem item = rail_[sel_rail_]; // a copy: the rail is rebuilt below
  switch (item.kind) {
  case RailKind::Home:
    if (!shelves_.empty())
      Set("home_shelves", bind_home_shelves_, true);
    RefreshImageBindings();
    break;
  case RailKind::Group:
    EnterRailGroup(item.group);
    break;
  case RailKind::Up:
    LeaveRailGroup();
    break;
  case RailKind::Source:
    OpenSource(item.source);
    break;
  case RailKind::Settings:
    OpenSettings();
    break;
  }
}

// ---------------------------------------------------------------------------
// Shelves
// ---------------------------------------------------------------------------

void App::RebuildShelves() {
  // Which place the shelves are for: everything, one source, or nothing.
  RailKind kind = RailKind::Home;
  if (sel_rail_ < (int)rail_.size())
    kind = rail_[sel_rail_].kind;
  const RailItem* item = sel_rail_ < (int)rail_.size() ? &rail_[sel_rail_] : nullptr;
  auto wanted = [&](const std::string& source_key) {
    switch (kind) {
    case RailKind::Source:
      return source_key == item->source->Key();
    case RailKind::Group:
    case RailKind::Up:
      return GroupOfKey(source_key) == item->group;
    default:
      return true;
    }
  };

  shelves_.clear();
  if (kind != RailKind::Settings) {
    Shelf resume{"Continue watching", continue_source_, {}};
    for (const auto& item : library_.InProgress()) {
      if (!wanted(item.first.source_key))
        continue;
      browse::Entry e = item.first.entry;
      e.id = library::EncodeId(item.first.source_key, item.first.entry.id);
      resume.entries.push_back(std::move(e));
    }
    if (!resume.entries.empty())
      shelves_.push_back(std::move(resume));

    Shelf favs{"Favorites", favorites_source_, {}};
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

  // The hero: what the rail is on, in large.
  switch (kind) {
  case RailKind::Home:
    Set("hero_icon", bind_hero_icon_, Rml::String(shelves_.empty() ? "🏠" : ""));
    Set("hero_title", bind_hero_title_,
        Rml::String(shelves_.empty() ? "Welcome" : ""));
    Set("hero_detail", bind_hero_detail_, Rml::String(shelves_.empty()
      ? "Pick a source on the left to start browsing." : ""));
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
    Set("hero_icon", bind_hero_icon_, Rml::String("⚙"));
    Set("hero_title", bind_hero_title_, Rml::String("Settings"));
    Set("hero_detail", bind_hero_detail_,
        Rml::String("Playback, languages and the screensaver."));
    Set("hero_note", bind_hero_note_, Rml::String("Press select to change them."));
    break;
  case RailKind::Group:
  case RailKind::Up: {
    const GroupInfo& info = InfoFor(item->group);
    const bool any = std::any_of(discovered_.begin(), discovered_.end(),
      [&](const browse::SourcePtr& s) { return s->RailGroup() == item->group; });
    Rml::String note;
    if (!shelves_.empty())
      note = "";
    else if (!any)
      // Network: discovery's own report ("Searching...", errors).
      note = item->group == Group::Network || discovering_
        ? bind_status_ : Rml::String("None found.");
    else if (kind == RailKind::Group)
      note = "Press select to see them.";
    Set("hero_icon", bind_hero_icon_, Rml::String(info.icon));
    Set("hero_title", bind_hero_title_, Rml::String(info.name));
    Set("hero_detail", bind_hero_detail_, Rml::String(info.blurb));
    Set("hero_note", bind_hero_note_, note);
    break;
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
  for (const Shelf& shelf : shelves_) {
    ShelfRow row;
    row.title = shelf.title;
    for (const browse::Entry& e : shelf.entries) {
      CardRow card;
      card.title = e.name.empty() ? "(untitled)" : e.name;
      card.glyph = IconFor(e);
      card.kind = KindName(e);
      library::Ref ref;
      std::string where;
      if (Identify(e, ref)) {
        where = ref.source_name;
        library::Progress p;
        if (library_.GetProgress(ref.source_key, ref.entry.id, p)) {
          card.watched = p.watched;
          if (!p.watched && p.position > 0 && p.duration > 0)
            card.progress = std::to_string(
              std::clamp((int)(p.position * 100 / p.duration), 2, 100)) + "%";
          if (!p.watched && p.position > 0 && p.duration > p.position)
            card.subtitle = FormatTime((p.duration - p.position) * 1000000) + " left";
        }
      }
      if (card.subtitle.empty())
        card.subtitle = TypeName(e);
      if (!where.empty())
        card.subtitle += "  ·  " + where;
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
  const browse::Entry entry = *card;
  // The card's own library source resolves and browses through the source
  // the entry came from.
  current_source_ = shelves_[sel_shelf_].source;
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
  const bool on_shelves = bind_home_shelves_ && !shelves_.empty();

  if (!on_shelves) {
    switch (key) {
    case Rml::Input::KI_UP:    MoveRail(-1); break;
    case Rml::Input::KI_DOWN:  MoveRail(+1); break;
    case Rml::Input::KI_HOME:  MoveRail(-sel_rail_); break;
    case Rml::Input::KI_END:   MoveRail((int)rail_.size() - 1 - sel_rail_); break;
    case Rml::Input::KI_RIGHT:
      if (!shelves_.empty()) {
        Set("home_shelves", bind_home_shelves_, true);
        RefreshImageBindings();
      }
      break;
    case Rml::Input::KI_SPACE: StartDiscovery(); break;
    case Rml::Input::KI_F1:    OpenMenu(); break;
    case Rml::Input::KI_RETURN:
    case Rml::Input::KI_NUMPADENTER:
      ActivateRail();
      break;
    case Rml::Input::KI_BACK:
    case Rml::Input::KI_ESCAPE:
      if (!rail_in_group_)
        return; // nothing above the top level
      LeaveRailGroup();
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
    if (sel_card_ == 0) {
      Set("home_shelves", bind_home_shelves_, false);
      UpdateBackdrop();
    } else {
      select(sel_shelf_, sel_card_ - 1);
    }
    break;
  case Rml::Input::KI_RIGHT:
    if (sel_card_ + 1 < cards)
      select(sel_shelf_, sel_card_ + 1);
    break;
  case Rml::Input::KI_UP:
    if (sel_shelf_ > 0)
      select(sel_shelf_ - 1, shelf_card_sel_[sel_shelf_ - 1]);
    break;
  case Rml::Input::KI_DOWN:
    if (sel_shelf_ + 1 < (int)shelves_.size())
      select(sel_shelf_ + 1, shelf_card_sel_[sel_shelf_ + 1]);
    break;
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

void App::OpenSettings() {
  menu_items_.clear();
  Set("menu_title", bind_menu_title_, Rml::String("Settings"));
  Set("menu_dialog", bind_menu_dialog_, false);

  auto step = [](int index, int d, int count) {
    return (index + (d < 0 ? count - 1 : 1)) % count;
  };

  menu_items_.push_back({"Next episode",
    [this] { return std::string(autoplay_ ? "Plays by itself" : "Stop at the end"); },
    [this](int) { autoplay_ = !autoplay_; SaveSettings(); }});
  menu_items_.push_back({"Audio language",
    [this] { return std::string(kLanguages[LanguageIndex(audio_lang_)].name); },
    [this, step](int d) {
      audio_lang_ = kLanguages[step(LanguageIndex(audio_lang_), d, kLanguageCount)].code;
      ApplyLanguages();
      SaveSettings();
    }});
  menu_items_.push_back({"Subtitle language",
    [this] { return std::string(kLanguages[LanguageIndex(sub_lang_)].name); },
    [this, step](int d) {
      sub_lang_ = kLanguages[step(LanguageIndex(sub_lang_), d, kLanguageCount)].code;
      ApplyLanguages();
      SaveSettings();
    }});
  menu_items_.push_back({"Screensaver",
    [this] {
      return screensaver_sec_ <= 0 ? std::string("Never")
        : "After " + std::to_string((int)(screensaver_sec_ / 60)) + " min";
    },
    [this, step](int d) {
      int i = 0;
      while (i < kSaverCount - 1 && kSaverMinutes[i] * 60.0 != screensaver_sec_)
        i++;
      screensaver_sec_ = kSaverMinutes[step(i, d, kSaverCount)] * 60.0;
      SaveSettings();
    }});
  menu_items_.push_back({"Night mode",
    [this] { return std::string(player_->NightMode() ? "On" : "Off"); },
    [this](int) { player_->SetNightMode(!player_->NightMode()); SaveSettings(); }});
  menu_items_.push_back({"Forget Continue watching", nullptr, [this](int) {
    const auto items = library_.InProgress();
    for (const auto& item : items)
      library_.SetWatched(item.first, false);
    library_.Save();
    ShowToast("Cleared " + std::to_string(items.size()) + " in progress", "🧹");
  }});
  menu_items_.push_back({"Rescan sources", nullptr, [this](int) { StartDiscovery(); }});

  sel_menu_ = 0;
  RebuildMenuRows();
  Set("menu_open", bind_menu_open_, true);
}
