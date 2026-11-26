#include "platform/tc002/audio/AudioScripting.h"

#include "berry.h"
#include "core/apps/IApp.h"
#include "platform/tc002/audio/AudioScriptModule.h"

namespace awtrix::tc002 {

void AudioScripting::install(script::ScriptExtensionHost& host) {
  host.defineNative("_native_song_beat", beat, this);
  host.defineNative("_native_music_pitch", pitch, this);
  host.defineNative("_native_sound_effect", effect, this);
  host.defineNative("_native_radio_play", radioPlay, this);
  host.defineNative("_native_radio_stop", radioStop, this);
  host.defineNative("_native_radio_static", radioStatic, this);
  host.defineNative("_native_radio_state", radioState, this);
  host.defineModule("_tc002_audio", kAudioScriptModule);
  host.defineModule("radio", kRadioScriptModule);
}

int64_t AudioScripting::now() const {
  const auto* context = script::ScriptExtensionHost::context();
  return context ? context->nowMs : (clock_ ? clock_() : 0);
}

int AudioScripting::beat(bvm* vm) {
  auto& self = *static_cast<AudioScripting*>(script::BerryVM::nativeSelf(vm));
  double value = 0;
  if (!self.beat_ || !self.beat_(self.now(), value)) be_return_nil(vm);
  be_pushreal(vm, static_cast<breal>(value));
  be_return(vm);
}

int AudioScripting::pitch(bvm* vm) {
  auto& self = *static_cast<AudioScripting*>(script::BerryVM::nativeSelf(vm));
  const bool onScreen = script::ScriptExtensionHost::canvas() != nullptr;
  be_pushreal(vm, static_cast<breal>(self.pitch_ ? self.pitch_(self.now(), onScreen) : 0));
  be_return(vm);
}

int AudioScripting::effect(bvm* vm) {
  auto& self = *static_cast<AudioScripting*>(script::BerryVM::nativeSelf(vm));
  int answer = 0;
  std::string error;
  if (self.sound_ && be_top(vm) >= 1 && be_isstring(vm, 1))
    answer = self.sound_(script::SoundAction::Effect, be_tostring(vm, 1),
                         script::ScriptExtensionHost::caller(), error);
  if (answer < 0) be_pushstring(vm, error.c_str());
  else be_pushbool(vm, answer > 0);
  be_return(vm);
}

}

namespace awtrix::tc002 {
namespace {

// A sound call on the script's behalf: true, false (no output), or the mistake as a string.
void answerSound(bvm* vm, const AudioScripting::Sound& sound, script::SoundAction action, const std::string& json) {
  int answer = 0;
  std::string error;
  if (sound) answer = sound(action, json, script::ScriptExtensionHost::caller(), error);
  if (answer < 0) be_pushstring(vm, error.c_str());
  else be_pushbool(vm, answer > 0);
}

}

// The station object is built and checked in Berry; it arrives as the JSON sound.play takes.
int AudioScripting::radioPlay(bvm* vm) {
  auto& self = *static_cast<AudioScripting*>(script::BerryVM::nativeSelf(vm));
  if (be_top(vm) < 1 || !be_isstring(vm, 1)) be_return_nil(vm);
  answerSound(vm, self.sound_, script::SoundAction::Play, be_tostring(vm, 1));
  be_return(vm);
}

int AudioScripting::radioStop(bvm* vm) {
  auto& self = *static_cast<AudioScripting*>(script::BerryVM::nativeSelf(vm));
  answerSound(vm, self.sound_, script::SoundAction::StopRadio, std::string());
  be_return(vm);
}

int AudioScripting::radioStatic(bvm* vm) {
  auto& self = *static_cast<AudioScripting*>(script::BerryVM::nativeSelf(vm));
  if (be_top(vm) < 1 || !be_isint(vm, 1)) be_return_nil(vm);
  answerSound(vm, self.sound_, script::SoundAction::RadioStatic, std::to_string(be_toint(vm, 1)));
  be_return(vm);
}

int AudioScripting::radioState(bvm* vm) {
  auto& self = *static_cast<AudioScripting*>(script::BerryVM::nativeSelf(vm));
  be_pushstring(vm, self.radioState_ ? self.radioState_().c_str() : "");
  be_return(vm);
}

}
