#include "MusicPlayer.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <SD.h>
#include <algorithm>
#include <iterator>
#include <new>
#include <utility>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "src/minimp3/minimp3.h"

namespace {
// The ESP_I2S driver in the documented Arduino-ESP32 build uses six 240-frame DMA buffers.
constexpr size_t kBlockFrames = 240;
constexpr unsigned kDmaBlocks = 6;
constexpr uint32_t kDmaFrames = kDmaBlocks * kBlockFrames;
constexpr size_t kMp3BufferBytes = 8192;
// Free-format sync needs room to inspect two following frame headers.
constexpr size_t kMp3SyncOverlap = 2 * MAX_FREE_FORMAT_FRAME_SIZE + HDR_SIZE;
constexpr size_t kDecodedSamples = MINIMP3_MAX_SAMPLES_PER_FRAME;
constexpr size_t kMp3CheckpointCount = 64;
constexpr unsigned kSeekDecodeBudget = 12;
constexpr unsigned kPlaybackRetryBudget = 6;
constexpr unsigned kDurationScanPrimeBlocks = 3;
constexpr uint32_t kDurationReadIntervalMs = 8;

// Read an unaligned 16-bit RIFF field.
uint16_t readLe16(const uint8_t *bytes) {
  return uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
}

// Read an unaligned 32-bit RIFF field.
uint32_t readLe32(const uint8_t *bytes) {
  return uint32_t(readLe16(bytes)) | (uint32_t(readLe16(bytes + 2)) << 16);
}

// Read an unaligned big-endian MP3 metadata field.
uint32_t readBe32(const uint8_t *bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
         (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

struct Mp3FrameHeader {
  unsigned size;
  unsigned sampleRate;
  unsigned samples;
  unsigned xingOffset;
};

// Parse MPEG version, rate, bitrate, and frame size for the duration scanner.
Mp3FrameHeader readMp3FrameHeader(const uint8_t *data) {
  constexpr unsigned sampleRates[] = {44100, 48000, 32000};
  constexpr uint16_t mpeg1Bitrates[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
  constexpr uint16_t mpeg2Bitrates[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
  const unsigned version = (data[1] >> 3) & 3;
  const unsigned layer = (data[1] >> 1) & 3;
  const unsigned bitrateIndex = data[2] >> 4;
  const unsigned rateIndex = (data[2] >> 2) & 3;
  if (data[0] != 0xff || (data[1] & 0xe0) != 0xe0 || layer != 1 || version == 1 ||
      bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3) {
    return {};
  }
  const bool mpeg1 = version == 3;
  const bool mono = (data[3] >> 6) == 3;
  const unsigned rate = sampleRates[rateIndex] >> (mpeg1 ? 0 : version == 2 ? 1 : 2);
  const unsigned samples = mpeg1 ? 1152 : 576;
  const unsigned bitrate = mpeg1 ? mpeg1Bitrates[bitrateIndex] : mpeg2Bitrates[bitrateIndex];
  const unsigned size = (samples / 8) * (bitrate * 1000) / rate + ((data[2] >> 1) & 1);
  const unsigned sideInfo = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
  return {size, rate, samples, 4 + sideInfo};
}

// Read a Xing, Info, or VBRI frame count when the first frame supplies one.
uint32_t readMp3FrameCount(const uint8_t *frame, size_t available, unsigned xingOffset) {
  if (available >= xingOffset + 12 &&
      (memcmp(frame + xingOffset, "Xing", 4) == 0 || memcmp(frame + xingOffset, "Info", 4) == 0) &&
      (readBe32(frame + xingOffset + 4) & 1)) {
    return readBe32(frame + xingOffset + 8);
  }
  constexpr size_t vbriOffset = 36;
  if (available >= vbriOffset + 18 && memcmp(frame + vbriOffset, "VBRI", 4) == 0 &&
      frame[vbriOffset + 4] == 0 && frame[vbriOffset + 5] == 1) {
    return readBe32(frame + vbriOffset + 14);
  }
  return 0;
}
}

struct MusicPlayer::Buffers {
  struct Checkpoint {
    uint64_t frame;
    uint32_t offset;
  };
  int16_t decoded[kDecodedSamples];
  alignas(4) int16_t block[kBlockFrames * 2];
  Checkpoint checkpoints[kMp3CheckpointCount];
};

struct MusicPlayer::Mp3Decoder {
  mp3dec_t state;
  mp3dec_scratch_t scratch;
  uint8_t input[kMp3BufferBytes];
};

struct MusicPlayer::DurationScan {
  File file;
  uint8_t bytes[2048];
  size_t fileSize = 0;
  size_t nextOffset = 0;
  uint64_t samples = 0;
  unsigned sampleRate = 0;
  uint32_t lastReadAt = 0;
};

// Start with no track open. Decoder and PCM buffers are allocated by open().
MusicPlayer::MusicPlayer() = default;

// Release an open file and any decoder buffers when the player is destroyed.
MusicPlayer::~MusicPlayer() {
  close();
}

// Open a track, allocate playback buffers, and initialize WAV or MP3 decoding.
const char *MusicPlayer::open(const char *path) {
  close();
  file_ = SD.open(path);
  if (!file_) {
    return "Could not open track";
  }

  const char *extension = strrchr(path, '.');
  if (extension && strcasecmp(extension, ".mp3") == 0) {
    if (!skipId3()) {
      close();
      return "Invalid MP3 file";
    }
    mp3_.reset(new (std::nothrow) Mp3Decoder);
    if (!mp3_) {
      close();
      return "Not enough memory for MP3";
    }
    mp3dec_init(&mp3_->state);
  } else if (!openWav()) {
    close();
    return "Use 16-bit PCM WAV or MP3";
  }

  buffers_.reset(new (std::nothrow) Buffers);
  if (!buffers_) {
    close();
    return "Not enough memory for audio";
  }
  if (mp3_) {
    auto scan = std::unique_ptr<DurationScan>(new (std::nothrow) DurationScan);
    if (scan) {
      scan->file = SD.open(path);
      if (scan->file) {
        scan->fileSize = scan->file.size();
        scan->nextOffset = mp3DataOffset_;
        scan->lastReadAt = millis() - kDurationReadIntervalMs;
        durationScan_ = std::move(scan);
      }
    }
  }
  return nullptr;
}

// Close file handles and reset decoder, duration, output, and seek state.
void MusicPlayer::close() {
  durationScan_.reset();
  durationMs_ = 0;
  file_.close();
  mp3_.reset();
  buffers_.reset();
  wavDataOffset_ = wavDataLength_ = 0;
  wavRemaining_ = 0;
  mp3DataOffset_ = 0;
  checkpointCount_ = 0;
  checkpointInterval_ = 0;
  mp3Preroll_ = false;
  inputBegin_ = inputEnd_ = 0;
  frameIndex_ = decodedFrames_ = 0;
  channels_ = repeat_ = repeatPosition_ = 0;
  outputRate_ = 0;
  blockFrames_ = blockSent_ = 0;
  outputPrimeBlocks_ = 0;
  durationReadReady_ = false;
  draining_ = false;
  decodedStartFrame_ = seekTargetMs_ = 0;
  seeking_ = false;
}

// Select the source channel count and an integer sample repeat factor for I2S output.
bool MusicPlayer::setFormat(uint32_t rate, unsigned channels) {
  if (channels != 1 && channels != 2) {
    return false;
  }
  // Repeat low-rate samples to reach an LRCLK supported by the MAX98357A.
  unsigned repeat = 1;
  switch (rate) {
    case 11025: case 12000:
      repeat = 4;
      break;
    case 22050: case 24000:
      repeat = 2;
      break;
    case 8000: case 16000: case 32000: case 44100: case 48000: case 88200: case 96000:
      break;
    default:
      return false;
  }
  const uint32_t outputRate = rate * repeat;
  if (outputRate_ && outputRate_ != outputRate) {
    return false;
  }
  outputRate_ = outputRate;
  repeat_ = repeat;
  channels_ = channels;
  return true;
}

// Find the format and data chunks, validate PCM parameters, and calculate duration.
bool MusicPlayer::openWav() {
  uint8_t header[12];
  if (file_.read(header, sizeof(header)) != sizeof(header) ||
      memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
    return false;
  }
  const uint64_t end = uint64_t(readLe32(header + 4)) + 8;
  if (end > file_.size()) {
    return false;
  }
  uint32_t rate = 0;
  unsigned channels = 0;
  uint32_t dataOffset = 0;
  uint32_t dataLength = 0;
  uint8_t chunk[8];
  while (uint64_t(file_.position()) + sizeof(chunk) <= end &&
         file_.read(chunk, sizeof(chunk)) == sizeof(chunk)) {
    const uint32_t length = readLe32(chunk + 4);
    const uint32_t position = file_.position();
    const uint64_t next = uint64_t(position) + length + (length & 1);
    if (next > end) {
      return false;
    }
    if (memcmp(chunk, "fmt ", 4) == 0) {
      uint8_t format[16];
      if (length < sizeof(format) || file_.read(format, sizeof(format)) != sizeof(format) ||
          readLe16(format) != 1 || readLe16(format + 14) != 16) {
        return false;
      }
      channels = readLe16(format + 2);
      rate = readLe32(format + 4);
      if (readLe16(format + 12) != channels * sizeof(int16_t)) {
        return false;
      }
    } else if (memcmp(chunk, "data", 4) == 0) {
      dataOffset = position;
      dataLength = length;
    }
    if (rate && dataLength) {
      if (!setFormat(rate, channels) || dataLength % (channels * sizeof(int16_t))) {
        return false;
      }
      wavDataOffset_ = dataOffset;
      wavDataLength_ = dataLength;
      wavRemaining_ = dataLength;
      const uint64_t frames = dataLength / (channels * sizeof(int16_t));
      durationMs_ = std::max<uint64_t>(1, frames * 1000 / rate);
      return file_.seek(dataOffset);
    }
    if (!file_.seek(static_cast<uint32_t>(next))) {
      return false;
    }
  }
  return false;
}

// Skip a leading ID3v2 tag so MP3 sync begins at the audio data.
bool MusicPlayer::skipId3() {
  uint8_t tag[10];
  if (file_.read(tag, sizeof(tag)) != sizeof(tag) || memcmp(tag, "ID3", 3) != 0) {
    return file_.seek(0);
  }
  uint32_t length = 0;
  for (size_t i = 6; i < sizeof(tag); ++i) {
    if (tag[i] & 0x80) {
      return false;
    }
    length = (length << 7) | tag[i];
  }
  mp3DataOffset_ = sizeof(tag) + length + ((tag[3] == 4 && (tag[5] & 0x10)) ? 10 : 0);
  return mp3DataOffset_ < file_.size() && file_.seek(mp3DataOffset_);
}

// Refill compressed input, decode one MP3 frame, and retain unread bytes for the next pass.
MusicPlayer::Decode MusicPlayer::decodeMp3() {
  auto &input = mp3_->input;
  size_t available = inputEnd_ - inputBegin_;
  if (available <= kMp3SyncOverlap && file_.position() < file_.size()) {
    memmove(input, input + inputBegin_, available);
    const size_t read = file_.read(input + available, sizeof(input) - available);
    if (!read || read > sizeof(input) - available) {
      return Decode::Error;
    }
    inputBegin_ = 0;
    inputEnd_ = available += read;
  }
  const bool atEnd = file_.position() >= file_.size();
  if (available <= HDR_SIZE) {
    return atEnd ? Decode::End : Decode::Retry;
  }

  const uint8_t *frame = input + inputBegin_;
  // Pass a complete frame when its size is known so trailing tags do not hide the last frame.
  const auto header = readMp3FrameHeader(frame);
  const size_t bytes = header.size && header.size <= available ? header.size : available;
  mp3dec_frame_info_t info{};
  const int samples = mp3dec_decode_frame_with_scratch(
      &mp3_->state, frame, int(bytes), buffers_->decoded, &info, &mp3_->scratch);
  if (!info.layer) {
    // Keep a partial frame or sync sequence that can be completed by the next read.
    inputBegin_ = atEnd ? inputEnd_ : inputEnd_ - std::min(available, kMp3SyncOverlap);
    return atEnd ? Decode::End : Decode::Retry;
  }

  const uint32_t frameOffset = file_.position() - available + info.frame_offset;
  inputBegin_ += info.frame_bytes;
  if (info.layer != 3 || (!samples && (!mp3Preroll_ || !mp3_->state.header[0]))) {
    return Decode::Retry;
  }
  if (!setFormat(info.hz, info.channels) || size_t(samples) * channels_ > kDecodedSamples) {
    return Decode::Error;
  }
  if (!samples) {
    // A checkpoint starts with an empty bit reservoir. Count the skipped frame for seek timing.
    const unsigned frameSamples = (frame[info.frame_offset + 1] & 8) ? 1152 : 576;
    decodedStartFrame_ += frameSamples * repeat_;
    return Decode::Retry;
  }
  mp3Preroll_ = false;
  decodedFrames_ = samples;
  rememberMp3Frame(frameOffset);
  return Decode::Data;
}

// Get the next PCM block from the active decoder or report retry, EOF, or error.
MusicPlayer::Decode MusicPlayer::decode() {
  decodedStartFrame_ += decodedFrames_ * repeat_;
  frameIndex_ = decodedFrames_ = repeatPosition_ = 0;
  if (mp3_) {
    return decodeMp3();
  }
  if (!wavRemaining_) {
    return Decode::End;
  }
  const size_t count = std::min(wavRemaining_, sizeof(buffers_->decoded));
  if (file_.read(reinterpret_cast<uint8_t *>(buffers_->decoded), count) != count) {
    return Decode::Error;
  }
  wavRemaining_ -= count;
  decodedFrames_ = count / (sizeof(int16_t) * channels_);
  return Decode::Data;
}

// Calculate position in output frames from the decoded PCM already packed into output blocks.
uint64_t MusicPlayer::outputPosition() const {
  return decodedStartFrame_ + frameIndex_ * repeat_ + repeatPosition_;
}

// Convert the output-frame cursor to milliseconds using the selected I2S sample rate.
uint64_t MusicPlayer::positionMs() const {
  return outputRate_ ? outputPosition() * 1000 / outputRate_ : 0;
}

// Scan MP3 frame headers incrementally using a separate file cursor after audio has been primed.
void MusicPlayer::updateDuration() {
  if (!durationScan_ || !durationReadReady_) {
    return;
  }
  auto &scan = *durationScan_;
  const uint32_t now = millis();
  if (now - scan.lastReadAt < kDurationReadIntervalMs) {
    return;
  }
  scan.lastReadAt = now;
  const size_t start = scan.nextOffset;
  if (start >= scan.fileSize || (scan.file.position() != start && !scan.file.seek(start))) {
    durationScan_.reset();
    return;
  }
  // A separate file handle keeps the duration scan independent of decoding and seeking.
  const size_t count = std::min(sizeof(scan.bytes), scan.fileSize - start);
  if (scan.file.read(scan.bytes, count) != count) {
    durationScan_.reset();
    return;
  }
  size_t cursor = 0;
  while (cursor + 4 <= count) {
    const uint8_t *data = scan.bytes + cursor;
    const size_t remaining = scan.fileSize - start - cursor;
    if (remaining == 128 && memcmp(data, "TAG", 3) == 0) {
      cursor += remaining;
      break;
    }
    const auto frame = readMp3FrameHeader(data);
    if (!frame.size || frame.size > remaining || (scan.sampleRate && scan.sampleRate != frame.sampleRate)) {
      ++cursor;
      continue;
    }
    if (!scan.sampleRate) {
      // Leave enough of the first frame in one read to inspect the optional frame count.
      constexpr size_t kTagProbeBytes = 54;
      const size_t available = std::min(count - cursor, size_t(frame.size));
      if (available < std::min(kTagProbeBytes, size_t(frame.size))) {
        break;
      }
      const uint32_t taggedFrames = readMp3FrameCount(data, available, frame.xingOffset);
      if (taggedFrames && taggedFrames <= remaining / 4) {
        durationMs_ = uint64_t(taggedFrames) * frame.samples * 1000 / frame.sampleRate;
        durationScan_.reset();
        return;
      }
      scan.sampleRate = frame.sampleRate;
    }
    scan.samples += frame.samples;
    cursor += frame.size;
  }
  scan.nextOffset = start + cursor;
  if (scan.fileSize - scan.nextOffset < 4) {
    if (scan.sampleRate) {
      durationMs_ = scan.samples * 1000 / scan.sampleRate;
    }
    durationScan_.reset();
  }
}

// Request a track position offset by a signed number of seconds.
void MusicPlayer::seekBy(int seconds) {
  const uint64_t base = seeking_ ? seekTargetMs_ : positionMs();
  const int64_t target = int64_t(base) + int64_t(seconds) * 1000;
  seekTo(target > 0 ? uint64_t(target) : 0);
}

// Clamp a seek target to the known duration and defer the actual repositioning to update.
void MusicPlayer::seekTo(uint64_t positionMs) {
  seekTargetMs_ = positionMs;
  seeking_ = true;
  blockFrames_ = blockSent_ = 0;
  outputPrimeBlocks_ = 0;
  durationReadReady_ = false;
  draining_ = false;
}

// Record bounded MP3 byte and sample checkpoints for faster future seeks.
void MusicPlayer::rememberMp3Frame(uint32_t offset) {
  auto &checkpoints = buffers_->checkpoints;
  if (!checkpointInterval_) {
    checkpointInterval_ = uint64_t(outputRate_) * 8;
  }
  const uint64_t lastFrame = checkpointCount_ ? checkpoints[checkpointCount_ - 1].frame : 0;
  if (decodedStartFrame_ < lastFrame + checkpointInterval_) {
    return;
  }
  if (checkpointCount_ == std::size(checkpoints)) {
    // Keep checkpoints across the entire track with a bounded memory cost.
    for (size_t i = 0; i < checkpointCount_ / 2; ++i) {
      checkpoints[i] = checkpoints[2 * i];
    }
    checkpointCount_ /= 2;
    checkpointInterval_ *= 2;
  }
  checkpoints[checkpointCount_++] = {decodedStartFrame_, offset};
}

// Restore the nearest usable checkpoint and reset decoder state for seek preroll.
bool MusicPlayer::rewindMp3(uint64_t target) {
  Buffers::Checkpoint start = {0, mp3DataOffset_};
  for (size_t i = checkpointCount_; i > 0; --i) {
    const auto &checkpoint = buffers_->checkpoints[i - 1];
    if (checkpoint.frame + outputRate_ <= target) {
      start = checkpoint;
      break;
    }
  }
  if (!file_.seek(start.offset)) {
    return false;
  }
  mp3dec_init(&mp3_->state);
  mp3Preroll_ = start.frame != 0;
  decodedStartFrame_ = start.frame;
  inputBegin_ = inputEnd_ = 0;
  frameIndex_ = decodedFrames_ = repeatPosition_ = 0;
  return true;
}

// Seek WAV data directly or move toward the MP3 target with a bounded decode budget.
MusicPlayer::Result MusicPlayer::updateSeek() {
  if (!mp3_) {
    const size_t bytesPerFrame = channels_ * sizeof(int16_t);
    const uint64_t frames = wavDataLength_ / bytesPerFrame;
    const uint64_t target = std::min(frames, seekTargetMs_ * (outputRate_ / repeat_) / 1000);
    const uint32_t offset = target * bytesPerFrame;
    if (!file_.seek(wavDataOffset_ + offset)) {
      return Result::BadTrack;
    }
    wavRemaining_ = wavDataLength_ - offset;
    frameIndex_ = decodedFrames_ = repeatPosition_ = 0;
    decodedStartFrame_ = target * repeat_;
    seeking_ = false;
    return Result::Playing;
  }

  if (outputRate_ && seekTargetMs_ * outputRate_ / 1000 < decodedStartFrame_) {
    if (!rewindMp3(seekTargetMs_ * outputRate_ / 1000)) {
      return Result::BadTrack;
    }
  }
  // Decode in small batches so the player can accept navigation and further seeks.
  for (unsigned i = 0; i < kSeekDecodeBudget; ++i) {
    if (outputRate_) {
      const uint64_t target = seekTargetMs_ * outputRate_ / 1000;
      const uint64_t frameEnd = decodedStartFrame_ + decodedFrames_ * repeat_;
      if (target >= decodedStartFrame_ && target <= frameEnd) {
        const uint64_t offset = target - decodedStartFrame_;
        frameIndex_ = offset / repeat_;
        repeatPosition_ = offset % repeat_;
        seeking_ = false;
        return Result::Playing;
      }
    }
    const Decode result = decode();
    if (result == Decode::Error || (result == Decode::End && !outputRate_)) {
      return Result::BadTrack;
    }
    if (result == Decode::End) {
      seeking_ = false;
      return Result::Playing;
    }
  }
  return Result::Playing;
}

// Expand source PCM to interleaved stereo, repeat samples as needed, and apply software volume.
MusicPlayer::Decode MusicPlayer::prepareOutputBlock(void (*applyVolume)(int16_t *, size_t)) {
  if (blockFrames_ == kBlockFrames) {
    return Decode::Data;
  }
  auto &block = buffers_->block;
  while (blockFrames_ < kBlockFrames) {
    if (frameIndex_ == decodedFrames_) {
      const Decode result = decode();
      if (result == Decode::End && blockFrames_) {
        std::fill(block + blockFrames_ * 2, block + std::size(block), 0);
        blockFrames_ = kBlockFrames;
        break;
      }
      if (result != Decode::Data) {
        return result;
      }
    }
    const int16_t *frame = buffers_->decoded + frameIndex_ * channels_;
    block[blockFrames_ * 2] = frame[0];
    block[blockFrames_ * 2 + 1] = frame[channels_ - 1];
    ++blockFrames_;
    if (++repeatPosition_ == repeat_) {
      repeatPosition_ = 0;
      ++frameIndex_;
    }
  }
  applyVolume(block, std::size(block));
  return Decode::Data;
}

// Submit pending PCM without blocking on I2S and advance playback, seek, or drain state.
MusicPlayer::Result MusicPlayer::update(I2SClass &audio, void (*applyVolume)(int16_t *, size_t), bool paused) {
  durationReadReady_ = false;
  if (seeking_) {
    return updateSeek();
  }
  if (paused) {
    outputPrimeBlocks_ = 0;
    durationReadReady_ = true;
    return Result::Playing;
  }
  if (draining_) {
    const uint32_t drainMs = (kDmaFrames * 1000 + outputRate_ - 1) / outputRate_ + 2;
    durationReadReady_ = true;
    return millis() - drainStarted_ >= drainMs ? Result::Finished : Result::Playing;
  }
  const auto &block = buffers_->block;
  const auto *bytes = reinterpret_cast<const uint8_t *>(block);
  unsigned retries = kPlaybackRetryBudget;
  // Refill all available DMA buffers after a UI or SD delay, within one bounded update.
  for (unsigned blocks = 0; blocks < kDmaBlocks;) {
    const Decode result = prepareOutputBlock(applyVolume);
    if (result == Decode::Retry) {
      if (--retries == 0) {
        break;
      }
      continue;
    }
    if (result == Decode::Error || (result == Decode::End && !outputRate_)) {
      return Result::BadTrack;
    }
    if (result == Decode::End) {
      draining_ = true;
      drainStarted_ = millis();
      break;
    }
    if (audio.txSampleRate() != outputRate_) {
      if (!audio.configureTX(outputRate_, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
        return Result::OutputError;
      }
      outputPrimeBlocks_ = 0;
    }
    size_t written = 0;
    const esp_err_t status = i2s_channel_write(audio.txChan(), bytes + blockSent_,
                                               sizeof(block) - blockSent_, &written, 0);
    if (status != ESP_OK && status != ESP_ERR_TIMEOUT) {
      return Result::OutputError;
    }
    blockSent_ += written;
    if (blockSent_ < sizeof(block)) {
      // On startup a zero-timeout write can also occur before the first DMA buffer completes.
      durationReadReady_ = status == ESP_ERR_TIMEOUT && outputPrimeBlocks_ >= kDurationScanPrimeBlocks;
      break;
    }
    blockFrames_ = blockSent_ = 0;
    if (outputPrimeBlocks_ < kDurationScanPrimeBlocks) {
      ++outputPrimeBlocks_;
    }
    ++blocks;
  }
  return Result::Playing;
}
