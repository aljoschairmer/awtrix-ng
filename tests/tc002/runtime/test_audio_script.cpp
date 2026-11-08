#include "../../support.h"
#include <memory>
#include <string>

#include "core/apps/AppRegistry.h"
#include "core/render/Canvas.h"
#include "core/script/ScriptApp.h"
#include "core/script/ScriptHost.h"
#include "platform/linux/script/ExtensionHost.h"
#include "platform/tc002/audio/AudioScripting.h"

using namespace awtrix;

namespace {
constexpr auto check = awtrix::test::check;

struct Rig {
  script::ScriptServices services;
  AppRegistry apps;
  tc002::AudioScripting audio;
  script::ScriptHost host;
  script::ExtensionHost extensions;
  Canvas canvas{52, 16};

  Rig(tc002::AudioScripting::Beat beat = {}, tc002::AudioScripting::Pitch pitch = {},
      tc002::AudioScripting::Sound sound = {}, tc002::AudioScripting::RadioState radioState = {})
      : audio(std::move(beat), std::move(pitch), [] { return 77; }, std::move(sound), std::move(radioState)),
        host(apps, services, {}, {}), extensions(host, {&audio}) {}

  // Evaluates expression in draw(), or in loop() with inLoop.
  std::string run(const std::string& expression, bool inLoop = false) {
    const std::string keep = "self.result = " + expression;
    check(host.set("AudioProbe", "import radio\nclass App\nvar result\ndef draw() " + (inLoop ? std::string() : keep) +
        " end\ndef loop() " + (inLoop ? keep : std::string()) +
        " end\ndef check() return self.result end\nend\nreturn App()"), "the audio app installs");
    auto* app = static_cast<script::ScriptApp*>(apps.find("AudioProbe"));
    if (!app) return {};
    RenderCtx context;
    context.nowMs = 1234;
    if (inLoop) app->tickLoop(context);
    else app->render(canvas, context);
    std::string result;
    app->callCheckForTest(result);
    return result;
  }
};

void absentWithoutExtension() {
  for (const char* call : {"sound.effect('tone')", "sound.beat()", "music.pitch()"}) {
    script::ScriptServices services;
    AppRegistry apps;
    script::ScriptHost host(apps, services, {}, {});
    check(host.set("MissingAudio", std::string("class App\ndef draw() ") + call + " end\nend\nreturn App()"),
          "an app using an unavailable member remains reportable");
    Canvas canvas(32, 8);
    RenderCtx context;
    if (auto* app = apps.find("MissingAudio")) app->render(canvas, context);
    check(!host.errorOf("MissingAudio").empty(), "TC002 audio members require their extension");
  }
}

void frameTimeAndOwner() {
  int64_t beatAt = 0, pitchAt = 0;
  bool pitchOnScreen = false;
  std::string owner, payload;
  script::SoundAction action = script::SoundAction::Play;
  Rig rig([&](int64_t now, double& beat) { beatAt = now; beat = 3.5; return true; },
          [&](int64_t now, bool onScreen) { pitchAt = now; pitchOnScreen = onScreen; return 440.5f; },
          [&](script::SoundAction kind, const std::string& json, const std::string& caller, std::string&) {
            action = kind; owner = caller; payload = json; return 1;
          });
  check(rig.run("str(sound.beat()) + '/' + str(music.pitch()) + '/' + str(sound.effect('tone'))") ==
        "3.5/440.5/true", "the existing audio module names expose the extension results");
  check(beatAt == 1234 && pitchAt == 1234, "analysis follows the rendering frame time");
  check(pitchOnScreen, "pitch asked from draw() may make the microphone listen");
  Rig looping({}, [&](int64_t, bool onScreen) { pitchOnScreen = onScreen; return 440.5f; });
  check(looping.run("str(music.pitch())", true) == "440.5" && !pitchOnScreen,
        "pitch asked from loop() may not");
  check(owner == "AudioProbe" && action == script::SoundAction::Effect && payload == "\"tone\"",
        "an effect carries its caller and sound specification");
}

void unavailableAndInvalid() {
  {
    Rig rig;
    check(rig.run("str(sound.beat()) + '/' + str(music.pitch() == 0.0) + '/' + str(sound.effect('tone'))") ==
          "nil/true/false", "missing audio services retain their empty results");
  }
  {
    Rig rig({}, {}, [](script::SoundAction, const std::string&, const std::string&, std::string& error) {
      error = "invalid sound"; return -1;
    });
    rig.run("sound.effect({})");
    check(!rig.host.errorOf("AudioProbe").empty(), "an invalid effect raises in the calling script");
  }
}

// import radio: each call reaches the engine as the sound action it is, with the caller's name.
void radioModule() {
  std::string owner, payload;
  script::SoundAction action = script::SoundAction::Play;
  Rig rig({}, {},
          [&](script::SoundAction kind, const std::string& json, const std::string& caller, std::string& error) {
            action = kind;
            owner = caller;
            payload = json;
            if (kind == script::SoundAction::Play && json.find("bad") != std::string::npos) {
              error = "station: invalid URL";
              return -1;
            }
            return 1;
          },
          [] { return std::string("{\"state\":\"playing\",\"id\":\"rg:1\",\"codec\":\"aac\",\"buffer\":42}"); });
  check(rig.run("str(radio.play('http://x/s', {'name': 'X', 'id': 'rg:1', 'fade': 500, 'noise': 30}))") == "true",
        "radio.play answers true");
  check(action == script::SoundAction::Play && owner == "AudioProbe", "it plays as the calling script");
  for (const char* part : {"\"station\":\"http://x/s\"", "\"name\":\"X\"", "\"id\":\"rg:1\"", "\"fade\":500",
                           "\"noise\":30"})
    check(payload.find(part) != std::string::npos, "the station object carries every option");
  check(rig.run("str(radio.stop())") == "true" && action == script::SoundAction::StopRadio, "radio.stop");
  check(rig.run("str(radio.noise(40))") == "true" && action == script::SoundAction::RadioStatic && payload == "40",
        "radio.noise sends its level");
  check(rig.run("radio.state()['state'] + '/' + radio.state()['codec'] + '/' + str(radio.state()['buffer'])") ==
            "playing/aac/42",
        "radio.state reads the engine's status");
  check(rig.run("str(radio.can('aac'))") == "false", "without an output nothing is promised");
  rig.run("radio.noise(101)");
  check(!rig.host.errorOf("AudioProbe").empty(), "a level out of range raises");
  Rig bad({}, {}, [](script::SoundAction kind, const std::string& json, const std::string&, std::string& error) {
    (void)kind;
    (void)json;
    error = "station: invalid URL";
    return -1;
  });
  bad.run("radio.play('bad')");
  check(bad.host.errorOf("AudioProbe").message.find("invalid URL") != std::string::npos, "a refused station raises its reason");
  Rig idle;
  check(idle.run("radio.state()['state']") == "idle", "with no engine the radio is idle, never nil");
}
}

int main() {
  absentWithoutExtension();
  frameTimeAndOwner();
  unavailableAndInvalid();
  radioModule();
  return awtrix::test::finish("TC002 audio script");
}
