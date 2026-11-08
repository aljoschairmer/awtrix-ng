#pragma once

#include <functional>
#include <utility>

#include "core/script/ScriptExtension.h"
#include "core/script/ScriptServices.h"

namespace awtrix::tc002 {

class AudioScripting final : public script::ScriptExtension {
 public:
  using Beat = std::function<bool(int64_t, double&)>;
  // onScreen: asked from draw(); only then may the microphone start listening.
  using Pitch = std::function<float(int64_t nowMs, bool onScreen)>;
  using Clock = std::function<int64_t()>;
  using Sound = std::function<int(script::SoundAction, const std::string&,
                                  const std::string&, std::string&)>;

  // radio.state() as JSON.
  using RadioState = std::function<std::string()>;

  AudioScripting(Beat beat, Pitch pitch, Clock clock, Sound sound, RadioState radioState = nullptr)
      : beat_(std::move(beat)), pitch_(std::move(pitch)), clock_(std::move(clock)), sound_(std::move(sound)),
        radioState_(std::move(radioState)) {}
  std::vector<std::string> modules() const override { return {"_tc002_audio", "radio"}; }
  void install(script::ScriptExtensionHost& host) override;

 private:
  static int beat(bvm* vm);
  static int pitch(bvm* vm);
  static int effect(bvm* vm);
  static int radioPlay(bvm* vm);
  static int radioStop(bvm* vm);
  static int radioStatic(bvm* vm);
  static int radioState(bvm* vm);
  int64_t now() const;

  Beat beat_;
  Pitch pitch_;
  Clock clock_;
  Sound sound_;
  RadioState radioState_;
};

}
