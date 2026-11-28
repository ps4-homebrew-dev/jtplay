// SPDX-License-Identifier: GPL-3.0-or-later
//
// The folder picker: where something goes in Favorites, as a file dialog
// is anywhere else. Triangle on anything that is not a favorite yet opens
// it (on a favorite, triangle removes it), as does Move to... in the
// Favorites tab.
//
//   Save "Track 26" to favorites
//   Favorites > Music                       the folder on screen
//   ..                                      up (not at the top)
//   Jazz                           12  >    select goes in
//   Rock                            4  >
//                     [New folder] [Cancel] [OK]
//
// OK saves (or moves) into the folder on screen; Cancel, back at the top,
// triangle and the options button leave it be. Moving, it opens in the
// folder the thing is in; saving, in the one the last favorite went to,
// with the focus on OK: saving another there is a press.
// Down from the last row reaches the buttons, up from them the list; back
// in a folder goes up out of it.
#include <algorithm>

#include "app.h"
#include "app_internal.h"
#include "gamepad.h"
#include "icons.h"

using namespace appdetail;

namespace {

// The path above the rows: its last folders, the rest as an ellipsis.
constexpr size_t kSheetPathFolders = 4;

} // namespace

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

// Triangle, and Add to / Remove from favorites: a favorite already is
// removed; anything else is saved, into the folder the picker is left in.
void App::ToggleFavorite(const browse::Entry& entry) {
  // A folder of favorites: deleted, as removing it from them is (it asks
  // first when there is something in it).
  std::string folder;
  if (current_source_ == favorites_source_ &&
      library::FavoriteFolderFromEntryId(entry.id, folder)) {
    ConfirmDeleteFavoriteFolder(folder);
    return;
  }
  library::Ref ref;
  if (!Identify(entry, ref))
    return;
  // Under a source that is offline, the name is whatever the library knew.
  if (ref.source_name.empty())
    ref.source_name = current_source_ ? current_source_->Name() : std::string();
  if (library_.IsFavorite(ref.source_key, ref.entry.id)) {
    library_.ToggleFavorite(ref);
    SaveLibrary();
    ShowToast("Removed from favorites", icon::kStar);
    return;
  }
  OpenSaveSheet(ref, DisplayName(entry));
}

void App::ToggleSourceFavorite(const browse::SourcePtr& source) {
  const library::Ref ref = SourceRef(source);
  if (library_.IsFavorite(ref.source_key, ref.entry.id)) {
    library_.ToggleFavorite(ref);
    SaveLibrary();
    ShowToast("Removed from favorites", icon::kStar);
    return;
  }
  OpenSaveSheet(ref, source->Name());
}

void App::OpenSaveSheet(const library::Ref& ref, const std::string& name) {
  sheet_folder_mode_ = false;
  sheet_folder_.clear();
  sheet_ref_ = ref;
  sheet_was_favorite_ = library_.IsFavorite(ref.source_key, ref.entry.id);
  sheet_current_ = sheet_was_favorite_
    ? library_.FavoriteFolderOf(ref.source_key, ref.entry.id) : std::string();
  Set("sheet_title", bind_sheet_title_, Rml::String(
    (sheet_was_favorite_ ? "Move “" : "Save “") + name +
    (sheet_was_favorite_ ? "” to" : "” to favorites")));
  const std::vector<std::string> recent = library_.RecentFavoriteFolders();
  SheetShow(sheet_was_favorite_ ? sheet_current_
            : !recent.empty() ? recent.front() : std::string());
  sel_sheet_button_ = kSheetOk;
  Set("sheet_on_buttons", bind_sheet_on_buttons_, true);
  model_.DirtyVariable("sel_sheet_button");
  Set("sheet_open", bind_sheet_open_, true);
}

void App::OpenFolderMoveSheet(const std::string& folder) {
  library::FavoriteFolder f;
  if (!library_.FindFavoriteFolder(folder, f))
    return;
  sheet_folder_mode_ = true;
  sheet_folder_ = folder;
  sheet_ref_ = library::Ref();
  sheet_was_favorite_ = true;
  sheet_current_ = f.parent;
  Set("sheet_title", bind_sheet_title_, Rml::String("Move “" + f.name + "” to"));
  SheetShow(f.parent);
  sel_sheet_button_ = kSheetOk;
  Set("sheet_on_buttons", bind_sheet_on_buttons_, true);
  model_.DirtyVariable("sel_sheet_button");
  Set("sheet_open", bind_sheet_open_, true);
}

