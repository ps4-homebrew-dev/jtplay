// SPDX-License-Identifier: GPL-3.0-or-later
//
// See gamepad.h for the button mapping.
#include "gamepad.h"

#include <cstdio>
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
// Devices SDL does not know as game controllers, opened only so that the
// input trace sees what they send.
std::map<SDL_JoystickID, SDL_Joystick*> joysticks;

std::function<void(const std::string&, bool)> trace;
// Where each traced axis was last: -1, 0 or +1 (past the press threshold).
std::map<std::pair<SDL_JoystickID, int>, int> trace_axes;

// The key that repeats while held, and when it fires next.
KeyIdentifier held_key = Rml::Input::KI_UNKNOWN;
Uint32 next_repeat = 0;

// What the analog inputs are pressing now: one entry per axis, the key the
// axis' direction stands for, or KI_UNKNOWN.
std::map<std::pair<SDL_JoystickID, int>, KeyIdentifier> axis_keys;

// How long OK is held to be the options button (as long as back is held to
// stop: kHoldStopSec).
constexpr Uint32 kOkHoldMs = 600;

// The OK press held back (see HoldOk), and since when; or, once it has
// been held long enough to be options, the key whose release is still to come
// and is to be dropped.
bool ok_pending = false;
SDL_Event ok_press;
Uint32 ok_since = 0;
SDL_Keycode ok_fired = SDLK_UNKNOWN;

bool IsOk(SDL_Keycode sym) {
  return sym == SDLK_RETURN || sym == SDLK_KP_ENTER;
}

