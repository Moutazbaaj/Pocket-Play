#include "PhotoViewer.h"

#include <Preferences.h>
#include <SD.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>
#include <new>
#include <strings.h>
#include <vector>

#include "Display.h"
#include "MenuStyle.h"
#include "PhotoImage.h"

extern "C" {
#include "src/gnuboy/gnuboy.h"
}

const char *fileName(const String &path);

namespace {
constexpr char kPhotoRoot[] = "/photos";
constexpr size_t kMaxEntries = 512;
constexpr int kRows = 8;
constexpr int kGridColumns = 3;
constexpr int kGridCount = kGridColumns * 2;
constexpr uint32_t kThumbIdleMs = 150;
constexpr uint32_t kImageTaskStackBytes = 6144;
constexpr uint16_t kDirections = GB_PAD_UP | GB_PAD_DOWN | GB_PAD_LEFT | GB_PAD_RIGHT;

enum class Layout : uint8_t { List, Carousel, Grid };
enum class Page { Browser, Preview, Options, ConfirmDelete };
constexpr const char *kLayouts[] = {"LIST", "CAROUSEL", "GRID"};
constexpr int kLayoutCount = std::size(kLayouts);
struct Entry { String path; bool directory; };
struct Rect { int x, y, w, h; };
using Pixels = std::unique_ptr<uint16_t[], decltype(&free)>;
struct ImageSlot {
  int entry = -1;
  Rect bounds{};
  Pixels pixels{nullptr, free};
  PhotoImage::Result result = PhotoImage::Result::DecodeFailed;
  bool finished = false;
};
struct Request {
  String path;
  int slot;
  int width;
  int height;
  Pixels pixels{nullptr, free};
  PhotoImage::Result result = PhotoImage::Result::DecodeFailed;
  std::atomic<bool> cancelled{false};
  std::atomic<bool> done{false};
};

Preferences *settings = nullptr;
std::vector<Entry> entries;
String folder;
Layout layout = Layout::List;
Page page = Page::Browser;
Page optionsReturn = Page::Browser;
int selection = 0;
int option = 0;
bool deleteYes = false;
bool cardAvailable = false;
bool folderAvailable = false;
const char *notice = nullptr;
std::array<ImageSlot, kGridCount> slots;
std::unique_ptr<Request> request;
uint32_t artworkQueuedAt = 0;

uint16_t accent() { return Screen::themeAccent(0xB67F); }

String shortened(const char *text, unsigned count) {
  String label(text);
  if (label.length() > count) {
    label.remove(count - 2);
    label += "..";
  }
  return label;
}

bool selectedPhoto() { return !entries.empty() && !entries[selection].directory; }
int count() { return int(entries.size()); }
int wrap(int index, int size) { return (index + size) % size; }

void cancelRequest() {
  if (request) request->cancelled.store(true, std::memory_order_relaxed);
}

// Only the loop owns the slots and the TFT. The task borrows a request and closes its file before publishing.
void loadImage(void *arg) {
  auto &job = *static_cast<Request *>(arg);
  const size_t bytes = job.width * job.height * sizeof(uint16_t);
  auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!pixels) pixels = static_cast<uint16_t *>(malloc(bytes));
  job.pixels.reset(pixels);
  if (pixels) job.result = PhotoImage::load(job.path.c_str(), pixels, job.width, job.height, job.cancelled);
  job.done.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

void clearArtwork() {
  cancelRequest();
  for (auto &slot : slots) slot = ImageSlot{};
  artworkQueuedAt = millis();
}

void assignImage(int slot, int index, Rect bounds) {
  if (index >= 0 && !entries[index].directory) {
    slots[slot].entry = index;
    slots[slot].bounds = bounds;
  }
}

void drawPlaceholder(Rect box, bool directory, uint16_t bg) {
  Screen::fill(box.x, box.y, box.w, box.h, bg);
  const int x = box.x + (box.w - 24) / 2;
  const int y = box.y + (box.h - 16) / 2;
  Screen::fill(x, y, directory ? 10 : 24, 4, accent());
  Screen::fill(x, y + 4, 24, 12, accent());
  if (!directory) {
    Screen::fill(x + 2, y + 2, 20, 12, bg);
    Screen::fill(x + 5, y + 5, 3, 3, accent());
    Screen::fill(x + 12, y + 9, 7, 3, accent());
  }
}

void drawSlot(const ImageSlot &slot, uint16_t bg) {
  const auto &box = slot.bounds;
  if (slot.pixels) {
    Screen::drawRgb565(slot.pixels.get(), box.x, box.y, box.w, box.h);
  } else if (page == Page::Preview) {
    const char *message = !slot.finished ? "READING IMAGE..." :
                          slot.result == PhotoImage::Result::TooLarge ? "IMAGE TOO LARGE" : "COULD NOT OPEN IMAGE";
    Screen::fill(box.x, box.y, box.w, box.h, bg);
    Screen::text(box.x + (box.w - int(strlen(message)) * 8) / 2, box.y + box.h / 2,
                 message, Screen::theme.muted, bg);
  } else {
    drawPlaceholder(box, false, bg);
  }
}

void readFolder(const String &path, const String &highlight = {}) {
  folder = path;
  selection = 0;
  entries.clear();
  File dir = cardAvailable ? SD.open(folder, FILE_READ) : File();
  folderAvailable = dir && dir.isDirectory();
  if (!folderAvailable) return;
  while (entries.size() < kMaxEntries) {
    bool directory = false;
    String entry = dir.getNextFileName(&directory);
    if (entry.isEmpty()) break;
    const char *name = fileName(entry);
    if (*name != '.' && (directory || PhotoImage::supportedName(name))) {
      entries.push_back({std::move(entry), directory});
    }
  }
  std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
    return a.directory != b.directory ? a.directory : strcasecmp(fileName(a.path), fileName(b.path)) < 0;
  });
  const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry &entry) {
    return entry.path == highlight;
  });
  if (found != entries.end()) selection = int(found - entries.begin());
}