void App::CloseSheet() {
  Set("sheet_open", bind_sheet_open_, false);
  sheet_lines_.clear();
  sheet_rows_.clear();
  model_.DirtyVariable("sheet_rows");
}

// ---------------------------------------------------------------------------
// One folder
// ---------------------------------------------------------------------------

void App::SheetShow(const std::string& at_folder, const std::string& select) {
  const std::vector<library::FavoriteFolder> folders = library_.FavoriteFolders();
  const std::map<std::string, size_t> counts = library_.FavoriteFolderCounts();
  auto find = [&folders](const std::string& id) -> const library::FavoriteFolder* {
    for (const library::FavoriteFolder& f : folders)
      if (f.id == id)
        return &f;
    return nullptr;
  };
  // Whether 'id' is 'ancestor' or inside it.
  auto inside = [&](std::string id, const std::string& ancestor) {
    for (size_t steps = 0; !id.empty() && steps <= folders.size(); steps++) {
      if (id == ancestor)
        return true;
      const library::FavoriteFolder* f = find(id);
      id = f ? f->parent : std::string();
    }
    return false;
  };
  // (Never the folder moved, nor one inside it: a folder does not go into
  // itself. Nor one that has gone.)
  auto usable = [&](const std::string& id) {
    return id.empty() || (find(id) && !(sheet_folder_mode_ && inside(id, sheet_folder_)));
  };
  sheet_at_ = usable(at_folder) ? at_folder : std::string();

  // The path down to it, its start left out when it is long.
  std::vector<std::string> names;
  for (std::string at = sheet_at_; !at.empty() && names.size() <= folders.size();) {
    const library::FavoriteFolder* f = find(at);
    if (!f)
      break;
    names.push_back(f->name);
    at = f->parent;
  }
  names.push_back("Favorites");
  std::reverse(names.begin(), names.end());
  std::string path;
  const size_t from = names.size() > kSheetPathFolders ? names.size() - kSheetPathFolders : 0;
  if (from > 0)
    path = "…";
  for (size_t i = from; i < names.size(); i++)
    path += (path.empty() ? "" : "  ›  ") + names[i];
  Set("sheet_path", bind_sheet_path_, Rml::String(path));

  sheet_lines_.clear();
  sheet_rows_.clear();
  if (!sheet_at_.empty()) {
    sheet_lines_.push_back({SheetLine::Kind::Up, std::string()});
    SheetRow row;
    row.kind = "up";
    row.icon = icon::kFolder;
    row.label = "..";
    sheet_rows_.push_back(std::move(row));
  }
  for (const library::FavoriteFolder& f : folders) {
    if (f.parent != sheet_at_ || !usable(f.id))
      continue;
    sheet_lines_.push_back({SheetLine::Kind::Folder, f.id});
    SheetRow row;
    row.kind = "folder";
    row.icon = icon::kFavoriteFolder;
    row.label = f.name;
    const auto it = counts.find(f.id);
    if (it != counts.end() && it->second > 0)
      row.detail = std::to_string(it->second);
    // Where it is now, or the way there.
    row.current = sheet_was_favorite_ && inside(sheet_current_, f.id);
    sheet_rows_.push_back(std::move(row));
  }

  sel_sheet_row_ = 0;
  for (size_t i = 0; i < sheet_lines_.size(); i++) {
    if (!select.empty() && sheet_lines_[i].kind == SheetLine::Kind::Folder &&
        sheet_lines_[i].folder == select) {
      sel_sheet_row_ = (int)i;
      break;
    }
  }
  // Nothing to choose from: the buttons.
  if (sheet_lines_.empty())
    Set("sheet_on_buttons", bind_sheet_on_buttons_, true);
  model_.DirtyVariable("sheet_rows");
  model_.DirtyVariable("sel_sheet_row");
  scroll_sheet_pending_ = true;
}

void App::SheetUp() {
  library::FavoriteFolder f;
  const std::string from = sheet_at_;
  SheetShow(library_.FindFavoriteFolder(from, f) ? f.parent : std::string(), from);
  Set("sheet_on_buttons", bind_sheet_on_buttons_, false);
}

// ---------------------------------------------------------------------------
// Choosing
// ---------------------------------------------------------------------------

