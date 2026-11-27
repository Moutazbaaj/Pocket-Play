#include "AppRuntime.h"
#include "AppPackage.h"
#include "src/lua/lua.hpp"

#include <SD.h>
#include <algorithm>
#include <cstring>
#include <esp_heap_caps.h>

namespace {
constexpr size_t heapLimit = 96 * 1024;
constexpr uint32_t frameInterval = 33;
constexpr int hookInterval = 1000;
constexpr int maxHooks = 60;
constexpr int maxDrawPixels = AppSdk::width * AppSdk::height + 12000;
constexpr int maxDrawCalls = 96;
lua_State *vm = nullptr;
enum class Callback { Init, Update, Draw };
int callbacks[3];
AppPackage::Info appInfo;
AppSdk::PlayTone playTone = nullptr;
size_t allocated = 0;
uint32_t lastFrame = 0;
uint32_t callStarted = 0;
uint32_t lastSave = 0;
uint16_t pendingButtons = 0;
int hooks = 0;
int drawPixels = 0;
int drawCalls = 0;
bool savedThisRun = false;
bool accessedStorage = false;
char failure[128]{};

void *allocate(void *, void *pointer, size_t oldSize, size_t size) {
  if (!pointer) oldSize = 0;
  if (!size) {
    heap_caps_free(pointer);
    allocated -= oldSize;
    return nullptr;
  }
  if (size > heapLimit - (allocated - oldSize)) return nullptr;
  void *next = heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (next) allocated = allocated - oldSize + size;
  return next;
}

void limitExecution(lua_State *lua, lua_Debug *) {
  if (++hooks >= maxHooks || millis() - callStarted > 100) luaL_error(lua, "App exceeded its execution budget");
}

void chargeDraw(lua_State *lua, int pixels) {
  if (++drawCalls > maxDrawCalls || (drawPixels += pixels) > maxDrawPixels) {
    luaL_error(lua, "App exceeded its drawing budget");
  }
}

int number(lua_State *lua, int index, int low, int high) {
  const lua_Integer value = luaL_checkinteger(lua, index);
  luaL_argcheck(lua, value >= low && value <= high, index, "value out of range");
  return int(value);
}

int rgb(lua_State *lua) {
  lua_pushinteger(lua, AppSdk::rgb(number(lua, 1, 0, 255), number(lua, 2, 0, 255), number(lua, 3, 0, 255)));
  return 1;
}

int clear(lua_State *lua) {
  const int color = number(lua, 1, 0, 65535);
  chargeDraw(lua, AppSdk::width * AppSdk::height);
  AppSdk::fill(0, 0, AppSdk::width, AppSdk::height, color);
  return 0;
}

int fill(lua_State *lua) {
  const int x = number(lua, 1, -AppSdk::width, AppSdk::width);
  const int y = number(lua, 2, -AppSdk::height, AppSdk::height);
  const int w = number(lua, 3, 1, AppSdk::width);
  const int h = number(lua, 4, 1, AppSdk::height);
  const int color = number(lua, 5, 0, 65535);
  chargeDraw(lua, w * h);
  AppSdk::fill(x, y, w, h, color);
  return 0;
}

int text(lua_State *lua) {
  const int x = number(lua, 1, 0, AppSdk::width - 1);
  const int y = number(lua, 2, 0, AppSdk::height - 1);
  size_t length = 0;
  const char *label = luaL_checklstring(lua, 3, &length);
  luaL_argcheck(lua, length <= 40, 3, "text must be 40 bytes or less");
  const int fg = number(lua, 4, 0, 65535);
  const int bg = number(lua, 5, 0, 65535);
  const int scale = lua_isnoneornil(lua, 6) ? 1 : number(lua, 6, 1, 3);
  chargeDraw(lua, int(length) * 64 * scale * scale);
  AppSdk::text(x, y, label, fg, bg, scale);
  return 0;
}

int sprite(lua_State *lua) {
  const int w = number(lua, 3, 1, 32);
  const int h = number(lua, 4, 1, 32);
  const int x = number(lua, 1, 0, AppSdk::width - w);
  const int y = number(lua, 2, 0, AppSdk::height - h);
  size_t length = 0;
  const auto *pixels = reinterpret_cast<const uint8_t *>(luaL_checklstring(lua, 5, &length));
  luaL_argcheck(lua, length == size_t(w * h * 2), 5, "expected packed little-endian RGB565");
  chargeDraw(lua, w * h);
  uint16_t row[32];
  for (int j = 0; j < h; ++j) {
    for (int i = 0; i < w; ++i, pixels += 2) row[i] = uint16_t(pixels[0]) | (uint16_t(pixels[1]) << 8);
    AppSdk::sprite(x, y + j, w, 1, row);
  }
  return 0;
}

int tone(lua_State *lua) {
  const int frequency = number(lua, 1, 80, 4000);
  const int duration = number(lua, 2, 10, 250);
  lua_pushboolean(lua, playTone && playTone(frequency, duration));
  return 1;
}

int save(lua_State *lua) {
  size_t length = 0;
  const char *data = luaL_checklstring(lua, 1, &length);
  luaL_argcheck(lua, length <= AppPackage::maxSave, 1, "save must be 256 bytes or less");
  const uint32_t now = millis();
  const bool allowed = !accessedStorage && (!savedThisRun || now - lastSave >= 1000);
  accessedStorage = true;
  if (allowed) {
    lastSave = now;
    savedThisRun = true;
  }
  lua_pushboolean(lua, allowed && AppPackage::save(appInfo.id, data, length));
  return 1;
}

int load(lua_State *lua) {
  char data[AppPackage::maxSave];
  const int length = accessedStorage ? -1 : AppPackage::load(appInfo.id, data);
  accessedStorage = true;
  if (length < 0) lua_pushnil(lua);
  else lua_pushlstring(lua, data, length);
  return 1;
}

int openApi(lua_State *lua) {
  luaL_requiref(lua, "_G", luaopen_base, 0);
  lua_newtable(lua);
  constexpr const char *base[] = {"assert", "error", "ipairs", "next", "pairs", "select", "tonumber", "tostring", "type"};
  for (const char *name : base) {
    lua_getfield(lua, -2, name);
    lua_setfield(lua, -2, name);
  }
  lua_rawseti(lua, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
  lua_pop(lua, 1);
  luaL_requiref(lua, "math", luaopen_math, 1);
  lua_pop(lua, 1);
  constexpr luaL_Reg api[] = {
    {"rgb", rgb}, {"clear", clear}, {"fill", fill}, {"text", text}, {"sprite", sprite},
    {"tone", tone}, {"save", save}, {"load", load}, {nullptr, nullptr},
  };
  luaL_newlib(lua, api);
  constexpr struct { const char *key; int value; } constants[] = {
    {"SDK", AppSdk::version}, {"WIDTH", AppSdk::width}, {"HEIGHT", AppSdk::height},
    {"UP", AppSdk::Up}, {"DOWN", AppSdk::Down}, {"LEFT", AppSdk::Left}, {"RIGHT", AppSdk::Right},
    {"A", AppSdk::A}, {"B", AppSdk::B}, {"START", AppSdk::Start}, {"SELECT", AppSdk::Select},
    {"L", AppSdk::L}, {"R", AppSdk::R},
  };
  for (const auto &constant : constants) {
    lua_pushinteger(lua, constant.value);
    lua_setfield(lua, -2, constant.key);
  }
  lua_setglobal(lua, "app");
  return 0;
}

bool check(int status) {
  if (status == LUA_OK) return true;
  const char *message = lua_type(vm, -1) == LUA_TSTRING ? lua_tostring(vm, -1) : nullptr;
  snprintf(failure, sizeof(failure), "%s", message ? message : "App runtime error");
  lua_pop(vm, 1);
  return false;
}

bool call(int args) {
  hooks = drawPixels = drawCalls = 0;
  accessedStorage = false;
  callStarted = millis();
  lua_sethook(vm, limitExecution, LUA_MASKCOUNT, hookInterval);
  const int result = lua_pcall(vm, args, 0, 0);
  lua_sethook(vm, nullptr, 0, 0);
  return check(result);
}

int collectCallbacks(lua_State *lua) {
  constexpr const char *names[] = {"init", "update", "draw"};
  for (int i = 0; i < 3; ++i) {
    if (lua_getglobal(lua, names[i]) == LUA_TFUNCTION) {
      callbacks[i] = luaL_ref(lua, LUA_REGISTRYINDEX);
    } else {
      lua_pop(lua, 1);
      callbacks[i] = LUA_NOREF;
    }
  }
  if (callbacks[int(Callback::Draw)] == LUA_NOREF) return luaL_error(lua, "Expected a draw(full) function");
  return 0;
}

bool pushCallback(Callback callback) {
  const int ref = callbacks[int(callback)];
  if (ref == LUA_NOREF) return false;
  lua_rawgeti(vm, LUA_REGISTRYINDEX, ref);
  return true;
}

bool draw(bool full) {
  pushCallback(Callback::Draw);
  lua_pushboolean(vm, full);
  return call(1);
}

struct ScriptReader {
  File &file;
  size_t remaining;
  char bytes[256];
  bool interrupted = false;
};

const char *readScript(lua_State *, void *context, size_t *length) {
  auto &source = *static_cast<ScriptReader *>(context);
  *length = 0;
  if (!source.remaining || source.interrupted) return nullptr;
  const size_t count = std::min(source.remaining, sizeof(source.bytes));
  if (source.file.read(reinterpret_cast<uint8_t *>(source.bytes), count) != count) {
    source.interrupted = true;
    return nullptr;
  }
  source.remaining -= count;
  *length = count;
  return source.bytes;
}

bool loadScript(const char *path) {
  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory() || !file.size() || file.size() > AppPackage::maxBytes) {
    snprintf(failure, sizeof(failure), "Could not read the app from microSD");
    return false;
  }
  ScriptReader source{file, file.size(), {}};
  const int status = lua_load(vm, readScript, &source, appInfo.id, "t");
  file.close();
  if (source.interrupted) {
    snprintf(failure, sizeof(failure), "App read was interrupted");
    return false;
  }
  return check(status) && call(0);
}
}

