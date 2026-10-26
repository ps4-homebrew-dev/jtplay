// SPDX-License-Identifier: GPL-3.0-or-later
//
// The photo viewer: pictures full screen, one at a time or as a slideshow.
//
// Two layers take turns showing the current picture. A new picture goes
// onto the hidden layer, which is then brought to the front; the layers'
// opacity transition makes that a cross-fade, and while the slideshow runs
// each picture slowly zooms and pans (one of a handful of Ken Burns moves,
// as an RCSS animation restarted per picture). The pictures themselves are
// the artwork cache's screen-sized copies: a camera original is never
// decoded on the UI thread or uploaded as a texture.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "app.h"
#include "app_internal.h"
#include "artcache.h"

using namespace appdetail;

namespace {

constexpr int kIntervals[] = {3, 5, 6, 8, 12, 20, 30};
constexpr int kIntervalCount = (int)(sizeof(kIntervals) / sizeof(kIntervals[0]));
// Distinct moves (keyframes kb0..kb4 in photo.rcss). An odd number, so a
// layer never gets the same one twice in a row: an animation property set
// to the value it already has would not start again.
constexpr int kKenBurnsMoves = 5;
constexpr double kPhotoInfoSec = 4.0;

const char* EffectName(int effect) {
  static const char* const kNames[] = {"Pan & zoom", "Cross-fade", "None"};
  return kNames[effect];
}

} // namespace

std::string App::PhotoUri(const browse::Entry& entry) {
  return entry.uri.empty() ? entry.image : entry.uri;
}

browse::Listing App::FolderPhotos(const browse::Entry& entry, int& index) const {
  browse::Listing photos;
  index = 0;
  if (view_ == View::Browse && !path_.empty()) {
    for (const browse::Entry& e : path_.back().entries) {
      if (!e.IsImage())
        continue;
      if (e.id == entry.id)
        index = (int)photos.size();
      photos.push_back(e);
    }
  }
  if (photos.empty())
    photos.push_back(entry);
  return photos;
}

void App::OpenPhotos(const browse::Listing& photos, int index, bool slideshow) {
  if (photos.empty())
    return;
  CancelLaunch();
  photo_list_ = photos;
  photo_rotation_.assign(photo_list_.size(), 0);
  for (int i = 0; i < 2; i++) {
    Set(i ? "photo_art1" : "photo_art0", bind_photo_art_[i], Rml::String("none"));
    Set(i ? "photo_anim1" : "photo_anim0", bind_photo_anim_[i], Rml::String("none"));
  }
  Set("photo", bind_photo_, true);
  Set("photo_fade", bind_photo_fade_, photo_effect_ != PhotoEffect::None);
  SetSlideshow(slideshow);
  ShowPhoto(std::clamp(index, 0, (int)photo_list_.size() - 1));
  ShowPhotoInfo(kPhotoInfoSec);
}

void App::ClosePhotos() {
  // Back in the folder, on the picture that was showing.
  if (view_ == View::Browse && !path_.empty() && photo_index_ < (int)photo_list_.size()) {
    const std::string& id = photo_list_[photo_index_].id;
    const browse::Listing& entries = path_.back().entries;
    for (size_t i = 0; i < entries.size(); i++) {
      if (entries[i].id == id) {
        sel_entry_ = (int)i;
        path_.back().selection = sel_entry_;
        model_.DirtyVariable("sel_entry");
        RebuildDetail();
        scroll_entries_pending_ = true;
        break;
      }
    }
  }
  Set("photo", bind_photo_, false);
  Set("photo_playing", bind_photo_playing_, false);
  Set("photo_loading", bind_photo_loading_, false);
  Set("photo_info", bind_photo_info_, false);
  if (bind_menu_open_)
    CloseMenu();
  photo_pending_ = false;
  photo_list_.clear();
  // Let the textures go.
  for (int i = 0; i < 2; i++)
    Set(i ? "photo_art1" : "photo_art0", bind_photo_art_[i], Rml::String("none"));
}

