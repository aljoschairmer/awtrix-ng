#pragma once
#include <cstdint>
#include <string>

namespace awtrix::tc002::mcu {
// Pure transformation; no device access. Throws on any unsupported input.
// The caller also checks the final package against the release's expected SHA-256.
std::string assembleFirmware(const std::string& base, const std::string& extension);

// Feature bit of an extension that carries the LED driver table commands 0x09..0x0b, and their
// entry addresses (tools/tc002/mcu/extension/build.py checks the linked handlers against them).
constexpr uint8_t kFeatureDriverTable = 0x10;
constexpr uint32_t kDriverTableWriteEntry = 0x100087c0;
constexpr uint32_t kDriverTableReadEntry = 0x10008848;
constexpr uint32_t kDriverClearOverrideEntry = 0x100088e0;
// The features byte of the extension's AWMC identity; throws unless there is exactly one.
uint8_t extensionFeatures(const std::string& extension);
}
