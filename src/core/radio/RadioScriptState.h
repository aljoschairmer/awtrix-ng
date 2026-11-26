#pragma once

#include <string>

#include "core/RuntimeState.h"
#include "core/api/JsonText.h"
#include "core/radio/RadioStatus.h"
#include "system/MonotonicClock.h"

namespace awtrix::radio {

// radio.state() as JSON. Where the output reports no status (the ESP32), the phase is told from
// whether a station plays and why it stopped.
inline std::string scriptStateJson(const RuntimeState& runtime) {
  Status s = runtime.radio;
  if (s.phase == Phase::Idle && runtime.radioPlaying) s.phase = Phase::Playing;
  if (s.phase == Phase::Idle && !runtime.radioError.empty()) {
    s.phase = Phase::Error;
    s.error = runtime.radioError;
  }
  const bool active = s.phase != Phase::Idle && s.phase != Phase::Stopped;
  std::string out = "{\"state\":";
  api::appendJsonString(out, phaseName(s.phase));
  out += ",\"id\":";
  api::appendJsonString(out, active ? runtime.radioId : std::string());
  out += ",\"name\":";
  api::appendJsonString(out, active ? runtime.radioStation : std::string());
  out += ",\"station_name\":";
  api::appendJsonString(out, s.stationName);
  out += ",\"title\":";
  api::appendJsonString(out, active ? runtime.radioTitle : std::string());
  out += ",\"error\":";
  api::appendJsonString(out, s.error);
  out += ",\"fault\":";
  if (s.fault == Fault::None) out += "null";
  else api::appendJsonString(out, faultName(s.fault));
  out += ",\"http_status\":" + std::to_string(s.httpStatus);
  out += ",\"retry_ms\":" + std::to_string(s.retryMs);
  out += ",\"codec\":";
  api::appendJsonString(out, s.codec);
  out += ",\"bitrate\":" + std::to_string(s.bitrateKbps);
  out += ",\"sample_rate\":" + std::to_string(s.sampleRate);
  out += ",\"channels\":" + std::to_string(s.channels);
  out += ",\"buffer\":" + std::to_string(s.bufferPercent);
  const int64_t since = s.sinceMs ? monotonicMs() - s.sinceMs : 0;
  out += ",\"since_ms\":" + std::to_string(since < 0 ? 0 : since);
  out += "}";
  return out;
}

}
