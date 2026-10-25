// SPDX-License-Identifier: GPL-3.0-or-later
//
// The browse view: the content tree of a source, as a list with an info
// panel or as a wall of artwork tiles. Everything here is protocol-
// agnostic; the per-protocol work lives behind browse::Source.
#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "app.h"
#include "app_internal.h"
#include "browse/natural_sort.h"

using namespace appdetail;

namespace {

// Index after moving 'delta' rows in a list of 'count': single steps wrap
// around the ends (down on the last row lands on the first, and back),
// bigger jumps stop at them.
int StepIndex(int index, int delta, int count) {
  if (count <= 0)
    return 0;
  if (delta == 1 || delta == -1)
    return (index + delta + count) % count;
  return std::clamp(index + delta, 0, count - 1);
}

const char* KindName(const browse::Entry& e) {
  switch (e.type) {
  case browse::Entry::Type::Folder: return "folder";
  case browse::Entry::Type::Audio:  return "audio";
  case browse::Entry::Type::Video:  return "video";
  case browse::Entry::Type::Image:  return "image";
  default:                          return "other";
  }
}

// The letter a name files under: its first letter or digit, upper-cased
// (ASCII; anything else is kept as its whole UTF-8 character), with
// leading "The " and punctuation skipped. Digits all file under "#".
std::string Initial(const std::string& name) {
  size_t i = 0;
  if (name.size() > 4 && (name.compare(0, 4, "The ") == 0 || name.compare(0, 4, "the ") == 0))
    i = 4;
  while (i < name.size() && (unsigned char)name[i] < 0x80 && !std::isalnum((unsigned char)name[i]))
    i++;
  if (i >= name.size())
    return "#";
  const unsigned char c = (unsigned char)name[i];
  if (c < 0x80)
    return std::isdigit(c) ? std::string("#") : std::string(1, (char)std::toupper(c));
  size_t n = 1;
  while (i + n < name.size() && ((unsigned char)name[i + n] & 0xc0) == 0x80)
    n++;
  return name.substr(i, n);
}

const char* SortModeName(int mode) {
  static const char* const kNames[] = {"Default", "Name", "Date", "Size"};
  return kNames[mode];
}

} // namespace

// ---------------------------------------------------------------------------
// Browse view
// ---------------------------------------------------------------------------

App::BrowseLevel* App::CurrentLevel() {
  return path_.empty() ? nullptr : &path_.back();
}

const browse::Entry* App::SelectedEntry() const {
  if (path_.empty())
    return nullptr;
  const BrowseLevel& level = path_.back();
  if (sel_entry_ < 0 || sel_entry_ >= (int)level.entries.size())
    return nullptr;
  return &level.entries[sel_entry_];
}

void App::OpenSource(const browse::SourcePtr& source) {
  if (!source)
    return;
  CancelLaunch();
  current_source_ = source;
  path_.clear();
  sel_entry_ = 0;

  Set("source_name", bind_source_name_, current_source_->Name());
  view_ = View::Browse;
  Set("view", bind_view_, Rml::String("browse"));

  RebuildEntryRows();
  RebuildCrumb();
  RebuildDetail();
  RequestBrowse(current_source_->RootId(), current_source_->Name(), false);
}

