// SPDX-License-Identifier: GPL-3.0-or-later
//
// Game controller navigation. Controller input is turned into the same
// RmlUi key presses the keyboard produces, so every view handles a pad
// exactly as it handles the keys it already knows:
//
//   d-pad, left stick     arrows (held: repeats)
//   cross / A             Return       open, play, pause
//   circle / B            Backspace    back, stop
//   square / X            Space        search a source group again, cycle video track
//   triangle / Y          T            cycle audio track
//   L1 / R1               PageUp/Down  page, previous/next item (repeats)
//   L2 / R2               Home / End   first / last row
//   options / start       F1           options menu
//   back / select         Escape       back, stop (not on a PS4: the
//                                      system keeps SHARE)
//
// The PS4's SDL port turns the DualShock 4 into an SDL game controller, so
// the same code serves the console and any pad SDL knows on a PC.
#ifndef GAMEPAD_H
#define GAMEPAD_H

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

// Once per frame, after the events: repeats the held direction.
void Update(Rml::Context* context);

} // namespace gamepad

#endif
