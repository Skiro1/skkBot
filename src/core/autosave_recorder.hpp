#pragma once

#include <string>

namespace skkbot2::autosave {

// Autosave while recording (Settings tab -> MACRO BOT -> Autosave).
//
// While either engine records (SKK MODE_CAPTURE or the SKC bot in Record
// state), a throttled rescue copy of the growing macro is written to the
// replays directory next to the file a later final SAVE would produce:
//
//     SKK: <level>_autosave.skk      (overwritten, never name-bumped)
//     SKC: <level>_autosave.skc      (overwritten)
//
// The base name is the LEVEL's name (user request, 31.10), sanitized for the
// filesystem. When no level is live or the name sanitizes away to nothing the
// own fallback "autosave" is used instead. The copies are rescue files, not
// saves: after a crash or a forgotten SAVE the last snapshot is still on disk.
// The final SAVE stay manual and keeps producing the unique-named real file.

// Persisted toggle ("gui_autosave_record", default on). Thread-safe.
bool enabled();
void setEnabled(bool on);

// Called once per drawn frame from applyCustomTickPlan (the same walk both
// recording engines ride). Cheap: two atomic loads plus one std::chrono call
// when nothing is due.
void tickRecording();

// One-shot last snapshot so the file on disk matches the end of the
// recording, not "whatever the last 10 s tick caught". MUST be called while
// the engines still report recording (before haltExecution()/bot.stop()) -
// it is deliberately a no-op from playback/idle, which is exactly what keeps
// autosave record-only. Cheap to call twice.
void flushRecording();

// Rescue file name (shared by both engines): the live level's name +
// "_autosave" (e.g. "Fingerbang_autosave"), sanitized for Windows paths;
// plain "autosave" when the level name is missing / collapses to nothing.
// Main-thread only (reads PlayLayer / the current level).
std::string skkAutosaveName();
std::string skcAutosaveName();

} // namespace skkbot2::autosave