bool AppRuntime::start(const char *path, AppSdk::PlayTone toneOutput) {
  stop();
  if (!AppPackage::inspect(path, appInfo)) {
    snprintf(failure, sizeof(failure), "Invalid Pocket Play app");
    return false;
  }
  vm = lua_newstate(allocate, nullptr);
  if (!vm) {
    snprintf(failure, sizeof(failure), "App needs available PSRAM");
    return false;
  }
  playTone = toneOutput;
  savedThisRun = false;
  pendingButtons = 0;
  lua_pushcfunction(vm, openApi);
  if (!call(0) || !loadScript(path)) {
    stop();
    return false;
  }
  lua_pushcfunction(vm, collectCallbacks);
  if (!call(0) || (pushCallback(Callback::Init) && !call(0)) || !redraw()) {
    stop();
    return false;
  }
  lastFrame = millis();
  return true;
}

bool AppRuntime::running() { return vm != nullptr; }

bool AppRuntime::tick(uint32_t now, AppSdk::Input input) {
  pendingButtons |= input.pressed;
  const uint32_t elapsed = now - lastFrame;
  if (elapsed < frameInterval) return true;
  lastFrame = now;
  const uint16_t pressed = pendingButtons;
  pendingButtons = 0;
  if (pushCallback(Callback::Update)) {
    lua_pushinteger(vm, std::min<uint32_t>(elapsed, 100));
    lua_pushinteger(vm, pressed);
    lua_pushinteger(vm, input.held);
    if (!call(3)) return false;
  }
  return draw(false);
}

bool AppRuntime::redraw() {
  AppSdk::fill(0, 0, AppSdk::width, AppSdk::height, 0);
  return draw(true);
}

void AppRuntime::stop() {
  if (vm) lua_close(vm);
  vm = nullptr;
  playTone = nullptr;
}

const char *AppRuntime::error() { return failure; }
