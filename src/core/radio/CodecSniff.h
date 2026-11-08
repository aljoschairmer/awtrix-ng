#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace awtrix::radio {

enum class Codec : uint8_t { Unknown, Mp3, Aac, Unsupported };

// What the station's Content-Type claims. A guess only: many AAC+ stations call themselves
// audio/mpeg, so the bytes decide (sniff) and this only breaks a tie. name gets the format's
// name when it is one the radio cannot play ("ogg", "opus", "flac", "hls", ...).
Codec codecFromContentType(const std::string& type, std::string* name = nullptr);

// Looks at the start of a stream: an ID3 tag is skipped, Ogg and FLAC are recognised by their
// magic, and MP3 or ADTS needs three frame headers in a row, each where the one before says the
// next begins. Unknown while the bytes say neither yet; name as above.
Codec sniff(const uint8_t* data, std::size_t size, std::string* name = nullptr);

const char* codecName(Codec codec);

}
