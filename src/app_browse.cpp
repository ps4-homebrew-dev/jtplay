// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sources view (results from all browse::Providers) and the browse
// view (the content tree of the chosen source). Everything here is
// protocol-agnostic; the per-protocol work lives behind browse::Source.
#include <algorithm>

#include "app.h"
#include "app_internal.h"

using namespace appdetail;

namespace {

// A short glyph for the entry list; the emoji font is loaded as a fallback
// face, so these render everywhere.
Rml::String IconFor(const browse::Entry& e) {
  switch (e.type) {
  case browse::Entry::Type::Folder: return "📁";
  case browse::Entry::Type::Audio:  return "🎵";
  case browse::Entry::Type::Video:  return "🎬";
  case browse::Entry::Type::Image:  return "🖼";
  default:                          return "📄";
  }
}

} // namespace

// ---------------------------------------------------------------------------
// Sources view
// ---------------------------------------------------------------------------

void App::StartDiscovery() {
  Set("status", bind_status_, Rml::String("Searching for media sources..."));

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
      sources_ = std::move(sources);
      RebuildSourceRows();
      Rml::String status;
      if (sources_.empty()) {
        status = errors.empty() ? "No media sources found." : errors;
      } else {
        status = std::to_string(sources_.size()) +
          (sources_.size() == 1 ? " source found" : " sources found");
        if (!errors.empty())
          status += "  -  " + errors;
      }
      Set("status", bind_status_, status);
    });
    busy_ops_--;
  });
}

void App::RebuildSourceRows() {
  source_rows_.clear();
  for (const browse::SourcePtr& s : sources_) {
    SourceRow row;
    row.icon = s->Icon();
    row.name = s->Name();
    row.detail = s->Detail();
    source_rows_.push_back(std::move(row));
  }
  source_count_ = (int)sources_.size();
  sel_source_ = std::clamp(sel_source_, 0, std::max(0, source_count_ - 1));
  model_.DirtyVariable("sources");
  model_.DirtyVariable("source_count");
  model_.DirtyVariable("sel_source");
  scroll_sources_pending_ = true;
}

void App::HandleKeySources(Rml::Event& event, int key) {
  switch (key) {
  case Rml::Input::KI_UP:
    sel_source_ = std::max(0, sel_source_ - 1);
    break;
  case Rml::Input::KI_DOWN:
    sel_source_ = std::min(std::max(0, source_count_ - 1), sel_source_ + 1);
    break;
  case Rml::Input::KI_SPACE:
    StartDiscovery();
    event.StopPropagation();
    return;
  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER:
    if (sel_source_ < (int)sources_.size())
      OpenSource(sel_source_);
    event.StopPropagation();
    return;
  default:
    return;
  }
  model_.DirtyVariable("sel_source");
  scroll_sources_pending_ = true;
  event.StopPropagation();
}

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

void App::OpenSource(int index) {
  if (index < 0 || index >= (int)sources_.size())
    return;
  CancelLaunch();
  current_source_ = sources_[index];
  path_.clear();
  sel_entry_ = 0;

  Set("source_name", bind_source_name_, current_source_->Name());
  view_ = View::Browse;
  Set("view", bind_view_, Rml::String("browse"));

  RebuildEntryRows();
  RebuildCrumb();
  RebuildDetail();
  RequestBrowse(current_source_->RootId(), current_source_->Name());
}

void App::RequestBrowse(const std::string& id, const std::string& name) {
  if (!current_source_)
    return;

  const uint32_t request = ++browse_request_;
  const browse::SourcePtr source = current_source_;

  busy_ops_++;
  PostTask([this, request, source, id, name] {
    browse::Listing result;
    std::string error;
    source->Browse(id, result, error);

    Reply([this, request, id, name, result = std::move(result), error]() mutable {
      if (request != browse_request_)
        return; // superseded while it was in flight
      if (!error.empty()) {
        ShowToast(error);
        // A failed root browse drops back to the source list.
        if (path_.empty()) {
          view_ = View::Sources;
          Set("view", bind_view_, Rml::String("sources"));
        }
        return;
      }
      BrowseLevel level;
      level.id = id;
      level.name = name;
      level.entries = std::move(result);
      path_.push_back(std::move(level));
      sel_entry_ = 0;
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
  RequestBrowse(entry.id, entry.name);
}

void App::LeaveContainer() {
  CancelLaunch();    // ... otherwise playback pops up over another folder
  browse_request_++; // invalidate any browse that is still in flight
  if (path_.size() <= 1) {
    // At the root: back to the source list.
    path_.clear();
    current_source_.reset();
    view_ = View::Sources;
    Set("view", bind_view_, Rml::String("sources"));
    scroll_sources_pending_ = true;
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
      row.name = e.name.empty() ? "(untitled)" : e.name;
      row.description = e.description;
      row.folder = e.IsFolder();
      entry_rows_.push_back(std::move(row));
    }
  }
  entry_count_ = (int)entry_rows_.size();
  sel_entry_ = std::clamp(sel_entry_, 0, std::max(0, entry_count_ - 1));
  model_.DirtyVariable("entries");
  model_.DirtyVariable("entry_count");
  model_.DirtyVariable("sel_entry");
}

void App::RebuildCrumb() {
  std::string crumb;
  for (const BrowseLevel& level : path_) {
    if (!crumb.empty())
      crumb += "  ›  ";
    crumb += level.name;
  }
  Set("crumb", bind_crumb_, crumb);
}

void App::RebuildDetail() {
  const browse::Entry* e = SelectedEntry();
  Set("detail_name", bind_detail_name_, e ? e->name : std::string());
  Set("detail_description", bind_detail_description_,
      e ? e->description : std::string());
  RefreshImageBindings();
}

void App::MoveSelection(int delta) {
  if (entry_count_ == 0)
    return;
  sel_entry_ = std::clamp(sel_entry_ + delta, 0, entry_count_ - 1);
  if (BrowseLevel* level = CurrentLevel())
    level->selection = sel_entry_;
  model_.DirtyVariable("sel_entry");
  RebuildDetail();
  scroll_entries_pending_ = true;
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
  switch (key) {
  case Rml::Input::KI_UP:
    MoveSelection(-1);
    break;
  case Rml::Input::KI_DOWN:
    MoveSelection(+1);
    break;
  case Rml::Input::KI_PRIOR: // page up / left shoulder
    MoveSelection(-10);
    break;
  case Rml::Input::KI_NEXT:  // page down / right shoulder
    MoveSelection(+10);
    break;
  case Rml::Input::KI_HOME:
    MoveSelection(-entry_count_);
    break;
  case Rml::Input::KI_END:
    MoveSelection(+entry_count_);
    break;
  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER:
    ActivateSelection();
    break;
  case Rml::Input::KI_BACK:   // backspace / circle
  case Rml::Input::KI_ESCAPE:
    // While something is opening, back cancels that rather than the folder.
    if (launch_request_) {
      CancelLaunch();
      ShowToast("Cancelled");
    } else {
      LeaveContainer();
    }
    break;
  default:
    return;
  }
  event.StopPropagation();
}
