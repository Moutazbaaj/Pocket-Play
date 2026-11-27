#pragma once

#include "AppSdk.h"

namespace NativeCounter {
void open(AppSdk::PlayTone playTone);
void update(AppSdk::Input input);
void draw();
void close();
}
