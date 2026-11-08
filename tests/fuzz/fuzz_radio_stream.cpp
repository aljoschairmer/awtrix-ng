// libFuzzer target: station bytes through the radio's codec sniffing and decoders, the way a
// station's stream reaches them. Network bytes go straight into C code (the FDK AAC library and the
// MP3 decoder), so this runs under AddressSanitizer:
//
//   cmake -S . -B .pio/fuzz -DAWTRIX_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
//   cmake --build .pio/fuzz --target fuzz-radio-stream
//   .pio/fuzz/tests/fuzz/fuzz-radio-stream -max_total_time=60 test/test_aacdecoder
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/audio/StreamInputBuffer.h"
#include "core/radio/CodecSniff.h"
#include "core/radio/StreamPolicy.h"
#include "core/radio/stream/StreamDecoder.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  using namespace awtrix;
  std::string name;
  radio::sniff(data, size, &name);
  radio::StreamDecoder decoder;
  radio::StreamOptions options;
  options.fadeMs = 200;
  options.staticLevel = size ? data[0] % 101 : 0;
  decoder.setOptions(options);
  decoder.restart(size > 1 && (data[1] & 1) ? "audio/aacp" : "audio/mpeg");
  audio::StreamInputBuffer input(radio::kInputBufferBytes, audio::StreamInputBuffer::kMaxAdtsFrameBytes);
  std::size_t at = 0;
  int64_t now = 0;
  radio::StreamDecoder::Block block;
  // The bytes arrive in pieces of uneven size, so frames cross the end of the ring.
  for (int step = 0; step < 4096; ++step) {
    if (at < size) {
      const std::size_t piece = std::min<std::size_t>({size - at, input.room(), 1 + (data[at] * 37u) % 3000u});
      input.push(data + at, piece);
      at += piece;
    }
    const auto result = decoder.next(input, now += 23, block);
    if (result == radio::StreamDecoder::Step::Error) break;
    if (at >= size && result == radio::StreamDecoder::Step::Wait && input.size() == 0) break;
  }
  return 0;
}

#ifdef AWTRIX_FUZZ_DRIVER
// Without libFuzzer (gcc): mutates the seed files at random for a while, under the sanitizers.
//   fuzz-radio-stream SECONDS SEED...
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const int seconds = std::atoi(argv[1]);
  std::vector<std::vector<uint8_t>> seeds;
  for (int i = 2; i < argc; ++i) {
    std::ifstream in(argv[i], std::ios::binary);
    seeds.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  std::mt19937 rng(12345);
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  long runs = 0;
  while (std::chrono::steady_clock::now() < end) {
    std::vector<uint8_t> input = seeds[rng() % seeds.size()];
    if (input.empty()) input.push_back(0);
    const int edits = 1 + static_cast<int>(rng() % 64);
    for (int e = 0; e < edits; ++e) {
      const std::size_t at = rng() % input.size();
      switch (rng() % 5) {
        case 0: input[at] = static_cast<uint8_t>(rng()); break;
        case 1: input[at] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
        case 2: input[at] = 0xFF; if (at + 1 < input.size()) input[at + 1] = static_cast<uint8_t>(0xF0 | (rng() & 0x0F)); break;
        case 3: input.resize(1 + rng() % input.size()); break;
        default: input.insert(input.begin() + static_cast<long>(at), static_cast<uint8_t>(rng())); break;
      }
    }
    LLVMFuzzerTestOneInput(input.data(), input.size());
    ++runs;
  }
  std::printf("fuzz-radio-stream: %ld runs, no fault\n", runs);
  return 0;
}
#endif