// Lists 'id' on the worker. The result is pushed as a new level, or with
// 'replace' takes the place of the current one (a refresh), keeping the
// cursor on the same entry if it is still there.
void App::RequestBrowse(const std::string& id, const std::string& name,
                        bool replace) {
  if (!current_source_)
    return;

  const uint32_t request = ++browse_request_;
  const browse::SourcePtr source = current_source_;

  busy_ops_++;
  PostTask([this, request, source, id, name, replace] {
    browse::Listing result;
    std::string error;
    source->Browse(id, result, error);

    Reply([this, request, id, name, replace, result = std::move(result),
           error]() mutable {
      if (request != browse_request_)
        return; // superseded while it was in flight
      if (!error.empty()) {
        ShowToast(error, "⚠");
        // A failed root browse drops back home.
        if (path_.empty())
          GoHome();
        return;
      }

      std::string keep;
      if (replace) {
        if (const browse::Entry* e = SelectedEntry())
          keep = e->id;
        if (!path_.empty())
          path_.pop_back();
      }

      BrowseLevel level;
      level.id = id;
      level.name = name;
      level.raw = std::move(result);
      SortLevel(level);
      for (size_t i = 0; i < level.entries.size() && !keep.empty(); i++) {
        if (level.entries[i].id == keep) {
          level.selection = (int)i;
          break;
        }
      }
      sel_entry_ = level.selection;
      path_.push_back(std::move(level));

      RebuildEntryRows();
      RebuildCrumb();
      RebuildDetail();
      scroll_entries_pending_ = true;
    });
    busy_ops_--;
  });
}

void App::EnterContainer(const browse::Entry& entry) {
  CancelLaunch(); // the row it was started from is about to go away
  if (BrowseLevel* level = CurrentLevel())
    level->selection = sel_entry_;
  RequestBrowse(entry.id, entry.name, false);
}

void App::RefreshContainer() {
  const BrowseLevel* level = CurrentLevel();
  if (!level)
    return;
  CancelLaunch();
  RequestBrowse(level->id, level->name, true);
}

void App::LeaveContainer() {
  CancelLaunch();    // ... otherwise playback pops up over another folder
  browse_request_++; // invalidate any browse that is still in flight
  if (path_.size() <= 1) {
    GoHome(); // at the root: back home, where the rail still points here
    return;
  }
  path_.pop_back();
  sel_entry_ = path_.back().selection;
  RebuildEntryRows();
  RebuildCrumb();
  RebuildDetail();
  scroll_entries_pending_ = true;
}

void App::RebuildEntryRows() {
  entry_rows_.clear();
  if (const BrowseLevel* level = CurrentLevel()) {
    for (const browse::Entry& e : level->entries) {
      EntryRow row;
      row.icon = IconFor(e);
      row.kind = KindName(e);
      row.name = e.name.empty() ? "(untitled)" : e.name;
      row.meta = EntryMeta(e);
      row.folder = e.IsFolder();
      library::Ref ref;
      if (Identify(e, ref)) {
        row.favorite = library_.IsFavorite(ref.source_key, ref.entry.id);
        library::Progress p;
        if (library_.GetProgress(ref.source_key, ref.entry.id, p)) {
          row.watched = p.watched;
          if (!p.watched && p.position > 0 && p.duration > 0) // never "0%"
            row.progress = std::to_string(
              std::clamp((int)(p.position * 100 / p.duration), 2, 100)) + "%";
        }
      }
      entry_rows_.push_back(std::move(row));
    }
  }
  entry_count_ = (int)entry_rows_.size();
  sel_entry_ = std::clamp(sel_entry_, 0, std::max(0, entry_count_ - 1));
  model_.DirtyVariable("entries");
  model_.DirtyVariable("entry_count");
  model_.DirtyVariable("sel_entry");
  Set("sort_label", bind_sort_label_, SortLabel());
  art_scan_pending_ = true; // artwork for the new rows, this frame
}

void App::RebuildCrumb() {
  // The source is the pane title already; the crumb is the path below it
  // (all of it when the browse started at a favorite folder, not the root).
  std::string crumb;
  const size_t first =
    !path_.empty() && current_source_ && path_[0].id == current_source_->RootId() ? 1 : 0;
  for (size_t i = first; i < path_.size(); i++) {
    if (!crumb.empty())
      crumb += "  ›  ";
    crumb += path_[i].name;
  }
  Set("crumb", bind_crumb_, crumb);
}

