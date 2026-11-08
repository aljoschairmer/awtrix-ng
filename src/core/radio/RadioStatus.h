#pragma once

#include <cstdint>
#include <string>

namespace awtrix::radio {

// Where a station is: what radio.state() and GET /api/v1/audio report.
enum class Phase : uint8_t { Idle, Connecting, Buffering, Playing, Error, Stopped };

// Why the last attempt failed. Codec, Undecodable, Url and Speaker do not get better by trying
// again; the others are retried with the back-off.
enum class Fault : uint8_t { None, Url, Connect, Timeout, Tls, Http, Playlist, Codec, Undecodable, Speaker };

struct Status {
  Phase phase = Phase::Idle;
  Fault fault = Fault::None;
  int httpStatus = 0;
  // Milliseconds until the next attempt; -1 when none comes.
  int retryMs = -1;
  std::string error;
  // "mp3", "aac", "he-aac" or "he-aac-v2" once a frame decoded.
  std::string codec;
  // The station's own name from its icy-name header.
  std::string stationName;
  uint32_t bitrateKbps = 0;
  uint32_t sampleRate = 0;
  uint8_t channels = 0;
  uint8_t bufferPercent = 0;
  // When the phase began, on the monotonic clock.
  int64_t sinceMs = 0;

  bool operator==(const Status& o) const {
    return phase == o.phase && fault == o.fault && httpStatus == o.httpStatus && retryMs == o.retryMs &&
           error == o.error && codec == o.codec && stationName == o.stationName &&
           bitrateKbps == o.bitrateKbps && sampleRate == o.sampleRate && channels == o.channels &&
           bufferPercent == o.bufferPercent && sinceMs == o.sinceMs;
  }
  bool operator!=(const Status& o) const { return !(*this == o); }

  // A phase change starts its clock; the same phase keeps it.
  void enter(Phase next, int64_t nowMs) {
    if (phase != next) sinceMs = nowMs;
    phase = next;
    if (next != Phase::Error) {
      fault = Fault::None;
      error.clear();
      retryMs = -1;
    }
  }
  void fail(Fault why, const std::string& message, int retryInMs, int64_t nowMs, int status = 0) {
    enter(Phase::Error, nowMs);
    fault = why;
    error = message;
    retryMs = retryInMs;
    httpStatus = status;
  }
};

inline bool fatal(Fault fault) {
  return fault == Fault::Codec || fault == Fault::Undecodable || fault == Fault::Url || fault == Fault::Speaker;
}

inline const char* phaseName(Phase phase) {
  switch (phase) {
    case Phase::Idle: return "idle";
    case Phase::Connecting: return "connecting";
    case Phase::Buffering: return "buffering";
    case Phase::Playing: return "playing";
    case Phase::Error: return "error";
    case Phase::Stopped: return "stopped";
  }
  return "idle";
}

inline const char* faultName(Fault fault) {
  switch (fault) {
    case Fault::None: return "";
    case Fault::Url: return "url";
    case Fault::Connect: return "connect";
    case Fault::Timeout: return "timeout";
    case Fault::Tls: return "tls";
    case Fault::Http: return "http";
    case Fault::Playlist: return "playlist";
    case Fault::Codec: return "codec";
    case Fault::Undecodable: return "undecodable";
    case Fault::Speaker: return "speaker";
  }
  return "";
}

// How a station is asked to start: what radio.play() adds to the address.
struct StreamOptions {
  // Echoed in the status so an app knows the station is still its own; "" from anyone else.
  std::string id;
  // The new station comes up from silence over this long once its first frame plays.
  uint32_t fadeMs = 0;
  // Radio static, 0..100, under the wait for the first frame (and while it starves).
  uint8_t staticLevel = 0;
};

}
