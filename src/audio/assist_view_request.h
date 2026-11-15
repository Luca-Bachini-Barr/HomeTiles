#pragma once

// ArduinoJson parser for the Assist view command (assist_view.h). Separate
// from the model so the model's host test needs no ArduinoJson.

#include <ArduinoJson.h>

#include "src/audio/assist_view.h"

namespace assist_view {

enum class ParseResult : uint8_t { NotView, Ok, Invalid };

// NotView for every payload that is not a view command (the other Assist
// commands, also malformed ones: assist_request.h reports those).
inline ParseResult parseViewCommand(const char* payload, size_t length, Update* out) {
  if (!payload || !out || length == 0 || length > kMaxViewPayloadBytes) return ParseResult::NotView;
  JsonDocument doc;
  if (deserializeJson(doc, payload, length) != DeserializationError::Ok ||
      !doc.is<JsonObjectConst>()) {
    return ParseResult::NotView;
  }
  JsonVariantConst op = doc["op"];
  if (!op.is<const char*>() || strcmp(op.as<const char*>(), "view") != 0) return ParseResult::NotView;
  *out = Update{};
  JsonVariantConst version = doc["v"];
  if (!version.is<long long>() || version.as<long long>() != 1) return ParseResult::Invalid;
  JsonVariantConst id = doc["id"];
  const char* id_text = id.is<const char*>() ? id.as<const char*>() : nullptr;
  const size_t id_length = id_text ? strlen(id_text) : 0;
  if (!isValidId(id_text, id_length)) return ParseResult::Invalid;
  memcpy(out->id, id_text, id_length + 1);
  JsonVariantConst stage = doc["stage"];
  if (!stage.is<const char*>() || !parseStage(stage.as<const char*>(), &out->stage)) {
    return ParseResult::Invalid;
  }
  JsonVariantConst heard = doc["heard"];
  if (heard.is<const char*>()) {
    out->has_heard = true;
    copyText(heard.as<const char*>(), out->heard, sizeof(out->heard));
  }
  JsonVariantConst answer = doc["answer"];
  if (answer.is<const char*>()) {
    out->has_answer = true;
    copyText(answer.as<const char*>(), out->answer, sizeof(out->answer));
  }
  if (out->stage == Stage::Error) {
    JsonVariantConst error = doc["error"];
    out->error = parseErrorKind(error.is<const char*>() ? error.as<const char*>() : nullptr);
  }
  return ParseResult::Ok;
}

}  // namespace assist_view
