#include "platform/tc002/audio/Tc002AudioStream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "core/net/Url.h"
#include "core/radio/IcyMetadata.h"
#include "core/radio/IcyStream.h"
#include "core/radio/PlaylistParser.h"
#include "core/radio/StationUrl.h"
#include "platform/linux/host/vendor/httplib.h"
#include "platform/linux/net/HttpClient.h"
#include "system/MonotonicClock.h"

namespace awtrix {
namespace tc002 {

struct StreamSource::Shared {
  std::mutex mutex;
  std::condition_variable changed;
  audio::StreamInputBuffer input{kRingBytes, audio::StreamInputBuffer::kMaxAdtsFrameBytes};
  uint32_t connection = 0;
  bool streaming = false;
  bool cancelled = false;
  std::string title;
  std::string error;
  bool titleNew = false;
  bool errorNew = false;
  std::string contentType;
  // The network's side of the status; the audio thread adds the decoder's.
  radio::Status status;
  uint32_t statusSeq = 0;
  uint32_t statusSeen = 0;
  net::SocketInterrupt interrupt;
};

namespace {

constexpr int kConnectTimeoutS = 5;

std::atomic<int> g_networkThreads{0};
std::mutex g_threadsMutex;
std::condition_variable g_threadsDone;

using Shared = StreamSource::Shared;

bool cancelled(Shared& shared) {
  std::lock_guard<std::mutex> lock(shared.mutex);
  return shared.cancelled;
}

bool pause(Shared& shared, int ms) {
  std::unique_lock<std::mutex> lock(shared.mutex);
  return !shared.changed.wait_for(lock, std::chrono::milliseconds(ms),
                                  [&] { return shared.cancelled; });
}

// Under the lock: a status change for whoever reads it next.
void setStatus(Shared& shared, const radio::Status& next) {
  if (next == shared.status) return;
  shared.status = next;
  ++shared.statusSeq;
}

void publishError(Shared& shared, const std::string& message, radio::Fault fault = radio::Fault::Connect,
                  int retryMs = -1, int httpStatus = 0) {
  std::lock_guard<std::mutex> lock(shared.mutex);
  shared.error = message;
  shared.errorNew = true;
  shared.streaming = false;
  radio::Status status = shared.status;
  status.fail(fault, message, retryMs, monotonicMs(), httpStatus);
  setStatus(shared, status);
}

void publishConnecting(Shared& shared) {
  std::lock_guard<std::mutex> lock(shared.mutex);
  radio::Status status = shared.status;
  status.enter(radio::Phase::Connecting, monotonicMs());
  setStatus(shared, status);
}

void publishTitle(Shared& shared, const std::string& title) {
  std::lock_guard<std::mutex> lock(shared.mutex);
  shared.title = title;
  shared.titleNew = true;
}

void beginConnection(Shared& shared, const std::string& type, const std::string& icyName, int icyBr,
                     int httpStatus) {
  std::lock_guard<std::mutex> lock(shared.mutex);
  shared.input.clear();
  ++shared.connection;
  shared.streaming = true;
  shared.error.clear();
  shared.errorNew = true;
  shared.contentType = type;
  radio::Status status = shared.status;
  status.enter(radio::Phase::Buffering, monotonicMs());
  status.httpStatus = httpStatus;
  status.stationName = icyName;
  if (icyBr > 0 && icyBr < 1000) status.bitrateKbps = static_cast<uint32_t>(icyBr);
  setStatus(shared, status);
  shared.changed.notify_all();
}

// Blocks while the ring is full: the socket then fills and TCP slows the station down.
bool push(Shared& shared, const uint8_t* data, std::size_t bytes) {
  std::unique_lock<std::mutex> lock(shared.mutex);
  while (bytes) {
    shared.changed.wait(lock, [&] { return shared.cancelled || shared.input.room(); });
    if (shared.cancelled) return false;
    const std::size_t room = std::min(bytes, shared.input.room());
    shared.input.push(data, room);
    data += room;
    bytes -= room;
  }
  return true;
}

enum class Fetch : uint8_t { Cancelled, Failed, Playlist, Ended };

// Why an attempt got no stream, for the status.
struct Failure {
  radio::Fault fault = radio::Fault::Connect;
  int httpStatus = 0;
  std::string message = "connect failed";
};

void explain(httplib::Error error, Failure& out) {
  switch (error) {
    case httplib::Error::ConnectionTimeout:
    case httplib::Error::Read:
      out = {radio::Fault::Timeout, 0, "no answer in time"};
      break;
    case httplib::Error::SSLConnection:
    case httplib::Error::SSLLoadingCerts:
    case httplib::Error::SSLServerVerification:
    case httplib::Error::SSLServerHostnameVerification:
      out = {radio::Fault::Tls, 0, "secure connection failed"};
      break;
    case httplib::Error::ExceedRedirectCount:
      out = {radio::Fault::Http, 0, "too many redirects"};
      break;
    default:
      out = {radio::Fault::Connect, 0, "connect failed"};
      break;
  }
}

Fetch fetch(Shared& shared, const net::Url& target, bool urlLooksLikePlaylist, std::string& body,
            Failure& failure) {
  httplib::Client client(target.origin());
  net::configure(client, {0, kConnectTimeoutS, true, radio::kReadTimeoutMs / 1000});
  shared.interrupt.watch(client);
  if (cancelled(shared)) return Fetch::Cancelled;
  bool accepted = false;
  bool playlist = false;
  radio::MetadataSplitter splitter;
  radio::TitleTracker titles;
  const httplib::Headers headers{{"Icy-MetaData", "1"}, {"User-Agent", "AWTRIX-NG"}};
  int answered = 0;
  const httplib::Result result = client.Get(
      target.requestTarget(), headers,
      [&](const httplib::Response& response) {
        answered = response.status;
        if (response.status != 200 || cancelled(shared)) return false;
        const std::string type = response.get_header_value("Content-Type");
        playlist = radio::isPlaylistType(type) ||
                   (urlLooksLikePlaylist && type.find("audio/") == std::string::npos);
        accepted = true;
        if (!playlist) {
          splitter.reset(std::atoi(response.get_header_value("icy-metaint").c_str()));
          beginConnection(shared, type, response.get_header_value("icy-name"),
                          std::atoi(response.get_header_value("icy-br").c_str()), response.status);
        }
        return true;
      },
      [&](const char* data, std::size_t bytes) {
        if (playlist) {
          if (body.size() + bytes > radio::kMaxPlaylistBytes) return false;
          body.append(data, bytes);
          return true;
        }
        bool okay = true;
        splitter.feed(
            reinterpret_cast<const uint8_t*>(data), bytes,
            [&](const uint8_t* audio, std::size_t n) {
              if (okay) okay = push(shared, audio, n);
            },
            [&](const std::string& block) {
              if (titles.update(block)) publishTitle(shared, titles.title());
            });
        return okay;
      });
  shared.interrupt.release();
  {
    std::lock_guard<std::mutex> lock(shared.mutex);
    shared.streaming = false;
    if (shared.cancelled) return Fetch::Cancelled;
  }
  if (!accepted) {
    if (answered && answered != 200)
      failure = {radio::Fault::Http, answered, "HTTP " + std::to_string(answered)};
    else
      explain(result.error(), failure);
    return Fetch::Failed;
  }
  return playlist ? Fetch::Playlist : Fetch::Ended;
}

void runNetwork(std::shared_ptr<Shared> shared, std::string station) {
  int attempt = 0;
  radio::StationUrl url(std::move(station));
  while (!cancelled(*shared)) {
    const auto target = net::parseUrl(url.current());
    if (!target) {
      publishError(*shared, "invalid URL", radio::Fault::Url, static_cast<int>(radio::kBackoffMs[2]));
      url.restart();
      pause(*shared, radio::kBackoffMs[2]);
      continue;
    }
    publishConnecting(*shared);
    std::string body;
    Failure failure;
    const Fetch result = fetch(*shared, *target,
                               radio::kindFromUrl(url.current()) != radio::PlaylistKind::None, body, failure);
    if (result == Fetch::Cancelled) break;
    if (result == Fetch::Playlist) {
      if (url.follow(body)) continue;
      publishError(*shared, "empty playlist", radio::Fault::Playlist, static_cast<int>(radio::kBackoffMs[2]));
      pause(*shared, radio::kBackoffMs[2]);
      continue;
    }
    url.restart();
    if (result == Fetch::Failed) {
      publishError(*shared, failure.message, failure.fault, static_cast<int>(radio::kBackoffMs[attempt]),
                   failure.httpStatus);
      pause(*shared, radio::kBackoffMs[attempt]);
      if (attempt < 2) ++attempt;
      continue;
    }
    attempt = 0;
    pause(*shared, radio::kBackoffMs[0]);
  }
  std::lock_guard<std::mutex> lock(g_threadsMutex);
  --g_networkThreads;
  g_threadsDone.notify_all();
}

}

StreamSource::StreamSource(const std::string& url, const radio::StreamOptions& options)
    : shared_(std::make_shared<Shared>()), staticOnly_(url.empty()) {
  decoder_.setOptions(options);
  if (staticOnly_) return;
  ++g_networkThreads;
  try {
    std::thread(runNetwork, shared_, url).detach();
  } catch (...) {
    --g_networkThreads;
    fatal_ = true;
  }
}

StreamSource::~StreamSource() {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->cancelled = true;
  }
  shared_->interrupt.interrupt();
  shared_->changed.notify_all();
}

bool StreamSource::waitForNetworkThreads(int timeoutMs) {
  std::unique_lock<std::mutex> lock(g_threadsMutex);
  return g_threadsDone.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                [] { return g_networkThreads.load() == 0; });
}

