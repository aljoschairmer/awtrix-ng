#pragma once

namespace awtrix::tc002 {
inline constexpr const char* kAudioScriptModule = R"BERRY(
import global
import json
var sound = global.sound
var music = global.music

def _sound_effect(x) # sound.effect(x)
  var result = _native_sound_effect(json.dump(x))
  if type(result) == 'string' raise 'value_error', result end
  return result
end

def _sound_beat() # sound.beat()
  return _native_song_beat()
end

def _music_pitch() # music.pitch()
  return _native_music_pitch()
end

sound.effect = _sound_effect
sound.beat = _sound_beat
music.pitch = _music_pitch
return sound
)BERRY";

// import radio: the clock's radio for an app. radio.play() starts a station by its address, with a
// name to show, the app's own id for it, a fade-in and static ('noise') under the wait;
// radio.stop() ends it (whoever started it); radio.noise() plays static alone, the sound between
// stations;
// radio.state() says what the station is doing and radio.on_change() calls back when that
// changes, also while the app is not on screen. The station keeps playing when the app is hidden.
inline constexpr const char* kRadioScriptModule = R"BERRY(
import global
import json
var radio = module('radio')

def _radio_check(r)
  if type(r) == 'string' raise 'value_error', r end
  return r
end

def _radio_play(url, opts) # radio.play(url, opts?)
  if type(url) != 'string' || size(url) == 0 raise 'value_error', 'url must be a station address' end
  var spec = {'station': url}
  if opts != nil
    if classname(opts) != 'map' raise 'value_error', 'opts must be a map' end
    for k : ['name', 'id', 'fade', 'noise']
      if opts.contains(k) spec[k] = opts[k] end
    end
  end
  return _radio_check(_native_radio_play(json.dump(spec)))
end

def _radio_stop() # radio.stop()
  return _radio_check(_native_radio_stop())
end

def _radio_noise(level) # radio.noise(level)
  if type(level) == 'real' level = int(level) end
  if type(level) != 'int' || level < 0 || level > 100 raise 'value_error', 'level must be 0..100' end
  return _radio_check(_native_radio_static(level))
end

def _radio_state() # radio.state()
  var s = json.load(_native_radio_state())
  if s == nil
    s = {'state': 'idle', 'id': '', 'name': '', 'station_name': '', 'title': '', 'error': '',
         'fault': nil, 'http_status': 0, 'retry_ms': -1, 'codec': '', 'bitrate': 0,
         'sample_rate': 0, 'channels': 0, 'buffer': 0, 'since_ms': 0}
  end
  return s
end

def _radio_can(key) # radio.can(key?)
  var b = _native_sound_caps()
  var m = {'play': (b & 32) != 0, 'aac': (b & 512) != 0, 'noise': (b & 1024) != 0,
           'fade': (b & 1024) != 0}
  if key == nil return m end
  return m.find(key, false)
end

# What on_change compares: the buffer level and the clock move all the time.
def _radio_key(s)
  return s['state'] + '|' + s['id'] + '|' + s['name'] + '|' + s['title'] + '|' + s['error'] + '|' +
         str(s['fault']) + '|' + s['codec'] + '|' + s['station_name']
end

var _radio_watch = {}

def _radio_on_change(fn) # radio.on_change(fn)
  var app = _native_app()
  var old = _radio_watch.find(app)
  if old != nil
    global.timer.cancel(old[0])
    _radio_watch.remove(app)
  end
  if fn == nil return nil end
  var entry = [nil, _radio_key(_radio_state()), fn]
  entry[0] = global.timer.every(250, def ()
    var s = _radio_state()
    var k = _radio_key(s)
    if k != entry[1]
      entry[1] = k
      entry[2](s)
    end
  end)
  _radio_watch[app] = entry
  return nil
end

radio.play = _radio_play
radio.stop = _radio_stop
radio.noise = _radio_noise
radio.state = _radio_state
radio.can = _radio_can
radio.on_change = _radio_on_change
return radio
)BERRY";
}
