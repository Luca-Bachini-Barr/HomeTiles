#pragma once

// The Assist screen's wave (src/ui/assist), pure and host-tested: a row of
// bars whose middle shows the newest loudness and whose sides show older
// ones moving outwards, so speech spreads from the centre. Fed once per
// screen frame with the peak levels the audio worker measured
// (audio_level.h).

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace assist_wave {

constexpr int kBars = 31;
constexpr int kHalf = kBars / 2;  // bars on each side of the middle one
// The history moves one bar outwards every kShiftMs.
constexpr uint32_t kShiftMs = 66;
// A level falls by this share per frame once the voice gets quieter (fast
// rise, slower fall: the bars do not flicker).
constexpr float kDecay = 0.82f;
// No answer audio for this long (the clip still downloads): a breath in the
// answer color instead of a flat line.
constexpr uint32_t kAnswerQuietMs = 400;
// The breath while Home Assistant thinks: one swell per period.
constexpr uint32_t kBreathPeriodMs = 1400;
constexpr uint8_t kBreathLow = 30;
constexpr uint8_t kBreathHigh = 110;

enum class Source : uint8_t { Voice, Breath, Answer, Flat };

// Bar i's distance from the middle bar (0 = middle).
constexpr int distanceFromMiddle(int index) {
  return index < kHalf ? kHalf - index : index - kHalf;
}

// Bar height for a level: at least `min_height` (a dot), at most
// `max_height`, lower towards both ends of the row.
inline int barHeight(uint8_t level, int distance, int min_height, int max_height) {
  if (max_height < min_height) max_height = min_height;
  const float edge = static_cast<float>(distance) / static_cast<float>(kHalf);
  const float taper = 1.0f - 0.55f * edge * edge;
  const int height =
      min_height + static_cast<int>(static_cast<float>(max_height - min_height) *
                                        (static_cast<float>(level) / 255.0f) * taper +
                                    0.5f);
  return height < min_height ? min_height : (height > max_height ? max_height : height);
}

inline uint8_t breath(uint32_t now) {
  const float phase = static_cast<float>(now % kBreathPeriodMs) / static_cast<float>(kBreathPeriodMs);
  const float swell = 0.5f - 0.5f * cosf(phase * 6.2831853f);
  return static_cast<uint8_t>(kBreathLow + (kBreathHigh - kBreathLow) * swell + 0.5f);
}

class Wave {
 public:
  void reset() {
    memset(history_, 0, sizeof(history_));
    current_ = 0.0f;
    last_shift_ms_ = 0;
    last_answer_ms_ = 0;
    started_ = false;
  }

  // One screen frame: `mic`/`out` are the peak levels since the last frame.
  void step(Source source, uint8_t mic, uint8_t out, uint32_t now) {
    if (!started_) {
      started_ = true;
      last_shift_ms_ = now;
      last_answer_ms_ = now - kAnswerQuietMs;
    }
    uint8_t target = 0;
    switch (source) {
      case Source::Voice:
        target = mic;
        break;
      case Source::Breath:
        target = breath(now);
        break;
      case Source::Answer:
        if (out > 0) last_answer_ms_ = now;
        target = static_cast<int32_t>(now - last_answer_ms_) >= static_cast<int32_t>(kAnswerQuietMs)
                     ? breath(now)
                     : out;
        break;
      case Source::Flat:
        target = 0;
        break;
    }
    const float decayed = current_ * kDecay;
    current_ = target > decayed ? static_cast<float>(target) : decayed;
    history_[0] = static_cast<uint8_t>(current_ + 0.5f);
    // Older levels move outwards; a long gap (screen hidden) moves once.
    if (static_cast<int32_t>(now - last_shift_ms_) >= static_cast<int32_t>(kShiftMs)) {
      last_shift_ms_ = now;
      memmove(history_ + 1, history_, kHalf * sizeof(history_[0]));
    }
  }

  // The level `distance` bars from the middle (0 = the newest).
  uint8_t level(int distance) const {
    if (distance < 0) distance = -distance;
    return distance > kHalf ? 0 : history_[distance];
  }

 private:
  uint8_t history_[kHalf + 1] = {};
  float current_ = 0.0f;
  uint32_t last_shift_ms_ = 0;
  uint32_t last_answer_ms_ = 0;
  bool started_ = false;
};

}  // namespace assist_wave