void App::RebuildDetail() {
  const browse::Entry* e = SelectedEntry();

  Set("detail_name", bind_detail_name_, e ? e->name : std::string());
  Set("detail_icon", bind_detail_icon_, Rml::String(e ? IconFor(*e) : ""));
  Set("detail_description", bind_detail_description_,
      e ? e->description : std::string());

  std::vector<PropRow> props;
  if (e) {
    auto add = [&props](const char* label, const std::string& value) {
      if (!value.empty())
        props.push_back({label, value});
    };
    add("Type", TypeName(*e));
    add("Duration", e->duration > 0 ? FormatTime(e->duration * 1000000) : "");
    add("Size", FormatSize(e->size));
    add("Date", FormatDate(e->date, true));

    library::Ref ref;
    if (Identify(*e, ref)) {
      library::Progress p;
      if (library_.GetProgress(ref.source_key, ref.entry.id, p)) {
        if (p.watched)
          add("Status", "Watched");
        else if (p.position > 0)
          add("Status", "Resume at " + FormatTime(p.position * 1000000));
      }
      // Under Favorites / Continue watching, where it actually lives.
      if (ref.source_key != (current_source_ ? current_source_->Key() : ""))
        add("Source", ref.source_name.empty() ? "Not available" : ref.source_name);
      Set("detail_favorite", bind_detail_favorite_,
          library_.IsFavorite(ref.source_key, ref.entry.id));
    }
  }
  bool same = props.size() == detail_props_.size();
  for (size_t i = 0; same && i < props.size(); i++)
    same = props[i].label == detail_props_[i].label &&
           props[i].value == detail_props_[i].value;
  if (!same) {
    detail_props_ = std::move(props);
    model_.DirtyVariable("detail_props");
  }

  RefreshImageBindings();
}

void App::MoveSelection(int delta) {
  if (entry_count_ == 0)
    return;
  sel_entry_ = StepIndex(sel_entry_, delta, entry_count_);
  if (BrowseLevel* level = CurrentLevel())
    level->selection = sel_entry_;
  model_.DirtyVariable("sel_entry");
  RebuildDetail();
  scroll_entries_pending_ = true;
}

// Folders come first, so the letters run twice (folders A-Z, then items
// A-Z); a jump simply goes to where the initial changes, so it walks
// through both runs and wraps around at the ends.
void App::JumpLetter(int direction) {
  const BrowseLevel* level = CurrentLevel();
  const int n = level ? (int)level->entries.size() : 0;
  if (n == 0)
    return;
  const browse::Listing& entries = level->entries;
  auto group_start = [&](int i) {
    const std::string letter = Initial(entries[i].name);
    while (i > 0 && Initial(entries[i - 1].name) == letter &&
           entries[i - 1].IsFolder() == entries[i].IsFolder())
      i--;
    return i;
  };
  int target;
  if (direction > 0) {
    const std::string letter = Initial(entries[sel_entry_].name);
    const bool folder = entries[sel_entry_].IsFolder();
    target = sel_entry_;
    while (target < n && Initial(entries[target].name) == letter &&
           entries[target].IsFolder() == folder)
      target++;
    if (target >= n)
      target = 0;
  } else {
    const int start = group_start(sel_entry_);
    // In the middle of a group: back to its start; at the start: the
    // start of the group before (the last one, from the top).
    target = start < sel_entry_ ? start : group_start(start > 0 ? start - 1 : n - 1);
  }
  MoveSelection(target - sel_entry_);
  Set("jump_letter", bind_jump_letter_, Rml::String(Initial(entries[target].name)));
  jump_letter_until_ = Now() + 0.8;
}

void App::ActivateSelection() {
  const browse::Entry* e = SelectedEntry();
  if (!e)
    return;
  if (e->IsFolder())
    EnterContainer(*e);
  else
    PlayEntry(*e);
}