void drawBrowserFooter() {
  Screen::fill(0, 216, Screen::width, 24, Screen::theme.background);
  if (notice) {
    Screen::text(12, 218, notice, Screen::theme.muted, Screen::theme.background);
  } else if (!entries.empty()) {
    char position[36];
    snprintf(position, sizeof(position), "%s     %d / %d", kLayouts[int(layout)], selection + 1, count());
    Screen::text(12, 218, position, Screen::theme.muted, Screen::theme.background);
  }
  const char *controls = folder == kPhotoRoot ? "A: OPEN    B: HOME    MENU: OPTIONS" : "A: OPEN    B: UP      MENU: OPTIONS";
  Screen::text(12, 230, controls, Screen::theme.muted, Screen::theme.background);
}

void drawList(int previous) {
  const int first = selection / kRows * kRows;
  const bool redrawPage = previous < 0 || previous / kRows != selection / kRows;
  for (int row = 0; row < kRows; ++row) {
    const int index = first + row;
    if (!redrawPage && index != selection && index != previous) continue;
    const int y = 51 + row * 20;
    if (index >= count()) {
      Screen::fill(6, y, 308, 18, Screen::theme.background);
      continue;
    }
    const bool selected = index == selection;
    const auto bg = selected ? Screen::theme.selected : Screen::theme.panel;
    const auto &entry = entries[index];
    drawCard(6, y, 308, 18, selected, accent());
    Screen::text(18, y + 5, entry.directory ? "[DIR]" : "[IMG]", accent(), bg);
    const String name = shortened(fileName(entry.path), 30);
    Screen::text(66, y + 5, name.c_str(), selected ? Screen::theme.text : Screen::theme.muted, bg);
  }
}

void drawImageCard(int slot, int index, Rect card, Rect artBox, bool selected) {
  if (index < 0 || index >= count()) {
    Screen::fill(card.x, card.y, card.w, card.h, Screen::theme.background);
    return;
  }
  const auto bg = selected ? Screen::theme.selected : Screen::theme.panel;
  drawCard(card.x, card.y, card.w, card.h, selected, accent());
  if (entries[index].directory) drawPlaceholder(artBox, true, Screen::theme.header);
  else drawSlot(slots[slot], Screen::theme.header);
  const String label = shortened(fileName(entries[index].path), (card.w - 20) / 8);
  Screen::text(card.x + 10, card.y + card.h - 14, label.c_str(),
               selected ? Screen::theme.text : Screen::theme.muted, bg);
}

