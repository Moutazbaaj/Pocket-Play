#include "SnakeGame.h"
#include "AppSdk.h"
#include "Display.h"

#include <algorithm>
#include <bitset>

namespace {
constexpr int columns = 28;
constexpr int rows = 15;
constexpr int cells = columns * rows;
constexpr int tile = 10;
constexpr int left = (AppSdk::width - columns * tile) / 2;
constexpr int top = 52;
constexpr uint16_t fruitColor = AppSdk::rgb(255, 104, 60);
constexpr uint16_t headColor = AppSdk::rgb(132, 230, 82);
constexpr uint16_t bodyColor = AppSdk::rgb(52, 164, 79);
uint16_t body[cells];
std::bitset<cells> occupied;
int head = 0;
int length = 0;
int fruit = -1;
uint16_t direction = AppSdk::Right;
uint16_t nextDirection = AppSdk::Right;
uint32_t lastMove = 0;
bool finished = false;

void drawCell(int cell, uint16_t color) {
  AppSdk::fill(left + cell % columns * tile, top + cell / columns * tile, tile - 1, tile - 1, color);
}

void drawScore() {
  char message[38];
  const char *state = !finished ? "" : length == cells ? "    BOARD CLEARED!" : "    GAME OVER";
  snprintf(message, sizeof(message), "SCORE %d%s", length - 3, state);
  AppSdk::fill(left, 209, AppSdk::width - left, 8, Screen::theme.background);
  AppSdk::text(left, 209, message, Screen::theme.text, Screen::theme.background);
}

void placeFruit() {
  fruit = -1;
  if (length == cells) return;
  int freeIndex = random(cells - length);
  for (int i = 0; i < cells; ++i) {
    if (!occupied[i] && freeIndex-- == 0) {
      fruit = i;
      return;
    }
  }
}
}

void SnakeGame::reset(uint32_t now) {
  occupied.reset();
  const int start = (rows / 2) * columns + columns / 2;
  for (int i = 0; i < 3; ++i) {
    body[i] = start - 2 + i;
    occupied.set(body[i]);
  }
  head = 2;
  length = 3;
  direction = nextDirection = AppSdk::Right;
  finished = false;
  lastMove = now;
  placeFruit();
}

void SnakeGame::draw() {
  AppSdk::fill(left, top, columns * tile, rows * tile, Screen::theme.panel);
  for (int i = 0; i < cells; ++i) {
    if (occupied[i]) drawCell(i, i == body[head] ? headColor : bodyColor);
  }
  if (fruit >= 0) drawCell(fruit, fruitColor);
  drawScore();
}

void SnakeGame::update(uint32_t now, uint16_t pressed) {
  if ((pressed & AppSdk::Start) || (finished && (pressed & AppSdk::A))) {
    reset(now);
    draw();
    return;
  }
  if (finished) return;
  if (direction & (AppSdk::Left | AppSdk::Right)) {
    if (pressed & AppSdk::Up) nextDirection = AppSdk::Up;
    else if (pressed & AppSdk::Down) nextDirection = AppSdk::Down;
  } else {
    if (pressed & AppSdk::Left) nextDirection = AppSdk::Left;
    else if (pressed & AppSdk::Right) nextDirection = AppSdk::Right;
  }
  if (now - lastMove < uint32_t(std::max(80, 175 - (length - 3) * 4))) return;
  lastMove = now;
  direction = nextDirection;
  const int oldHead = body[head];
  const int x = oldHead % columns + (direction == AppSdk::Right) - (direction == AppSdk::Left);
  const int y = oldHead / columns + (direction == AppSdk::Down) - (direction == AppSdk::Up);
  const int next = y * columns + x;
  const int tailCell = body[(head + cells + 1 - length) % cells];
  const bool eats = next == fruit;
  if (x < 0 || x >= columns || y < 0 || y >= rows || (occupied[next] && (eats || next != tailCell))) {
    finished = true;
    drawScore();
    return;
  }
  if (eats) {
    ++length;
  } else {
    occupied.reset(tailCell);
    drawCell(tailCell, Screen::theme.panel);
  }
  drawCell(oldHead, bodyColor);
  head = (head + 1) % cells;
  body[head] = next;
  occupied.set(next);
  drawCell(next, headColor);
  if (eats) {
    placeFruit();
    if (fruit >= 0) drawCell(fruit, fruitColor);
    else finished = true;
    drawScore();
  }
}
