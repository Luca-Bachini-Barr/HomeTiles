// Host regression for the Assist screen: the view command the Bridge sends
// during an Assist run (assist_view.h, assist_view_request.h) and the
// screen's state model (wake word, stages, done linger, timeouts, a closed
// run staying closed). Also checks the firmware wiring: the command reaches
// the screen before the 256-byte Assist parser, the screen runs in the awake
// and the sleep loop, and the panel announces "assist_view".
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const out = path.join(root, 'build/tests/assist-view');
fs.mkdirSync(out, {recursive: true});
const read = (file) => fs.readFileSync(path.join(root, file), 'utf8');

// Wiring (no compiler needed).
const speaker = read('src/audio/speaker.cpp');
const hook = speaker.indexOf('assist_screen::handleCommandPayload(payload, length, sealed)');
const parser = speaker.indexOf('assist::parseCommand(reinterpret_cast<const char*>(payload), length, &command)');
assert.ok(hook > 0 && parser > hook, 'view commands are handled before the Assist parser and its 256-byte limit');
assert.match(speaker, /g_wake_detections\.fetch_add\(1\)/);
assert.match(speaker, /uint32_t wakeDetections\(\) \{ return g_wake_detections\.load\(\); \}/);
const ino = read('HomeTiles.ino');
assert.equal(ino.match(/assist_screen::service\(\);/g)?.length, 2, 'awake and sleep loop');
assert.match(read('src/network/bridge/ha_bridge_config.cpp'), /\\"assist_view\\":";\s*json \+= speaker::micCapability\(\)/);
assert.match(read('src/ui/ui_manager.cpp'), /popup_shell_delete_popups\(\);\s*assist_screen::resetForTheme\(\);/);
const screen = read('src/ui/assist/assist_screen.cpp');
assert.match(screen, /lv_layer_sys\(\)/, 'over the popups on the top layer');
assert.match(screen, /PairingState::Off/, 'a paired panel takes only sealed view commands');
assert.doesNotMatch(screen, /lv_label_set_text\([^,]+,\s*"[A-Za-z]/, 'no display text outside i18n');
const i18n = read('src/core/i18n/i18n.cpp');
assert.equal(i18n.match(/^\s*\{"Assist", /gm)?.length, 8, 'Assist labels in all 8 languages');
console.log('assist view wiring ok');

function findCompiler() {
  for (const candidate of [process.env.CXX, 'clang++', 'g++', 'c++'].filter(Boolean)) {
    const check = spawnSync(candidate, ['--version'], {encoding: 'utf8'});
    if (!check.error && check.status === 0) return candidate;
  }
  return null;
}

const compiler = findCompiler();
if (!compiler) {
  console.log('SKIP: assist view test needs a host C++ compiler');
  process.exit(0);
}

function compileAndRun(name, source, includes) {
  const cpp = path.join(out, `${name}.cpp`);
  const exe = path.join(out, process.platform === 'win32' ? `${name}.exe` : name);
  fs.writeFileSync(cpp, source);
  const args = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', root];
  for (const include of includes) args.push('-I', include);
  const build = spawnSync(compiler, [...args, cpp, '-o', exe], {encoding: 'utf8'});
  assert.equal(build.status, 0, `${name} did not compile:\n${build.stdout}${build.stderr}`);
  const run = spawnSync(exe, [], {encoding: 'utf8'});
  assert.equal(run.status, 0, `${name} failed:\n${run.stdout}${run.stderr}`);
  return run.stdout;
}

const modelProgram = String.raw`
#include "src/audio/assist_view.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace assist_view;

static const char* kRun = "0123456789abcdef";
static const char* kOther = "fedcba9876543210";

static Update update(Stage stage, const char* id = kRun, const char* heard = nullptr,
                     const char* answer = nullptr, ErrorKind error = ErrorKind::None) {
  Update u;
  u.stage = stage;
  strcpy(u.id, id);
  if (heard) { u.has_heard = true; strcpy(u.heard, heard); }
  if (answer) { u.has_answer = true; strcpy(u.answer, answer); }
  u.error = error;
  return u;
}

int main() {
  // Text copy: control characters become spaces, a cut never splits UTF-8.
  char small[6];
  assert(copyText("ab\tc\nd", small, sizeof(small)) == 5 && strcmp(small, "ab c\n") == 0);
  assert(copyText("abcd\xC3\xA9", small, sizeof(small)) == 4 && strcmp(small, "abcd") == 0);
  assert(copyText("\xD0\x94\xD0\x94\xD0\x94", small, sizeof(small)) == 4);
  assert(copyText(nullptr, small, sizeof(small)) == 0 && small[0] == '\0');
  assert(isValidId(kRun, 16) && !isValidId("0123456789ABCDEF", 16) && !isValidId("abc", 3));
  Stage stage;
  assert(parseStage("responding", &stage) && stage == Stage::Responding);
  assert(!parseStage("speaking", &stage));
  assert(parseErrorKind("no_speech") == ErrorKind::NoSpeech);
  assert(parseErrorKind("stt-stream-failed") == ErrorKind::Failed);
  assert(parseErrorKind(nullptr) == ErrorKind::Failed);

  // A full run: wake word, the Bridge's stages, done, the linger.
  Model m;
  assert(!m.visible());
  m.onWake(1000);
  assert(m.screen() == Screen::Listening && m.id()[0] == '\0');
  uint32_t revision = m.revision();
  assert(m.apply(update(Stage::Listening), 1200));
  assert(m.screen() == Screen::Listening && strcmp(m.id(), kRun) == 0 && m.revision() != revision);
  assert(m.apply(update(Stage::Processing, kRun, "turn on the lights"), 3000));
  assert(m.screen() == Screen::Processing && strcmp(m.heard(), "turn on the lights") == 0);
  assert(m.apply(update(Stage::Responding, kRun, nullptr, "Turned on the lights."), 4000));
  assert(m.screen() == Screen::Responding && strcmp(m.answer(), "Turned on the lights.") == 0);
  assert(strcmp(m.heard(), "turn on the lights") == 0);
  assert(!m.tick(100000));  // the answer is still being spoken
  assert(m.apply(update(Stage::Done), 100000));
  assert(!m.tick(100000 + kDoneLingerMs - 1) && m.visible());
  assert(m.tick(100000 + kDoneLingerMs) && !m.visible());
  assert(m.answer()[0] == '\0' && m.heard()[0] == '\0');
  // Late updates of the ended run do not open it again; done never opens.
  assert(!m.apply(update(Stage::Done), 100100) && !m.visible());
  assert(!m.apply(update(Stage::Error, kRun, nullptr, nullptr, ErrorKind::Failed), 100100));
  assert(!m.visible());

  // Tap to talk: the Bridge's listening opens the screen without a wake word.
  assert(m.apply(update(Stage::Listening, kOther), 200000) && m.screen() == Screen::Listening);
  // Done while still listening (nothing understood): closes quickly.
  assert(m.apply(update(Stage::Done, kOther), 201000));
  assert(m.tick(201000 + kDoneWithoutAnswerMs) && !m.visible());

  // An error shows its kind and closes after the linger.
  m.onWake(300000);
  assert(m.apply(update(Stage::Error, kRun, nullptr, nullptr, ErrorKind::NoSpeech), 301000));
  assert(m.screen() == Screen::Error && m.error() == ErrorKind::NoSpeech);
  assert(!m.tick(301000 + kErrorLingerMs - 1));
  assert(m.tick(301000 + kErrorLingerMs) && !m.visible());

  // A wake word without any Bridge answer times out.
  m.onWake(400000);
  assert(!m.tick(400000 + kWakeTimeoutMs - 1) && m.visible());
  assert(m.tick(400000 + kWakeTimeoutMs) && !m.visible());

  // A stage that never moves on hides the screen (lost done).
  assert(m.apply(update(Stage::Processing, "00000000000000000000000000000001", "hello"), 500000));
  assert(m.tick(500000 + kProcessingTimeoutMs) && !m.visible());

  // Closed by the user: the run stays closed.
  Model c;
  assert(c.apply(update(Stage::Listening), 1000));
  c.dismiss(1500);
  assert(!c.visible());
  assert(!c.apply(update(Stage::Processing, kRun, "x"), 2000) && !c.visible());
  assert(!c.apply(update(Stage::Responding, kRun, nullptr, "y"), 3000) && !c.visible());
  // A new wake word opens it again; the closed run's late answer does not
  // take the new screen.
  c.onWake(4000);
  assert(c.visible() && c.id()[0] == '\0');
  assert(!c.apply(update(Stage::Responding, kRun, nullptr, "late"), 4100));
  assert(c.answer()[0] == '\0');
  assert(c.apply(update(Stage::Listening, kOther), 4200) && strcmp(c.id(), kOther) == 0);

  // Closed while it waited for the Bridge (no id yet): the run that starts
  // right after belongs to it and stays closed; a later run shows.
  Model w;
  w.onWake(1000);
  w.dismiss(1100);
  assert(!w.apply(update(Stage::Listening), 1300) && !w.visible());
  assert(!w.apply(update(Stage::Processing, kRun, "x"), 2000) && !w.visible());
  Model w2;
  w2.onWake(1000);
  w2.dismiss(1100);
  assert(w2.apply(update(Stage::Listening), 1100 + kDismissGraceMs) && w2.visible());

  // A newer run replaces the one shown and clears its texts.
  Model n;
  n.apply(update(Stage::Responding, kRun, "a", "b"), 1000);
  assert(n.apply(update(Stage::Listening, kOther), 2000));
  assert(strcmp(n.id(), kOther) == 0 && n.heard()[0] == '\0' && n.answer()[0] == '\0');
  // An update without an id is ignored.
  Update empty;
  empty.stage = Stage::Listening;
  assert(!n.apply(empty, 3000));

  // millis() wraps.
  Model r;
  r.onWake(0xFFFFFF00u);
  assert(!r.tick(0xFFFFFF00u + 100));
  assert(r.tick(0xFFFFFF00u + kWakeTimeoutMs));

  puts("assist view model ok");
  return 0;
}
`;

console.log(compileAndRun('assist_view_model', modelProgram, []).trim());

const arduinoJsonCandidates = [
  process.env.ARDUINOJSON_SRC,
  path.join(os.homedir(), 'Arduino/libraries/ArduinoJson/src'),
  path.join(os.homedir(), 'Documents/Arduino/libraries/ArduinoJson/src'),
].filter(Boolean);
const arduinoJson = arduinoJsonCandidates.find((dir) => fs.existsSync(path.join(dir, 'ArduinoJson.h')));
if (!arduinoJson) {
  console.log('SKIP: assist view parser test needs the ArduinoJson library');
  process.exit(0);
}

const requestProgram = String.raw`
#include "src/audio/assist_view_request.h"
#include "src/audio/assist_request.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace assist_view;

static Update u;
static ParseResult parse(const std::string& json) {
  return parseViewCommand(json.c_str(), json.size(), &u);
}

int main() {
  assert(parse("{\"v\":1,\"op\":\"view\",\"id\":\"0123456789abcdef\",\"stage\":\"listening\"}") ==
         ParseResult::Ok);
  assert(u.stage == Stage::Listening && strcmp(u.id, "0123456789abcdef") == 0);
  assert(!u.has_heard && !u.has_answer);
  assert(parse("{\"v\":1,\"op\":\"view\",\"id\":\"0123456789abcdef\",\"stage\":\"responding\","
               "\"heard\":\"what's the weather\",\"answer\":\"Sunny, 12 \xC2\xB0" "C.\"}") ==
         ParseResult::Ok);
  assert(u.stage == Stage::Responding && u.has_heard && u.has_answer);
  assert(strcmp(u.heard, "what's the weather") == 0);
  assert(strcmp(u.answer, "Sunny, 12 \xC2\xB0" "C.") == 0);
  assert(parse("{\"v\":1,\"op\":\"view\",\"id\":\"0123456789abcdef\",\"stage\":\"error\","
               "\"error\":\"no_speech\"}") == ParseResult::Ok);
  assert(u.stage == Stage::Error && u.error == ErrorKind::NoSpeech);
  assert(parse("{\"v\":1,\"op\":\"view\",\"id\":\"0123456789abcdef\",\"stage\":\"error\"}") ==
         ParseResult::Ok && u.error == ErrorKind::Failed);
  // A long answer (beyond the other Assist commands' 256 bytes) is cut at a
  // character boundary.
  std::string longAnswer;
  for (int i = 0; i < 700; ++i) longAnswer += "\xC3\xA9";  // 1400 bytes, under the payload limit
  assert(parse("{\"v\":1,\"op\":\"view\",\"id\":\"0123456789abcdef\",\"stage\":\"responding\","
               "\"answer\":\"" + longAnswer + "\"}") == ParseResult::Ok);
  assert(strlen(u.answer) == kMaxAnswerBytes - 1 && strlen(u.answer) % 2 == 0);

  // Refused view commands.
  assert(parse("{\"v\":2,\"op\":\"view\",\"id\":\"0123456789abcdef\",\"stage\":\"done\"}") ==
         ParseResult::Invalid);
  assert(parse("{\"v\":1,\"op\":\"view\",\"id\":\"0123\",\"stage\":\"done\"}") == ParseResult::Invalid);
  assert(parse("{\"v\":1,\"op\":\"view\",\"id\":\"0123456789abcdef\",\"stage\":\"speaking\"}") ==
         ParseResult::Invalid);
  assert(parse("{\"v\":1,\"op\":\"view\",\"id\":\"0123456789abcdef\"}") == ParseResult::Invalid);
  // Everything else is for the Assist parser.
  assert(parse("{\"v\":1,\"op\":\"start\",\"id\":\"0123456789abcdef\"}") == ParseResult::NotView);
  assert(parse("{\"v\":1,\"op\":\"wake_words\",\"active\":[]}") == ParseResult::NotView);
  assert(parse("not json") == ParseResult::NotView);
  assert(parse(std::string(kMaxViewPayloadBytes + 1, ' ')) == ParseResult::NotView);
  // The Assist parser still refuses a view command (an older firmware).
  assist_contract::Command command;
  const std::string view = "{\"v\":1,\"op\":\"view\",\"id\":\"0123456789abcdef\",\"stage\":\"done\"}";
  assist_contract::parseCommand(view.c_str(), view.size(), &command);
  assert(command.status == assist_contract::CommandStatus::UnsupportedOp);

  puts("assist view parser ok");
  return 0;
}
`;

console.log(compileAndRun('assist_view_request', requestProgram, [arduinoJson]).trim());
