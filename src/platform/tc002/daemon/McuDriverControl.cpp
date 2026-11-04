#include "platform/tc002/daemon/McuDriverControl.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/api/JsonWriter.h"
#include "platform/tc002/daemon/Log.h"

namespace awtrix {
namespace tc002d {
namespace {

std::string reply(const std::string& error) {
  std::string out;
  api::JsonWriter json(out);
  json.beginObject().member("ok", error.empty());
  if (!error.empty()) json.member("error", error);
  json.endObject();
  return out;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Bytes separated by spaces, each one or two hex digits with an optional 0x.
bool parseBytes(std::string_view text, std::vector<uint8_t>& out) {
  out.clear();
  std::size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n')) ++at;
    if (at == text.size()) break;
    std::size_t end = at;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\n') ++end;
    std::string_view word = text.substr(at, end - at);
    if (word.size() > 2 && word[0] == '0' && (word[1] == 'x' || word[1] == 'X')) word.remove_prefix(2);
    if (word.empty() || word.size() > 2) return false;
    int value = 0;
    for (char c : word) {
      const int digit = hexDigit(c);
      if (digit < 0) return false;
      value = value * 16 + digit;
    }
    out.push_back(static_cast<uint8_t>(value));
    at = end;
  }
  return true;
}

}

void addDriverRegisterCommands(ControlRegistry& control, McuService& mcu) {
  control.add("mcu-registers", [&mcu](std::string_view) {
    std::string out;
    api::JsonWriter json(out);
    mcu.appendDriverRegisters(json);
    return out;
  });
  control.add("mcu-register-read", [&mcu](std::string_view payload) {
    std::vector<uint8_t> bytes;
    std::string_view trimmed = payload;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == ' ')) trimmed.remove_suffix(1);
    if (trimmed == "all") return reply(mcu.readDriverRegisters());
    if (!parseBytes(payload, bytes) || bytes.size() != 1) return reply("payload: a register in hex, or all");
    return reply(mcu.readDriverRegister(bytes[0]));
  });
  control.add("mcu-register-write", [&mcu](std::string_view payload) {
    std::vector<uint8_t> bytes;
    if (!parseBytes(payload, bytes) || bytes.size() != 4) return reply("payload: REG RED GREEN BLUE in hex");
    const uint8_t values[3] = {bytes[1], bytes[2], bytes[3]};
    const std::string error = mcu.writeDriverRegister(bytes[0], values);
    if (error.empty())
      Log::line("control", "LED driver register %02x write %02x %02x %02x requested", bytes[0], bytes[1], bytes[2], bytes[3]);
    return reply(error);
  });
  control.add("mcu-register-restore", [&mcu](std::string_view) { return reply(mcu.restoreDriverRegisters()); });
  control.add("mcu-register-clear-override", [&mcu](std::string_view) { return reply(mcu.clearDriverOverride()); });
}

}
}