void drawGrid(int previous) {
  const int first = selection / kGridCount * kGridCount;
  const bool redrawPage = previous < 0 || previous / kGridCount != selection / kGridCount;
  if (redrawPage) clearArtwork();
  for (int slot = 0; slot < kGridCount; ++slot) {
    const int index = first + slot;
    if (!redrawPage && index != selection && index != previous) continue;
    const int x = 6 + (slot % kGridColumns) * 104;
    const int y = 51 + (slot / kGridColumns) * 82;
    const Rect artBox{x + 9, y + 6, 82, 50};
    if (redrawPage && index < count()) assignImage(slot, index, artBox);
    drawImageCard(slot, index, {x, y, 100, 77}, artBox, index == selection);
  }
}

void drawCarousel() {
  clearArtwork();
  constexpr Rect cards[] = {{6, 82, 68, 96}, {80, 53, 160, 151}, {246, 82, 68, 96}};
  for (int i = 0; i < 3; ++i) {
    const bool center = i == 1;
    const int index = center || (i == 0 ? count() > 2 : count() > 1) ? wrap(selection + i - 1, count()) : -1;
    const Rect &card = cards[i];
    const Rect artBox{card.x + 8, card.y + 8, card.w - 16, card.h - 33};
    if (index >= 0) assignImage(i, index, artBox);
    drawImageCard(i, index, card, artBox, center);
  }
}

void drawBrowser(bool full, int previous = -1) {
  if (full) {
    const String path = shortened(folder.c_str(), 37);
    drawHeader("PHOTOS", path.c_str(), accent());
    if (layout == Layout::List || entries.empty()) clearArtwork();
  }
  if (entries.empty()) {
    const char *message = !cardAvailable ? "MICROSD NOT MOUNTED" :
                          folderAvailable ? "NO SUPPORTED IMAGES" : "CREATE /photos ON THE SD CARD";
    Screen::text(18, 123, message, Screen::theme.text, Screen::theme.background);
  } else if (layout == Layout::List) drawList(previous);
  else if (layout == Layout::Grid) drawGrid(previous);
  else drawCarousel();
  drawBrowserFooter();
}

void drawPreview() {
  clearArtwork();
  const String path = shortened(folder.c_str(), 37);
  drawHeader("PHOTOS", path.c_str(), accent());
  constexpr Rect image{14, 58, 292, 131};
  drawCard(6, 50, 308, 164, false, accent());
  assignImage(0, selection, image);
  drawSlot(slots[0], Screen::theme.header);
  const String name = shortened(fileName(entries[selection].path), 36);
  Screen::text(16, 198, name.c_str(), Screen::theme.text, Screen::theme.panel);
  Screen::text(12, 228, "< / >: PHOTO  B: ALBUM  MENU: OPTIONS", Screen::theme.muted, Screen::theme.background);
}

void drawOptions(int previous = -1) {
  const bool full = previous < 0;
  if (full) {
    drawHeader("PHOTOS OPTIONS", "A: SELECT   LEFT/RIGHT: CHANGE", accent());
    const String name = shortened(selectedPhoto() ? fileName(entries[selection].path) : folder.c_str(), 36);
    Screen::text(12, 53, name.c_str(), Screen::theme.text, Screen::theme.background);
    Screen::text(12, 224, "B / MENU: BACK", Screen::theme.muted, Screen::theme.background);
  }
  for (int i = 0; i < (selectedPhoto() ? 2 : 1); ++i) {
    if (!full && i != previous && i != option) continue;
    const int y = 73 + i * 27;
    const bool selected = i == option;
    const auto bg = selected ? Screen::theme.selected : Screen::theme.panel;
    drawCard(6, y, 308, 23, selected, accent());
    Screen::text(18, y + 8, i ? "DELETE PHOTO" : "BROWSER LAYOUT",
                 selected ? Screen::theme.text : Screen::theme.muted, bg);
    if (i) Screen::text(292, y + 8, ">", accent(), bg);
    else drawMenuValue(230, y + 8, kLayouts[int(layout)], accent(), bg, true);
  }
}

