// McuService against a pseudo terminal standing in for /dev/ttyS1: the test plays the MCU on the
// master side and drives the service with synthetic timestamps.
#include <asm/ioctls.h>
#include <asm/termbits.h>
#include <sys/ioctl.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "platform/tc002/daemon/McuDriverControl.h"
#include "platform/tc002/daemon/McuService.h"
#include "support.h"

using namespace awtrix::tc002d;
using tc002d_test::check;

namespace {

std::vector<uint8_t> frame(uint8_t command, std::vector<uint8_t> payload) {
  std::vector<uint8_t> out{0xff, 0x55, command, static_cast<uint8_t>(payload.size())};
  out.insert(out.end(), payload.begin(), payload.end());
  uint16_t sum = 0;
  for (uint8_t byte : out) sum = static_cast<uint16_t>(sum + byte);
  out.push_back(static_cast<uint8_t>(sum >> 8));
  out.push_back(static_cast<uint8_t>(sum));
  return out;
}

struct Mcu {
  int master = -1;
  std::string slave;
  std::vector<uint8_t> received;

  Mcu() {
    master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (master >= 0 && ::grantpt(master) == 0 && ::unlockpt(master) == 0) slave = ::ptsname(master);
  }
  ~Mcu() {
    if (master >= 0) ::close(master);
  }
  void send(const std::vector<uint8_t>& bytes) { (void)!::write(master, bytes.data(), bytes.size()); }
  // Collects what the service wrote, waiting up to waitMs for the first byte.
  std::vector<uint8_t> take(int waitMs = 200) {
    std::vector<uint8_t> out;
    pollfd item{master, POLLIN, 0};
    int wait = waitMs;
    while (::poll(&item, 1, wait) > 0 && (item.revents & POLLIN)) {
      uint8_t buffer[256];
      const ssize_t count = ::read(master, buffer, sizeof buffer);
      if (count <= 0) break;
      out.insert(out.end(), buffer, buffer + count);
      wait = 20;
    }
    received.insert(received.end(), out.begin(), out.end());
    return out;
  }
};

// Delivers whatever arrived on the service's descriptor.
void serve(McuService& mcu, int64_t now, int waitMs = 200) {
  std::vector<PollInterest> interest;
  mcu.pollInterest(interest);
  for (const auto& item : interest) {
    pollfd p{item.fd, item.events, 0};
    if (::poll(&p, 1, waitMs) > 0) mcu.onReady(item.fd, p.revents, now);
  }
}

bool isQuery(const std::vector<uint8_t>& bytes, uint8_t command) {
  return bytes == frame(command, {});
}

// Splits what the service wrote into frames; false if the bytes are not a clean frame sequence.
bool splitFrames(const std::vector<uint8_t>& bytes, std::vector<std::vector<uint8_t>>& out) {
  std::size_t at = 0;
  while (at < bytes.size()) {
    if (at + 6 > bytes.size() || bytes[at] != 0xff || bytes[at + 1] != 0x55) return false;
    const std::size_t size = 6u + bytes[at + 3];
    if (at + size > bytes.size()) return false;
    out.emplace_back(bytes.begin() + at, bytes.begin() + at + size);
    at += size;
  }
  return true;
}

void batteryConversion() {
  check(McuService::batteryMillivolts(3113) == 4120, "measured battery word 3113 is 4120 mV");
  check(McuService::batteryMillivolts(3117) == 4125, "battery word 3117 rounds to 4125 mV");
  check(McuService::batteryMillivolts(0) == 0, "zero word");
  check(McuService::batteryMillivolts(65535) == -1, "out of protocol range becomes unknown");
}

void conversation() {
  Mcu device;
  check(!device.slave.empty(), "pseudo terminal available");
  {
    posix::UniqueFd slave(::open(device.slave.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC));
    termios2 cooked{};
    ::ioctl(slave.get(), TCGETS2, &cooked);
    cooked.c_lflag |= ICANON | ECHO;
    cooked.c_oflag |= OPOST | ONLCR;
    ::ioctl(slave.get(), TCSETS2, &cooked);
  }
  DeviceState state;
  unsigned changes = 0;
  state.onPower([&] { ++changes; });
  McuOptions options;
  options.path = device.slave;
  McuService mcu(state, options);
  int64_t now = 10000;
  check(mcu.start(now) && mcu.open(), "service opens the UART");
  {
    posix::UniqueFd view(::open(device.slave.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC));
    termios2 t{};
    check(::ioctl(view.get(), TCGETS2, &t) == 0, "termios readable");
    check((t.c_cflag & CBAUD) == B1500000 && t.c_ispeed == 1500000 && t.c_ospeed == 1500000, "1.5 Mbaud selected");
    check((t.c_cflag & (CSIZE | CREAD | CLOCAL | PARENB | CSTOPB | CRTSCTS)) == (CS8 | CREAD | CLOCAL), "raw 8N1");
    check(t.c_iflag == 0 && t.c_oflag == 0 && t.c_lflag == 0, "no line processing");
  }
  check(isQuery(device.take(), 0x11), "version is queried first");

  auto version = frame(0x11, {'V', '1', '.', '0', '.', '1', '7'});
  auto prefixed = frame(0xfe, {});
  prefixed.insert(prefixed.end(), {0x00, 0x13});
  prefixed.insert(prefixed.end(), version.begin(), version.begin() + 5);
  device.send(prefixed);
  serve(mcu, now);
  device.send(std::vector<uint8_t>(version.begin() + 5, version.end()));
  serve(mcu, now);
  check(mcu.version() == "V1.0.17", "fragmented version after an empty 0xfe frame and noise");
  check(isQuery(device.take(), 0x08), "identity is probed even without a firmware bundle");
  device.send(frame(0x08, {}));
  serve(mcu, now);
  check(isQuery(device.take(), 0x02), "USB is queried after the identity reply");

  device.send(frame(0x02, {1}));
  serve(mcu, now);
  check(isQuery(device.take(), 0x03), "battery is queried after the USB reply");
  device.send(frame(0x03, {0x4c, 0x0c, 0x29}));
  serve(mcu, now);
  check(state.power().usbPower && state.power().batteryPercent == 100 && state.power().batteryMillivolts == 4120,
        "power published from the measured sample, not the MCU's 76 %");
  const unsigned afterFirst = changes;
  check(device.take().empty(), "no legacy microphone enable command");

  now += 999;
  mcu.onTime(now);
  check(device.take(50).empty(), "no query before the USB interval");
  now += 1;
  check(mcu.nextDeadlineMs() <= now, "USB interval due");
  mcu.onTime(now);
  check(isQuery(device.take(), 0x02), "USB polled every second");
  auto burst = frame(0x01, {0x00, 0x10});
  const auto unknown = frame(0x7e, {1, 2, 3});
  burst.insert(burst.end(), unknown.begin(), unknown.end());
  auto corrupt = frame(0x02, {0});
  corrupt.back() ^= 0xff;
  burst.insert(burst.end(), corrupt.begin(), corrupt.end());
  const auto usbOff = frame(0x02, {0});
  burst.insert(burst.end(), usbOff.begin(), usbOff.end());
  device.send(burst);
  serve(mcu, now);
  check(!state.power().usbPower && state.power().batteryPercent == 100, "unsolicited, unknown and corrupt frames tolerated");
  check(changes == afterFirst + 1, "one change notification per changed value");
  mcu.onTime(now);
  check(changes == afterFirst + 1, "unchanged values are not republished");

  now += 1000;
  mcu.onTime(now);
  check(isQuery(device.take(), 0x02), "next USB query");
  now += 499;
  mcu.onTime(now);
  check(device.take(50).empty(), "one query in flight");
  now += 1;
  mcu.onTime(now);
  check(device.take(50).empty(), "timed-out query is not retried before its interval");

  for (int step = 0; step < 40; ++step) {
    now += 1000;
    mcu.onTime(now);
    device.take(20);
  }
  check(!state.power().usbPower && state.power().batteryPercent == -1 && state.power().batteryMillivolts == -1,
        "silence for 30 s makes power unknown");
  device.send(frame(0x03, {150, 0x0c, 0x29}));
  serve(mcu, now);
  check(state.power().batteryPercent == 100 && state.power().batteryMillivolts == 4120,
        "the MCU's percentage byte is ignored; the estimate returns with the voltage");

  unsigned versionQueries = 0;
  std::vector<std::vector<uint8_t>> written;
  bool onlyKnown = splitFrames(device.received, written);
  for (const auto& sent : written) {
    if (!isQuery(sent, 0x02) && !isQuery(sent, 0x03) && !isQuery(sent, 0x08) && !isQuery(sent, 0x11))
      onlyKnown = false;
    if (isQuery(sent, 0x11)) ++versionQueries;
  }
  check(onlyKnown, "only power, identity and version queries were ever written");
  check(versionQueries == 1, "version queried once after a reply");

  ::close(device.master);
  device.master = -1;
  serve(mcu, now);
  check(!mcu.open(), "hangup closes the UART");
  check(mcu.nextDeadlineMs() == now + options.reopenDelayMs, "reopen scheduled");
  mcu.requestStop(now);
  check(mcu.stopped() && mcu.nextDeadlineMs() == -1, "stop is immediate");
}

void missingUart() {
  DeviceState state;
  McuOptions options;
  options.path = "/nonexistent/ttyS1";
  McuService mcu(state, options);
  check(mcu.start(0) && !mcu.open(), "a missing UART is not fatal");
  check(mcu.nextDeadlineMs() == options.reopenDelayMs, "retry scheduled");
  mcu.onTime(options.reopenDelayMs);
  check(mcu.nextDeadlineMs() == 2 * options.reopenDelayMs, "retry rescheduled");
}

std::vector<uint8_t> pcmMeta(unsigned kind, unsigned halves) {
  std::vector<uint8_t> data(16);
  std::memcpy(data.data(), "PCM2", 4);
  data[4] = kind; data[6] = halves; data[7] = halves >> 8;
  return frame(6, data);
}

void pcmConversation() {
  Mcu device;
  {
    posix::UniqueFd slave(::open(device.slave.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC));
    termios2 t{};
    check(::ioctl(slave.get(), TCGETS2, &t) == 0, "PCM tty settings readable");
    t.c_cc[VMIN] = t.c_cc[VTIME] = 0;
    check(::ioctl(slave.get(), TCSETS2, &t) == 0, "PCM uses stock-style zero-length idle reads");
  }
  DeviceState state;
  McuOptions options;
  options.path = device.slave;
  McuService service(state, options);
  int64_t now = 1000;
  service.start(now); device.take();
  check(!service.requestPcm(44, [](const mcu::PcmCapture&) {}, now), "PCM requires a compatible identity");
  device.send(frame(0x11, {'V','1','.','0','.','1','7'})); serve(service, now);
  check(isQuery(device.take(), 8), "identity probe precedes PCM");
  device.send(frame(8, {'A','W','M','C',1,3,123,0,3,0,0,0,1,0,0,0})); serve(service, now);
  check(isQuery(device.take(), 2), "normal USB query after identity");
  device.send(frame(2, {1})); serve(service, now); device.take();
  device.send(frame(3, {75,12,41})); serve(service, now); device.take();
  check(service.pcmAvailable(), "PCM capability detected without update artifacts");
  unsigned powerChanges = 0;
  state.onPower([&] { ++powerChanges; });
  bool complete = false, valid = false;
  std::vector<int16_t> received;
  check(service.requestPcm(44, [&](const mcu::PcmCapture& pcm) {
    complete = true; valid = pcm.succeeded(); received = pcm.samples();
  }, now), "capture request accepted");
  check(device.take() == frame(6, {44,0}), "bounded request uses command 06");
  check(service.busy() && !service.requestPcm(1, [](const mcu::PcmCapture&) {}, now), "one UART transaction owns capture");
  check(!service.firmwareBusy(), "bounded PCM capture does not block Linux update handoff");
  auto bytes = pcmMeta(1,0);
  for (unsigned half = 0; half < 44; ++half) {
    for (unsigned part = 0; part < 6; ++part) {
      std::vector<uint8_t> p{'P','2',static_cast<uint8_t>(half),0,static_cast<uint8_t>(part),
                              static_cast<uint8_t>(half % 2),0,0};
      for (unsigned i = 0; i < 4; ++i) {
        const auto sample = static_cast<uint16_t>(half * 24 + part * 4 + i - 1000);
        p.push_back(sample); p.push_back(sample >> 8);
      }
      const auto packet = frame(6, p); bytes.insert(bytes.end(), packet.begin(), packet.end());
    }
  }
  const auto end = pcmMeta(2,44); bytes.insert(bytes.end(), end.begin(), end.end());
  // Fill an entire service read, leaving an incomplete frame and no more bytes. With
  // VMIN=0 the next read returns zero: neither condition is a UART disconnect.
  for (std::size_t offset = 0; offset < bytes.size();) {
    const std::size_t count = std::min<std::size_t>(
        mcu::FrameParser::kCapacity - mcu::kMaxPayload - mcu::kQueryBytes, bytes.size() - offset);
    device.send({bytes.begin() + offset, bytes.begin() + offset + count});
    serve(service, now, 50); serve(service, now, 10);
    offset += count;
  }
  check(complete && valid && received.size() == 1056, "full PCM window survives serial read boundaries");
  check(service.open(), "zero-byte idle read never closes the UART");
  bool ordered = true;
  for (unsigned i = 0; i < received.size(); ++i) ordered &= received[i] == static_cast<int>(i) - 1000;
  check(ordered, "PCM reaches consumer unchanged");
  check(isQuery(device.take(), 2), "fresh USB query immediately follows capture");
  check(state.power().usbPower && powerChanges == 0, "capture preserves the last USB measurement while refreshing");
  device.send(frame(2, {1})); serve(service, now); device.take();
  check(state.power().usbPower && powerChanges == 0, "USB refresh does not pulse the published power state");
  complete = false;
  check(service.requestPcm(44, [&](const mcu::PcmCapture& pcm) { complete = true; valid = pcm.succeeded(); }, now),
        "second capture accepted");
  device.take();
  service.onTime(now + 1000);
  check(device.take(10).empty(), "no power or firmware commands interleave with capture");
  service.requestStop(now + 1000);
  check(!service.stopped(), "shutdown waits for bounded MCU capture");
  service.onTime(now + mcu::PcmCapture::kTimeoutMs);
  check(complete && !valid && service.stopped(), "timeout finishes pending callback and shutdown");
}

// The control commands as the daemon registers them.
struct Commands : ControlRegistry {
  std::map<std::string, Handler> handlers;
  void add(std::string command, Handler handler) override { handlers[command] = std::move(handler); }
  std::string run(const std::string& command, std::string_view payload = {}) { return handlers.at(command)(payload); }
};

bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// Brings a service to the point where only the periodic power queries remain.
void bootTo(Mcu& device, McuService& mcu, int64_t now, const char* version) {
  mcu.start(now);
  device.take();
  const std::string text = version;
  device.send(frame(0x11, std::vector<uint8_t>(text.begin(), text.end())));
  serve(mcu, now);
  device.take();
  device.send(frame(0x08, {}));
  serve(mcu, now);
  device.take();
  device.send(frame(0x02, {1}));
  serve(mcu, now);
  device.take();
  device.send(frame(0x03, {0x4c, 0x0c, 0x29}));
  serve(mcu, now);
  device.take();
}

void driverRegisters() {
  Mcu device;
  DeviceState state;
  McuOptions options;
  options.path = device.slave;
  McuService mcu(state, options);
  Commands commands;
  addDriverRegisterCommands(commands, mcu);
  int64_t now = 10000;
  check(has(commands.run("mcu-register-read", "0c"), "\"ok\":false"), "no register requests before the firmware is known");
  bootTo(device, mcu, now, "V1.0.17");
  check(mcu.driverRegistersAvailable(), "register requests on V1.0.17");

  check(has(commands.run("mcu-register-read", "21"), "outside"), "registers past 0x20 are refused");
  check(has(commands.run("mcu-register-read", "zz"), "payload"), "a malformed register is refused");
  check(has(commands.run("mcu-register-write", "0c 08 18"), "payload"), "a write needs three values");
  std::string table = commands.run("mcu-registers");
  check(has(table, "\"override\":null") &&
            has(table, "{\"register\":12,\"factory\":[8,24,48],\"known\":false,\"changed\":false}"),
        "the factory table of V1.0.17 is known before any read: " + table);
  check(commands.run("mcu-register-read", "0x0c") == "{\"ok\":true}", "a read is queued");
  check(mcu.nextDeadlineMs() <= now, "a queued request is due at once");
  mcu.onTime(now);
  check(device.take() == frame(0x13, {0x0c, 0x0c, 0x0c}), "read: command 13 names the register for each colour");
  check(device.take(30).empty(), "one request in flight");
  device.send(frame(0x13, {0x0c, 0x08, 0x0c, 0x18, 0x0c, 0x30}));
  serve(mcu, now);
  table = commands.run("mcu-registers");
  check(has(table, "\"register\":12,\"factory\":[8,24,48],\"value\":[8,24,48],\"known\":true,\"changed\":false"),
        "the answer fills the table: " + table);

  check(commands.run("mcu-register-write", "0c 10 18 30") == "{\"ok\":true}", "a write is queued");
  mcu.onTime(now);
  check(device.take() == frame(0x12, {0x0c, 0x10, 0x0c, 0x18, 0x0c, 0x30}),
        "write: command 12 with a word per colour, register high");
  device.send(frame(0x12, {0x0c, 0x10, 0x0c, 0x18, 0x0c, 0x30}));
  serve(mcu, now);
  check(device.take(30).empty(), "a write is not read back: the MCU would only answer with the override");
  table = commands.run("mcu-registers");
  check(has(table, "\"override\":{\"register\":12,\"value\":[16,24,48],\"factory\":[8,24,48],\"changed\":true}") &&
            has(table, "\"value\":[16,24,48],\"known\":true,\"changed\":true") && has(table, "\"writes\":1"),
        "the write is the MCU's override and marks the change: " + table);

  check(commands.run("mcu-register-read", "14") == "{\"ok\":true}", "another read");
  mcu.onTime(now);
  device.take();
  device.send(frame(0x13, {0x0c, 0x10, 0x0c, 0x18, 0x0c, 0x30}));
  serve(mcu, now);
  table = commands.run("mcu-registers");
  check(has(table, "\"failures\":0") && has(table, "\"overrideAnswers\":1") && has(table, "saved override, register 0x0c") &&
            has(table, "{\"register\":20,\"factory\":[20,34,50],\"known\":false,\"changed\":false}"),
        "an answer with the override is no failure and no value for 0x14: " + table);

  check(commands.run("mcu-register-restore") == "{\"ok\":true}", "restore is queued");
  mcu.onTime(now);
  check(device.take() == frame(0x12, {0x0c, 0x08, 0x0c, 0x18, 0x0c, 0x30}), "restore writes the factory value");
  device.send(frame(0x12, {0x0c, 0x08, 0x0c, 0x18, 0x0c, 0x30}));
  serve(mcu, now);
  check(device.take(30).empty(), "only the changed register is written");
  table = commands.run("mcu-registers");
  check(has(table, "\"override\":{\"register\":12,\"value\":[8,24,48],\"factory\":[8,24,48],\"changed\":false}"),
        "the override left behind is the factory value: " + table);

  check(commands.run("mcu-register-read", "14") == "{\"ok\":true}", "a read of no register");
  mcu.onTime(now);
  device.take();
  device.send(frame(0x13, {0, 0, 0, 0, 0, 0}));
  serve(mcu, now);
  table = commands.run("mcu-registers");
  check(has(table, "did not answer for LED driver register 0x14") && has(table, "\"failures\":1"),
        "an answer for no register is an error, not a value: " + table);

  check(commands.run("mcu-register-read", "all") == "{\"ok\":true}", "all registers are queued");
  now += 1000;
  mcu.onTime(now);
  check(isQuery(device.take(), 0x02), "a due USB query goes before queued register reads");
  device.send(frame(0x02, {1}));
  serve(mcu, now);
  check(device.take() == frame(0x13, {0x02, 0x02, 0x02}), "then the first register");
  now += options.replyTimeoutMs;
  mcu.onTime(now);
  check(device.take() == frame(0x13, {0x03, 0x03, 0x03}) && has(commands.run("mcu-registers"), "no answer"),
        "a silent MCU costs that request only");

  Mcu earlier;
  McuOptions earlierOptions;
  earlierOptions.path = earlier.slave;
  McuService fresh(state, earlierOptions);
  Commands freshCommands;
  addDriverRegisterCommands(freshCommands, fresh);
  bootTo(earlier, fresh, now, "V1.0.17");
  check(freshCommands.run("mcu-register-read", "02") == "{\"ok\":true}", "a read after a restart");
  fresh.onTime(now);
  earlier.take();
  earlier.send(frame(0x13, {0x16, 0x20, 0x16, 0x20, 0x16, 0x20}));
  serve(fresh, now);
  check(freshCommands.run("mcu-register-restore") == "{\"ok\":true}", "restore after a restart");
  fresh.onTime(now);
  check(earlier.take() == frame(0x12, {0x16, 0x36, 0x16, 0x32, 0x16, 0x2e}),
        "a changed override from an earlier run goes back to its factory value");

  Mcu other;
  McuOptions otherOptions;
  otherOptions.path = other.slave;
  McuService newer(state, otherOptions);
  bootTo(other, newer, now, "V1.0.18");
  check(!newer.driverRegistersAvailable() && !newer.readDriverRegister(0x0c).empty(),
        "another MCU firmware gets no register requests");
}

}

int main() {
  tc002d_test::quietLogs();
  batteryConversion();
  conversation();
  missingUart();
  pcmConversation();
  driverRegisters();
  return tc002d_test::finish("tc002d mcu");
}
