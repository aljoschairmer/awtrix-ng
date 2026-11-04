#include "platform/tc002/daemon/McuFrame.h"

#include <cstring>

namespace awtrix {
namespace tc002d {
namespace mcu {
namespace {

uint16_t checksum(const uint8_t* bytes, std::size_t length) {
  uint16_t sum = 0;
  for (std::size_t i = 0; i < length; ++i) sum = static_cast<uint16_t>(sum + bytes[i]);
  return sum;
}

}

bool encodeQuery(uint8_t command, uint8_t (&out)[kQueryBytes]) {
  if (command != kUsb && command != kBattery && command != kVersion && command != kIdentity) return false;
  out[0] = 0xff;
  out[1] = 0x55;
  out[2] = command;
  out[3] = 0;
  const uint16_t sum = checksum(out, 4);
  out[4] = static_cast<uint8_t>(sum >> 8);
  out[5] = static_cast<uint8_t>(sum);
  return true;
}

void FrameParser::consume(std::size_t count) {
  std::memmove(bytes_, bytes_ + count, length_ - count);
  length_ -= count;
}

void encodePcmRequest(uint16_t halves, uint8_t (&out)[kPcmRequestBytes]) {
  out[0] = 0xff;
  out[1] = 0x55;
  out[2] = kPcm;
  out[3] = 2;
  out[4] = static_cast<uint8_t>(halves);
  out[5] = static_cast<uint8_t>(halves >> 8);
  const uint16_t sum = checksum(out, 6);
  out[6] = static_cast<uint8_t>(sum >> 8);
  out[7] = static_cast<uint8_t>(sum);
}

bool encodeDriverRead(uint8_t reg, uint8_t (&out)[kDriverReadBytes]) {
  if (reg < kDriverFirstRegister || reg > kDriverLastRegister) return false;
  out[0] = 0xff;
  out[1] = 0x55;
  out[2] = kDriverRead;
  out[3] = 3;
  out[4] = out[5] = out[6] = reg;
  const uint16_t sum = checksum(out, 7);
  out[7] = static_cast<uint8_t>(sum >> 8);
  out[8] = static_cast<uint8_t>(sum);
  return true;
}

bool encodeDriverWrite(uint8_t reg, const uint8_t (&values)[3], uint8_t (&out)[kDriverWriteBytes]) {
  if (reg < kDriverFirstRegister || reg > kDriverLastRegister) return false;
  out[0] = 0xff;
  out[1] = 0x55;
  out[2] = kDriverWrite;
  out[3] = 6;
  for (int colour = 0; colour < 3; ++colour) {
    out[4 + colour * 2] = reg;
    out[5 + colour * 2] = values[colour];
  }
  const uint16_t sum = checksum(out, 10);
  out[10] = static_cast<uint8_t>(sum >> 8);
  out[11] = static_cast<uint8_t>(sum);
  return true;
}

bool decodeDriverReply(const Frame& frame, uint8_t reg, uint8_t (&values)[3]) {
  if ((frame.command != kDriverRead && frame.command != kDriverWrite) || frame.length != 6) return false;
  for (int colour = 0; colour < 3; ++colour)
    if (frame.payload[colour * 2] != reg) return false;
  for (int colour = 0; colour < 3; ++colour) values[colour] = frame.payload[colour * 2 + 1];
  return true;
}

void FrameParser::push(const uint8_t* bytes, std::size_t length) {
  if (length >= kCapacity) {
    discarded_ += length_ + (length - kCapacity);
    std::memcpy(bytes_, bytes + length - kCapacity, kCapacity);
    length_ = kCapacity;
    return;
  }
  if (length > kCapacity - length_) {
    const std::size_t excess = length - (kCapacity - length_);
    consume(excess);
    discarded_ += excess;
  }
  std::memcpy(bytes_ + length_, bytes, length);
  length_ += length;
}

bool FrameParser::next(Frame& out) {
  while (length_ > 0) {
    if (bytes_[0] != 0xff || (length_ >= 2 && bytes_[1] != 0x55)) {
      consume(1);
      ++discarded_;
      continue;
    }
    if (length_ < 4) return false;
    if (bytes_[3] > kMaxPayload) {
      ++corrupt_;
      consume(1);
      ++discarded_;
      continue;
    }
    const std::size_t frameLength = static_cast<std::size_t>(bytes_[3]) + kQueryBytes;
    if (length_ < frameLength) return false;
    const uint16_t expected = static_cast<uint16_t>((bytes_[frameLength - 2] << 8) | bytes_[frameLength - 1]);
    if (checksum(bytes_, frameLength - 2) != expected) {
      ++corrupt_;
      consume(1);
      ++discarded_;
      continue;
    }
    out = Frame{};
    out.command = bytes_[2];
    out.length = bytes_[3];
    std::memcpy(out.payload, bytes_ + 4, out.length);
    consume(frameLength);
    return true;
  }
  return false;
}

}
}
}