void drawDeletePrompt(int previous = -1) {
  const bool full = previous < 0;
  if (full) {
    drawHeader("DELETE PHOTO?", "A: SELECT   B / MENU: CANCEL", accent());
    const String name = shortened(fileName(entries[selection].path), 72);
    Screen::text(12, 53, name.substring(0, 36).c_str(), Screen::theme.text, Screen::theme.background);
    Screen::text(12, 65, name.substring(36).c_str(), Screen::theme.text, Screen::theme.background);
    Screen::text(12, 86, "THIS REMOVES THE FILE FROM MICROSD.", Screen::theme.muted, Screen::theme.background);
  }
  for (int i = 0; i < 2; ++i) {
    const bool selected = i == int(deleteYes);
    if (!full && !selected && i != previous) continue;
    const int y = 110 + i * 27;
    drawCard(6, y, 308, 23, selected, accent());
    Screen::text(18, y + 8, i ? "YES, DELETE PHOTO" : "NO, KEEP PHOTO",
                 selected ? Screen::theme.text : Screen::theme.muted,
                 selected ? Screen::theme.selected : Screen::theme.panel);
  }
  Screen::fill(0, 220, Screen::width, 20, Screen::theme.background);
  if (notice) Screen::text(12, 224, notice, Screen::theme.muted, Screen::theme.background);
}

// Read the current directory entry again in case the card changed after the folder was indexed.
bool deleteSelectedPhoto() {
  if (!selectedPhoto()) return false;
  const String &path = entries[selection].path;
  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory()) return false;
  file.close();
  return SD.remove(path);
}

void moveSelection(uint16_t pressed) {
  const int previous = selection;
  if (layout == Layout::Grid && (pressed & (GB_PAD_UP | GB_PAD_DOWN))) {
    const int lastRow = (count() - 1) / kGridColumns;
    const int row = wrap(selection / kGridColumns + ((pressed & GB_PAD_UP) ? -1 : 1), lastRow + 1);
    selection = std::min(row * kGridColumns + selection % kGridColumns, count() - 1);
  } else {
    const bool backward = pressed & (GB_PAD_UP | GB_PAD_LEFT);
    selection = wrap(selection + (backward ? -1 : 1), count());
  }
  if (selection != previous) {
    notice = nullptr;
    drawBrowser(false, previous);
  }
}
}

void PhotoViewer::begin(Preferences &preferences) {
  settings = &preferences;
  const uint8_t saved = preferences.getUChar("photoLayout", uint8_t(Layout::List));
  if (saved < kLayoutCount) layout = static_cast<Layout>(saved);
}

void PhotoViewer::open(bool cardMounted) {
  cardAvailable = cardMounted;
  page = Page::Browser;
  notice = nullptr;
  clearArtwork();
  readFolder(kPhotoRoot);
  draw();
}

void PhotoViewer::draw() {
  switch (page) {
    case Page::Browser: drawBrowser(true); break;
    case Page::Preview: drawPreview(); break;
    case Page::Options: drawOptions(); break;
    case Page::ConfirmDelete: drawDeletePrompt(); break;
  }
}

