#pragma once

#include <cstddef>
#include <cstdint>

// TC002 MCU wire format on /dev/ttyS1: ff 55 <command> <length> <payload...> <sum hi> <sum lo>,
// where the 16-bit sum covers every byte before it. Encodes queries and bounded PCM
// acquisition. Upgrade frames belong to McuUpgrade. Commands may receive an empty 0xfe ACK.
namespace awtrix {
namespace tc002d {
namespace mcu {

constexpr uint8_t kMicrophone = 0x01;
constexpr uint8_t kUsb = 0x02;
constexpr uint8_t kBattery = 0x03;
constexpr uint8_t kPcm = 0x06;
constexpr uint8_t kIdentity = 0x08;
constexpr uint8_t kVersion = 0x11;
// The stock firmware V1.0.17 keeps one 8-bit register table per colour for the panel's LED drivers
// (RAM 0x15794, sent to the drivers at start and whenever the panel comes back on). 0x12 is no plain
// write: it saves its three words as the MCU's single override (settings flash 0x300, marker 0xe5)
// and sends them to the drivers, but leaves the table alone. At every start the MCU loads the
// override into the table, so it outlasts restarts; only the last write is kept. 0x13 answers with
// the override whenever one exists, whatever register was asked, and reads the table only when
// none does.
constexpr uint8_t kDriverWrite = 0x12;
constexpr uint8_t kDriverRead = 0x13;
// AWTRIX MCU extension build 14 (feature kFeatureDriverTable, tools/tc002/mcu/extension/registers.S):
// 0x09 and 0x0a take the payloads of 0x12 and 0x13 but work on the table itself, without flash;
// 0x0b clears the stock override. A refused request is answered with no payload.
constexpr uint8_t kDriverTableWrite = 0x09;
constexpr uint8_t kDriverTableRead = 0x0a;
constexpr uint8_t kDriverClearOverride = 0x0b;
constexpr uint8_t kFeatureDriverTable = 0x10;
constexpr uint8_t kDriverFirstRegister = 0x02;
constexpr uint8_t kDriverLastRegister = 0x20;
constexpr uint8_t kAck = 0xfe;
constexpr std::size_t kMaxPayload = 32;
constexpr std::size_t kQueryBytes = 6;
constexpr std::size_t kPcmRequestBytes = 8;
constexpr std::size_t kDriverReadBytes = kQueryBytes + 3;
constexpr std::size_t kDriverWriteBytes = kQueryBytes + 6;

struct Frame {
  uint8_t command = 0;
  uint8_t length = 0;
  uint8_t payload[kMaxPayload]{};
};

bool encodeQuery(uint8_t command, uint8_t (&out)[kQueryBytes]);
void encodePcmRequest(uint16_t halves, uint8_t (&out)[kPcmRequestBytes]);
// Payload: the register once per colour (red, green, blue). False outside the register range.
bool encodeDriverRead(uint8_t reg, uint8_t (&out)[kDriverReadBytes], uint8_t command = kDriverRead);
// Payload: one big-endian word per colour, the register in the high byte and its value in the low.
bool encodeDriverWrite(uint8_t reg, const uint8_t (&values)[3], uint8_t (&out)[kDriverWriteBytes],
                       uint8_t command = kDriverWrite);
// The register and values of a 0x12, 0x13, 0x09 or 0x0a reply, red first; false unless all three
// words name the same register in range.
bool decodeDriverReply(const Frame& frame, uint8_t& reg, uint8_t (&values)[3]);
// The table V1.0.17 starts with, red, green and blue per register from kDriverFirstRegister, as read
// from a stock TC002 before any 0x12.
extern const uint8_t kDriverFactory[kDriverLastRegister - kDriverFirstRegister + 1][3];

// Resynchronising stream parser: bytes that cannot start a valid frame are skipped one at a
// time, so garbage, truncated frames and checksum errors never stall later frames.
class FrameParser {
 public:
  static constexpr std::size_t kCapacity = 256;

  void push(const uint8_t* bytes, std::size_t length);
  bool next(Frame& out);
  void clear() { length_ = 0; }

  uint64_t discardedBytes() const { return discarded_; }
  uint64_t corruptFrames() const { return corrupt_; }

 private:
  void consume(std::size_t count);

  uint8_t bytes_[kCapacity]{};
  std::size_t length_ = 0;
  uint64_t discarded_ = 0;
  uint64_t corrupt_ = 0;
};

}
}
}
