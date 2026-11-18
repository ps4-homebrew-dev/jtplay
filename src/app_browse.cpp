// SPDX-License-Identifier: GPL-3.0-or-later
//
// The browse view: the content tree of a source, as a list with an info
// panel or as a wall of artwork tiles. Everything here is protocol-
// agnostic; the per-protocol work lives behind browse::Source.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_set>

#include <RmlUi/Core/Elements/ElementFormControl.h>

#include "app.h"
#include "app_internal.h"
#include "gamepad.h"
#include "browse/js_source.h"
#include "browse/natural_sort.h"
#include "icons.h"

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

// ASCII lower-casing; the bytes of other characters are left alone, so those
// match in the case they were typed in.
std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
  }
  return s;
}

// The words of a filter, lower-cased.
std::vector<std::string> FilterWords(const std::string& filter) {
  std::vector<std::string> words;
  const std::string q = Lower(filter);
  for (size_t i = 0; i < q.size();) {
    while (i < q.size() && std::isspace((unsigned char)q[i]))
      i++;
    const size_t start = i;
    while (i < q.size() && !std::isspace((unsigned char)q[i]))
      i++;
    if (i > start)
      words.push_back(q.substr(start, i - start));
  }
  return words;
}

// Whether the name or the description of 'e' holds every one of 'words'
// (no words: everything matches).
bool MatchesFilter(const browse::Entry& e, const std::vector<std::string>& words) {
  if (words.empty())
    return true;
  const std::string text = Lower(e.name + "\n" + e.description);
  for (const std::string& word : words) {
    if (text.find(word) == std::string::npos)
      return false;
  }
  return true;
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
  CancelForegroundLaunch();
  RememberPlace(); // the one on screen, if any
  // From the list of sources: back from its top returns there, on it.
  from_sources_list_ = current_source_ && current_source_ == sources_source_;
  if (from_sources_list_)
    sources_list_key_ = source->Key();
  // Its top, without a view of its own, is shown as the list of sources.
  view_root_ = SourcesListView();
  current_source_ = source;
  path_.clear();
  sel_entry_ = 0;
  // Counted, for the order of the list of sources.
  library_.SourceUsed(source->Key());
  library_.Save();

  Set("source_name", bind_source_name_, current_source_->Name());
  view_ = View::Browse;
  Set("view", bind_view_, Rml::String("browse"));

  // Where it was left: the folders above it are named in the path at
  // once, and listed when back reaches them; the one it was left in is
  // listed now, on the entry it was left on. A place that is not there
  // any more says so, and back leads out of it as from any folder.
  std::vector<library::PlaceLevel> place = library_.Place(source->Key());
  if (place.empty() || place.front().id != source->RootId())
    place = {{source->RootId(), source->Name(), std::string()}};
  for (size_t i = 0; i + 1 < place.size(); i++) {
    BrowseLevel level;
    level.id = place[i].id;
    level.name = place[i].name;
    level.keep = place[i].selected;
    level.stale = true;
    path_.push_back(std::move(level));
  }

  RebuildEntryRows();
  RebuildCrumb();
  RebuildDetail();
  RequestBrowse(place.back().id, place.back().name, false, place.back().selected);
}

bool App::ResumeInPlace(const browse::Entry& card) {
  library::Ref ref;
  if (!IdentifyIn(card, continue_source_, ref))
    return false;
  std::string key;
  std::vector<library::PlaceLevel> place;
  if (!library_.ResumePlace(ref.source_key, ref.entry.id, key, place) || place.empty())
    return false;
  // Played from a folder of a source (opened from the list of sources),
  // or from a favorite folder (opened from its card at home).
  browse::SourcePtr source = key == favorites_source_->Key() ? favorites_source_
                                                             : registry_.Find(key);
  if (!source)
    return false;
  const bool library = LibraryView(source);
  if (!library && place.front().id != source->RootId())
    return false;

  CancelForegroundLaunch();
  AbandonBrowse();
  RememberPlace();
  // Back from its top leads where it led then: to the list of sources,
  // on this one; or home, to the favorite.
  from_sources_list_ = !library;
  if (!library) {
    sources_list_key_ = key;
    library_.SourceUsed(key);
  }
  home_return_id_ = library ? place.front().id : std::string();
  view_root_ = library ? default_view_ : SourcesListView();
  current_source_ = source;
  path_.clear();
  for (size_t i = 0; i + 1 < place.size(); i++) {
    BrowseLevel level;
    level.id = place[i].id;
    level.name = place[i].name;
    level.keep = place[i].selected;
    level.stale = true;
    path_.push_back(std::move(level));
  }
  sel_entry_ = 0;
  view_ = View::Browse;
  Set("view", bind_view_, Rml::String("browse"));
  Set("source_name", bind_source_name_,
      library ? (ref.source_name.empty() ? source->Name() : ref.source_name)
              : source->Name());
  RebuildEntryRows();
  RebuildCrumb();
  RebuildDetail();
  const library::PlaceLevel& last = place.back();
  RequestBrowse(last.id, last.name, false, last.selected);
  pending_resume_request_ = browse_request_;
  pending_resume_id_ = last.selected;
  pending_resume_card_ = card;
  return true;
}

void App::NoteSourceArt(const browse::SourcePtr& source, const browse::Listing& entries) {
  if (!source || LibraryView(source) || !source->Image().empty())
    return;
  // The first picture listed: a show's poster, an album's cover, a folder's
  // artwork. Taken once; the list of sources keeps it.
  for (const browse::Entry& e : entries) {
    if (e.image.empty())
      continue;
    if (library_.SetSourceImage(source->Key(), e.image))
      library_.Save();
    return;
  }
}

bool App::LibraryView(const browse::SourcePtr& source) const {
  return source && (source == favorites_source_ || source == continue_source_ ||
                    source == sources_source_);
}

std::vector<library::PlaceLevel> App::CurrentPlace() const {
  // The folders, not search results; down to the last one that was listed.
  std::vector<library::PlaceLevel> levels;
  for (size_t i = 0; i < path_.size(); i++) {
    const BrowseLevel& level = path_[i];
    if (!level.query.empty() || !level.error.empty())
      break;
    library::PlaceLevel place{level.id, level.name, level.keep};
    // (The one on top: where the cursor is now.)
    const int at = i + 1 == path_.size() ? sel_entry_ : level.selection;
    if (!level.stale && at >= 0 && at < (int)level.entries.size())
      place.selected = level.entries[at].id;
    levels.push_back(std::move(place));
  }
  return levels;
}

void App::RememberPlace() {
  if (view_ != View::Browse || !current_source_ || LibraryView(current_source_))
    return;
  std::vector<library::PlaceLevel> levels = CurrentPlace();
  // The top of it, on its first entry: nothing worth remembering.
  if (levels.size() == 1 && levels[0].selected.empty())
    levels.clear();
  library_.SetPlace(current_source_->Key(), levels);
}

