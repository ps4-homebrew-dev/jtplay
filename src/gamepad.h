// SPDX-License-Identifier: GPL-3.0-or-later
//
// Game controller navigation. Controller input is turned into the same
// RmlUi key presses the keyboard produces, so every view handles a pad
// exactly as it handles the keys it already knows:
//
//   d-pad, left stick     arrows (held: repeats)
//   cross / A             Return       open, play, pause
//   circle / B            Backspace    back; in full screen: the menus,
//                                      playing on (held: stop)
//   square / X            Space        search a source group again, next sort key
//   triangle / Y          T            full screen and back (Kodi's FullScreen);
//                                      browsing: reverse the order
//   L1 / R1               PageUp/Down  page, previous/next item (repeats)
//   L2 / R2               Home / End   first / last row
//   options / start       F1           options menu
//   back / select         Escape       back; in full screen: the menus,
//                                      playing on (not on a PS4: the
//                                      system keeps SHARE)
//
// A remote's options key, and a keyboard's menu key, are F1 as well (see
// RmlSDL::ConvertKey). Holding OK / Return is F1 too, for the remotes that
// have no options key or whose TV does not pass it on over HDMI-CEC: see
// HoldOk().
//
// The PS4's SDL port turns the DualShock 4 into an SDL game controller, so
// the same code serves the console and any pad SDL knows on a PC.
#ifndef GAMEPAD_H
#define GAMEPAD_H

#include <functional>
#include <string>

#include <SDL.h>

namespace Rml {
class Context;
}

namespace gamepad {

// Opens the game controller subsystem and the pads already connected.
// Call after SDL_Init(). Failure only means there will be no pads.
void Initialize();
void Shutdown();

// Feeds one SDL event. Returns true if it was a controller event, which
// then needs no further handling.
bool HandleEvent(Rml::Context* context, const SDL_Event& event);

// Once per frame, after the events: repeats the held direction, and turns
// an OK held long enough into F1.
void Update(Rml::Context* context);

// The long press of OK (Return or keypad Enter: a remote's OK arrives as
// Return). Its press is held back until it is clear which it is: let go
// within kOkHoldMs, it was OK after all; held that long, Update() sends F1
// (the options button) and the press and its release go nowhere. Any other
// press while OK is held back also makes it OK, delivered first.
//
// Feed every event before handling it. What to do with it:
enum class Hold {
  kPass,    // handle it as usual
  kSwallow, // drop it
  kReplay,  // handle *press (the held-back OK) first, then it as usual
};
Hold HoldOk(const SDL_Event& event, SDL_Event* press);

// Input tracing: with a callback set, every input event SDL delivers --
// keys, controller and joystick buttons, hats, axes, also the ones nothing
// reacts to -- is described to it in a line, with 'press' set for a
// button or key going down (or an axis or hat leaving rest). nullptr ends
// it. For finding out what a remote (over HDMI-CEC, say) sends.
void SetTrace(std::function<void(const std::string& line, bool press)> trace);
// Feeds one SDL event to the trace, if there is one. Call for every event,
// before handling it.
void Trace(const SDL_Event& event);

} // namespace gamepad

#endif
