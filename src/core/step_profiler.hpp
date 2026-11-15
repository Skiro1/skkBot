
#pragma once

namespace skkbot2::debug {

bool stepProfilerOptIn();

void profilerNoteMainThread();

void profilerStart(const char* label);

void profilerStopAndReport();

}
