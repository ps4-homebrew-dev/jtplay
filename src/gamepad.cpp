// SPDX-License-Identifier: GPL-3.0-or-later
//
// See gamepad.h for the button mapping.
#include "gamepad.h"

#include <map>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Input.h>

namespace gamepad {
namespace {

using Rml::Input::KeyIdentifier;

// How long a direction or page key is held before it repeats, and how
// often it repeats then.
constexpr Uint32 kRepeatDelayMs = 400;
constexpr Uint32 kRepeatIntervalMs = 90;

// A stick or trigger counts as pressed past kPress and as released again
// below kRelease, so that a value hovering at the threshold doesn't
// chatter.
constexpr int kPress = 20000;
constexpr int kRelease = 12000;

std::map<SDL_JoystickID, SDL_GameController*> pads;

// The key that repeats while held, and when it fires next.
KeyIdentifier held_key = Rml::Input::KI_UNKNOWN;
Uint32 next_repeat = 0;

// What the analog inputs are pressing now: one entry per axis, the key the
// axis' direction stands for, or KI_UNKNOWN.
std::map<std::pair<SDL_JoystickID, int>, KeyIdentifier> axis_keys;

KeyIdentifier ButtonKey(Uint8 button) {
  switch (button) {
  case SDL_CONTROLLER_BUTTON_A:             return Rml::Input::KI_RETURN;
  case SDL_CONTROLLER_BUTTON_B:             return Rml::Input::KI_BACK;
  case SDL_CONTROLLER_BUTTON_X:             return Rml::Input::KI_SPACE;
  case SDL_CONTROLLER_BUTTON_Y:             return Rml::Input::KI_T;
  case SDL_CONTROLLER_BUTTON_START:         return Rml::Input::KI_F1;
  case SDL_CONTROLLER_BUTTON_BACK:          return Rml::Input::KI_ESCAPE;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return Rml::Input::KI_PRIOR;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return Rml::Input::KI_NEXT;
  case SDL_CONTROLLER_BUTTON_DPAD_UP:       return Rml::Input::KI_UP;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return Rml::Input::KI_DOWN;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return Rml::Input::KI_LEFT;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return Rml::Input::KI_RIGHT;
  default:                                  return Rml::Input::KI_UNKNOWN;
  }
}

bool Repeats(KeyIdentifier key) {
  switch (key) {
  case Rml::Input::KI_UP:
  case Rml::Input::KI_DOWN:
  case Rml::Input::KI_LEFT:
  case Rml::Input::KI_RIGHT:
  case Rml::Input::KI_PRIOR:
  case Rml::Input::KI_NEXT:
    return true;
  default:
    return false;
  }
}

void Press(Rml::Context* context, KeyIdentifier key) {
  if (key == Rml::Input::KI_UNKNOWN)
    return;
  context->ProcessKeyDown(key, 0);
  if (Repeats(key)) {
    held_key = key;
    next_repeat = SDL_GetTicks() + kRepeatDelayMs;
  }
}

void Release(Rml::Context* context, KeyIdentifier key) {
  if (key == Rml::Input::KI_UNKNOWN)
    return;
  context->ProcessKeyUp(key, 0);
  if (key == held_key)
    held_key = Rml::Input::KI_UNKNOWN;
}

// The key an axis stands for at 'value', keeping 'current' while the value
// is between the release and the press thresholds.
KeyIdentifier AxisKey(Uint8 axis, int value, KeyIdentifier current) {
  KeyIdentifier negative = Rml::Input::KI_UNKNOWN;
  KeyIdentifier positive = Rml::Input::KI_UNKNOWN;
  switch (axis) {
  case SDL_CONTROLLER_AXIS_LEFTX:
    negative = Rml::Input::KI_LEFT;
    positive = Rml::Input::KI_RIGHT;
    break;
  case SDL_CONTROLLER_AXIS_LEFTY:
    negative = Rml::Input::KI_UP;
    positive = Rml::Input::KI_DOWN;
    break;
  case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
    positive = Rml::Input::KI_HOME;
    break;
  case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
    positive = Rml::Input::KI_END;
    break;
  default:
    return Rml::Input::KI_UNKNOWN;
  }

  if (value >= kPress)
    return positive;
  if (value <= -kPress)
    return negative;
  if (value > -kRelease && value < kRelease)
    return Rml::Input::KI_UNKNOWN;
  return current;
}

void Open(int device_index) {
  if (!SDL_IsGameController(device_index))
    return;
  const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
  if (id < 0 || pads.count(id))
    return;
  SDL_GameController* pad = SDL_GameControllerOpen(device_index);
  if (!pad) {
    SDL_Log("gamepad: cannot open %d: %s", device_index, SDL_GetError());
    return;
  }
  SDL_Log("gamepad: %s", SDL_GameControllerName(pad));
  pads[id] = pad;
}

void Close(Rml::Context* context, SDL_JoystickID id) {
  auto it = pads.find(id);
  if (it == pads.end())
    return;
  for (auto axis = axis_keys.begin(); axis != axis_keys.end();) {
    if (axis->first.first == id) {
      if (context)
        Release(context, axis->second);
      axis = axis_keys.erase(axis);
    } else {
      ++axis;
    }
  }
  SDL_GameControllerClose(it->second);
  pads.erase(it);
  // Whatever it was holding is let go.
  held_key = Rml::Input::KI_UNKNOWN;
}

} // namespace

void Initialize() {
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
    SDL_Log("gamepad: %s", SDL_GetError());
    return;
  }
  for (int i = 0; i < SDL_NumJoysticks(); i++)
    Open(i);
}

void Shutdown() {
  while (!pads.empty())
    Close(nullptr, pads.begin()->first);
  axis_keys.clear();
  SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

bool HandleEvent(Rml::Context* context, const SDL_Event& event) {
  switch (event.type) {
  case SDL_CONTROLLERDEVICEADDED:
    Open(event.cdevice.which);
    return true;
  case SDL_CONTROLLERDEVICEREMOVED:
    Close(context, event.cdevice.which);
    return true;
  case SDL_CONTROLLERDEVICEREMAPPED:
    return true;
  case SDL_CONTROLLERBUTTONDOWN:
    Press(context, ButtonKey(event.cbutton.button));
    return true;
  case SDL_CONTROLLERBUTTONUP:
    Release(context, ButtonKey(event.cbutton.button));
    return true;
  case SDL_CONTROLLERAXISMOTION: {
    KeyIdentifier& current = axis_keys[{event.caxis.which, event.caxis.axis}];
    const KeyIdentifier next = AxisKey(event.caxis.axis, event.caxis.value, current);
    if (next != current) {
      Release(context, current);
      current = next;
      Press(context, next);
    }
    return true;
  }
  default:
    return false;
  }
}

void Update(Rml::Context* context) {
  if (held_key == Rml::Input::KI_UNKNOWN)
    return;
  const Uint32 now = SDL_GetTicks();
  if (!SDL_TICKS_PASSED(now, next_repeat))
    return;
  next_repeat = now + kRepeatIntervalMs;
  context->ProcessKeyDown(held_key, 0);
}

} // namespace gamepad