KeyIdentifier ButtonKey(Uint8 button) {
  switch (button) {
  case SDL_CONTROLLER_BUTTON_A:             return Rml::Input::KI_RETURN;
  case SDL_CONTROLLER_BUTTON_B:             return Rml::Input::KI_BACK;
  case SDL_CONTROLLER_BUTTON_X:             return Rml::Input::KI_SPACE;
  case SDL_CONTROLLER_BUTTON_Y:             return Rml::Input::KI_T;
  case SDL_CONTROLLER_BUTTON_START:         return kKeyOptions;
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
  const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
  if (id < 0 || pads.count(id) || joysticks.count(id))
    return;
  if (!SDL_IsGameController(device_index)) {
    if (SDL_Joystick* joystick = SDL_JoystickOpen(device_index)) {
      SDL_Log("joystick: %s", SDL_JoystickName(joystick));
      joysticks[id] = joystick;
    }
    return;
  }
  SDL_GameController* pad = SDL_GameControllerOpen(device_index);
  if (!pad) {
    SDL_Log("gamepad: cannot open %d: %s", device_index, SDL_GetError());
    return;
  }
  SDL_Log("gamepad: %s", SDL_GameControllerName(pad));
  pads[id] = pad;
}

void Close(Rml::Context* context, SDL_JoystickID id) {
  if (auto joystick = joysticks.find(id); joystick != joysticks.end()) {
    SDL_JoystickClose(joystick->second);
    joysticks.erase(joystick);
  }
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
  trace = nullptr;
  while (!pads.empty())
    Close(nullptr, pads.begin()->first);
  while (!joysticks.empty())
    Close(nullptr, joysticks.begin()->first);
  axis_keys.clear();
  ok_pending = false;
  ok_fired = SDLK_UNKNOWN;
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
  case SDL_JOYDEVICEADDED:     // not a game controller: for the trace only
    Open(event.jdevice.which);
    return true;
  case SDL_JOYDEVICEREMOVED:
    if (joysticks.count(event.jdevice.which))
      Close(context, event.jdevice.which);
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

Hold HoldOk(const SDL_Event& event, SDL_Event* press) {
  switch (event.type) {
  case SDL_KEYDOWN:
    if (IsOk(event.key.keysym.sym)) {
      if (event.key.repeat) // the keyboard's own repeats of a held key
        return ok_pending || ok_fired == event.key.keysym.sym ? Hold::kSwallow : Hold::kPass;
      // A new press. One still held back (its release lost, or the other
      // Enter key) is dropped; so is an earlier long press' release.
      ok_pending = true;
      ok_press = event;
      ok_since = SDL_GetTicks();
      ok_fired = SDLK_UNKNOWN;
      return Hold::kSwallow;
    }
    break;
  case SDL_KEYUP:
    if (ok_fired != SDLK_UNKNOWN && event.key.keysym.sym == ok_fired) {
      ok_fired = SDLK_UNKNOWN; // options has been sent for it
      return Hold::kSwallow;
    }
    if (ok_pending && event.key.keysym.sym == ok_press.key.keysym.sym) {
      ok_pending = false; // let go in time: OK
      *press = ok_press;
      return Hold::kReplay;
    }
    return Hold::kPass;
  default:
    break;
  }

  // Something else pressed while OK is held back: OK, then that.
  if (ok_pending &&
      (event.type == SDL_KEYDOWN || event.type == SDL_CONTROLLERBUTTONDOWN ||
       event.type == SDL_MOUSEBUTTONDOWN)) {
    ok_pending = false;
    *press = ok_press;
    return Hold::kReplay;
  }
  return Hold::kPass;
}

void Update(Rml::Context* context) {
  const Uint32 now = SDL_GetTicks();

  // OK held long enough: the options button.
  if (ok_pending && SDL_TICKS_PASSED(now, ok_since + kOkHoldMs)) {
    ok_pending = false;
    ok_fired = ok_press.key.keysym.sym;
    if (trace)
      trace("OK held: options", true);
    context->ProcessKeyDown(kKeyOptions, 0);
    context->ProcessKeyUp(kKeyOptions, 0);
  }

  if (held_key == Rml::Input::KI_UNKNOWN)
    return;
  if (!SDL_TICKS_PASSED(now, next_repeat))
    return;
  next_repeat = now + kRepeatIntervalMs;
  context->ProcessKeyDown(held_key, 0);
}

void SetTrace(std::function<void(const std::string& line, bool press)> callback) {
  trace = std::move(callback);
  trace_axes.clear();
}

void Trace(const SDL_Event& event) {
  if (!trace)
    return;
  char line[160];
  bool press = false;

  // An axis is reported when it crosses the press threshold, either way,
  // not for every small movement. Returns false when nothing crossed.
  auto axis = [&](SDL_JoystickID id, int index, int value) {
    const int where = value >= kPress ? 1 : value <= -kPress ? -1 : 0;
    int& was = trace_axes[{id, index}];
    if (where == was || (where == 0 && value <= -kRelease) || (where == 0 && value >= kRelease))
      return false;
    was = where;
    press = where != 0;
    return true;
  };

  switch (event.type) {
  case SDL_KEYDOWN:
  case SDL_KEYUP: {
    const SDL_Keysym& k = event.key.keysym;
    press = event.type == SDL_KEYDOWN;
    std::snprintf(line, sizeof(line), "key %s: %s (keycode 0x%x, scancode %d)%s",
                  press ? "down" : "up", SDL_GetKeyName(k.sym), (unsigned)k.sym,
                  (int)k.scancode, event.key.repeat ? ", repeat" : "");
    break;
  }
  case SDL_CONTROLLERBUTTONDOWN:
  case SDL_CONTROLLERBUTTONUP: {
    const char* name =
      SDL_GameControllerGetStringForButton((SDL_GameControllerButton)event.cbutton.button);
    press = event.type == SDL_CONTROLLERBUTTONDOWN;
    std::snprintf(line, sizeof(line), "controller %d button %s: %s (%d)",
                  (int)event.cbutton.which, press ? "down" : "up",
                  name ? name : "?", (int)event.cbutton.button);
    break;
  }
  case SDL_CONTROLLERAXISMOTION: {
    if (!axis(event.caxis.which, event.caxis.axis, event.caxis.value))
      return;
    const char* name =
      SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)event.caxis.axis);
    std::snprintf(line, sizeof(line), "controller %d axis %s (%d): %d",
                  (int)event.caxis.which, name ? name : "?", (int)event.caxis.axis,
                  (int)event.caxis.value);
    break;
  }
  // A game controller's raw joystick events would only repeat the above.
  case SDL_JOYBUTTONDOWN:
  case SDL_JOYBUTTONUP:
    if (pads.count(event.jbutton.which))
      return;
    press = event.type == SDL_JOYBUTTONDOWN;
    std::snprintf(line, sizeof(line), "joystick %d button %s: %d",
                  (int)event.jbutton.which, press ? "down" : "up", (int)event.jbutton.button);
    break;
  case SDL_JOYHATMOTION:
    if (pads.count(event.jhat.which))
      return;
    press = event.jhat.value != SDL_HAT_CENTERED;
    std::snprintf(line, sizeof(line), "joystick %d hat %d: 0x%x", (int)event.jhat.which,
                  (int)event.jhat.hat, (unsigned)event.jhat.value);
    break;
  case SDL_JOYAXISMOTION:
    if (pads.count(event.jaxis.which) ||
        !axis(event.jaxis.which, 1000 + event.jaxis.axis, event.jaxis.value))
      return;
    std::snprintf(line, sizeof(line), "joystick %d axis %d: %d", (int)event.jaxis.which,
                  (int)event.jaxis.axis, (int)event.jaxis.value);
    break;
  case SDL_CONTROLLERDEVICEADDED:
  case SDL_JOYDEVICEADDED:
    std::snprintf(line, sizeof(line), "%s added: %s",
                  event.type == SDL_CONTROLLERDEVICEADDED ? "controller" : "joystick",
                  SDL_JoystickNameForIndex(event.cdevice.which)
                    ? SDL_JoystickNameForIndex(event.cdevice.which) : "?");
    break;
  case SDL_TEXTINPUT:
    std::snprintf(line, sizeof(line), "text: %s", event.text.text);
    break;
  default:
    return;
  }
  trace(line, press);
}

} // namespace gamepad
