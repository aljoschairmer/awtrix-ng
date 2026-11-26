#include "platform/tc002/daemon/McuService.h"
#include "platform/posix/Text.h"

#include <asm/ioctls.h>
#include <asm/termbits.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <csignal>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "platform/tc002/daemon/Log.h"
#include "platform/tc002/daemon/Process.h"

namespace awtrix {
namespace tc002d {
namespace {

constexpr unsigned kBaud = 1500000;
static_assert(B1500000 == 0010012, "Linux B1500000 encoding");

bool rawLine(const termios2& t) {
  return t.c_iflag == 0 && t.c_oflag == 0 && t.c_lflag == 0 && t.c_line == 0 && t.c_ispeed == kBaud &&
         t.c_ospeed == kBaud && (t.c_cflag & CSIZE) == CS8 && (t.c_cflag & (CREAD | CLOCAL)) == (CREAD | CLOCAL) &&
         !(t.c_cflag & (PARENB | PARODD | CSTOPB | CRTSCTS | CMSPAR));
}

int64_t earliest(int64_t a, int64_t b) {
  if (a < 0) return b;
  if (b < 0) return a;
  return a < b ? a : b;
}

}

McuService::McuService(DeviceState& state, McuOptions options) : state_(state), options_(std::move(options)) {
  firmware_.onStatus([](const std::string& status) { Log::line("mcu", "update: %s", status.c_str()); });
}

int McuService::batteryMillivolts(uint16_t word) {
  const int64_t millivolts = (static_cast<int64_t>(word) * 13235294 + 5000000) / 10000000;
  return millivolts <= 10000 ? static_cast<int>(millivolts) : -1;
}

bool McuService::start(int64_t nowMs) {
  stopping_ = false;
  firmware_.load(options_.firmwareDirectory, options_.firmwareJournal);
  prepared_ = false;
  prepareRetryAt_ = -1;
  prepareStatus_.clear();
  openUart(nowMs);
  return true;
}

bool McuService::openUart(int64_t nowMs) {
  posix::UniqueFd fd(::open(options_.path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
  termios2 current{};
  const char* failure = nullptr;
  if (!fd.valid()) failure = "open";
  else if (::ioctl(fd.get(), TCGETS2, &current) < 0) failure = "read termios";
  if (!failure) {
    observedCflag_ = current.c_cflag;
    termiosAdjusted_ = false;
    if (!rawLine(current)) {
      termios2 wanted = current;
      wanted.c_iflag = 0;
      wanted.c_oflag = 0;
      wanted.c_lflag = 0;
      wanted.c_line = 0;
      wanted.c_cflag = (current.c_cflag & HUPCL) | B1500000 | CS8 | CREAD | CLOCAL;
      wanted.c_ispeed = kBaud;
      wanted.c_ospeed = kBaud;
      termios2 readback{};
      if (::ioctl(fd.get(), TCSETS2, &wanted) < 0 || ::ioctl(fd.get(), TCGETS2, &readback) < 0) failure = "set termios";
      else if (!rawLine(readback)) {
        errno = EPROTO;
        failure = "verify termios";
      } else {
        termiosAdjusted_ = true;
        Log::line("mcu", "configured %s: cflag 0x%x -> 0x%x, %u baud", options_.path.c_str(),
                  static_cast<unsigned>(current.c_cflag), static_cast<unsigned>(readback.c_cflag), kBaud);
      }
    }
  }
  if (failure) {
    const int error = errno;
    if (openFailures_ == 0 || error != lastOpenError_ || openFailures_ % 12 == 0)
      Log::line("mcu", "cannot use %s (%s: %s), retrying every %lld ms", options_.path.c_str(), failure,
                std::strerror(error), static_cast<long long>(options_.reopenDelayMs));
    ++openFailures_;
    lastOpenError_ = error;
    reopenAt_ = nowMs + options_.reopenDelayMs;
    return false;
  }
  if (!termiosAdjusted_)
    Log::line("mcu", "opened %s: cflag 0x%x already raw at %u baud", options_.path.c_str(),
              static_cast<unsigned>(current.c_cflag), kBaud);
  uart_ = std::move(fd);
  ++opens_;
  openFailures_ = 0;
  reopenAt_ = -1;
  parser_.clear();
  outstanding_ = 0;
  outstandingDeadline_ = -1;
  versionTries_ = 0;
  version_.clear();
  versionAt_ = nowMs;
  resetIdentity(nowMs);
  usbSeen_ = batterySeen_ = false;
  usbRefreshRequired_ = true;
  usbAt_ = nowMs;
  batteryAt_ = nowMs;
  pump(nowMs);
  return true;
}

void McuService::closeUart(int64_t nowMs, const char* reason) {
  if (!uart_.valid()) return;
  if (upgrading_) { upgrade_.fail(reason); firmwareProgress(nowMs); }
  uart_.reset();
  cancelPendingStream(reason, nowMs);
  stream_.abort(reason, nowMs);
  pcm_.abort(reason);
  finishPcm(nowMs);
  outstanding_ = 0;
  outstandingDeadline_ = -1;
  if (stopping_) {
    Log::line("mcu", "closed %s", options_.path.c_str());
    return;
  }
  Log::line("mcu", "closed %s after %s; reopening in %lld ms", options_.path.c_str(), reason,
            static_cast<long long>(options_.reopenDelayMs));
  reopenAt_ = nowMs + options_.reopenDelayMs;
}

void McuService::pollInterest(std::vector<PollInterest>& out) const {
  if (uart_.valid()) out.push_back({uart_.get(), static_cast<short>(POLLIN |
      ((upgrading_ && upgrade_.pending()) || (pcm_.active() && pcmSent_ < sizeof pcmTx_) ? POLLOUT : 0))});
}

void McuService::onReady(int fd, short revents, int64_t nowMs) {
  if (fd != uart_.get()) return;
  if (upgrading_ && (revents & POLLOUT)) { upgrade_.flush(fd, nowMs); firmwareProgress(nowMs); }
  if (pcm_.active() && (revents & POLLOUT)) flushPcm(nowMs);
  if (!uart_.valid()) return;
  if (revents & POLLIN) {
    // Match the parser's storage capacity, but retain room in its read budget
    // for a partial maximum-sized frame left from the previous read.
    uint8_t bytes[mcu::FrameParser::kCapacity];
    constexpr auto readBudget = sizeof bytes - mcu::kMaxPayload - mcu::kQueryBytes;
    for (int round = 0; round < 4; ++round) {
      const ssize_t count = ::read(fd, bytes, readBudget);
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      // The stock raw tty may retain VMIN=0/VTIME=0. An empty nonblocking read
      // then returns zero rather than EAGAIN, including just after a full block.
      // Only a poll hangup/error makes that a disconnect.
      if (count == 0 && !(revents & (POLLHUP | POLLERR | POLLNVAL))) break;
      if (count <= 0) {
        closeUart(nowMs, count == 0 ? "end of file" : std::strerror(errno));
        publish(nowMs);
        return;
      }
      rxBytes_ += static_cast<uint64_t>(count);
      if (upgrading_) {
        upgrade_.receive(bytes, static_cast<std::size_t>(count), nowMs);
        firmwareProgress(nowMs);
      } else {
        const auto beforePush = parser_.discardedBytes();
        parser_.push(bytes, static_cast<std::size_t>(count));
        if (parser_.discardedBytes() != beforePush) stream_.fail("microphone UART receive overflow", nowMs);
        mcu::Frame frame;
        for (;;) {
          const auto discarded = parser_.discardedBytes();
          const bool next = parser_.next(frame);
          if (parser_.discardedBytes() != discarded) {
            pcm_.corrupt();
            stream_.fail("microphone UART frame lost or corrupt", nowMs);
          }
          if (!next) break;
          handle(frame, nowMs);
        }
      }
      if (!uart_.valid()) return;
      if (static_cast<std::size_t>(count) < readBudget) break;
    }
  }
  if (uart_.valid() && (revents & (POLLERR | POLLHUP | POLLNVAL))) {
    closeUart(nowMs, "hangup");
  }
  pump(nowMs);
  publish(nowMs);
}

void McuService::handle(const mcu::Frame& frame, int64_t nowMs) {
  ++frames_;
  bool reply = false;
  switch (frame.command) {
    case mcu::kPcm:
      if (stream_.active()) {
        stream_.receive(frame, nowMs, [&](const uint8_t* bytes, std::size_t size) {
          // Small control frame: never queue a partial/late response into a
          // subsequent RX window. Failure stops renewal and drains the MCU lease.
          ssize_t n;
          do { n = ::write(uart_.get(), bytes, size); } while (n < 0 && errno == EINTR);
          if (n == static_cast<ssize_t>(size)) return true;
          ++writeErrors_;
          return false;
        });
        if (stream_.powerValid()) {
          usbRaw_ = stream_.usb(); batteryFirst_ = stream_.batteryFirst(); batteryWord_ = stream_.batteryWord();
          usbSeen_ = true;
          usbSeenAt_ = nowMs;
          batteryMeasured(nowMs);
        }
        finishStream(nowMs);
        return;
      }
      pcm_.receive(frame);
      finishPcm(nowMs);
      return;
    case mcu::kIdentity:
      identitySeen_ = mcu::Identity::decode(frame.payload, frame.length, identity_);
      identityMalformed_ = identityMalformed_ || !identitySeen_;
      identityAt_ = -1;
      reply = true;
      break;
    case mcu::kUsb:
      if (frame.length != 1) break;
      usbRaw_ = frame.payload[0];
      usbSeen_ = true;
      usbRefreshRequired_ = false;
      usbSeenAt_ = nowMs;
      reply = true;
      break;
    case mcu::kBattery:
      if (frame.length != 3) break;
      batteryFirst_ = frame.payload[0];
      batteryWord_ = static_cast<uint16_t>((frame.payload[1] << 8) | frame.payload[2]);
      batteryMeasured(nowMs);
      reply = true;
      break;
    case mcu::kVersion: {
      if (frame.length == 0) break;
      const std::string text = posix::printable(
          std::string_view(reinterpret_cast<const char*>(frame.payload), frame.length));
      if (text != version_) Log::line("mcu", "firmware %s", text.c_str());
      version_ = text;
      versionAt_ = -1;
      reply = true;
      break;
    }
    case mcu::kAck:
      ++acks_;
      return;
    case mcu::kDriverRead:
    case mcu::kDriverWrite:
    case mcu::kDriverTableRead:
    case mcu::kDriverTableWrite:
    case mcu::kDriverClearOverride:
      if (outstanding_ != frame.command) break;
      driverReply(frame);
      reply = true;
      break;
    case mcu::kMicrophone:
      return; // Ignore legacy level reports left enabled by an earlier owner.
    default:
      if (++unknown_ <= 3) Log::line("mcu", "ignored frame 0x%02x length %u", frame.command, frame.length);
      return;
  }
  if (!reply) {
    if (++unknown_ <= 3) Log::line("mcu", "ignored frame 0x%02x with length %u", frame.command, frame.length);
    return;
  }
  if (outstanding_ == frame.command) {
    outstanding_ = 0;
    outstandingDeadline_ = -1;
  }
}

void McuService::batteryMeasured(int64_t nowMs) {
  batterySeen_ = true;
  batterySeenAt_ = nowMs;
  if (usbSeen_) gauge_.sample(batteryMillivolts(batteryWord_), usbRaw_ == 1, nowMs, panelLoad_.steady(nowMs));
}

void McuService::setPanelLoad(int perMille, int64_t nowMs) { panelLoad_.report(perMille, nowMs); }

void McuService::pump(int64_t nowMs) {
  if (stream_.active()) return;
  if (pcm_.active()) { flushPcm(nowMs); return; }
  if (upgrading_) {
    upgrade_.flush(uart_.get(), nowMs);
    firmwareProgress(nowMs);
    return;
  }
  if (!uart_.valid() || stopping_ || upgradeHalted_) return;
  if (verifyStart_ >= 0 && nowMs < verifyStart_) return;
  if (verificationExpired(nowMs)) return;
  if (outstanding_ && nowMs >= outstandingDeadline_) {
    ++timeouts_;
    if (outstanding_ == mcu::kDriverRead || outstanding_ == mcu::kDriverWrite || outstanding_ == mcu::kDriverTableRead ||
        outstanding_ == mcu::kDriverTableWrite || outstanding_ == mcu::kDriverClearOverride) {
      ++driverFailures_;
      driverError_ = "no answer for an LED driver register request";
    }
    outstanding_ = 0;
    outstandingDeadline_ = -1;
  }
  if (outstanding_) return;
  if (pendingStream_) {
    const int epoch = pendingStream_;
    auto consumer = std::move(pendingStreamConsumer_);
    pendingStream_ = 0; pendingStreamAt_ = -1;
    if (!startStream(epoch, consumer, nowMs)) {
      tc002::StreamEvent event;
      event.epoch = epoch; event.kind = tc002::StreamEvent::Kind::Ended;
      event.hostAtMs = nowMs; event.error = "microphone became unavailable before start";
      consumer(event);
    }
    return;
  }
  if (!options_.firmwareDirectory.empty() && identityAt_ < 0 && versionAt_ < 0) {
    const bool needed = firmware_.needed(version_, identitySeen_ ? &identity_ : nullptr, identityMalformed_);
    if (!needed && preparePid_<0) prepareStatus_.clear();
    if (verifyDeadline_ >= 0 && identitySeen_ && !firmware_.pending()) {
      verifyDeadline_ = verifyStart_ = -1;
      Log::line("mcu", "installed MCU build %u verified", identity_.version);
    }
    if (needed && verifyDeadline_ < 0 && updateAllowed_ && updateAllowed_() &&
        usbSeen_ && !usbRefreshRequired_ && usbRaw_ == 1 && nowMs - usbSeenAt_ < 2000 &&
        prepareFirmware(nowMs) && firmware_.recordAttempt()) {
      parser_.clear();
      upgrading_ = true;
      upgrade_.start(firmware_.image(), nowMs);
      upgrade_.flush(uart_.get(), nowMs);
      firmwareProgress(nowMs);
      return;
    }
  }
  uint8_t command = 0;
  if (versionAt_ >= 0 && nowMs >= versionAt_) command = mcu::kVersion;
  else if (identityAt_ >= 0 && nowMs >= identityAt_) command = mcu::kIdentity;
  else if (nowMs >= usbAt_) command = mcu::kUsb;
  else if (nowMs >= batteryAt_) command = mcu::kBattery;
  else if (!driverJobs_.empty() && driverRegistersAvailable()) {
    sendDriverJob(nowMs);
    return;
  }
  if (!command) return;
  if (command == mcu::kVersion) {
    ++versionTries_;
    versionAt_ = versionTries_ < options_.versionAttempts ? nowMs + options_.versionRetryMs : -1;
  } else if (command == mcu::kIdentity) {
    ++identityTries_;
    identityAt_ = identityTries_ < options_.versionAttempts ? nowMs + options_.versionRetryMs : -1;
  } else if (command == mcu::kUsb) {
    usbAt_ = nowMs + options_.usbIntervalMs;
  } else {
    batteryAt_ = nowMs + options_.batteryIntervalMs;
  }
  uint8_t query[mcu::kQueryBytes];
  if (!mcu::encodeQuery(command, query) || !writeFrame(query, sizeof query, command, nowMs)) return;
  ++queries_;
  outstanding_ = command;
  outstandingDeadline_ = nowMs + options_.replyTimeoutMs;
}

bool McuService::pcmAvailable() const {
  return uart_.valid() && !stopping_ && !upgradeHalted_ && identitySeen_ && !identityMalformed_ &&
      identity_.abi == 1 && identity_.family == 123 && (identity_.features & 1);
}

bool McuService::streamAvailable() const {
  return pcmAvailable() && identity_.version >= 13 && (identity_.features & 8);
}

bool McuService::startStream(int epoch, mcu::Stream::Consumer consumer, int64_t nowMs) {
  if (!streamAvailable() || firmwareBusy() || stream_.active() || pendingStream_ || epoch <= 0 || !consumer) return false;
  if (pcm_.active() || outstanding_) {
    pendingStream_ = epoch; pendingStreamAt_ = nowMs + 500; pendingStreamConsumer_ = std::move(consumer);
    return true;
  }
  if (!stream_.start(epoch, nowMs, std::move(consumer))) return false;
  const auto frame = mcu::Stream::startFrame(epoch);
  ssize_t sent;
  do { sent = ::write(uart_.get(), frame.data(), frame.size()); } while (sent < 0 && errno == EINTR);
  if (sent != static_cast<ssize_t>(frame.size())) {
    ++writeErrors_;
    stream_.fail("microphone start write failed", nowMs);
  }
  return true; // Admitted: exactly one terminal event follows, including on write failure.
}

void McuService::controlStream(int epoch, bool stop, int64_t nowMs) {
  if (pendingStream_ == epoch) {
    if (stop) cancelPendingStream("microphone start cancelled", nowMs);
    return;
  }
  if (epoch != stream_.epoch()) return;
  if (stop) stream_.stop(nowMs); else stream_.renew(epoch, nowMs);
}

void McuService::cancelPendingStream(const char* reason, int64_t nowMs) {
  if (!pendingStream_) return;
  tc002::StreamEvent event;
  event.kind = tc002::StreamEvent::Kind::Ended; event.epoch = pendingStream_;
  event.hostAtMs = nowMs; event.error = reason;
  auto consumer = std::move(pendingStreamConsumer_);
  pendingStream_ = 0; pendingStreamAt_ = -1;
  if (consumer) consumer(event);
}

void McuService::finishStream(int64_t nowMs) {
  if (stream_.active()) return;
  usbRefreshRequired_ = true;
  usbAt_ = nowMs;
  if (stopping_ && uart_.valid()) requestStop(nowMs);
}

bool McuService::requestPcm(unsigned halves, PcmCallback done, int64_t nowMs) {
  if (!pcmAvailable() || busy() || outstanding_ || !done || !pcm_.start(halves, nowMs)) return false;
  pcmDone_ = std::move(done);
  pcmError_.clear();
  mcu::encodePcmRequest(static_cast<uint16_t>(halves), pcmTx_);
  pcmSent_ = 0;
  flushPcm(nowMs);
  return true;
}

void McuService::flushPcm(int64_t nowMs) {
  while (pcm_.active() && pcmSent_ < sizeof pcmTx_) {
    const ssize_t n = ::write(uart_.get(), pcmTx_ + pcmSent_, sizeof pcmTx_ - pcmSent_);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (n <= 0) { closeUart(nowMs, "PCM request write failed"); return; }
    pcmSent_ += static_cast<std::size_t>(n);
  }
}

void McuService::finishPcm(int64_t nowMs) {
  if (pcm_.active() || !pcmDone_) return;
  if (pcm_.succeeded()) ++pcmCaptures_;
  else { ++pcmFailures_; pcmError_ = pcm_.error(); }
  auto done = std::move(pcmDone_);
  // Resume power polling immediately; do not reuse pre-capture USB information for flashing.
  usbRefreshRequired_ = true;
  usbAt_ = nowMs;
  done(pcm_);
  if (stopping_ && uart_.valid()) requestStop(nowMs);
}

void McuService::resetIdentity(int64_t nowMs) {
  identitySeen_ = identityMalformed_ = false;
  identityTries_ = 0;
  identityAt_ = nowMs;
}

bool McuService::prepareFirmware(int64_t nowMs) {
  if (prepared_) return true;
  if (preparePid_ > 0 || (prepareRetryAt_ >= 0 && nowMs < prepareRetryAt_)) return false;
  if (options_.firmwareHelper.empty() || options_.firmwareCache.empty()) {
    firmware_.failed("MCU local preparation helper unavailable"); return false;
  }
  ProcessSpec spec;
  spec.path=options_.firmwareHelper;
  spec.argv={spec.path};
  spec.argv.insert(spec.argv.end(), options_.firmwareHelperArguments.begin(), options_.firmwareHelperArguments.end());
  spec.argv.insert(spec.argv.end(), {options_.firmwareDirectory,options_.firmwareCache,options_.firmwareCaFile});
  spec.parentDeathSignal=SIGKILL; spec.group=ProcessGroup::Own; spec.umask=0077;
  spec.descriptors={{STDERR_FILENO,STDERR_FILENO}};
  std::string why;
  preparePid_=spawnProcess(spec,why);
  if (preparePid_<0) { firmware_.failed("cannot start MCU local preparation"); return false; }
  prepareDeadline_=nowMs+60000;
  prepareStatus_="preparing locally; downloading original only if missing";
  Log::line("mcu","preparing build %u from local extension and verified original",firmware_.target().version);
  return false; // Always refresh the normal power/identity policy before flashing.
}

bool McuService::onChildExit(pid_t pid,int status,int64_t nowMs) {
  if (pid != preparePid_) return false;
  preparePid_=-1; prepareDeadline_=-1;
  if (stopping_) return true;
  const int code=WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (code==0 && firmware_.loadPrepared(options_.firmwareCache+"/prepared.pot")) {
    prepared_=true; prepareStatus_.clear();
    // No reuse of USB readings obtained before the child completed.
    usbRefreshRequired_=true; usbAt_=nowMs;
    Log::line("mcu","local image verified against release SHA-256");
  } else if (code==11 || code==-1) {
    prepareStatus_="original download unavailable; retry in 60 seconds";
    prepareRetryAt_=nowMs+60000;
    Log::line("mcu","preparation %s; normal operation continues",describeWait(status).c_str());
  } else {
    prepareStatus_.clear();
    firmware_.failed(code==12 ? "MCU original SHA-256 mismatch; update blocked" :
                               "MCU local preparation failed; update blocked");
  }
  return true;
}

void McuService::firmwareProgress(int64_t nowMs) {
  if (!upgrading_ || upgrade_.active()) return;
  upgrading_ = false;
  if (upgrade_.resumedAt())
    Log::line("mcu", "transfer continued an interrupted one from byte %zu of %zu", upgrade_.resumedAt(), firmware_.image().size());
  if (!upgrade_.succeeded()) {
    firmware_.attemptFailed(upgrade_.error());
    upgradeHalted_ = true;
    // The last readings stay published until they go stale; only a fresh one may allow a flash.
    usbRefreshRequired_ = true;
  } else {
    // The tested MCU updater reboots the device. Persisted intent survives that reboot;
    // if Linux stays up, query the running build here as well.
    verifyStart_ = nowMs + 1500;
    verifyDeadline_ = nowMs + 30000;
    version_.clear();
    versionTries_ = 0;
    versionAt_ = verifyStart_;
    resetIdentity(verifyStart_);
    outstanding_ = 0;
    outstandingDeadline_ = -1;
    usbRefreshRequired_ = true;
    usbAt_ = batteryAt_ = verifyStart_;
    parser_.clear();
    Log::line("mcu", "transfer acknowledged; awaiting running build verification");
  }
  if (stopping_) {
    verifyDeadline_ = verifyStart_ = -1;
    closeUart(nowMs, "update completed during stop");
  }
}

bool McuService::verificationExpired(int64_t nowMs) {
  if (verifyDeadline_ < 0 || nowMs < verifyDeadline_) return false;
  firmware_.attemptFailed("MCU build not confirmed after transfer");
  verifyDeadline_ = verifyStart_ = -1;
  upgradeHalted_ = true;
  return true;
}

bool McuService::writeFrame(const uint8_t* bytes, std::size_t length, uint8_t command, int64_t nowMs) {
  ssize_t sent;
  do { sent = ::write(uart_.get(), bytes, length); } while (sent < 0 && errno == EINTR);
  if (sent == static_cast<ssize_t>(length)) return true;
  if (++writeErrors_ <= 3 || writeErrors_ % 100 == 0)
    Log::line("mcu", "command 0x%02x write returned %zd (%s)", command, sent, sent < 0 ? std::strerror(errno) : "short");
  if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) closeUart(nowMs, "write error");
  return false;
}

bool McuService::driverRegistersAvailable() const {
  // The register commands and their table were read from V1.0.17; another build may differ.
  return uart_.valid() && !stopping_ && !upgradeHalted_ && !firmwareBusy() && version_ == "V1.0.17";
}

bool McuService::driverTableAvailable() const {
  return driverRegistersAvailable() && pcmAvailable() && identity_.version >= 14 &&
      (identity_.features & mcu::kFeatureDriverTable);
}

namespace {

std::string hexRegister(uint8_t reg) {
  static const char digits[] = "0123456789abcdef";
  return std::string("0x") + digits[reg >> 4] + digits[reg & 15];
}

}

std::string McuService::readDriverRegister(uint8_t reg) {
  if (reg < mcu::kDriverFirstRegister || reg > mcu::kDriverLastRegister)
    return "register " + hexRegister(reg) + " is outside 0x02 to 0x20";
  if (!driverRegistersAvailable()) return "the MCU does not answer LED driver register requests now";
  if (driverJobs_.size() >= kDriverQueue) return "too many register requests queued";
  driverJobs_.push_back(DriverJob{DriverOp::Read, reg, {}});
  return {};
}

std::string McuService::readDriverRegisters() {
  if (!driverRegistersAvailable()) return "the MCU does not answer LED driver register requests now";
  if (driverJobs_.size() + driver_.size() > kDriverQueue) return "too many register requests queued";
  for (std::size_t i = 0; i < driver_.size(); ++i)
    driverJobs_.push_back(DriverJob{DriverOp::Read, static_cast<uint8_t>(mcu::kDriverFirstRegister + i), {}});
  return {};
}

std::string McuService::writeDriverRegister(uint8_t reg, const uint8_t (&values)[3]) {
  if (reg < mcu::kDriverFirstRegister || reg > mcu::kDriverLastRegister)
    return "register " + hexRegister(reg) + " is outside 0x02 to 0x20";
  if (!driverRegistersAvailable()) return "the MCU does not answer LED driver register requests now";
  if (driverJobs_.size() + 1 >= kDriverQueue) return "too many register requests queued";
  DriverJob job{DriverOp::Write, reg, {}};
  std::memcpy(job.values, values, sizeof job.values);
  driverJobs_.push_back(job);
  return {};
}

std::string McuService::clearDriverOverride() {
  if (!driverTableAvailable()) return "clearing the override needs MCU build 14";
  if (driverJobs_.size() + 1 >= kDriverQueue) return "too many register requests queued";
  driverJobs_.push_back(DriverJob{DriverOp::Clear, 0, {}});
  return {};
}

std::string McuService::restoreDriverRegisters() {
  if (!driverRegistersAvailable()) return "the MCU does not answer LED driver register requests now";
  if (driverTableAvailable()) {
    // The table may hold an override loaded at start; clearing it and writing every factory value
    // leaves nothing behind.
    if (driverJobs_.size() + 1 + driver_.size() > kDriverQueue) return "too many register requests queued";
    driverJobs_.push_back(DriverJob{DriverOp::Clear, 0, {}});
    for (std::size_t i = 0; i < driver_.size(); ++i) {
      DriverJob job{DriverOp::Write, static_cast<uint8_t>(mcu::kDriverFirstRegister + i), {}};
      std::memcpy(job.values, mcu::kDriverFactory[i], sizeof job.values);
      driverJobs_.push_back(job);
    }
    return {};
  }
  std::array<bool, mcu::kDriverLastRegister - mcu::kDriverFirstRegister + 1> due{};
  std::size_t count = 0;
  for (std::size_t i = 0; i < driver_.size(); ++i) {
    const bool overridden = overrideKnown_ && overrideRegister_ == mcu::kDriverFirstRegister + i &&
                            std::memcmp(overrideValue_, mcu::kDriverFactory[i], 3) != 0;
    due[i] = overridden || (driver_[i].known && std::memcmp(driver_[i].value, mcu::kDriverFactory[i], 3) != 0);
    count += due[i];
  }
  if (driverJobs_.size() + count > kDriverQueue) return "too many register requests queued";
  // The override ends as the last one written; a changed override goes last so it is the one kept.
  for (int pass = 0; pass < 2; ++pass) {
    for (std::size_t i = 0; i < driver_.size(); ++i) {
      const bool isOverride = overrideKnown_ && overrideRegister_ == mcu::kDriverFirstRegister + i;
      if (!due[i] || isOverride != (pass == 1)) continue;
      DriverJob job{DriverOp::Write, static_cast<uint8_t>(mcu::kDriverFirstRegister + i), {}};
      std::memcpy(job.values, mcu::kDriverFactory[i], sizeof job.values);
      driverJobs_.push_back(job);
    }
  }
  return {};
}

bool McuService::sendDriverJob(int64_t nowMs) {
  const DriverJob job = driverJobs_.front();
  driverJobs_.pop_front();
  const bool table = driverTableAvailable();
  if (job.op == DriverOp::Clear && !table) return false;
  uint8_t command = 0;
  bool sent = false;
  if (job.op == DriverOp::Clear) {
    uint8_t clear[mcu::kQueryBytes];
    command = mcu::kDriverClearOverride;
    sent = mcu::encodeQuery(command, clear) && writeFrame(clear, sizeof clear, command, nowMs);
  } else if (job.op == DriverOp::Write) {
    uint8_t write[mcu::kDriverWriteBytes];
    command = table ? mcu::kDriverTableWrite : mcu::kDriverWrite;
    sent = mcu::encodeDriverWrite(job.reg, job.values, write, command) && writeFrame(write, sizeof write, command, nowMs);
  } else {
    uint8_t read[mcu::kDriverReadBytes];
    command = table ? mcu::kDriverTableRead : mcu::kDriverRead;
    sent = mcu::encodeDriverRead(job.reg, read, command) && writeFrame(read, sizeof read, command, nowMs);
  }
  if (!sent) return false;
  driverSent_ = job;
  ++queries_;
  outstanding_ = command;
  outstandingDeadline_ = nowMs + options_.replyTimeoutMs;
  return true;
}

void McuService::driverReply(const mcu::Frame& frame) {
  const DriverJob job = driverSent_;
  if (frame.command == mcu::kDriverClearOverride) {
    overrideKnown_ = false;
    Log::line("mcu", "LED driver override cleared");
    return;
  }
  uint8_t reg = 0, values[3];
  const bool table = frame.command == mcu::kDriverTableRead || frame.command == mcu::kDriverTableWrite;
  if (table && frame.length == 0) {
    driverFailed("the MCU refused the request for LED driver register " + hexRegister(job.reg));
    return;
  }
  if (!mcu::decodeDriverReply(frame, reg, values) || ((table || frame.command == mcu::kDriverWrite) && reg != job.reg)) {
    driverFailed("the MCU did not answer for LED driver register " + hexRegister(job.reg));
    return;
  }
  DriverRegister& r = driver_[reg - mcu::kDriverFirstRegister];
  std::memcpy(r.value, values, sizeof r.value);
  r.known = true;
  if (frame.command == mcu::kDriverTableWrite) {
    ++driverWrites_;
    Log::line("mcu", "LED driver register 0x%02x set to %02x %02x %02x", reg, values[0], values[1], values[2]);
    return;
  }
  if (frame.command == mcu::kDriverWrite) {
    // The answer repeats the request, which is now the override; reading it back would only
    // return the override again.
    ++driverWrites_;
    overrideKnown_ = true;
    overrideRegister_ = reg;
    std::memcpy(overrideValue_, values, sizeof overrideValue_);
    Log::line("mcu", "LED driver register 0x%02x set to %02x %02x %02x (saved as the MCU's override)", reg,
              values[0], values[1], values[2]);
    return;
  }
  if (reg != job.reg) {
    // An override exists: the MCU answers every read with it.
    ++overrideAnswers_;
    overrideKnown_ = true;
    overrideRegister_ = reg;
    std::memcpy(overrideValue_, values, sizeof overrideValue_);
    driverError_ = "the MCU answers every read with its saved override, register " + hexRegister(reg) +
                   "; other registers cannot be read";
    return;
  }
  ++driverReads_;
}

void McuService::driverFailed(const std::string& why) {
  ++driverFailures_;
  driverError_ = why;
}

void McuService::appendDriverRegisters(api::JsonWriter& json) const {
  json.beginObject().member("available", driverRegistersAvailable()).member("firmware", version_)
      .member("backend", driverTableAvailable() ? "extension" : "stock")
      .member("queued", static_cast<unsigned long long>(driverJobs_.size()))
      .member("reads", static_cast<unsigned long long>(driverReads_))
      .member("writes", static_cast<unsigned long long>(driverWrites_))
      .member("failures", static_cast<unsigned long long>(driverFailures_)).member("error", driverError_)
      .member("overrideAnswers", static_cast<unsigned long long>(overrideAnswers_));
  const auto values = [&json](const char* key, const uint8_t* v) {
    json.key(key).beginArray();
    for (int c = 0; c < 3; ++c) json.value(static_cast<unsigned>(v[c]));
    json.endArray();
  };
  json.key("override");
  if (overrideKnown_) {
    const uint8_t* factory = mcu::kDriverFactory[overrideRegister_ - mcu::kDriverFirstRegister];
    json.beginObject().member("register", static_cast<unsigned>(overrideRegister_));
    values("value", overrideValue_);
    values("factory", factory);
    json.member("changed", std::memcmp(overrideValue_, factory, 3) != 0).endObject();
  } else {
    json.null();
  }
  json.key("registers").beginArray();
  for (std::size_t i = 0; i < driver_.size(); ++i) {
    const DriverRegister& r = driver_[i];
    json.beginObject().member("register", static_cast<unsigned>(mcu::kDriverFirstRegister + i));
    values("factory", mcu::kDriverFactory[i]);
    if (r.known) values("value", r.value);
    json.member("known", r.known)
        .member("changed", r.known && std::memcmp(r.value, mcu::kDriverFactory[i], 3) != 0).endObject();
  }
  json.endArray().endObject();
}

void McuService::publish(int64_t nowMs) {
  tc002::PowerStatus power;
  const bool usbFresh = usbSeen_ && nowMs - usbSeenAt_ < options_.staleMs;
  const bool batteryFresh = batterySeen_ && nowMs - batterySeenAt_ < options_.staleMs;
  power.usbKnown = usbFresh;
  power.usbPower = usbFresh && usbRaw_ == 1;
  if (batteryFresh) {
    power.batteryPercent = gauge_.percent();
    power.batteryMillivolts = batteryMillivolts(batteryWord_);
  }
  const auto& current = state_.power();
  if (current.usbKnown == power.usbKnown && current.usbPower == power.usbPower &&
      current.batteryPercent == power.batteryPercent &&
      current.batteryMillivolts == power.batteryMillivolts)
    return;
  state_.setPower(power);
}

int64_t McuService::nextDeadlineMs() const {
  if (stream_.active()) return stream_.deadline();
  if (pcm_.active()) return earliest(pcm_.deadline(), pendingStreamAt_);
  if (upgrading_) return upgrade_.deadline();
  if (stopping_) return -1;
  int64_t due = -1;
  if (!upgradeHalted_) {
    if (uart_.valid() && verifyStart_ >= 0 && versionTries_ == 0) return verifyStart_;
    if (!uart_.valid()) due = reopenAt_;
    else if (outstanding_) due = outstandingDeadline_;
    else due = earliest(earliest(earliest(versionAt_, identityAt_), usbAt_), batteryAt_);
    if (uart_.valid() && !outstanding_ && !driverJobs_.empty() && driverRegistersAvailable()) due = 0;
    due = earliest(due, verifyDeadline_);
    due = earliest(due, prepareDeadline_);
    due = earliest(due, pendingStreamAt_);
  }
  // After a failed update the UART stays silent, but the last readings still go stale.
  if (usbSeen_) due = earliest(due, usbSeenAt_ + options_.staleMs);
  if (batterySeen_) due = earliest(due, batterySeenAt_ + options_.staleMs);
  return due;
}

void McuService::onTime(int64_t nowMs) {
  if (pendingStream_ && nowMs >= pendingStreamAt_) cancelPendingStream("microphone start timed out", nowMs);
  if (stream_.active()) {
    stream_.tick(nowMs);
    if (stream_.active()) { publish(nowMs); return; }
    finishStream(nowMs);
  }
  if (pcm_.active()) { pcm_.tick(nowMs); finishPcm(nowMs); if (pcm_.active()) return; }
  if (preparePid_>0 && prepareDeadline_>=0 && nowMs>=prepareDeadline_) {
    ::kill(-preparePid_,SIGKILL); prepareDeadline_=-1;
  }
  if (upgrading_) { upgrade_.tick(nowMs); firmwareProgress(nowMs); return; }
  if (stopping_) return;
  if (verificationExpired(nowMs)) return;
  if (!uart_.valid() && reopenAt_ >= 0 && nowMs >= reopenAt_) openUart(nowMs);
  pump(nowMs);
  if (usbSeen_ && nowMs - usbSeenAt_ >= options_.staleMs) {
    usbSeen_ = false;
    Log::line("mcu", "no USB report for %lld ms; USB power unknown", static_cast<long long>(options_.staleMs));
  }
  if (batterySeen_ && nowMs - batterySeenAt_ >= options_.staleMs) {
    batterySeen_ = false;
    Log::line("mcu", "no battery report for %lld ms; battery unknown", static_cast<long long>(options_.staleMs));
  }
  publish(nowMs);
}

void McuService::requestStop(int64_t nowMs) {
  cancelPendingStream("microphone supervisor stopping", nowMs);
  if (stream_.active()) { stopping_ = true; stream_.stop(nowMs); return; }
  if (pcm_.active()) { stopping_ = true; return; }
  if (preparePid_>0) { ::kill(-preparePid_,SIGKILL); prepareDeadline_=-1; }
  if (upgrading_ || verifyDeadline_ >= 0) {
    stopping_ = true;
    if (!upgrading_) { verifyDeadline_ = verifyStart_ = -1; closeUart(nowMs, "stop awaiting MCU reboot"); }
    return;
  }
  stopping_ = true;
  closeUart(nowMs, "stop");
}

void McuService::appendStatus(api::JsonWriter& json, int64_t nowMs) const {
  json.key("mcu").beginObject()
      .member("path", options_.path)
      .member("open", uart_.valid())
      .member("opens", opens_)
      .member("termiosAdjusted", termiosAdjusted_)
      .member("observedCflag", static_cast<unsigned>(observedCflag_))
      .member("version", version_);
  json.key("pcm").beginObject().member("available", pcmAvailable()).member("active", pcm_.active())
      .member("captures", static_cast<unsigned long long>(pcmCaptures_))
      .member("failures", static_cast<unsigned long long>(pcmFailures_)).member("error", pcmError_).endObject();
  json.key("microphoneStream").beginObject().member("available", streamAvailable()).member("active", stream_.active())
      .member("samples", stream_.samples()).member("acceptedWindows", static_cast<unsigned long long>(stream_.accepted()))
      .member("missedWindows", static_cast<unsigned long long>(stream_.missed())).member("error", stream_.error()).endObject();
  if (!options_.firmwareDirectory.empty()) {
    json.key("firmware").beginObject().member("status", prepareStatus_.empty() ? firmware_.status() : prepareStatus_)
        .member("preparing", preparePid_>0)
        .member("busy", firmwareBusy()).member("target", firmware_.target().version)
        .member("attempts", firmware_.attempts())
        .member("bytesSent", static_cast<unsigned long long>(upgrade_.progress()))
        .member("awaitingVerification", verifyDeadline_ >= 0);
    if (identitySeen_) json.member("installed", identity_.version).member("buildTag", identity_.tag)
        .member("abi", identity_.abi).member("features", identity_.features);
    json.endObject();
  }
  if (usbSeen_) json.member("usbRaw", static_cast<unsigned>(usbRaw_)).member("usbAgeMs", static_cast<long long>(nowMs - usbSeenAt_));
  if (batterySeen_)
    json.member("batteryFirst", static_cast<unsigned>(batteryFirst_))
        .member("batteryWord", static_cast<unsigned>(batteryWord_))
        .member("batteryAgeMs", static_cast<long long>(nowMs - batterySeenAt_))
        .member("batteryCompensatedMillivolts", gauge_.lastCompensated());
  json.member("panelLoad", panelLoad_.last(nowMs)).member("panelLoadSteady", panelLoad_.steady(nowMs) >= 0)
      .member("batterySag", gauge_.sag(), 3).member("batterySagRejected", gauge_.sagRejected());
  // [seconds ago, load before, load after, mV before, mV after] for each pair behind batterySag.
  std::array<BatteryGauge::SagPair, 9> pairs{};
  const std::size_t pairCount = gauge_.sagPairs(pairs);
  json.key("batterySagPairs").beginArray();
  for (std::size_t i = 0; i < pairCount; ++i)
    json.beginArray().value(static_cast<long long>((nowMs - pairs[i].atMs) / 1000)).value(pairs[i].fromLoad)
        .value(pairs[i].toLoad).value(pairs[i].fromMillivolts).value(pairs[i].toMillivolts).endArray();
  json.endArray();
  json.member("rxBytes", static_cast<unsigned long long>(rxBytes_))
      .member("frames", static_cast<unsigned long long>(frames_))
      .member("acks", static_cast<unsigned long long>(acks_))
      .member("unknown", static_cast<unsigned long long>(unknown_))
      .member("corrupt", static_cast<unsigned long long>(parser_.corruptFrames()))
      .member("discardedBytes", static_cast<unsigned long long>(parser_.discardedBytes()))
      .member("queries", static_cast<unsigned long long>(queries_))
      .member("timeouts", static_cast<unsigned long long>(timeouts_))
      .member("writeErrors", static_cast<unsigned long long>(writeErrors_))
      .endObject();
}

}
}
