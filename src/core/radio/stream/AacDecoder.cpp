#include "core/radio/stream/AacDecoder.h"

#include <aacdecoder_lib.h>

#include "core/radio/stream/AdtsHeader.h"

namespace awtrix::radio {
namespace {

HANDLE_AACDECODER open() {
  HANDLE_AACDECODER handle = aacDecoder_Open(TT_MP4_ADTS, 1);
  if (!handle) return nullptr;
  // The speaker takes stereo at most; mono stays mono.
  aacDecoder_SetParam(handle, AAC_PCM_MAX_OUTPUT_CHANNELS, 2);
  // A damaged frame is filled in from the last good one instead of clicking.
  aacDecoder_SetParam(handle, AAC_CONCEAL_METHOD, 1);
  return handle;
}

// The next byte that may start a header, after the one at data[0].
std::size_t nextSync(const uint8_t* data, std::size_t size) {
  for (std::size_t i = 1; i + 1 < size; ++i)
    if (data[i] == 0xFF && (data[i + 1] & 0xF6) == 0xF0) return i;
  // Keep a last 0xFF: the rest of its header may still be on its way.
  return size && data[size - 1] == 0xFF ? size - 1 : size;
}

}

AacDecoder::AacDecoder() : handle_(open()) {}

AacDecoder::~AacDecoder() {
  if (handle_) aacDecoder_Close(static_cast<HANDLE_AACDECODER>(handle_));
}

void AacDecoder::reset() {
  if (handle_) aacDecoder_Close(static_cast<HANDLE_AACDECODER>(handle_));
  handle_ = open();
  format_ = "";
  bitrateKbps_ = 0;
  averageKbps_ = 0;
}

mp3::DecodeResult AacDecoder::decode(const uint8_t* data, std::size_t bytes, int16_t* pcm) {
  mp3::DecodeResult result;
  if (!data || bytes < kAdtsHeaderBytes) return result;
  AdtsHeader header;
  if (!parseAdts(data, bytes, header)) {
    result.status = mp3::DecodeStatus::NoSync;
    result.bytesConsumed = nextSync(data, bytes);
    return result;
  }
  if (header.frameBytes > bytes) return result;
  result.bytesConsumed = header.frameBytes;
  auto* handle = static_cast<HANDLE_AACDECODER>(handle_);
  if (!handle) {
    result.status = mp3::DecodeStatus::Unsupported;
    return result;
  }
  // The profile in an ADTS header is 2 bits; only LC (and SBR or PS on top of LC) is broadcast.
  if (header.profile != 1) {
    result.status = mp3::DecodeStatus::Unsupported;
    return result;
  }
  UCHAR* in = const_cast<UCHAR*>(data);
  UINT size = static_cast<UINT>(header.frameBytes);
  UINT valid = size;
  if (aacDecoder_Fill(handle, &in, &size, &valid) != AAC_DEC_OK) {
    result.status = mp3::DecodeStatus::CorruptFrame;
    return result;
  }
  const AAC_DECODER_ERROR error = aacDecoder_DecodeFrame(handle, pcm, kMaxPcmPerFrame, 0);
  if (error == AAC_DEC_NOT_ENOUGH_BITS) {
    // The transport layer waits for the next header before it trusts this one: no output yet.
    result.status = mp3::DecodeStatus::Ok;
    return result;
  }
  if (!IS_OUTPUT_VALID(error)) {
    result.status = mp3::DecodeStatus::CorruptFrame;
    return result;
  }
  const CStreamInfo* info = aacDecoder_GetStreamInfo(handle);
  if (!info || info->numChannels < 1 || info->numChannels > 2 || info->frameSize <= 0) {
    result.status = mp3::DecodeStatus::Unsupported;
    return result;
  }
  format_ = info->aot == AOT_PS ? "he-aac-v2" : info->aot == AOT_SBR ? "he-aac" : "aac";
  // ADTS carries no bitrate: it is the frame's size over the time the frame lasts, smoothed.
  if (info->aacSampleRate > 0 && info->aacSamplesPerFrame > 0) {
    const double kbps = header.frameBytes * 8.0 * info->aacSampleRate / info->aacSamplesPerFrame / 1000.0;
    averageKbps_ = averageKbps_ > 0 ? averageKbps_ * 0.9 + kbps * 0.1 : kbps;
    bitrateKbps_ = static_cast<int>(averageKbps_ + 0.5);
  }
  result.status = mp3::DecodeStatus::Ok;
  result.sampleRateHz = info->sampleRate;
  result.channels = info->numChannels;
  result.samples = info->frameSize;
  return result;
}

}
