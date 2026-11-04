// Bench tool for an MCU extension release (tools/tc002/mcu/extension/build.py): builds the image
// from the vendor original and an extension exactly as the clock's preparation does, and prints its
// size and SHA-256 for the manifest. Usage: awtrix-tc002-mcu-assemble BASE.fot EXTENSION.bin [OUT]
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>

#include "platform/tc002/daemon/McuFirmware.h"
#include "platform/tc002/mcu/McuPatch.h"

namespace {
bool readFile(const char* path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}
}

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) {
    std::fprintf(stderr, "usage: %s BASE.fot EXTENSION.bin [OUT.pot]\n", argv[0]);
    return 2;
  }
  std::string base, extension;
  if (!readFile(argv[1], base) || !readFile(argv[2], extension)) {
    std::fprintf(stderr, "cannot read the inputs\n");
    return 1;
  }
  try {
    const std::string image = awtrix::tc002::mcu::assembleFirmware(base, extension);
    if (argc == 4) {
      std::ofstream out(argv[3], std::ios::binary | std::ios::trunc);
      out.write(image.data(), static_cast<std::streamsize>(image.size()));
      if (!out) {
        std::fprintf(stderr, "cannot write %s\n", argv[3]);
        return 1;
      }
    }
    std::printf("%zu %s\n", image.size(), awtrix::tc002d::mcu::firmwareHash(image).c_str());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
