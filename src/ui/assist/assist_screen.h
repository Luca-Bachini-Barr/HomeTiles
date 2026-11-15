#pragma once

#include <stddef.h>
#include <stdint.h>

// The Assist screen: a popup card over everything (the system layer, so an
// open popup or folder stays as it was underneath) while a voice run goes:
// it opens on the panel's own wake word, shows "Listening…", then what Home
// Assistant understood, "Thinking…" and the answer, and closes a moment
// after the spoken answer ends. Driven by the Bridge's view command
// (assist_view.h) on {base}/cmnd/assist. Loop task only.
namespace assist_screen {

// Handles a view command (also from the encrypted channel). Returns true
// when the payload was a view command, handled or refused; false for every
// other Assist command.
bool handleCommandPayload(const uint8_t* payload, size_t length, bool sealed);

// Shows a new wake word detection, hides the screen when its stage timed
// out and keeps the display awake while it shows. Awake and sleep loop.
void service();

// The theme changed: the next opening builds the card in the new colors.
void resetForTheme();

bool visible();

}  // namespace assist_screen