bool StreamSource::takeTitle(std::string& out) {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  if (!shared_->titleNew) return false;
  shared_->titleNew = false;
  out = shared_->title;
  return true;
}

bool StreamSource::takeError(std::string& out) {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  if (!shared_->errorNew) return false;
  shared_->errorNew = false;
  out = shared_->error;
  return true;
}

bool StreamSource::takeStatus(radio::Status& out) {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  out = shared_->status;
  out.bufferPercent = static_cast<uint8_t>(shared_->input.size() * 100 / kRingBytes);
  if (shared_->statusSeen == shared_->statusSeq) return false;
  shared_->statusSeen = shared_->statusSeq;
  return true;
}

bool StreamSource::connected() const {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->streaming;
}

void StreamSource::setStatic(uint8_t level) { decoder_.setStatic(level); }

// Under the lock, on the audio thread: what the decoder knows joins what the network said.
void StreamSource::updateStatus(radio::StreamDecoder::Step step, int64_t nowMs) {
  radio::Status status = shared_->status;
  if (step == radio::StreamDecoder::Step::Error) {
    status.fail(decoder_.fault(), decoder_.error(), -1, nowMs, status.httpStatus);
  } else if (shared_->streaming) {
    status.enter(decoder_.playing() ? radio::Phase::Playing : radio::Phase::Buffering, nowMs);
  }
  if (decoder_.playing()) {
    status.codec = decoder_.format();
    if (decoder_.bitrateKbps()) status.bitrateKbps = decoder_.bitrateKbps();
    status.sampleRate = decoder_.rate();
    status.channels = decoder_.channels();
  }
  setStatus(*shared_, status);
}

