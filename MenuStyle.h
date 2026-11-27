#pragma once

#include <stdint.h>

// Draw the shared launcher chrome and menu controls from the active Screen theme.
void drawHeader(const char *title, const char *subtitle, uint16_t accent);
void drawCard(int x, int y, int width, int height, bool selected, uint16_t accent);
void drawMenuValue(int center, int y, const char *value, uint16_t color,
                   uint16_t background, bool adjustable);
