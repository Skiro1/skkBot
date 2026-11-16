#pragma once

#include <string>

namespace skkbot2::autosave {


bool enabled();
void setEnabled(bool on);

void tickRecording();

void flushRecording();

std::string skkAutosaveName();
std::string skcAutosaveName();

}