// Lists 'id' on the worker. The result is pushed as a new level, or with
// 'replace' takes the place of the current one (a refresh), keeping the
// cursor on the same entry if it is still there.
void App::RequestBrowse(const std::string& id, const std::string& name,
                        bool replace, const std::string& keep_id) {
  if (!current_source_)
    return;

  const BrowseTicket ticket = NewBrowseRequest();
  const uint32_t request = ticket.id;
  const browse::SourcePtr source = current_source_;

  PostTask(Lane::Browse, [this, ticket, request, source, id, name, replace, keep_id] {
    browse::Listing result;
    std::string next, error;
    {
      net::CancelScope scope(ticket.cancel);
      if (!net::Cancelled()) // given up on while it waited its turn
        source->BrowsePage(id, std::string(), result, next, error);
    }

    Reply([this, request, id, name, replace, keep_id, result = std::move(result), next,
           error]() mutable {
      if (request != browse_request_)
        return; // superseded while it was in flight
      BrowseArrived();
      // Continue watching, back in its folder (ResumeInPlace): the item
      // resumes from there once it is listed, or as it is if it is not.
      std::string resume_id;
      browse::Entry resume_card;
      if (request == pending_resume_request_) {
        resume_id = std::move(pending_resume_id_);
        resume_card = std::move(pending_resume_card_);
        pending_resume_request_ = 0;
      }
      auto resume_anyway = [this, resume_id, resume_card] {
        if (resume_id.empty())
          return;
        const browse::SourcePtr here = current_source_;
        current_source_ = continue_source_; // what the card is listed under
        PlayEntry(resume_card, true);
        current_source_ = here;
      };
      if (!error.empty()) {
        resume_anyway();
        // A refresh that failed leaves the listing there was, and says so.
        if (replace && !path_.empty() && path_.back().error.empty()) {
          ShowToast(error, icon::kWarning);
          return;
        }
        // Anything else becomes the folder on screen: where it is, what
        // went wrong, and select to try again (see ActivateSelection). The
        // way back is the usual one.
        if (replace && !path_.empty())
          path_.pop_back();
        BrowseLevel failed;
        failed.id = id;
        failed.name = name;
        failed.error = error;
        sel_entry_ = 0;
        path_.push_back(std::move(failed));
        RebuildEntryRows();
        RebuildCrumb();
        RebuildDetail();
        return;
      }

      std::string keep = keep_id, filter;
      // Listed now for the first time since the source was reopened below
      // it (see OpenSource).
      const bool was_stale = replace && !path_.empty() && path_.back().stale;
      if (replace) {
        if (const browse::Entry* e = SelectedEntry())
          keep = e->id;
        if (!path_.empty()) {
          filter = path_.back().filter; // a refresh keeps the list narrowed
          path_.pop_back();
        }
      }

      NoteSourceArt(current_source_, result);
      BrowseLevel level;
      level.id = id;
      level.name = name;
      level.filter = filter;
      level.raw = std::move(result);
      SortLevel(level);
      level.next = next; // (after the sort: the cursor goes on an entry)
      for (size_t i = 0; i < level.entries.size() && !keep.empty(); i++) {
        if (level.entries[i].id == keep) {
          level.selection = (int)i;
          break;
        }
      }
      // A folder that holds one folder and nothing else (a server's
      // "Video" holding only "All video"): that one is opened in its place,
      // and back passes over it. Not while coming back up to it.
      const bool through = !replace && keep_id.empty() && level.raw.size() == 1 &&
                           level.raw[0].IsFolder() && level.filter.empty() &&
                           level.next.empty();
      const browse::Entry only = through ? level.raw[0] : browse::Entry();
      level.passthrough = through;
      // Back reached a folder above a reopened one, and it holds nothing
      // but that one: passed over, as it was on the way down.
      if (was_stale && level.raw.size() == 1 && level.raw[0].IsFolder() && level.next.empty()) {
        level.passthrough = true;
        path_.push_back(std::move(level));
        LeaveContainer();
        return;
      }
      sel_entry_ = level.selection;
      path_.push_back(std::move(level));

      RebuildEntryRows();
      RebuildCrumb();
      RebuildDetail();
      scroll_entries_pending_ = true;
      if (!resume_id.empty()) {
        const browse::Entry* e = SelectedEntry();
        if (e && e->id == resume_id) {
          const browse::Entry item = *e;
          PlayEntry(item, true);
        } else {
          resume_anyway();
        }
        return;
      }
      if (through)
        RequestBrowse(only.id, only.name, false);
    });
  });
}

void App::EnterContainer(const browse::Entry& entry) {
  CancelForegroundLaunch(); // the row it was started from is about to go away
  if (BrowseLevel* level = CurrentLevel())
    level->selection = sel_entry_;
  RequestBrowse(entry.id, entry.name, false);
}

void App::RefreshContainer() {
  const BrowseLevel* level = CurrentLevel();
  if (!level)
    return;
  CancelForegroundLaunch();
  if (OnSourcesList()) {
    // The list of sources: every group looked for again (and the list
    // made again as each is done).
    SearchAllSources();
    RefreshSourcesList();
    return;
  }
  if (!level->query.empty()) {
    // A list of results: search again rather than list a folder.
    const std::string query = level->query;
    RequestSearch(query);
    return;
  }
  RequestBrowse(level->id, level->name, true);
}

void App::LeaveContainer() {
  CancelForegroundLaunch(); // ... otherwise playback pops up over another folder
  AbandonBrowse(); // any browse that is still in flight
  // Past the folders that were only passed through.
  size_t depth = path_.empty() ? 0 : path_.size() - 1;
  while (depth > 0 && path_[depth - 1].passthrough)
    depth--;
  if (depth == 0) {
    // At the top of a source opened from the list of sources: back to
    // the list, on it. (Left as it is until then, so that it is
    // remembered as it was left.)
    if (from_sources_list_ && current_source_ != sources_source_) {
      OpenSourcesList(current_source_->Key());
      return;
    }
    // The list itself: up to its tab. Anything else: home, to the card
    // it was opened from.
    if (current_source_ == sources_source_) {
      SetBrowseTabs(true);
      return;
    }
    GoHome();
    return;
  }
  path_.resize(depth);
  BrowseLevel& level = path_.back();
  if (level.stale) {
    // Named only (the source was reopened below it): listed now.
    RebuildEntryRows();
    RebuildCrumb();
    RebuildDetail();
    RequestBrowse(level.id, level.name, true, level.keep);
    return;
  }
  sel_entry_ = level.selection;
  RebuildEntryRows();
  RebuildCrumb();
  RebuildDetail();
  scroll_entries_pending_ = true;
}

void App::RebuildEntryRows() {
  // The folder on screen is shown as it was set to be (or inherits).
  if (!path_.empty())
    ShowViewMode(LevelView(path_.size() - 1));
  entry_rows_.clear();
  if (const BrowseLevel* level = CurrentLevel()) {
    for (const browse::Entry& e : level->entries) {
      EntryRow row;
      row.icon = EntryIcon(e);
      row.kind = browse::TypeName(e.type);
      row.logo = ShowsLogo(e);
      row.name = DisplayName(e);
      row.meta = EntryMeta(e);
      row.folder = e.IsFolder();
      library::Ref ref;
      if (Identify(e, ref)) {
        row.playing = bind_watching_ && playing_identified_ &&
                      ref.source_key == playing_ref_.source_key &&
                      ref.entry.id == playing_ref_.entry.id;
        row.favorite = library_.IsFavorite(ref.source_key, ref.entry.id);
        library::Progress p;
        if (library_.GetProgress(ref.source_key, ref.entry.id, p)) {
          row.watched = p.watched;
          if (!p.watched)
            row.progress = ProgressWidth(p.position, p.duration);
        }
      }
      entry_rows_.push_back(std::move(row));
    }
  }
  item_count_ = (int)entry_rows_.size();
  Set("has_more", bind_has_more_, HasMore());
  if (bind_has_more_) {
    EntryRow row;
    row.icon = icon::kMore;
    row.kind = "more";
    row.more = true;
    entry_rows_.push_back(std::move(row));
    UpdateMoreRow();
  }
  entry_count_ = (int)entry_rows_.size();
  const BrowseLevel* shown = CurrentLevel();
  entry_total_ = shown ? (int)shown->raw.size() : 0;
  Set("filter_text", bind_filter_text_, shown ? shown->filter : std::string());
  Set("browse_error", bind_browse_error_, shown ? shown->error : std::string());
  sel_entry_ = std::clamp(sel_entry_, 0, std::max(0, entry_count_ - 1));
  entry_rows_dirty_ = true;
  SyncEntryWindow();
  model_.DirtyVariable("entry_count");
  model_.DirtyVariable("entry_total");
  model_.DirtyVariable("item_count");
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
  // The list of sources is the Sources tab's page: the tabs over it.
  Set("sources_tab", bind_sources_tab_, OnSourcesList());
  if (!bind_sources_tab_ && bind_browse_tabs_)
    SetBrowseTabs(false);
}

