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
// The stock firmware V1.0.17 keeps one 8-bit register table per colour for the panel's LED drivers.
// 0x13 reads a register of all three, 0x12 writes them; the change lasts until the MCU restarts.
constexpr uint8_t kDriverWrite = 0x12;
constexpr uint8_t kDriverRead = 0x13;
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
bool encodeDriverRead(uint8_t reg, uint8_t (&out)[kDriverReadBytes]);
// Payload: one big-endian word per colour, the register in the high byte and its value in the low.
bool encodeDriverWrite(uint8_t reg, const uint8_t (&values)[3], uint8_t (&out)[kDriverWriteBytes]);
// The values of a 0x12 or 0x13 reply for reg, red first; false when a word names another register.
bool decodeDriverReply(const Frame& frame, uint8_t reg, uint8_t (&values)[3]);

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