void App::HandleKeyBrowse(Rml::Event& event, int key) {
  const bool grid = view_mode_ == ViewMode::Grid;
  // Grid: up/down move a row, staying in the column, and wrap from the
  // last row to the first (and back) like the list does.
  auto grid_row = [this](int direction) {
    const int n = entry_count_;
    if (n == 0)
      return;
    const int col = sel_entry_ % kGridColumns;
    int target = sel_entry_ + direction * kGridColumns;
    if (target >= n)
      target = col;                          // past the bottom: top row
    else if (target < 0) {
      target = ((n - 1) / kGridColumns) * kGridColumns + col; // bottom row
      if (target >= n)
        target = n - 1;
    }
    MoveSelection(target - sel_entry_);
  };

  switch (key) {
  case Rml::Input::KI_UP:
    if (grid) grid_row(-1); else MoveSelection(-1);
    break;
  case Rml::Input::KI_DOWN:
    if (grid) grid_row(+1); else MoveSelection(+1);
    break;
  case Rml::Input::KI_LEFT:  // remotes rarely have page keys
    if (grid) MoveSelection(-1);
    else if (sort_mode_ == SortMode::Name) JumpLetter(-1);
    else MoveSelection(-kPageRows);
    break;
  case Rml::Input::KI_RIGHT:
    if (grid) MoveSelection(+1);
    else if (sort_mode_ == SortMode::Name) JumpLetter(+1);
    else MoveSelection(+kPageRows);
    break;
  case Rml::Input::KI_PRIOR:
    MoveSelection(grid ? -3 * kGridColumns : -kPageRows);
    break;
  case Rml::Input::KI_NEXT:
    MoveSelection(grid ? +3 * kGridColumns : +kPageRows);
    break;
  case Rml::Input::KI_HOME:  MoveSelection(-entry_count_); break;
  case Rml::Input::KI_END:   MoveSelection(+entry_count_); break;
  case Rml::Input::KI_SPACE: CycleSortMode(+1); break;
  case Rml::Input::KI_T:     SetSort(sort_mode_, !sort_desc_); break;
  case Rml::Input::KI_F1:    OpenMenu(); break;
  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER:
    ActivateSelection();
    break;
  case Rml::Input::KI_BACK:
  case Rml::Input::KI_ESCAPE:
    // While something is opening, back cancels that rather than the folder.
    if (launch_request_) {
      CancelLaunch();
      ShowToast("Cancelled", "⏹");
    } else {
      LeaveContainer();
    }
    break;
  default:
    return;
  }
  event.StopPropagation();
}

// ---------------------------------------------------------------------------
// Sorting
// ---------------------------------------------------------------------------

// Rebuilds level.entries from level.raw in the current order, keeping the
// selection on the same entry. Folders stay on top; entries that do not
// know the sort key (no size, no date) go last in either direction, and
// ties fall back to the name.
void App::SortLevel(BrowseLevel& level) const {
  std::string selected;
  if (level.selection >= 0 && level.selection < (int)level.entries.size())
    selected = level.entries[level.selection].id;

  // Sort indices into raw so Default can use the source order as its key.
  std::vector<size_t> order(level.raw.size());
  for (size_t i = 0; i < order.size(); i++)
    order[i] = i;

  const browse::Listing& raw = level.raw;
  const SortMode mode = sort_mode_;
  const bool desc = sort_desc_;
  auto key_less = [&](size_t ia, size_t ib) -> int { // -1, 0, 1
    const browse::Entry& a = raw[ia];
    const browse::Entry& b = raw[ib];
    switch (mode) {
    case SortMode::Name: return browse::NaturalCompare(a.name, b.name);
    case SortMode::Date: return a.date == b.date ? 0 : a.date < b.date ? -1 : 1;
    case SortMode::Size: return a.size == b.size ? 0 : a.size < b.size ? -1 : 1;
    default:             return ia == ib ? 0 : ia < ib ? -1 : 1;
    }
  };
  auto unknown = [mode](const browse::Entry& e) {
    return (mode == SortMode::Date && e.date <= 0) ||
           (mode == SortMode::Size && e.size < 0);
  };

  std::stable_sort(order.begin(), order.end(), [&](size_t ia, size_t ib) {
    const browse::Entry& a = raw[ia];
    const browse::Entry& b = raw[ib];
    if (a.IsFolder() != b.IsFolder())
      return a.IsFolder();
    if (unknown(a) != unknown(b))
      return unknown(b);
    int c = key_less(ia, ib);
    if (desc)
      c = -c;
    if (c == 0 && mode != SortMode::Default && mode != SortMode::Name)
      c = browse::NaturalCompare(a.name, b.name);
    return c < 0;
  });

  level.entries.clear();
  level.entries.reserve(order.size());
  for (size_t i : order)
    level.entries.push_back(raw[i]);

  level.selection = 0;
  for (size_t i = 0; i < level.entries.size() && !selected.empty(); i++) {
    if (level.entries[i].id == selected) {
      level.selection = (int)i;
      break;
    }
  }
}