void App::SheetChoose(const std::string& folder) {
  library::FavoriteFolder f;
  const std::string name = folder.empty() ? std::string("Favorites")
    : library_.FindFavoriteFolder(folder, f) ? f.name : std::string();
  CloseSheet();
  if (sheet_was_favorite_ && folder == sheet_current_)
    return; // where it is already
  if (sheet_folder_mode_) {
    if (!library_.MoveFavoriteFolder(sheet_folder_, folder))
      return;
    ShowToast("Moved to " + name, icon::kFavoriteFolder);
  } else if (sheet_was_favorite_) {
    library_.MoveFavorite(sheet_ref_.source_key, sheet_ref_.entry.id, folder);
    ShowToast("Moved to " + name, icon::kFavoriteFolder);
  } else {
    library_.AddFavorite(sheet_ref_, folder);
    ShowToast("Saved to " + name, icon::kStar);
  }
  library_.NoteFavoriteFolderUsed(folder);
  SaveLibrary();
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

void App::HandleKeySheet(Rml::Event& event, int key) {
  event.StopPropagation(); // the dialog is modal
  const int n = (int)sheet_lines_.size();
  auto select_row = [this](int row) {
    sel_sheet_row_ = row;
    model_.DirtyVariable("sel_sheet_row");
    scroll_sheet_pending_ = true;
  };
  auto new_folder = [this]() {
    // Named over the dialog; made in the folder on screen, and gone into,
    // ready for OK.
    const std::string parent = sheet_at_;
    OpenTextEntry("New folder", std::string(), kFolderNameMaxLength, false,
                  [this, parent](const std::string& name) {
      const std::string id = library_.AddFavoriteFolder(parent, name);
      if (id.empty())
        return;
      SaveLibrary();
      SheetShow(id);
      sel_sheet_button_ = kSheetOk;
      Set("sheet_on_buttons", bind_sheet_on_buttons_, true);
      model_.DirtyVariable("sel_sheet_button");
    });
  };

  switch (key) {
  case Rml::Input::KI_ESCAPE:
  case gamepad::kKeyOptions:
  case Rml::Input::KI_T: // triangle again: never mind
  case Rml::Input::KI_TAB:
    CloseSheet();
    return;
  case Rml::Input::KI_BACK:
    // Up out of the folder on screen; at the top, Cancel.
    if (sheet_at_.empty())
      CloseSheet();
    else
      SheetUp();
    return;
  default:
    break;
  }

  if (bind_sheet_on_buttons_) {
    switch (key) {
    case Rml::Input::KI_LEFT:
    case Rml::Input::KI_RIGHT: {
      const int to = std::clamp(sel_sheet_button_ + (key == Rml::Input::KI_LEFT ? -1 : 1), 0,
                                kSheetButtons - 1);
      if (to != sel_sheet_button_) {
        sel_sheet_button_ = to;
        model_.DirtyVariable("sel_sheet_button");
      }
      break;
    }
    case Rml::Input::KI_UP:
      if (n > 0) {
        Set("sheet_on_buttons", bind_sheet_on_buttons_, false);
        select_row(n - 1);
      }
      break;
    case Rml::Input::KI_RETURN:
    case Rml::Input::KI_NUMPADENTER:
      switch (sel_sheet_button_) {
      case kSheetNewFolder: new_folder(); break;
      case kSheetCancel:    CloseSheet(); break;
      default:              SheetChoose(sheet_at_); break;
      }
      break;
    default:
      HandleTransportKey(key); // a remote's pause and stop reach what plays
      break;
    }
    return;
  }

  const SheetLine line = sheet_lines_[std::clamp(sel_sheet_row_, 0, std::max(0, n - 1))];
  switch (key) {
  case Rml::Input::KI_UP:
    if (sel_sheet_row_ > 0)
      select_row(sel_sheet_row_ - 1);
    break;
  case Rml::Input::KI_DOWN:
    if (sel_sheet_row_ + 1 < n) {
      select_row(sel_sheet_row_ + 1);
    } else {
      Set("sheet_on_buttons", bind_sheet_on_buttons_, true);
    }
    break;
  case Rml::Input::KI_PRIOR:
  case Rml::Input::KI_NEXT:
    select_row(std::clamp(sel_sheet_row_ + (key == Rml::Input::KI_PRIOR ? -kPageRows
                                                                        : kPageRows),
                          0, std::max(0, n - 1)));
    break;
  case Rml::Input::KI_RIGHT:
  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER:
    if (n == 0)
      break;
    if (line.kind == SheetLine::Kind::Up)
      SheetUp();
    else
      SheetShow(line.folder);
    break;
  case Rml::Input::KI_LEFT:
    if (!sheet_at_.empty())
      SheetUp();
    break;
  default:
    HandleTransportKey(key);
    break;
  }
}
