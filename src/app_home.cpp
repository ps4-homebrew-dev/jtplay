// SPDX-License-Identifier: GPL-3.0-or-later
//
// The home screen: a rail of places down the left (Home, every source,
// Rescan) and shelves of cards on the right, drawn from the library --
// Continue watching and Favorites, for everything when the rail is on Home
// and for one source when it is on that source. Selecting a source opens
// it in the browse view; selecting a card plays it, or opens a favorite
// folder, without going through the source's own tree.
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

void App::RebuildHome() {
  auto key_of = [](const RailItem& item) -> std::string {
    switch (item.kind) {
    case RailKind::Home:   return "home";
    case RailKind::Rescan: return "rescan";
    default:               return item.source->Key();
    }
  };
  const std::string selected =
    sel_rail_ < (int)rail_.size() ? key_of(rail_[sel_rail_]) : std::string("home");

  const size_t in_progress = library_.InProgress().size();
  const size_t favorites = library_.Favorites().size();

  rail_.clear();
  rail_rows_.clear();
  rail_.push_back({RailKind::Home, nullptr});
  std::string home_detail;
  if (in_progress)
    home_detail = Plural(in_progress, "in progress", "in progress");
  if (favorites)
    home_detail += (home_detail.empty() ? "" : "  ·  ") +
                   Plural(favorites, "favorite", "favorites");
  rail_rows_.push_back({"🏠", "Home", home_detail});
  for (const browse::SourcePtr& s : discovered_) {
    rail_.push_back({RailKind::Source, s});
    rail_rows_.push_back({s->Icon(), s->Name(), s->Detail()});
  }
  rail_.push_back({RailKind::Rescan, nullptr});
  rail_rows_.push_back({"🔄", "Rescan", ""});

  sel_rail_ = 0;
  for (size_t i = 0; i < rail_.size(); i++) {
    if (key_of(rail_[i]) == selected) {
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
  const RailItem& item = rail_[sel_rail_];
  switch (item.kind) {
  case RailKind::Home:
    if (!shelves_.empty())
      Set("home_shelves", bind_home_shelves_, true);
    RefreshImageBindings();
    break;
  case RailKind::Source:
    OpenSource(item.source);
    break;
  case RailKind::Rescan:
    StartDiscovery();
    break;
  }
}

// ---------------------------------------------------------------------------
// Shelves
// ---------------------------------------------------------------------------

void App::RebuildShelves() {
  // Which place the shelves are for: everything, one source, or nothing.
  std::string only;
  RailKind kind = RailKind::Home;
  if (sel_rail_ < (int)rail_.size()) {
    kind = rail_[sel_rail_].kind;
    if (kind == RailKind::Source)
      only = rail_[sel_rail_].source->Key();
  }

  shelves_.clear();
  if (kind != RailKind::Rescan) {
    Shelf resume{"Continue watching", continue_source_, {}};
    for (const auto& item : library_.InProgress()) {
      if (!only.empty() && item.first.source_key != only)
        continue;
      browse::Entry e = item.first.entry;
      e.id = library::EncodeId(item.first.source_key, item.first.entry.id);
      resume.entries.push_back(std::move(e));
    }
    if (!resume.entries.empty())
      shelves_.push_back(std::move(resume));

    Shelf favs{"Favorites", favorites_source_, {}};
    for (const library::Ref& ref : library_.Favorites()) {
      if (!only.empty() && ref.source_key != only)
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
    const browse::SourcePtr& s = rail_[sel_rail_].source;
    Set("hero_icon", bind_hero_icon_, Rml::String(s->Icon()));
    Set("hero_title", bind_hero_title_, s->Name());
    Set("hero_detail", bind_hero_detail_, s->Detail());
    Set("hero_note", bind_hero_note_, Rml::String(shelves_.empty()
      ? "Nothing from here in your library yet." : ""));
    break;
  }
  case RailKind::Rescan:
    Set("hero_icon", bind_hero_icon_, Rml::String("🔄"));
    Set("hero_title", bind_hero_title_, Rml::String("Rescan"));
    Set("hero_detail", bind_hero_detail_,
        Rml::String("Look for media servers on the network again."));
    Set("hero_note", bind_hero_note_, bind_status_);
    break;
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
