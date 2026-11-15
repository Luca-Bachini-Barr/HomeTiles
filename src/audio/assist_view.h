#pragma once

// Pure, host-testable part of the Assist screen (src/ui/assist): the view
// command the Bridge sends while an Assist run is going, and the screen's
// state model. Nothing here touches LVGL, FreeRTOS or Arduino.
//
// MQTT contract (v1 addendum to assist_contract.h, same command topic and
// sealed data name "assist"):
//   {base}/cmnd/assist
//     {"v":1,"op":"view","id":"<16-32 lowercase hex>","stage":"<stage>",
//      "heard":"<text>","answer":"<text>","error":"<code>"}
//   stage: "listening" (capture started), "processing" (speech-to-text done;
//   "heard" = what was understood), "responding" (the agent answered;
//   "answer" = the reply text, "heard" may repeat), "error" ("error" =
//   "no_speech" or "failed") and "done" (the spoken answer finished or the
//   run ended without one). heard/answer/error are optional. Panels announce
//   the capability "assist_view"; the Bridge sends nothing to other panels.
//   A view command may be up to kMaxViewPayloadBytes, unlike the other
//   Assist commands (kMaxRequestPayloadBytes).
//
// The panel also shows the screen on its own wake word detection, before
// the Bridge answers (Model::onWake).

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace assist_view {

constexpr size_t kMaxViewPayloadBytes = 2048;
constexpr size_t kMaxIdLength = 32;
constexpr size_t kMinIdLength = 16;
// Text limits in UTF-8 bytes; longer texts are cut at a character boundary.
constexpr size_t kMaxHeardBytes = 255;
constexpr size_t kMaxAnswerBytes = 1279;

enum class Stage : uint8_t { Listening, Processing, Responding, Error, Done };
enum class ErrorKind : uint8_t { None, NoSpeech, Failed };

inline bool parseStage(const char* text, Stage* out) {
  if (!text || !out) return false;
  static const struct {
    const char* name;
    Stage stage;
  } kStages[] = {{"listening", Stage::Listening},
                 {"processing", Stage::Processing},
                 {"responding", Stage::Responding},
                 {"error", Stage::Error},
                 {"done", Stage::Done}};
  for (const auto& entry : kStages) {
    if (strcmp(text, entry.name) == 0) {
      *out = entry.stage;
      return true;
    }
  }
  return false;
}

// Unknown error codes show as a general failure.
inline ErrorKind parseErrorKind(const char* text) {
  if (text && strcmp(text, "no_speech") == 0) return ErrorKind::NoSpeech;
  return ErrorKind::Failed;
}

inline bool isValidId(const char* id, size_t length) {
  if (!id || length < kMinIdLength || length > kMaxIdLength) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = id[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// Copies at most `capacity - 1` bytes of UTF-8 `text` into `out`, never
// splitting a character; control characters other than the line break
// become spaces. Returns the copied length.
inline size_t copyText(const char* text, char* out, size_t capacity) {
  if (!out || capacity == 0) return 0;
  out[0] = '\0';
  if (!text) return 0;
  const size_t limit = capacity - 1;
  size_t length = strlen(text);
  if (length > limit) {
    length = limit;
    // Step back over continuation bytes to the start of the cut character.
    while (length > 0 && (static_cast<uint8_t>(text[length]) & 0xC0) == 0x80) --length;
  }
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    out[i] = (c < 0x20 && c != '\n') || c == 0x7F ? ' ' : static_cast<char>(c);
  }
  out[length] = '\0';
  return length;
}

struct Update {
  Stage stage = Stage::Done;
  char id[kMaxIdLength + 1] = {};
  bool has_heard = false;
  char heard[kMaxHeardBytes + 1] = {};
  bool has_answer = false;
  char answer[kMaxAnswerBytes + 1] = {};
  ErrorKind error = ErrorKind::None;
};

// What the screen shows.
enum class Screen : uint8_t { Hidden, Listening, Processing, Responding, Error };

// Timeouts (ms). A stage that never moves on hides the screen, so a lost
// "done" cannot leave it up.
constexpr uint32_t kWakeTimeoutMs = 8000;         // wake heard, no Bridge answer
constexpr uint32_t kListeningTimeoutMs = 35000;   // capture is at most 30 s
constexpr uint32_t kProcessingTimeoutMs = 60000;  // the agent thinks
constexpr uint32_t kRespondingTimeoutMs = 150000; // Bridge TTS timeout is 120 s
constexpr uint32_t kErrorLingerMs = 5000;
constexpr uint32_t kDoneLingerMs = 6000;
constexpr uint32_t kDoneWithoutAnswerMs = 1500;
// A run that starts this soon after the user closed the screen while it
// waited for the Bridge belongs to the closed screen.
constexpr uint32_t kDismissGraceMs = 5000;
// Runs that ended (done and closed, timed out, closed by the user or
// replaced by a newer run) are remembered, so their late updates never take
// the screen again.
constexpr size_t kEndedRuns = 4;

// A cheap check for a view command before it is parsed (a paired panel
// refuses unencrypted ones without spending the JSON parse on them). The
// Bridge sends compact JSON.
inline bool looksLikeViewCommand(const char* payload, size_t length) {
  static const char kNeedle[] = "\"op\":\"view\"";
  const size_t needle = sizeof(kNeedle) - 1;
  if (!payload || length < needle) return false;
  for (size_t i = 0; i + needle <= length; ++i) {
    if (memcmp(payload + i, kNeedle, needle) == 0) return true;
  }
  return false;
}

inline bool reached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

class Model {
 public:
  Screen screen() const { return screen_; }
  bool visible() const { return screen_ != Screen::Hidden; }
  const char* id() const { return id_; }
  const char* heard() const { return heard_; }
  const char* answer() const { return answer_; }
  ErrorKind error() const { return error_; }
  // Changes whenever anything shown changed.
  uint32_t revision() const { return revision_; }

  // The panel detected its wake word: show at once, before the Bridge's run.
  void onWake(uint32_t now) {
    // A run shown until now ends; closed runs stay closed (their late
    // updates must not take the new screen).
    endRun();
    clearRun();
    dismissed_without_id_ = false;
    show(Screen::Listening, now + kWakeTimeoutMs);
  }

  // Returns true when the update changed the screen.
  bool apply(const Update& update, uint32_t now) {
    if (!update.id[0]) return false;
    if (ended(update.id)) return false;
    const bool same_run = id_[0] && strcmp(update.id, id_) == 0;
    if (!same_run) {
      // Closed while waiting for the Bridge: its run stays closed.
      if (dismissed_without_id_ && !reached(now, dismissed_at_ + kDismissGraceMs)) {
        remember(update.id);
        dismissed_without_id_ = false;
        return false;
      }
      // An end for a run that is not shown has nothing to end.
      if (update.stage == Stage::Done || (update.stage == Stage::Error && !visible())) return false;
      // A new run (or the wake word's run) takes the screen; the one it
      // replaces has ended.
      endRun();
      if (id_[0] || screen_ != Screen::Listening) clearRun();
      copyId(id_, update.id);
    }
    if (update.has_heard) copyText(update.heard, heard_, sizeof(heard_));
    if (update.has_answer) copyText(update.answer, answer_, sizeof(answer_));
    switch (update.stage) {
      case Stage::Listening:
        if (!same_run) error_ = ErrorKind::None;
        show(Screen::Listening, now + kListeningTimeoutMs);
        break;
      case Stage::Processing:
        show(Screen::Processing, now + kProcessingTimeoutMs);
        break;
      case Stage::Responding:
        show(Screen::Responding, now + kRespondingTimeoutMs);
        break;
      case Stage::Error:
        error_ = update.error == ErrorKind::None ? ErrorKind::Failed : update.error;
        show(Screen::Error, now + kErrorLingerMs);
        break;
      case Stage::Done:
        if (!visible()) return false;
        // The answer stays readable a moment; a run that ended without
        // one (nothing heard, no answer) closes quickly.
        deadline_ = now + (screen_ == Screen::Responding || screen_ == Screen::Error
                               ? (screen_ == Screen::Error ? kErrorLingerMs : kDoneLingerMs)
                               : kDoneWithoutAnswerMs);
        ++revision_;
        break;
    }
    return true;
  }

  // Hides the screen once its stage timed out. True when it changed.
  bool tick(uint32_t now) {
    if (!visible() || !reached(now, deadline_)) return false;
    screen_ = Screen::Hidden;
    endRun();
    clearRun();
    ++revision_;
    return true;
  }

  // The user closed the screen: later updates of this run stay hidden.
  void dismiss(uint32_t now) {
    if (!visible()) return;
    if (id_[0]) {
      endRun();
      dismissed_without_id_ = false;
    } else {
      dismissed_without_id_ = true;
      dismissed_at_ = now;
    }
    screen_ = Screen::Hidden;
    clearRun();
    ++revision_;
  }

  uint32_t deadline() const { return deadline_; }

 private:
  static void copyId(char* out, const char* id) {
    const size_t length = strnlen(id, kMaxIdLength);
    memcpy(out, id, length);
    out[length] = '\0';
  }

  bool ended(const char* id) const {
    for (const auto& entry : ended_) {
      if (entry[0] && strcmp(entry, id) == 0) return true;
    }
    return false;
  }

  void remember(const char* id) {
    if (!id[0] || ended(id)) return;
    copyId(ended_[ended_next_], id);
    ended_next_ = static_cast<uint8_t>((ended_next_ + 1) % kEndedRuns);
  }

  // The run shown (if it has an id) is over.
  void endRun() { remember(id_); }

  void clearRun() {
    id_[0] = '\0';
    heard_[0] = '\0';
    answer_[0] = '\0';
    error_ = ErrorKind::None;
  }

  void show(Screen screen, uint32_t deadline) {
    screen_ = screen;
    deadline_ = deadline;
    ++revision_;
  }

  Screen screen_ = Screen::Hidden;
  char id_[kMaxIdLength + 1] = {};
  char ended_[kEndedRuns][kMaxIdLength + 1] = {};
  uint8_t ended_next_ = 0;
  bool dismissed_without_id_ = false;
  uint32_t dismissed_at_ = 0;
  char heard_[kMaxHeardBytes + 1] = {};
  char answer_[kMaxAnswerBytes + 1] = {};
  ErrorKind error_ = ErrorKind::None;
  uint32_t deadline_ = 0;
  uint32_t revision_ = 0;
};

}  // namespace assist_view