void App::SetSort(SortMode mode, bool descending) {
  if (mode == sort_mode_ && descending == sort_desc_)
    return;
  sort_mode_ = mode;
  sort_desc_ = descending;
  SaveSettings();

  if (BrowseLevel* level = CurrentLevel())
    level->selection = sel_entry_;
  for (BrowseLevel& level : path_)
    SortLevel(level);
  if (BrowseLevel* level = CurrentLevel())
    sel_entry_ = level->selection;

  RebuildEntryRows();
  RebuildDetail();
  scroll_entries_pending_ = true;
}

// Each key starts in the direction people usually want it: names A-Z,
// newest and largest first.
void App::CycleSortMode(int direction) {
  const int count = (int)SortMode::Count;
  const SortMode next =
    (SortMode)(((int)sort_mode_ + direction + count) % count);
  SetSort(next, next == SortMode::Date || next == SortMode::Size);
}

std::string App::SortLabel() const {
  // The emoji font has the triangles; the text font does not.
  return std::string(SortModeName((int)sort_mode_)) +
         (sort_desc_ ? "  🔽" : "  🔼");
}

// The right-hand column of a row, Kodi style: whatever the list is sorted
// by, so the order explains itself; otherwise duration, then size.
std::string App::EntryMeta(const browse::Entry& e) const {
  switch (sort_mode_) {
  case SortMode::Date:
    return FormatDate(e.date, false);
  case SortMode::Size:
    return FormatSize(e.size);
  default:
    if (e.IsFolder())
      return {};
    if (e.duration > 0)
      return FormatTime(e.duration * 1000000);
    return FormatSize(e.size);
  }
}

// ---------------------------------------------------------------------------
// Options menu (the options button, or F1)
// ---------------------------------------------------------------------------

// The actions for one entry, Kodi's context menu style: shared by the
// browse list and the home shelves.
void App::AddEntryActions(const browse::Entry& entry) {
  library::Ref ref;
  if (!Identify(entry, ref))
    return;
  if (entry.IsImage()) {
    menu_items_.push_back({"Open", nullptr, [this, entry](int) { PlayEntry(entry); }});
    menu_items_.push_back({"Slideshow from here", nullptr, [this, entry](int) {
      int index = 0;
      const browse::Listing photos = FolderPhotos(entry, index);
      OpenPhotos(photos, index, true);
    }});
  } else if (!entry.IsFolder()) {
    library::Progress p;
    const bool known = library_.GetProgress(ref.source_key, ref.entry.id, p);
    if (known && !p.watched && p.position > 0) {
      const int64_t at = p.position;
      menu_items_.push_back({"Resume from " + FormatTime(at * 1000000), nullptr,
        [this, entry, at](int) { Launch(entry, at); }});
      menu_items_.push_back({"Play from beginning", nullptr,
        [this, entry](int) { Launch(entry, 0); }});
    } else {
      menu_items_.push_back({"Play", nullptr,
        [this, entry](int) { Launch(entry, 0); }});
    }
  }
  const bool fav = library_.IsFavorite(ref.source_key, ref.entry.id);
  menu_items_.push_back({fav ? "Remove from favorites" : "Add to favorites",
    nullptr, [this, entry](int) { ToggleFavorite(entry); }});
  if (entry.IsVideo() || entry.IsAudio()) {
    library::Progress p;
    const bool known = library_.GetProgress(ref.source_key, ref.entry.id, p);
    const bool watched = known && p.watched;
    menu_items_.push_back({watched ? "Mark as unwatched" : "Mark as watched",
      nullptr, [this, entry, watched](int) { SetWatched(entry, !watched); }});
  }
}