PcmSource::Read StreamSource::next(const int16_t*& samples, std::size_t& frames) {
  if (fatal_) return Read::Error;
  radio::StreamDecoder::Block block;
  if (staticOnly_) {
    if (decoder_.noise(block) != radio::StreamDecoder::Step::Data) return Read::Wait;
  } else {
    std::unique_lock<std::mutex> lock(shared_->mutex);
    auto& input = shared_->input;
    if (shared_->connection != seenConnection_) {
      seenConnection_ = shared_->connection;
      decoder_.restart(shared_->contentType);
    }
    const int64_t now = monotonicMs();
    const std::size_t before = input.size();
    const auto step = decoder_.next(input, now, block);
    buffered_ = static_cast<uint32_t>(input.size());
    if (input.size() != before) shared_->changed.notify_all();
    updateStatus(step, now);
    lock.unlock();
    if (step == radio::StreamDecoder::Step::Error) {
      publishError(*shared_, decoder_.error(), decoder_.fault());
      fatal_ = true;
      return Read::Error;
    }
    if (step == radio::StreamDecoder::Step::Wait) return Read::Wait;
  }
  synthetic_ = block.synthetic;
  rate_ = block.rate;
  channels_ = block.channels;
  samples = block.samples;
  frames = block.frames;
  return Read::Data;
}

}
}
