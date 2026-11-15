#pragma once

// Loudness of a block of 16-bit PCM for the Assist screen's waves (the
// microphone while someone speaks, the speaker while the answer plays).
// Pure and host-tested; the audio worker calls it once per block (20 ms of
// capture, one decoded frame of playback).

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>

namespace audio_level {

// The RMS level on a speech scale: kFloorDbfs and below = 0, kTopDbfs and
// above = 255. Speech at the panel's microphone gain sits around -35..-15
// dBFS, quiet rooms below -55.
constexpr float kFloorDbfs = -55.0f;
constexpr float kTopDbfs = -10.0f;

inline uint8_t fromSamples(const int16_t* samples, size_t count) {
  if (!samples || count == 0) return 0;
  // Integer sum (the P4's FPU is single precision only; a block of 20 ms
  // never comes near the int64 range).
  int64_t sum = 0;
  for (size_t i = 0; i < count; ++i) {
    const int32_t s = samples[i];
    sum += static_cast<int64_t>(s * s);
  }
  const float rms = sqrtf(static_cast<float>(sum) / static_cast<float>(count)) / 32768.0f;
  if (rms <= 0.0f) return 0;
  const float dbfs = 20.0f * log10f(rms);
  if (dbfs <= kFloorDbfs) return 0;
  if (dbfs >= kTopDbfs) return 255;
  return static_cast<uint8_t>((dbfs - kFloorDbfs) * 255.0f / (kTopDbfs - kFloorDbfs) + 0.5f);
}

// Keeps the highest level until the reader takes it (peak hold between two
// screen frames). Any task.
inline void hold(std::atomic<uint8_t>& slot, uint8_t level) {
  uint8_t current = slot.load(std::memory_order_relaxed);
  while (level > current &&
         !slot.compare_exchange_weak(current, level, std::memory_order_relaxed)) {
  }
}

inline uint8_t take(std::atomic<uint8_t>& slot) {
  return slot.exchange(0, std::memory_order_relaxed);
}

}  // namespace audio_level