// Makes 'index' the current picture. It goes on screen as soon as its
// screen-sized copy is ready (at once if it is cached); until then the
// previous one stays up with a loading indicator, so stepping quickly
// through a folder never flashes black.
void App::ShowPhoto(int index) {
  const int n = (int)photo_list_.size();
  if (n == 0)
    return;
  photo_index_ = (index % n + n) % n;
  const browse::Entry& e = photo_list_[photo_index_];

  Set("photo_name", bind_photo_name_, DisplayName(e));
  std::string meta = std::to_string(photo_index_ + 1) + " / " + std::to_string(n);
  const std::string date = FormatDate(e.date, true);
  if (!date.empty())
    meta += "  ·  " + date;
  const std::string size = FormatSize(e.size);
  if (!size.empty())
    meta += "  ·  " + size;
  Set("photo_meta", bind_photo_meta_, meta);

  const int turns = photo_rotation_.empty() ? 0 : photo_rotation_[photo_index_];
  Set("photo_rotate", bind_photo_rotate_,
      turns ? Rml::String("rotate(" + std::to_string(turns * 90) + "deg)") : Rml::String("none"));
  Set("photo_sideways", bind_photo_sideways_, (turns & 1) != 0);

  photo_pending_ = true;
  Set("photo_loading", bind_photo_loading_, false);

  // The neighbours first: the art thread takes the newest request first,
  // so the picture asked for last -- this one -- comes before them.
  if (n > 1) {
    ImagePathFor(PhotoUri(photo_list_[(photo_index_ + 1) % n]));
    ImagePathFor(PhotoUri(photo_list_[(photo_index_ + n - 1) % n]));
  }
  UpdatePhotos(Now());
}

void App::StepPhoto(int direction) {
  const int n = (int)photo_list_.size();
  if (n == 0)
    return;
  int next = photo_index_ + direction;
  if (photo_shuffle_ && bind_photo_playing_ && n > 2 && direction > 0) {
    // Anything but the one showing.
    next = photo_index_ + 1 + std::rand() % (n - 1);
  }
  ShowPhoto(next);
}

// A quarter turn clockwise, remembered for as long as the viewer is open.
void App::RotatePhoto() {
  if (photo_index_ >= (int)photo_rotation_.size())
    return;
  photo_rotation_[photo_index_] = (photo_rotation_[photo_index_] + 1) % 4;
  ShowPhoto(photo_index_);
}

void App::SetSlideshow(bool playing) {
  Set("photo_playing", bind_photo_playing_, playing);
  photo_next_at_ = Now() + photo_interval_;
  if (!playing) {
    // Hold the picture where the move has got to rather than snapping
    // back: a paused slideshow is for looking at.
    for (int i = 0; i < 2; i++)
      Set(i ? "photo_anim1" : "photo_anim0", bind_photo_anim_[i], Rml::String("none"));
  }
}

void App::ShowPhotoInfo(double seconds) {
  photo_info_until_ = Now() + seconds;
  Set("photo_info", bind_photo_info_, true);
}

// Per frame while the viewer is up: puts the current picture on screen
// once it has loaded, and moves the slideshow on.
void App::UpdatePhotos(double now) {
  if (!bind_photo_)
    return;

  if (photo_pending_ && photo_index_ < (int)photo_list_.size()) {
    const std::string uri = PhotoUri(photo_list_[photo_index_]);
    std::string path;
    const artcache::Status status = uri.empty() ? artcache::Status::Failed
                                                : artcache::LookupScreen(uri, path);
    if (status == artcache::Status::Missing) {
      ImagePathFor(uri); // queues it (once)
      Set("photo_loading", bind_photo_loading_, true);
    } else {
      photo_pending_ = false;
      Set("photo_loading", bind_photo_loading_, false);
      if (status == artcache::Status::Failed) {
        ShowToast("Cannot show " + DisplayName(photo_list_[photo_index_]), "⚠");
      } else {
        const int back = 1 - bind_photo_front_;
        const char* art = back ? "photo_art1" : "photo_art0";
        const char* anim = back ? "photo_anim1" : "photo_anim0";
        // "contain": the whole picture, letterboxed, never cropped.
        Set(art, bind_photo_art_[back],
            Rml::String("image(\"" + path + "\" contain)"));
        if (bind_photo_playing_ && photo_effect_ == PhotoEffect::KenBurns) {
          // Longer than the picture is up, so it never comes to a stop.
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%ds linear-in-out kb%d",
                        photo_interval_ + 3, photo_moves_++ % kKenBurnsMoves);
          Set(anim, bind_photo_anim_[back], Rml::String(buf));
        } else {
          Set(anim, bind_photo_anim_[back], Rml::String("none"));
        }
        Set("photo_front", bind_photo_front_, back);
      }
      photo_next_at_ = now + photo_interval_;
    }
  }

  if (bind_photo_playing_ && !photo_pending_ && !bind_menu_open_ &&
      now >= photo_next_at_ && photo_list_.size() > 1)
    StepPhoto(+1);

  if (bind_photo_info_ && !bind_menu_open_ && now > photo_info_until_)
    Set("photo_info", bind_photo_info_, false);
}