void App::OpenMenu() {
  menu_items_.clear();
  Set("menu_dialog", bind_menu_dialog_, false);

  if (view_ == View::Browse) {
    const browse::Entry* e = SelectedEntry();
    Set("menu_title", bind_menu_title_, e ? e->name : Rml::String("Options"));
    if (e)
      AddEntryActions(*e);
    // A folder of music or episodes plays as a whole, in order or shuffled.
    if (PlayOrder().size() > 1) {
      menu_items_.push_back({"Play all", nullptr, [this](int) { PlayFolder(false); }});
      menu_items_.push_back({"Shuffle all", nullptr, [this](int) { PlayFolder(true); }});
    }
    // A folder with pictures in it can be shown as a slideshow from the
    // start, whatever is selected.
    if (const BrowseLevel* level = CurrentLevel()) {
      browse::Listing photos;
      for (const browse::Entry& item : level->entries)
        if (item.IsImage())
          photos.push_back(item);
      if (photos.size() > 1 && !(e && e->IsImage())) {
        menu_items_.push_back({"Slideshow (" + std::to_string(photos.size()) + " pictures)",
          nullptr, [this, photos](int) { OpenPhotos(photos, 0, true); }});
      }
    }
    menu_items_.push_back({"View",
      [this] { return std::string(view_mode_ == ViewMode::Grid ? "Grid" : "List"); },
      [this](int) {
        SetViewMode(view_mode_ == ViewMode::Grid ? ViewMode::List : ViewMode::Grid);
      }});
    menu_items_.push_back({"Sort by",
      [this] { return std::string(SortModeName((int)sort_mode_)); },
      [this](int d) { CycleSortMode(d == 0 ? +1 : d); }});
    menu_items_.push_back({"Order",
      [this] { return std::string(sort_desc_ ? "Descending" : "Ascending"); },
      [this](int) { SetSort(sort_mode_, !sort_desc_); }});
    menu_items_.push_back({"Refresh", nullptr,
      [this](int) { RefreshContainer(); }});
    menu_items_.push_back({"Go to top", nullptr,
      [this](int) { MoveSelection(-entry_count_); }});
    menu_items_.push_back({"Home", nullptr, [this](int) { GoHome(); }});
  } else if (bind_home_shelves_ && SelectedCard()) {
    const browse::Entry card = *SelectedCard();
    Set("menu_title", bind_menu_title_, card.name);
    if (card.IsFolder())
      menu_items_.push_back({"Open", nullptr, [this](int) { ActivateCard(); }});
    AddEntryActions(card);
    if (shelves_[sel_shelf_].source == continue_source_)
      menu_items_.push_back({"Remove from Continue watching", nullptr,
        [this, card](int) { SetWatched(card, false); }});
  } else {
    Set("menu_title", bind_menu_title_, Rml::String("Options"));
    menu_items_.push_back({"Rescan sources", nullptr,
      [this](int) { StartDiscovery(); }});
  }

  sel_menu_ = 0;
  RebuildMenuRows();
  Set("menu_open", bind_menu_open_, true);
}

void App::CloseMenu() {
  Set("menu_open", bind_menu_open_, false);
  menu_items_.clear();
}

void App::RebuildMenuRows() {
  scroll_menu_pending_ = true;
  menu_rows_.clear();
  for (const MenuItem& item : menu_items_)
    menu_rows_.push_back({item.label, item.value ? item.value() : ""});
  model_.DirtyVariable("menu");
  model_.DirtyVariable("sel_menu");
}