void App::RebuildDetail() {
  const browse::Entry* e = SelectedEntry();

  Set("detail_name", bind_detail_name_, e ? DisplayName(*e) : std::string());
  Set("detail_icon", bind_detail_icon_, Rml::String(e ? EntryIcon(*e) : ""));
  Set("detail_kind", bind_detail_kind_, Rml::String(e ? browse::TypeName(e->type) : ""));
  Set("detail_description", bind_detail_description_,
      e ? e->description : std::string());

  std::vector<PropRow> props;
  bool favorite = false;
  if (e) {
    auto add = [&props](const char* label, const std::string& value) {
      if (!value.empty())
        props.push_back({label, value});
    };
    // What it is goes without saying (its icon or artwork says it).
    add("Duration", e->duration > 0 ? FormatTime(e->duration * 1000000) : "");
    add("Size", FormatSize(e->size));
    add("Date", FormatDate(e->date, HasTimeOfDay(e->date)));

    library::Ref ref;
    if (Identify(*e, ref)) {
      library::Progress p;
      // The one in the player (behind the menus, or the music): what it
      // was left at is out of date while it plays on.
      if (bind_watching_ && playing_identified_ && ref.source_key == playing_ref_.source_key &&
          ref.entry.id == playing_ref_.entry.id) {
        add("Status", "Now playing");
      } else if (library_.GetProgress(ref.source_key, ref.entry.id, p)) {
        if (p.watched)
          add("Status", "Watched");
        else if (p.position > 0)
          add("Status", "Resume at " + FormatTime(p.position * 1000000));
      }
      // Under Favorites / Continue watching, where it actually lives.
      if (ref.source_key != (current_source_ ? current_source_->Key() : "") &&
          !ListedSourceOf(*e))
        add("Source", ref.source_name.empty() ? "Not available" : ref.source_name);
      favorite = library_.IsFavorite(ref.source_key, ref.entry.id);
    }
  }
  Set("detail_favorite", bind_detail_favorite_, favorite);
  bool same = props.size() == detail_props_.size();
  for (size_t i = 0; same && i < props.size(); i++)
    same = props[i].label == detail_props_[i].label &&
           props[i].value == detail_props_[i].value;
  if (!same) {
    detail_props_ = std::move(props);
    model_.DirtyVariable("detail_props");
  }

  // A folder: what is in it, once the focus has rested on it a moment.
  // Only where listing is quick and can run beside the browse itself:
  // drives and media servers (a plugin's script runs one call at a time,
  // so a preview would hold up opening the folder).
  preview_key_.clear();
  preview_at_ = 0.0;
  if (e && e->IsFolder() && current_source_ && view_ == View::Browse) {
    std::string key, id;
    if (!library::DecodeId(e->id, key, id))
      key = current_source_->Key();
    if (key.compare(0, 3, "fs:") == 0 || key.compare(0, 5, "dlna:") == 0) {
      preview_key_ = current_source_->Key() + '\n' + e->id;
      if (!previews_.count(preview_key_))
        preview_at_ = Now() + kPreviewDelaySec;
    }
  }
  ShowPreview();

  RefreshImageBindings();
}

void App::ShowPreview() {
  std::vector<ChildRow> children;
  Rml::String more;
  auto it = preview_key_.empty() ? previews_.end() : previews_.find(preview_key_);
  if (it != previews_.end() && it->second.count != size_t(-1)) {
    children = it->second.children;
    if (it->second.count == 0)
      more = "Empty";
    else if (it->second.count > children.size())
      more = "and " + std::to_string(it->second.count - children.size()) + " more";
  }
  bool same = children.size() == detail_children_.size();
  for (size_t i = 0; same && i < children.size(); i++)
    same = children[i].name == detail_children_[i].name;
  if (!same) {
    detail_children_ = std::move(children);
    model_.DirtyVariable("detail_children");
  }
  Set("detail_more", bind_detail_more_, more);
  Set("detail_preview", bind_detail_preview_, !detail_children_.empty() || !more.empty());
}

void App::RequestPreview() {
  preview_at_ = 0.0;
  const browse::Entry* e = SelectedEntry();
  if (!e || !e->IsFolder() || !current_source_ || preview_key_.empty() ||
      previews_.count(preview_key_))
    return;
  preview_running_ = true;
  const std::string key = preview_key_;
  const std::string id = e->id;
  const browse::SourcePtr source = current_source_;
  PostTask(Lane::Preview, [this, key, id, source] {
    browse::Listing out;
    std::string error;
    const bool ok = source->Browse(id, out, error);
    Reply([this, key, ok, out = std::move(out)] {
      preview_running_ = false;
      // What it holds, folders first as the list shows them; a folder that
      // cannot be listed shows nothing, and is not asked again.
      Preview preview;
      if (ok) {
        preview.count = out.size();
        for (int pass = 0; pass < 2; pass++)
          for (const browse::Entry& child : out)
            if (child.IsFolder() == (pass == 0) && preview.children.size() < kPreviewEntries)
              preview.children.push_back({IconFor(child), DisplayName(child)});
      }
      if (previews_.size() > kPreviewsKept)
        previews_.clear();
      if (ok)
        previews_[key] = std::move(preview);
      else
        previews_[key] = Preview{{}, size_t(-1)};
      if (key == preview_key_)
        ShowPreview();
      else if (!preview_key_.empty() && !previews_.count(preview_key_))
        preview_at_ = Now(); // the focus moved on meanwhile: that one next
    });
  }, false);
}

void App::MoveSelection(int delta) {
  if (entry_count_ == 0)
    return;
  const bool was_on_more = OnMoreRow();
  sel_entry_ = StepIndex(sel_entry_, delta, entry_count_);
  if (BrowseLevel* level = CurrentLevel())
    level->selection = sel_entry_;
  model_.DirtyVariable("sel_entry");
  RebuildDetail();
  scroll_entries_pending_ = true;
  // Down onto "Load more" loads more, so going down the list goes on into
  // the next page by itself. (Not up onto it, round from the top.)
  if (delta > 0 && !was_on_more && OnMoreRow())
    LoadMore();
}

bool App::HasMore() const {
  return !path_.empty() && !path_.back().next.empty() && path_.back().error.empty() &&
         !OnSourcesList();
}

bool App::OnMoreRow() const {
  return HasMore() && sel_entry_ >= (int)path_.back().entries.size();
}

