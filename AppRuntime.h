#pragma once

#include "AppSdk.h"

namespace AppRuntime {
bool start(const char *path, AppSdk::PlayTone tone);
bool running();
bool tick(uint32_t now, AppSdk::Input input);
bool redraw();
void stop();
const char *error();
}