void App::OpenPhotoMenu() {
  menu_items_.clear();
  Set("menu_title", bind_menu_title_,
      photo_index_ < (int)photo_list_.size() ? DisplayName(photo_list_[photo_index_])
                                             : Rml::String("Photos"));
  Set("menu_dialog", bind_menu_dialog_, false);

  if (photo_list_.size() > 1) {
    menu_items_.push_back({"Slideshow",
      [this] { return std::string(bind_photo_playing_ ? "Playing" : "Paused"); },
      [this](int) { SetSlideshow(!bind_photo_playing_); }});
    menu_items_.push_back({"Seconds per picture",
      [this] { return std::to_string(photo_interval_) + " s"; },
      [this](int d) {
        int i = 0;
        while (i < kIntervalCount - 1 && kIntervals[i] < photo_interval_)
          i++;
        i = d == 0 ? (i + 1) % kIntervalCount
                   : std::clamp(i + (d < 0 ? -1 : 1), 0, kIntervalCount - 1);
        photo_interval_ = kIntervals[i];
        photo_next_at_ = Now() + photo_interval_;
        SaveSettings();
      }});
    menu_items_.push_back({"Transition",
      [this] { return std::string(EffectName((int)photo_effect_)); },
      [this](int d) {
        const int n = (int)PhotoEffect::Count;
        photo_effect_ = (PhotoEffect)(((int)photo_effect_ + (d < 0 ? n - 1 : 1)) % n);
        Set("photo_fade", bind_photo_fade_, photo_effect_ != PhotoEffect::None);
        SaveSettings();
      }});
    menu_items_.push_back({"Shuffle",
      [this] { return std::string(photo_shuffle_ ? "On" : "Off"); },
      [this](int) { photo_shuffle_ = !photo_shuffle_; SaveSettings(); }});
  }
  menu_items_.push_back({"Rotate", nullptr, [this](int) { RotatePhoto(); }});
  if (photo_index_ < (int)photo_list_.size()) {
    const browse::Entry entry = photo_list_[photo_index_];
    library::Ref ref;
    if (Identify(entry, ref)) {
      const bool fav = library_.IsFavorite(ref.source_key, ref.entry.id);
      menu_items_.push_back({fav ? "Remove from favorites" : "Add to favorites",
        nullptr, [this, entry](int) { ToggleFavorite(entry); }});
    }
  }
  menu_items_.push_back({"Close", nullptr, [this](int) { ClosePhotos(); }});

  sel_menu_ = 0;
  RebuildMenuRows();
  Set("menu_open", bind_menu_open_, true);
}

void App::HandleKeyPhoto(Rml::Event& event, int key) {
  switch (key) {
  case Rml::Input::KI_LEFT:
  case Rml::Input::KI_PRIOR:
    StepPhoto(-1);
    photo_next_at_ = Now() + photo_interval_; // a manual step restarts the clock
    break;
  case Rml::Input::KI_RIGHT:
  case Rml::Input::KI_NEXT:
    StepPhoto(+1);
    photo_next_at_ = Now() + photo_interval_;
    break;
  case Rml::Input::KI_HOME: ShowPhoto(0); break;
  case Rml::Input::KI_END:  ShowPhoto((int)photo_list_.size() - 1); break;
  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER:
    if (photo_list_.size() > 1) {
      SetSlideshow(!bind_photo_playing_);
      ShowToast(bind_photo_playing_ ? "Slideshow" : "Slideshow paused",
                bind_photo_playing_ ? "▶" : "⏸");
    }
    break;
  case Rml::Input::KI_UP:
  case Rml::Input::KI_DOWN:
    if (bind_photo_info_) {
      Set("photo_info", bind_photo_info_, false);
    } else {
      // Stays until dismissed, unless the slideshow is running.
      ShowPhotoInfo(bind_photo_playing_ ? kPhotoInfoSec : 1e9);
    }
    break;
  case Rml::Input::KI_T: // triangle: turn the picture
    RotatePhoto();
    break;
  case Rml::Input::KI_F1:
    OpenPhotoMenu();
    break;
  case Rml::Input::KI_BACK:
  case Rml::Input::KI_ESCAPE:
    ClosePhotos();
    break;
  default:
    return;
  }
  event.StopPropagation();
}