void App::UpdateMoreRow() {
  if (!bind_has_more_ || entry_rows_.empty() || !entry_rows_.back().more)
    return;
  const bool loading = more_request_ != 0 && more_request_ == browse_request_ && browse_pending_;
  const Rml::String name = loading ? "Loading more\u2026" : "Load more";
  if (entry_rows_.back().name != name) {
    entry_rows_.back().name = name;
    entry_rows_dirty_ = true;
  }
}

void App::LoadMore() {
  BrowseLevel* level = CurrentLevel();
  if (!level || !HasMore() || !current_source_)
    return;
  if (more_request_ != 0 && more_request_ == browse_request_ && browse_pending_)
    return; // already on its way
  level->selection = sel_entry_;

  const BrowseTicket ticket = NewBrowseRequest();
  const uint32_t request = ticket.id;
  more_request_ = request;
  const browse::SourcePtr source = current_source_;
  const size_t depth = path_.size();
  const std::string id = level->id, cursor = level->next;
  const std::string query = level->query, folder = level->folder;
  UpdateMoreRow();

  PostTask(Lane::Browse, [this, ticket, request, source, depth, id, cursor, query, folder] {
    browse::Listing result;
    std::string next, error;
    {
      net::CancelScope scope(ticket.cancel);
      if (!net::Cancelled()) { // given up on while it waited its turn
        if (query.empty())
          source->BrowsePage(id, cursor, result, next, error);
        else if (!source->SearchPage(folder, query, cursor, result, next, error) &&
                 error.empty())
          error = "the search failed";
      }
    }

    Reply([this, request, depth, id, cursor, result = std::move(result), next,
           error]() mutable {
      if (request != browse_request_)
        return; // superseded while it was in flight
      BrowseArrived();
      more_request_ = 0;
      // Still the folder it was asked for, as it was then.
      if (path_.size() != depth || path_.back().id != id || path_.back().next != cursor) {
        UpdateMoreRow();
        return;
      }
      BrowseLevel& level = path_.back();
      if (!error.empty()) {
        // What there is stays, and "Load more" tries again.
        ShowToast(error, icon::kWarning);
        UpdateMoreRow();
        return;
      }

      // Where the cursor goes: on the first of the new entries if it was on
      // "Load more" (in the folder's own order, that is where the row was),
      // and otherwise where it is.
      const bool was_on_more = sel_entry_ >= (int)level.entries.size();
      std::string keep;
      if (!was_on_more && sel_entry_ >= 0)
        keep = level.entries[sel_entry_].id;

      // A page that overlaps the one before (a listing that shifted while
      // it was paged through) adds only what is new.
      std::unordered_set<std::string> seen;
      for (const browse::Entry& e : level.raw)
        seen.insert(e.id);
      size_t added = 0;
      for (browse::Entry& e : result) {
        if (!seen.insert(e.id).second)
          continue;
        if (keep.empty() && was_on_more)
          keep = e.id;
        level.raw.push_back(std::move(e));
        added++;
      }
      // A source that keeps handing back the same page has no more.
      level.next = added == 0 && next == cursor ? std::string() : next;
      NoteSourceArt(current_source_, level.raw);

      level.selection = -1;
      SortLevel(level);
      // Nothing new to go to (or all of it filtered out): "Load more"
      // again, or the last entry when that has gone.
      level.selection = was_on_more ? (int)level.entries.size() : 0;
      for (size_t i = 0; i < level.entries.size() && !keep.empty(); i++) {
        if (level.entries[i].id == keep) {
          level.selection = (int)i;
          break;
        }
      }
      if (level.next.empty())
        level.selection = std::min(level.selection, std::max(0, (int)level.entries.size() - 1));
      sel_entry_ = level.selection;
      RebuildEntryRows();
      RebuildDetail();
      scroll_entries_deferred_ = true;
      if (added == 0 && level.next.empty())
        ShowToast("That was all", icon::kMore);
    });
  });
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
  if (sel_entry_ >= n) // on "Load more": from the last entry
    sel_entry_ = n - 1;
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
  if (!e && OnMoreRow()) {
    LoadMore();
    return;
  }
  if (!e) {
    // Nothing listed because listing failed: select tries again.
    if (const BrowseLevel* level = CurrentLevel(); level && !level->error.empty())
      RefreshContainer();
    return;
  }
  if (const ListedSource* listed = ListedSourceOf(*e)) {
    // A source: opened as itself, where it was left (see OpenSource).
    path_.front().selection = sel_entry_;
    const ListedSource open = *listed; // the list may be made again
    sources_list_key_ = open.key;
    OpenKnownSource(open.key, open.name);
    return;
  }
  if (e->IsFolder())
    EnterContainer(*e);
  else
    PlayEntry(*e);
}