void App::HandleKeyMenu(Rml::Event& event, int key) {
  event.StopPropagation(); // the menu is modal
  const int count = (int)menu_items_.size();
  if (count == 0) {
    CloseMenu();
    return;
  }

  switch (key) {
  case Rml::Input::KI_UP:
  case Rml::Input::KI_DOWN:
    sel_menu_ = StepIndex(sel_menu_, key == Rml::Input::KI_UP ? -1 : +1, count);
    model_.DirtyVariable("sel_menu");
    scroll_menu_pending_ = true;
    break;
  case Rml::Input::KI_LEFT:
  case Rml::Input::KI_RIGHT:
    if (menu_items_[sel_menu_].value) {
      menu_items_[sel_menu_].change(key == Rml::Input::KI_LEFT ? -1 : +1);
      RebuildMenuRows();
    }
    break;
  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER: {
    if (menu_items_[sel_menu_].value) {
      menu_items_[sel_menu_].change(0);
      RebuildMenuRows();
    } else {
      // Close first: the action may replace the view the menu was for.
      const std::function<void(int)> action = menu_items_[sel_menu_].change;
      CloseMenu();
      action(0);
    }
    break;
  }
  case Rml::Input::KI_BACK:
  case Rml::Input::KI_ESCAPE:
  case Rml::Input::KI_F1:
    CloseMenu();
    break;
  default:
    break;
  }
}

// ---------------------------------------------------------------------------
// View modes and artwork
// ---------------------------------------------------------------------------

void App::SetViewMode(ViewMode mode) {
  if (mode == view_mode_)
    return;
  view_mode_ = mode;
  SaveSettings();
  Set("view_mode", bind_view_mode_, Rml::String(mode == ViewMode::Grid ? "grid" : "list"));
  scroll_entries_pending_ = true;
  art_scan_pending_ = true;
}

void App::UpdateVisibleArt() {
  if (view_ == View::Browse) {
    const BrowseLevel* level = CurrentLevel();
    if (!level || entry_rows_.size() != level->entries.size())
      return;
    // What is on screen and a screenful either side, so scrolling finds
    // the next rows ready. The art thread takes the newest request first,
    // so the selection's neighbourhood always wins.
    int first, last;
    if (view_mode_ == ViewMode::Grid) {
      const int row = sel_entry_ / kGridColumns;
      first = (row - 4) * kGridColumns;
      last = (row + 5) * kGridColumns;
    } else {
      first = sel_entry_ - 16;
      last = sel_entry_ + 17;
    }
    first = std::max(0, first);
    last = std::min((int)level->entries.size(), last);
    bool changed = false;
    // Outwards from the selection, so it is asked for last (= fetched first).
    for (int i = last - 1; i >= first; i--) {
      EntryRow& row = entry_rows_[i];
      if (row.art != "none")
        continue;
      const std::string uri = ArtUri(level->entries[i]);
      if (uri.empty())
        continue;
      const std::string path = ImagePathFor(uri, true);
      if (!path.empty()) {
        row.art = CoverDecorator(path);
        changed = true;
      }
    }
    if (changed)
      model_.DirtyVariable("entries");
    return;
  }

  // Home: every card of every shelf; there are only a few.
  bool changed = false;
  for (size_t s = 0; s < shelves_.size() && s < shelf_rows_.size(); s++) {
    for (size_t c = 0; c < shelves_[s].entries.size() && c < shelf_rows_[s].cards.size(); c++) {
      CardRow& card = shelf_rows_[s].cards[c];
      const std::string& uri = shelves_[s].entries[c].image;
      if (uri.empty() || card.art != "none")
        continue;
      const std::string path = ImagePathFor(uri, true);
      if (!path.empty()) {
        card.art = CoverDecorator(path);
        changed = true;
      }
    }
  }
  if (changed)
    model_.DirtyVariable("shelves");
}
