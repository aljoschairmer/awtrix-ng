#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace awtrix::radio {

enum class Codec : uint8_t { Unknown, Mp3, Unsupported };

// What the station's Content-Type claims. A guess only: many AAC+ stations call themselves
// audio/mpeg, so the bytes decide (sniff) and this only breaks a tie. name gets the format's
// name when it is one the radio cannot play ("aac", "ogg", "opus", "flac", "hls", ...). An AAC
// type stays Unknown with name "aac": the bytes may still be MP3, and if they are not, the
// error names AAC.
Codec codecFromContentType(const std::string& type, std::string* name = nullptr);

// Looks at the start of a stream: an ID3 tag is skipped, Ogg and FLAC are recognised by their
// magic, and MP3 or AAC in ADTS needs three frame headers in a row, each where the one before
// says the next begins. AAC is Unsupported (name "aac"): only MP3 plays. Unknown while the bytes
// say neither yet; name as above.
Codec sniff(const uint8_t* data, std::size_t size, std::string* name = nullptr);

const char* codecName(Codec codec);

}