void App::HandleKeyBrowse(Rml::Event& event, int key) {
  if (bind_browse_tabs_ && OnSourcesList()) {
    HandleKeySourcesTabs(event, key);
    return;
  }
  const bool grid = view_mode_ == ViewMode::Grid;
  // The list of sources: up from its first row reaches the tabs over it.
  if (key == Rml::Input::KI_UP && OnSourcesList() &&
      sel_entry_ < (grid ? kGridColumns : 1)) {
    SetBrowseTabs(true);
    event.StopPropagation();
    return;
  }
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
  // Triangle: pins the item (or folder) in focus to Home's Favorites, or
  // unpins it. (Reversing the order is in the options menu.)
  case Rml::Input::KI_T:
  case Rml::Input::KI_TAB:
    if (const browse::Entry* e = SelectedEntry()) {
      if (const ListedSource* listed = ListedSourceOf(*e)) {
        if (const browse::SourcePtr source = listed->source)
          ToggleSourceFavorite(source);
        else
          ShowToast("Not found right now", icon::kWarning);
      } else {
        ToggleFavorite(*e);
      }
    }
    break;
  case gamepad::kKeyOptions: OpenMenu(); break;
  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER:
    ActivateSelection();
    break;
  case Rml::Input::KI_BACK:
  case Rml::Input::KI_ESCAPE:
    // While something is opening, back cancels that rather than the folder
    // (not the next item of what plays behind the menus).
    if (launch_request_ && !launch_behind_) {
      CancelLaunch();
    } else if (browse_pending_ && !path_.empty()) {
      // Likewise a folder that is still being opened (or listed again):
      // back gives up on that, and stays where it was asked from.
      AbandonBrowse();
      UpdateMoreRow();
    } else if (const BrowseLevel* level = CurrentLevel();
               level && !level->filter.empty()) {
      ClearFilter(); // back out of the filter before the folder
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
  // On "Load more", which stays at the end.
  const bool on_more = !level.next.empty() && level.selection >= (int)level.entries.size();

  // Sort indices into raw (so Default can use the source order as its key),
  // of the entries that pass the filter.
  const std::vector<std::string> words = FilterWords(level.filter);
  std::vector<size_t> order;
  order.reserve(level.raw.size());
  for (size_t i = 0; i < level.raw.size(); i++) {
    if (MatchesFilter(level.raw[i], words))
      order.push_back(i);
  }

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

  level.selection = on_more ? (int)level.entries.size() : 0;
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

// The order in words, for the list header: neither font has arrows, and
// "Date ▼" leaves the reader to work out which end is new.
std::string App::SortLabel() const {
  switch (sort_mode_) {
  case SortMode::Name: return sort_desc_ ? "Name, Z–A" : "Name, A–Z";
  case SortMode::Date: return sort_desc_ ? "Newest first" : "Oldest first";
  case SortMode::Size: return sort_desc_ ? "Largest first" : "Smallest first";
  default:             return sort_desc_ ? "Reversed" : "Default order";
  }
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
// Options menu (the options button: gamepad::kKeyOptions)
// ---------------------------------------------------------------------------

// The actions for one entry, Kodi's context menu style: shared by the
// browse list and the home shelves.
void App::AddEntryActions(const browse::Entry& entry) {
  library::Ref ref;
  if (!Identify(entry, ref))
    return;
  if (entry.IsImage()) {
    AddMenuItem({"Slideshow from here", nullptr, [this, entry](int) {
      int index = 0;
      const browse::Listing photos = FolderPhotos(entry, index);
      OpenPhotos(photos, index, true);
    }});
  } else if (!entry.IsFolder()) {
    library::Progress p;
    const bool known = library_.GetProgress(ref.source_key, ref.entry.id, p);
    if (known && !p.watched && p.position > 0) {
      const int64_t at = p.position;
      AddMenuItem({"Resume from " + FormatTime(at * 1000000), nullptr,
        [this, entry, at](int) { Launch(entry, at); }});
      AddMenuItem({"Play from beginning", nullptr,
        [this, entry](int) { Launch(entry, 0); }});
    }
    // (Anything else plays with select: no entry for that.)
  }
  const bool fav = library_.IsFavorite(ref.source_key, ref.entry.id);
  AddMenuItem({fav ? "Remove from favorites" : "Add to favorites",
    nullptr, [this, entry](int) { ToggleFavorite(entry); }});
  if (entry.IsVideo() || entry.IsAudio()) {
    library::Progress p;
    const bool known = library_.GetProgress(ref.source_key, ref.entry.id, p);
    const bool watched = known && p.watched;
    AddMenuItem({watched ? "Mark as unwatched" : "Mark as watched",
      nullptr, [this, entry, watched](int) { SetWatched(entry, !watched); }});
  }
}

void App::MenuSection(const std::string& title) {
  menu_section_ = title;
}

void App::AddMenuItem(MenuItem item) {
  item.section = std::move(menu_section_);
  menu_section_.clear();
  menu_items_.push_back(std::move(item));
}

void App::ShowMenu(int selection) {
  menu_section_.clear();
  sel_menu_ = std::clamp(selection, 0, std::max(0, (int)menu_items_.size() - 1));
  RebuildMenuRows();
  Set("menu_open", bind_menu_open_, true);
}

void App::OpenMenu() {
  menu_items_.clear();
  menu_parent_ = nullptr;
  plugin_menu_open_ = false;
  menu_section_.clear();
  Set("menu_dialog", bind_menu_dialog_, false);
  Set("menu_title", bind_menu_title_, Rml::String("Options"));

  if (view_ == View::Browse && bind_browse_tabs_ && OnSourcesList()) {
    // Up on the Sources tab: what is done to all of them, and how they
    // are shown.
    Set("menu_title", bind_menu_title_, Rml::String("Sources"));
    AddMenuItem({"Search", nullptr, [this](int) { OpenSearch(); }});
    AddMenuItem({"Refresh", nullptr, [this](int) { RefreshContainer(); }});
    AddMenuItem({"View",
      [this] { return std::string(view_mode_ == ViewMode::Grid ? "Grid" : "List"); },
      [this](int) {
        SetViewMode(view_mode_ == ViewMode::Grid ? ViewMode::List : ViewMode::Grid);
      }});
  } else if (view_ == View::Browse) {
    // The item in focus first: it is what the menu was most likely opened
    // for, and its name is the menu's title.
    const browse::Entry* e = SelectedEntry();
    const ListedSource* listed = e ? ListedSourceOf(*e) : nullptr;
    if (listed) {
      // A source, on the list of them: one that is found can be a
      // favorite, one that is not (any more) can be forgotten.
      const ListedSource source = *listed;
      Set("menu_title", bind_menu_title_, Rml::String(source.name));
      if (source.source) {
        const bool fav = library_.IsFavorite(source.key, source.source->RootId());
        AddMenuItem({fav ? "Remove from favorites" : "Add to favorites", nullptr,
          [this, s = source.source](int) {
            ToggleSourceFavorite(s);
            RebuildEntryRows();
          }});
      } else {
        AddMenuItem({"Forget this source", nullptr, [this, key = source.key](int) {
          library_.ForgetSource(key);
          library_.Save();
          RefreshSourcesList();
        }});
      }
    } else if (e) {
      Set("menu_title", bind_menu_title_, DisplayName(*e));
      AddEntryActions(*e);
    }

    // (The list of sources: what is done to the lot of them.)
    const bool everything = current_source_ == sources_source_ && SearchScope() == 0;
    MenuSection(everything ? "Sources" : "Folder");
    // A folder of music or episodes plays as a whole, in order or shuffled.
    if (FolderPlayableCount() > 1) {
      AddMenuItem({"Play all", nullptr, [this](int) { PlayFolder(false); }});
      AddMenuItem({"Shuffle all", nullptr, [this](int) { PlayFolder(true); }});
    }
    // A folder with pictures in it can be shown as a slideshow from the
    // start, whatever is selected.
    if (const BrowseLevel* level = CurrentLevel()) {
      browse::Listing photos;
      for (const browse::Entry& item : level->entries)
        if (item.IsImage())
          photos.push_back(item);
      if (photos.size() > 1 && !(e && e->IsImage())) {
        AddMenuItem({"Slideshow (" + std::to_string(photos.size()) + " pictures)",
          nullptr, [this, photos](int) { OpenPhotos(photos, 0, true); }});
      }
    }
    if (HasMore())
      AddMenuItem({"Load more", nullptr, [this](int) { LoadMore(); }});
    AddMenuItem({"Refresh", nullptr, [this](int) { RefreshContainer(); }});

    // Search asks the source for more, and is only there for sources that
    // can; Filter narrows what is listed.
    MenuSection("Find");
    if (current_source_ && !path_.empty() &&
        current_source_->CanSearch(path_[SearchScope()].id))
      AddMenuItem({everything ? "Search all sources" : "Search", nullptr,
                   [this](int) { OpenSearch(); }});
    AddMenuItem({"Filter", nullptr, [this](int) { OpenFilter(); }});
    if (const BrowseLevel* level = CurrentLevel(); level && !level->filter.empty())
      AddMenuItem({"Clear filter", nullptr, [this](int) { ClearFilter(); }});

    MenuSection("View");
    AddMenuItem({"View",
      [this] { return std::string(view_mode_ == ViewMode::Grid ? "Grid" : "List"); },
      [this](int) {
        SetViewMode(view_mode_ == ViewMode::Grid ? ViewMode::List : ViewMode::Grid);
      }});
    AddMenuItem({"Sort by",
      [this] { return std::string(SortModeName((int)sort_mode_)); },
      [this](int d) { CycleSortMode(d == 0 ? +1 : d); }});
    AddMenuItem({"Order",
      [this] { return std::string(sort_desc_ ? "Descending" : "Ascending"); },
      [this](int) { SetSort(sort_mode_, !sort_desc_); }});

    // The plugin this is from: of the source, or in Favorites and Continue
    // watching, of the item in focus.
    library::Ref ref;
    if (listed)
      AddPluginSettingsEntry(listed->key);
    else if (e && Identify(*e, ref))
      AddPluginSettingsEntry(ref.source_key);
    else if (current_source_)
      AddPluginSettingsEntry(current_source_->Key());
  } else if (bind_home_shelves_ && NowPlayingShelf(sel_shelf_)) {
    Set("menu_title", bind_menu_title_, bind_watch_name_); // back to it, or stop
  } else if (bind_home_shelves_ && SelectedCard()) {
    const browse::Entry card = *SelectedCard();
    Set("menu_title", bind_menu_title_, DisplayName(card));
    AddEntryActions(card);
    if (shelves_[sel_shelf_].source == continue_source_)
      AddMenuItem({"Remove from Continue watching", nullptr,
        [this, card](int) { SetWatched(card, false); }});
    library::Ref ref;
    if (Identify(card, ref))
      AddPluginSettingsEntry(ref.source_key);
  } else if (view_ == View::Home && !bind_settings_page_ && !bind_home_shelves_) {
    MenuSection("Sources");
    AddMenuItem({"Refresh", nullptr, [this](int) { SearchAllSources(); }});
  } else if (bind_watching_) {
    // The top level of the rail: only what plays.
    Set("menu_title", bind_menu_title_, bind_watch_name_);
  }

  // Something plays behind the menus: back to it, or an end to it. Last,
  // in every menu: the menu is for what is on screen first, and one step
  // up from its first row is its last.
  if (bind_watching_) {
    MenuSection("Now playing");
    AddMenuItem({bind_watch_audio_ ? "Show player" : "Back to full screen",
      nullptr, [this](int) { SetFullscreen(true); }});
    AddMenuItem({"Stop playback", nullptr, [this](int) { StopPlayback(); }});
  }

  if (menu_items_.empty())
    return; // nothing to offer on the top level of the rail
  ShowMenu();
}

// ---------------------------------------------------------------------------
// A plugin's settings, from the options menu
// ---------------------------------------------------------------------------

std::shared_ptr<browse::JsPlugin> App::PluginOf(const std::string& source_key) const {
  if (!js_provider_)
    return nullptr;
  // A plugin's sources are keyed "js:<provider name>/<source name>" (see
  // JsSource::Key); the longest name that fits is the plugin's.
  std::shared_ptr<browse::JsPlugin> found;
  for (const browse::JsPluginPtr& plugin : js_provider_->Plugins()) {
    const std::string prefix = "js:" + plugin->ProviderName() + "/";
    if (source_key.compare(0, prefix.size(), prefix) == 0 &&
        (!found || plugin->ProviderName().size() > found->ProviderName().size()))
      found = plugin;
  }
  return found;
}

void App::AddPluginSettingsEntry(const std::string& source_key) {
  const browse::JsPluginPtr plugin = PluginOf(source_key);
  if (!plugin || plugin->Settings().empty())
    return;
  MenuSection("Plugin");
  // (Select on an entry without a value closes the menu first: this opens
  // the next one in its place.)
  AddMenuItem({plugin->ProviderName() + " settings", nullptr, [this, plugin](int) {
    plugin_menu_open_ = true;
    plugin_menu_revision_ = plugin_settings_->Revision();
    OpenPluginMenu(plugin);
  }});
}

// The plugin's rows of the Settings page, as a menu: left / right change
// the one in focus, select steps it on, lists its choices or asks for its
// text. A list of choices takes the menu's place and gives it back.
void App::OpenPluginMenu(const std::shared_ptr<browse::JsPlugin>& plugin, int selection) {
  menu_items_.clear();
  menu_section_.clear();
  menu_parent_ = nullptr;
  std::vector<SettingItem> settings;
  AddPluginSettings(plugin, settings,
                    [this, plugin] { OpenPluginMenu(plugin, sel_plugin_setting_); });
  for (size_t i = 0; i < settings.size(); i++) {
    AddMenuItem({settings[i].label, settings[i].value,
      [this, change = settings[i].change, i](int d) {
        sel_plugin_setting_ = (int)i; // where to come back to
        change(d);
      }});
  }
  Set("menu_title", bind_menu_title_, Rml::String(plugin->ProviderName() + " settings"));
  Set("menu_meta", bind_menu_meta_, Rml::String());
  Set("menu_dialog", bind_menu_dialog_, false);
  ShowMenu(selection);
}

// The plugin's settings menu is done with. Over a folder, what is listed
// was made with the settings as they were: with others, it is listed again.
void App::ClosePluginMenu() {
  if (!plugin_menu_open_)
    return;
  plugin_menu_open_ = false;
  if (plugin_settings_->Revision() != plugin_menu_revision_ && view_ == View::Browse &&
      !path_.empty() && !bind_photo_ && !FullscreenPlayback())
    RefreshContainer();
}

void App::CloseMenu() {
  Set("menu_open", bind_menu_open_, false);
  Set("menu_meta", bind_menu_meta_, Rml::String());
  menu_items_.clear();
  menu_section_.clear();
  menu_parent_ = nullptr;
  // The info bar was held up for the menu: it stays a while yet.
  if (FullscreenPlayback())
    info_deadline_ = Now() + kWatchInfoSec;
}

void App::RebuildMenuRows() {
  scroll_menu_pending_ = true;
  menu_rows_.clear();
  for (const MenuItem& item : menu_items_)
    menu_rows_.push_back({item.section, item.label, item.value ? item.value() : ""});
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

  // Changing an entry may put another menu in this one's place (a list of
  // choices): the entry is gone by the time its function returns, so that
  // runs as a copy, and the rows are only made again for the same menu.
  auto change = [this](int d) {
    const std::function<void(int)> fn = menu_items_[sel_menu_].change;
    fn(d);
    if (bind_menu_open_)
      RebuildMenuRows();
  };

  switch (key) {
  case Rml::Input::KI_UP:
  case Rml::Input::KI_DOWN:
    sel_menu_ = StepIndex(sel_menu_, key == Rml::Input::KI_UP ? -1 : +1, count);
    model_.DirtyVariable("sel_menu");
    scroll_menu_pending_ = true;
    break;
  // A long menu (every country there is) pages and jumps as a list does.
  case Rml::Input::KI_PRIOR:
  case Rml::Input::KI_NEXT:
    sel_menu_ = StepIndex(sel_menu_, key == Rml::Input::KI_PRIOR ? -kPageRows : +kPageRows, count);
    model_.DirtyVariable("sel_menu");
    scroll_menu_pending_ = true;
    break;
  case Rml::Input::KI_HOME:
  case Rml::Input::KI_END:
    sel_menu_ = key == Rml::Input::KI_HOME ? 0 : count - 1;
    model_.DirtyVariable("sel_menu");
    scroll_menu_pending_ = true;
    break;
  case Rml::Input::KI_LEFT:
  case Rml::Input::KI_RIGHT:
    if (menu_items_[sel_menu_].value)
      change(key == Rml::Input::KI_LEFT ? -1 : +1);
    break;
  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER: {
    if (menu_items_[sel_menu_].value) {
      change(0);
    } else {
      // Close first: the action may replace the view the menu was for, or
      // open the next menu.
      const std::function<void(int)> action = menu_items_[sel_menu_].change;
      const std::function<void()> parent = menu_parent_;
      CloseMenu();
      action(0);
      // A choice made in a menu that was opened from another: back to
      // that one, unless the choice opened something itself.
      if (parent && !bind_menu_open_ && !bind_search_open_)
        parent();
    }
    break;
  }
  case Rml::Input::KI_BACK:
  case Rml::Input::KI_ESCAPE:
    // One menu back, where this one was opened from another.
    if (menu_parent_) {
      const std::function<void()> parent = menu_parent_;
      CloseMenu();
      parent();
      break;
    }
    ClosePluginMenu();
    CloseMenu();
    break;
  case gamepad::kKeyOptions:
    ClosePluginMenu();
    CloseMenu();
    break;
  default:
    HandleTransportKey(key); // a remote's pause and stop reach what plays
    break;
  }
}

// ---------------------------------------------------------------------------
// View modes and artwork
// ---------------------------------------------------------------------------

void App::SetViewMode(ViewMode mode) {
  BrowseLevel* level = CurrentLevel();
  if (!level || !current_source_)
    return;
  level->view = (int)mode;
  // Remembered for a folder, not for results of a search (nor a folder
  // that failed to list, whose id may be anything).
  if (level->query.empty() && level->error.empty()) {
    library_.SetView(current_source_->Key(), level->id, mode == ViewMode::Grid);
    library_.Save();
  }
  ShowViewMode(mode);
}

App::ViewMode App::LevelView(size_t index) {
  if (index >= path_.size())
    return view_root_;
  BrowseLevel& level = path_[index];
  if (level.view < 0) {
    bool grid = false;
    if (current_source_ && level.query.empty() &&
        library_.GetView(current_source_->Key(), level.id, grid))
      level.view = (int)(grid ? ViewMode::Grid : ViewMode::List);
    else
      level.view = (int)(index == 0 ? view_root_ : LevelView(index - 1));
  }
  return (ViewMode)level.view;
}

void App::ShowViewMode(ViewMode mode) {
  if (mode == view_mode_)
    return;
  view_mode_ = mode;
  Set("view_mode", bind_view_mode_, Rml::String(mode == ViewMode::Grid ? "grid" : "list"));
  scroll_entries_pending_ = true;
  art_scan_pending_ = true;
}

App::ViewMode App::SourcesListView() const {
  bool grid = false;
  if (library_.GetView(sources_source_->Key(), sources_source_->RootId(), grid))
    return grid ? ViewMode::Grid : ViewMode::List;
  return default_view_;
}

// Brings the rows the document has (window_rows_) in line with entry_rows_
// and the selection. Cheap when nothing moved: the window follows the
// selection only when it comes near the window's edge, and then jumps so
// that the selection is in its middle.
void App::SyncEntryWindow() {
  const int n = (int)entry_rows_.size();
  const bool grid = view_mode_ == ViewMode::Grid;
  const int unit = grid ? kGridColumns : 1;
  const int span = grid ? kWindowGridRows * kGridColumns : kWindowRows;
  const int margin = grid ? kWindowGridMargin * kGridColumns : kWindowMargin;
  const int sel = std::clamp(sel_entry_, 0, std::max(0, n - 1));

  const int last_now = std::min(n, window_first_ + span);
  const bool near_edge = (window_first_ > 0 && sel < window_first_ + margin) ||
                         (last_now < n && sel >= last_now - margin);
  if (entry_rows_dirty_ || near_edge || window_mode_ != (int)view_mode_ ||
      (int)window_rows_.size() != last_now - window_first_ || window_first_ >= std::max(n, 1)) {
    int first = 0;
    if (n > span) {
      first = (sel - span / 2) / unit * unit;
      const int last_start = (n - span + unit - 1) / unit * unit;
      first = std::clamp(first, 0, last_start);
    }
    const int last = std::min(n, first + span);
    window_first_ = first;
    window_mode_ = (int)view_mode_;
    entry_rows_dirty_ = false;
    window_rows_.assign(entry_rows_.begin() + first, entry_rows_.begin() + last);
    model_.DirtyVariable("entries");

    // What the rows outside take up. Grid rows are flex lines with a gap
    // between them, a spacer's line among them.
    auto height = [&](int rows) -> std::string {
      if (rows <= 0)
        return "0px"; // never empty: the style binding is applied even while hidden
      const float px = grid ? rows * kGridRowPitch - kGridGap : rows * kEntryRowPitch;
      return std::to_string((int)px) + "px";
    };
    const int rows_before = first / unit;
    const int rows_after = (n + unit - 1) / unit - (last + unit - 1) / unit;
    Set("spacer_top", bind_spacer_top_, Rml::String(height(rows_before)));
    Set("spacer_bottom", bind_spacer_bottom_, Rml::String(height(rows_after)));
  }
  Set("sel_window", bind_sel_window_, sel - window_first_);
}

void App::UpdateVisibleArt() {
  if (view_ == View::Browse) {
    const BrowseLevel* level = CurrentLevel();
    if (!level || item_count_ != (int)level->entries.size() ||
        entry_rows_.size() < level->entries.size())
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
    // Rows well away from the selection give their artwork back, so a long
    // scroll through a big library does not keep every cover it passed as
    // a texture. Coming back finds them in the cache on disk.
    const int keep = view_mode_ == ViewMode::Grid ? 4 * kGridColumns : 16;
    for (int i = 0; i < (int)entry_rows_.size(); i++) {
      if ((i < first - keep || i >= last + keep) && entry_rows_[i].art != "none") {
        entry_rows_[i].art = "none";
        changed = true;
      }
    }
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
        row.art = row.logo ? LogoDecorator(path) : CoverDecorator(path);
        if (row.logo)
          if (const std::string edge = EdgeColorOf(path); !edge.empty())
            row.bg = edge;
        changed = true;
      }
    }
    if (changed)
      entry_rows_dirty_ = true;
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

// ---------------------------------------------------------------------------
// Filter
// ---------------------------------------------------------------------------

namespace {

// What was typed into a field: without the newline a Return also arrives as
// (see RmlUi_Platform_SDL.cpp), and without spaces at either end.
std::string CleanFieldText(const std::string& raw) {
  std::string text;
  for (char c : raw) {
    if (c != '\n' && c != '\r')
      text += c;
  }
  const size_t first = text.find_first_not_of(" \t");
  if (first == std::string::npos)
    return {};
  return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

} // namespace

void App::OpenFilter() {
  if (view_ != View::Browse || path_.empty() || FullscreenPlayback() || bind_photo_)
    return;
  CancelForegroundLaunch(); // the list is about to change under it
  // The bar is only hidden between uses (data-if), so the field still holds
  // whatever it held last: put this folder's filter back in, to be edited.
  if (Rml::Element* input = document_->GetElementById("filter-input"))
    static_cast<Rml::ElementFormControl*>(input)->SetValue(path_.back().filter);
  filter_focus_pending_ = true;
  Set("filter_open", bind_filter_open_, true);
}

void App::CloseFilter() {
  filter_focus_pending_ = false;
  // Losing the focus is what takes the system keyboard down again.
  if (Rml::Element* input = document_->GetElementById("filter-input"))
    input->Blur();
  Set("filter_open", bind_filter_open_, false);
}

void App::ClearFilter() {
  if (Rml::Element* input = document_->GetElementById("filter-input"))
    static_cast<Rml::ElementFormControl*>(input)->SetValue("");
  ApplyFilter(std::string());
}

// Narrows (or widens) the list on screen to 'text', keeping the selection on
// the same entry when it is still there.
void App::ApplyFilter(const std::string& text) {
  BrowseLevel* level = CurrentLevel();
  if (!level || level->filter == text)
    return;
  CancelForegroundLaunch(); // the entry it was started from may be about to go
  level->selection = sel_entry_;
  level->filter = text;
  SortLevel(*level);
  sel_entry_ = level->selection;
  RebuildEntryRows();
  RebuildDetail();
  scroll_entries_pending_ = true;
}

std::string App::FilterFieldText() const {
  Rml::Element* element = document_ ? document_->GetElementById("filter-input") : nullptr;
  if (!element)
    return {};
  // The field is an <input>, which is a form control.
  return CleanFieldText(static_cast<Rml::ElementFormControl*>(element)->GetValue());
}

void App::HandleKeyFilter(Rml::Event& event, int key) {
  switch (key) {
  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER:
    event.StopPropagation();
    if (FilterFieldText().empty()) {
      // Nothing typed, or the system keyboard was dismissed: ask for it again.
      if (Rml::Element* input = document_->GetElementById("filter-input")) {
        input->Blur();
        input->Focus();
      }
    } else {
      CloseFilter(); // the list stays narrowed
    }
    return;
  case Rml::Input::KI_ESCAPE:
    event.StopPropagation();
    ClearFilter();
    CloseFilter();
    return;
  case Rml::Input::KI_BACK:
    // Backspace belongs to the field. With nothing in it, back (a
    // controller's circle button too) closes the bar.
    if (FilterFieldText().empty()) {
      event.StopPropagation();
      CloseFilter();
    }
    return;
  // The list stays in reach while typing; left, right, home and end are
  // the field's.
  case Rml::Input::KI_UP:
    event.StopPropagation();
    MoveSelection(view_mode_ == ViewMode::Grid ? -kGridColumns : -1);
    return;
  case Rml::Input::KI_DOWN:
    event.StopPropagation();
    MoveSelection(view_mode_ == ViewMode::Grid ? +kGridColumns : +1);
    return;
  case Rml::Input::KI_PRIOR:
    event.StopPropagation();
    MoveSelection(-kPageRows);
    return;
  case Rml::Input::KI_NEXT:
    event.StopPropagation();
    MoveSelection(+kPageRows);
    return;
  default:
    if (HandleTransportKey(key))
      event.StopPropagation();
    return; // typing and moving the caret: the field's business
  }
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

size_t App::SearchScope() const {
  if (path_.empty())
    return 0;
  const size_t top = path_.size() - 1;
  return (top > 0 && !path_[top].query.empty()) ? top - 1 : top;
}

void App::OpenSearch() {
  if (view_ != View::Browse || !current_source_ || path_.empty() ||
      FullscreenPlayback() || bind_photo_)
    return;
  if (!current_source_->CanSearch(path_[SearchScope()].id)) {
    ShowToast("This source cannot search", icon::kSearch);
    return;
  }
  // "Search Podcasts"; under Favorites it is the favorite's own source that
  // is asked, whose name is not at hand.
  const bool library = LibraryView(current_source_);
  const bool everything = current_source_ == sources_source_ && SearchScope() == 0;
  Set("search_title", bind_search_title_,
      everything ? std::string("Search all sources")
      : library  ? std::string("Search") : "Search " + current_source_->Name());
  // The field is only hidden between searches (data-if), so it still holds
  // the last query.
  if (Rml::Element* input = document_->GetElementById("search-input"))
    static_cast<Rml::ElementFormControl*>(input)->SetValue("");
  search_focus_pending_ = true;
  Set("search_open", bind_search_open_, true);
}

void App::OpenTextEntry(const std::string& title, const std::string& initial,
                        size_t max_length, bool secret,
                        std::function<void(const std::string&)> done) {
  text_entry_ = std::move(done);
  Set("search_title", bind_search_title_, Rml::String(title));
  if (Rml::Element* input = document_->GetElementById("search-input")) {
    input->SetAttribute("maxlength", std::to_string(max_length));
    input->SetAttribute("type", secret ? "password" : "text");
    static_cast<Rml::ElementFormControl*>(input)->SetValue(initial);
  }
  search_focus_pending_ = true;
  Set("search_open", bind_search_open_, true);
}

void App::CloseSearch() {
  search_focus_pending_ = false;
  if (text_entry_) {
    text_entry_ = nullptr;
    if (Rml::Element* input = document_->GetElementById("search-input")) {
      input->SetAttribute("maxlength", std::to_string(kSearchMaxLength));
      input->SetAttribute("type", "text");
    }
  }
  // Losing the focus is what takes the system keyboard down again.
  if (Rml::Element* input = document_->GetElementById("search-input"))
    input->Blur();
  Set("search_open", bind_search_open_, false);
}

std::string App::SearchText() const {
  Rml::Element* element = document_ ? document_->GetElementById("search-input") : nullptr;
  if (!element)
    return {};
  // The field is an <input>, which is a form control.
  return CleanFieldText(static_cast<Rml::ElementFormControl*>(element)->GetValue());
}

void App::HandleKeySearch(Rml::Event& event, int key) {
  switch (key) {
  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER: {
    event.StopPropagation();
    const std::string text = SearchText();
    if (text.empty()) {
      // Nothing typed, or the system keyboard was dismissed: ask for it again.
      if (Rml::Element* input = document_->GetElementById("search-input")) {
        input->Blur();
        input->Focus();
      }
      return;
    }
    if (text_entry_) {
      const auto done = text_entry_;
      CloseSearch();
      done(text);
      return;
    }
    CloseSearch();
    RequestSearch(text);
    return;
  }
  case Rml::Input::KI_ESCAPE:
    event.StopPropagation();
    CloseSearch();
    return;
  case Rml::Input::KI_BACK:
    // Backspace belongs to the field. With nothing in it, back (a
    // controller's circle button too) closes the dialog.
    if (SearchText().empty()) {
      event.StopPropagation();
      CloseSearch();
    }
    return;
  default:
    if (HandleTransportKey(key))
      event.StopPropagation();
    return; // typing and moving the caret: the field's business
  }
}

// Asks the source's backend for 'query' (Source::Search, from the folder on
// screen). The hits become a level of their own above the folder, or replace
// the one on top when that is a list of hits, so back always returns to the
// folder.
void App::RequestSearch(const std::string& query) {
  if (!current_source_ || path_.empty())
    return;
  CancelForegroundLaunch();

  const size_t scope = SearchScope();
  const bool replace = scope + 1 < path_.size();
  if (!replace)
    path_[scope].selection = sel_entry_; // where back returns to
  const std::string folder = path_[scope].id;

  const BrowseTicket ticket = NewBrowseRequest();
  const uint32_t request = ticket.id;
  const browse::SourcePtr source = current_source_;

  PostTask(Lane::Browse, [this, ticket, request, source, folder, query, scope, replace] {
    browse::Listing result;
    std::string next, error;
    {
      net::CancelScope cancel(ticket.cancel);
      if (!net::Cancelled() &&
          !source->SearchPage(folder, query, std::string(), result, next, error) &&
          error.empty())
        error = "the search failed";
    }

    Reply([this, request, folder, query, scope, replace,
           result = std::move(result), next, error]() mutable {
      if (request != browse_request_ || scope >= path_.size())
        return; // superseded while it was in flight
      BrowseArrived();
      if (!error.empty()) {
        ShowToast(error, icon::kWarning);
        return;
      }
      if (result.empty()) {
        ShowToast("Nothing found for \"" + query + "\"", icon::kSearch);
        return;
      }

      if (replace)
        path_.pop_back();
      BrowseLevel level;
      // Never the id of a folder: the play order and the like are keyed on it.
      level.id = "search:" + folder + "\n" + query;
      level.name = "Search: " + query;
      level.query = query;
      level.folder = folder;
      level.raw = std::move(result);
      SortLevel(level);
      level.next = next;
      sel_entry_ = level.selection;
      path_.push_back(std::move(level));

      RebuildEntryRows();
      RebuildCrumb();
      RebuildDetail();
      scroll_entries_pending_ = true;
    });
  });
}
