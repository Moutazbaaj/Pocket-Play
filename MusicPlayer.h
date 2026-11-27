#pragma once

#include <FS.h>
#include <memory>

class I2SClass;

// A single-track MP3 or PCM WAV decoder. The caller owns I2S and serializes all access to this object.
class MusicPlayer {
 public:
  enum class Result { Playing, Finished, BadTrack, OutputError };

  MusicPlayer();
  ~MusicPlayer();
  // Open an SD path. On failure return a displayable error string, otherwise return nullptr.
  const char *open(const char *path);
  // Close all file handles owned by this track and discard decoder state.
  void close();
  // Return whether the player has an allocated playback session.
  bool isOpen() const { return bool(buffers_); }
  // Report a requested seek that the decoder has yet to complete.
  bool isSeeking() const { return seeking_; }
  // Return the logical playback position in milliseconds.
  uint64_t positionMs() const;
  // Return duration in milliseconds, or zero until it is known.
  uint64_t durationMs() const { return durationMs_; }
  // Run an incremental duration scan when playback has enough queued audio.
  void updateDuration();
  // Move relative to the current position by a signed number of seconds.
  void seekBy(int seconds);
  // Schedule a seek to an absolute position in milliseconds.
  void seekTo(uint64_t positionMs);
  // Advance playback against the caller's I2S device. The volume callback receives individual int16_t values.
  Result update(I2SClass &audio, void (*applyVolume)(int16_t *, size_t), bool paused);

 private:
  struct Buffers;
  struct DurationScan;
  struct Mp3Decoder;
  enum class Decode { Data, Retry, End, Error };

  bool openWav();
  bool skipId3();
  bool setFormat(uint32_t rate, unsigned channels);
  Decode decode();
  Decode decodeMp3();
  Decode prepareOutputBlock(void (*applyVolume)(int16_t *, size_t));
  uint64_t outputPosition() const;
  void rememberMp3Frame(uint32_t offset);
  bool rewindMp3(uint64_t target);
  Result updateSeek();

  File file_;
  std::unique_ptr<Buffers> buffers_;
  std::unique_ptr<DurationScan> durationScan_;
  uint64_t durationMs_ = 0;
  std::unique_ptr<Mp3Decoder> mp3_;
  uint32_t wavDataOffset_ = 0;
  uint32_t wavDataLength_ = 0;
  size_t wavRemaining_ = 0;
  uint32_t mp3DataOffset_ = 0;
  size_t checkpointCount_ = 0;
  uint64_t checkpointInterval_ = 0;
  bool mp3Preroll_ = false;
  size_t inputBegin_ = 0;
  size_t inputEnd_ = 0;
  size_t frameIndex_ = 0;
  size_t decodedFrames_ = 0;
  unsigned channels_ = 0;
  unsigned repeat_ = 0;
  unsigned repeatPosition_ = 0;
  uint32_t outputRate_ = 0;
  size_t blockFrames_ = 0;
  size_t blockSent_ = 0;
  unsigned outputPrimeBlocks_ = 0;
  bool durationReadReady_ = false;
  bool draining_ = false;
  uint32_t drainStarted_ = 0;
  uint64_t decodedStartFrame_ = 0;
  uint64_t seekTargetMs_ = 0;
  bool seeking_ = false;
};