bool PhotoViewer::handle(uint16_t pressed, bool menuPressed, bool rescan) {
  if (page == Page::ConfirmDelete) {
    if (menuPressed || (pressed & GB_PAD_B) || ((pressed & GB_PAD_A) && !deleteYes)) {
      page = Page::Options;
      notice = nullptr;
      drawOptions();
    } else if (pressed & kDirections) {
      const int previous = int(deleteYes);
      deleteYes = !deleteYes;
      notice = nullptr;
      drawDeletePrompt(previous);
    } else if (pressed & GB_PAD_A) {
      stop();
      if (!deleteSelectedPhoto()) {
        notice = "COULD NOT DELETE PHOTO";
        deleteYes = false;
        drawDeletePrompt(1);
      } else {
        entries.erase(entries.begin() + selection);
        selection = std::max(0, std::min(selection, count() - 1));
        page = Page::Browser;
        notice = "PHOTO DELETED";
        draw();
      }
    }
  } else if (page == Page::Options) {
    if (menuPressed || (pressed & GB_PAD_B)) {
      page = optionsReturn;
      draw();
    } else if ((pressed & (GB_PAD_UP | GB_PAD_DOWN)) && selectedPhoto()) {
      const int previous = option;
      option = 1 - option;
      drawOptions(previous);
    } else if (option == 0 && (pressed & (GB_PAD_LEFT | GB_PAD_RIGHT | GB_PAD_A))) {
      const int step = pressed & GB_PAD_LEFT ? -1 : 1;
      layout = static_cast<Layout>(wrap(int(layout) + step, kLayoutCount));
      if (settings) settings->putUChar("photoLayout", uint8_t(layout));
      drawOptions(0);
    } else if (option == 1 && (pressed & (GB_PAD_A | GB_PAD_RIGHT))) {
      page = Page::ConfirmDelete;
      deleteYes = false;
      notice = nullptr;
      drawDeletePrompt();
    }
  } else if (menuPressed) {
    optionsReturn = page;
    page = Page::Options;
    option = 0;
    notice = nullptr;
    clearArtwork();
    drawOptions();
  } else if (pressed & GB_PAD_B) {
    notice = nullptr;
    if (page == Page::Preview) {
      page = Page::Browser;
    } else if (folder != kPhotoRoot) {
      const String child = folder;
      clearArtwork();
      readFolder(child.substring(0, child.lastIndexOf('/')), child);
    } else {
      clearArtwork();
      std::vector<Entry>().swap(entries);
      return false;
    }
    draw();
  } else if (page == Page::Preview) {
    if (pressed & (GB_PAD_LEFT | GB_PAD_RIGHT)) {
      const auto firstPhoto = std::find_if(entries.begin(), entries.end(), [](const Entry &e) { return !e.directory; });
      const int first = int(firstPhoto - entries.begin());
      const int step = pressed & GB_PAD_LEFT ? -1 : 1;
      const int next = first + wrap(selection - first + step, count() - first);
      if (next != selection) {
        selection = next;
        drawPreview();
      }
    }
  } else if (rescan || (pressed & GB_PAD_SELECT)) {
    const String previous = entries.empty() ? String() : entries[selection].path;
    clearArtwork();
    readFolder(String(folder), previous);
    notice = nullptr;
    draw();
  } else if (!entries.empty()) {
    if (pressed & (GB_PAD_A | GB_PAD_START)) {
      notice = nullptr;
      if (selectedPhoto()) page = Page::Preview;
      else {
        clearArtwork();
        readFolder(String(entries[selection].path));
      }
      draw();
    } else if (pressed & kDirections) moveSelection(pressed);
  }
  return true;
}

bool PhotoViewer::canRepeatScroll() {
  return page == Page::Browser && entries.size() > 1;
}

void PhotoViewer::stop() {
  cancelRequest();
  if (!request) return;
  while (!request->done.load(std::memory_order_acquire)) vTaskDelay(1);
  request.reset();
}

void PhotoViewer::service(bool controlsActive, bool musicPlaying) {
  const bool browsing = page == Page::Browser;
  if (browsing && musicPlaying) cancelRequest();
  if (request && request->done.load(std::memory_order_acquire)) {
    if (!request->cancelled.load(std::memory_order_relaxed)) {
      auto &slot = slots[request->slot];
      slot.result = request->result;
      slot.finished = true;
      if (slot.result == PhotoImage::Result::Drawn) slot.pixels = std::move(request->pixels);
      drawSlot(slot, Screen::theme.header);
    }
    request.reset();
  }
  if (request || (page != Page::Preview && (!browsing || musicPlaying || controlsActive ||
      millis() - artworkQueuedAt < kThumbIdleMs))) return;

  // Load the highlighted image first, then the other visible photos if the card is idle.
  int next = -1;
  for (int i = 0; i < int(slots.size()); ++i) {
    if (slots[i].entry < 0 || slots[i].finished) continue;
    if (next < 0 || slots[i].entry == selection) next = i;
  }
  if (next < 0) return;
  auto &slot = slots[next];
  request.reset(new (std::nothrow) Request{entries[slot.entry].path, next, slot.bounds.w, slot.bounds.h});
  const BaseType_t core = portNUM_PROCESSORS > 1 ? 1 - xPortGetCoreID() : 0;
  if (!request || xTaskCreatePinnedToCore(loadImage, "photo-image", kImageTaskStackBytes,
                                        request.get(), tskIDLE_PRIORITY + 1, nullptr, core) != pdPASS) {
    request.reset();
    slot.finished = true;
    drawSlot(slot, Screen::theme.header);
  }
}
