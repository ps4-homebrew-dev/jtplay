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
//   options / start       Menu         options menu
//   back / select         Escape       back; in full screen: the menus,
//                                      playing on (not on a PS4: the
//                                      system keeps SHARE)
//
// A remote's options and disc menu keys, and a keyboard's menu key and F10,
// are options as well (see RmlSDL::ConvertKey). Holding OK / Return is
// options too, for the remotes that have no options key or whose TV does
// not pass it on over HDMI-CEC: see HoldOk(). (Not F1: that is what a
// remote's subtitle key arrives as.)
//
// The PS4's SDL port turns the DualShock 4 into an SDL game controller, so
// the same code serves the console and any pad SDL knows on a PC.
#ifndef GAMEPAD_H
#define GAMEPAD_H

#include <functional>
#include <string>

#include <SDL.h>

#include <RmlUi/Core/Input.h>

namespace Rml {
class Context;
}

namespace gamepad {

// A remote's rewind and fast forward keys. RmlUi has no identifiers for
// them, so RmlSDL::ConvertKey gives them these two, which no other key
// produces (a keyboard's browser back is Backspace there).
inline constexpr Rml::Input::KeyIdentifier kKeyRewind = Rml::Input::KI_BROWSER_BACK;
inline constexpr Rml::Input::KeyIdentifier kKeyFastForward = Rml::Input::KI_BROWSER_FORWARD;

// The options button, whatever it comes from: a pad's options / start, a
// held OK, a remote's options, menu or disc menu key, a keyboard's menu key
// or F10. RmlUi's name for the menu key.
inline constexpr Rml::Input::KeyIdentifier kKeyOptions = Rml::Input::KI_APPS;

// Opens the game controller subsystem and the pads already connected.
// Call after SDL_Init(). Failure only means there will be no pads.
void Initialize();
void Shutdown();

// Feeds one SDL event. Returns true if it was a controller event, which
// then needs no further handling.
bool HandleEvent(Rml::Context* context, const SDL_Event& event);

// Once per frame, after the events: repeats the held direction, and turns
// an OK held long enough into options (kKeyOptions).
void Update(Rml::Context* context);

// The long press of OK (Return or keypad Enter: a remote's OK arrives as
// Return). Its press is held back until it is clear which it is: let go
// within kOkHoldMs, it was OK after all; held that long, Update() sends
// kKeyOptions (the options button) and the press and its release go nowhere. Any other
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
