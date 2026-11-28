// Pocket Play sketch entry point and application coordinator.
// The Arduino loop owns UI state, SD navigation, and emulator progression.
// The video and Music tasks borrow their peripherals until the loop joins them.
// See the README for build instructions and the firmware source map.

#include <Arduino.h>
#include <ESP_I2S.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <algorithm>
#include <array>
#include <string_view>
#include <cerrno>
#include <atomic>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <esp32-hal-rgb-led.h>
#include <esp32-hal-rmt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <iterator>
#include <memory>
#include <sys/stat.h>
#include <vector>

#include "AppPackage.h"
#include "AppRuntime.h"
#include "BatteryGauge.h"
#include "BluetoothControls.h"
#include "DeviceLog.h"
#include "Display.h"
#include "DoomLauncher.h"
#include "FirmwareUpdate.h"
#include "MenuStyle.h"
#include "GameCover.h"
#include "MusicPlayer.h"
#include "PhotoImage.h"
#include "PhotoViewer.h"
#include "Pins.h"
#include "SnakeGame.h"
#include "StandbyDisplay.h"
#include "UsbControls.h"
#include "WifiTransfer.h"
#include "src/cartridge/CartridgeCore.h"
#include "src/gpsp/GbaCore.h"
#include "src/gpsp/gpsp_config.h"

extern "C" {
#include "src/gnuboy/gnuboy.h"
}

constexpr int kAudioRate = 32768;
constexpr char kConsoleName[] = "Pocket Play";
constexpr char kConsoleVersion[] = "1.0.1";
#ifndef POCKETPLAY_BUILD_ID
#define POCKETPLAY_BUILD_ID __DATE__ " " __TIME__
#endif
constexpr char kConsoleDeveloper[] = "Moutaz Baaj";
constexpr char kDeveloperGitHub[] = "github.com/Moutazbaaj";
constexpr uint8_t kDefaultVolumePercent = 30;
constexpr int kVolumeStep = 5;
constexpr uint8_t kBrightnessStep = 20;
// The ESP_I2S driver in the documented build uses 240 stereo frames per DMA buffer.
constexpr size_t kGbaAudioBlockSamples = 240 * 2;
constexpr size_t kGbaAudioStartSamples = 3 * kGbaAudioBlockSamples;
constexpr int64_t kFrameUs = 16743;
constexpr int64_t kAutoFrameDebtUs = 1500;
constexpr int64_t kGameStatsWindowUs = 3000000;
constexpr uint32_t kAutoSaveMs = 10000;
constexpr uint32_t kBootSplashMinMs = 800;
// The splash bar represents startup phases. Counts are local to the directory being indexed.
enum class BootStage { Starting, Mounting, Library, Root, Restore, Ready };
constexpr size_t kGbFrameBytes = GB_WIDTH * GB_HEIGHT * sizeof(uint16_t);
constexpr size_t kGbaFrameBytes = GBA_FRAME_WIDTH * GBA_FRAME_HEIGHT * sizeof(uint16_t);
constexpr uint32_t kVideoPresenterStackBytes = 4096;
constexpr uint32_t kBackgroundMusicStackBytes = 6144;
constexpr size_t kMaxRomBytes = 4 * 1024 * 1024;
constexpr size_t kMaxGamesPerSystem = 512;
constexpr int kRowsPerPage = 8;
constexpr uint16_t kMenuKey = 0x100;
static_assert(uint16_t(AppSdk::Up) == GB_PAD_UP && uint16_t(AppSdk::Down) == GB_PAD_DOWN &&
              uint16_t(AppSdk::Left) == GB_PAD_LEFT && uint16_t(AppSdk::Right) == GB_PAD_RIGHT &&
              uint16_t(AppSdk::A) == GB_PAD_A && uint16_t(AppSdk::B) == GB_PAD_B &&
              uint16_t(AppSdk::Start) == GB_PAD_START && uint16_t(AppSdk::Select) == GB_PAD_SELECT &&
              uint16_t(AppSdk::L) == GBA_PAD_L && uint16_t(AppSdk::R) == GBA_PAD_R);
constexpr uint16_t kListScrollButtons = GB_PAD_UP | GB_PAD_DOWN;
constexpr uint16_t kConsoleWakeButtons = GB_PAD_START | GB_PAD_SELECT;
constexpr uint32_t kSleepChordHoldMs = 600;
constexpr uint32_t kSleepChordGraceMs = 60;

// Separate the physical sleep shortcut from individual Start and Select actions.
class SleepChord {
 public:
  // Keep the entry or wake buttons from reaching the current screen before release.
  void waitForRelease() {
    phase_ = Phase::Release;
    tap_ = 0;
  }

  // Filter button events and report the sleep request once for this hold.
  bool update(uint16_t raw, uint32_t now, uint16_t &held, uint16_t &pressed) {
    const uint16_t rawButtons = raw & kConsoleWakeButtons;
    const uint16_t buttons = held & kConsoleWakeButtons;
    held &= ~kConsoleWakeButtons;
    pressed &= ~kConsoleWakeButtons;
    if (phase_ == Phase::Release) {
      if (!rawButtons && !buttons) phase_ = Phase::Idle;
      return false;
    }
    if (phase_ == Phase::Together) {
      if (buttons != kConsoleWakeButtons) {
        waitForRelease();
      } else if (rawButtons == kConsoleWakeButtons && now - startedAt_ >= kSleepChordHoldMs) {
        waitForRelease();
        return true;
      }
      return false;
    }
    if (buttons == kConsoleWakeButtons) {
      phase_ = Phase::Together;
      startedAt_ = now;
      tap_ = 0;
      return false;
    }
    // A second contact may already be down but still waiting for the debounce.
    if (rawButtons == kConsoleWakeButtons) return false;

    if (phase_ == Phase::Idle || buttons != single_) {
      if (phase_ == Phase::PendingSingle) {
        pressed |= single_;
        tap_ = single_;
        tappedAt_ = now;
      }
      single_ = buttons;
      startedAt_ = now;
      phase_ = buttons ? Phase::PendingSingle : Phase::Idle;
    }
    if (phase_ == Phase::PendingSingle && rawButtons == buttons &&
        now - startedAt_ >= kSleepChordGraceMs) {
      phase_ = Phase::SingleSent;
      pressed |= buttons;
    }
    if (phase_ == Phase::SingleSent) held |= buttons;
    // A solo tap released during the grace period still reaches a game frame.
    constexpr uint32_t tapPlaybackMs = 35;
    if (tap_ && now - tappedAt_ >= tapPlaybackMs) tap_ = 0;
    if (!buttons && !rawButtons) held |= tap_;
    return false;
  }

 private:
  enum class Phase : uint8_t { Idle, PendingSingle, SingleSent, Together, Release };
  Phase phase_ = Phase::Idle;
  uint16_t single_ = 0;
  uint16_t tap_ = 0;
  uint32_t startedAt_ = 0;
  uint32_t tappedAt_ = 0;
};
constexpr uint32_t kWakeDebounceMs = 20;
constexpr char kSleepWakeHint[] = "Wake: hold Start+Select";
constexpr uint32_t kTftClockOptions[] = {20000000, 40000000, 60000000, 80000000};
constexpr uint8_t kTftClockOptionCount = std::size(kTftClockOptions);

struct Button {
  int pin;
  uint16_t mask;
};

constexpr Button kButtons[] = {
  {kRight, GB_PAD_RIGHT}, {kLeft, GB_PAD_LEFT},
  {kUp, GB_PAD_UP}, {kDown, GB_PAD_DOWN},
  {kA, GB_PAD_A}, {kB, GB_PAD_B},
  {kSelect, GB_PAD_SELECT}, {kStart, GB_PAD_START}, {kMenu, kMenuKey},
  {kL, GBA_PAD_L}, {kR, GBA_PAD_R},
};

// Keep these values in library order. They index the per-console ROM lists and cursors.
enum class System { GameBoy, GameBoyColor, GameBoyAdvance, Nes, MasterSystem, GameGear, Genesis };
struct SystemInfo {
  const char *title;
  const char *suffix;
  const char *directory;
  const char *cardTitle;
  const char *cardSubtitle;
  uint16_t accent;
  const CartridgeCore *core;
};
constexpr SystemInfo kSystems[] = {
  {"GAME BOY", ".gb", "/roms/gb", "GAME BOY", "", 0x07E0, nullptr},
  {"GAME BOY COLOR", ".gbc", "/roms/gbc", "GAME BOY", "COLOR", 0xA81F, nullptr},
  {"GAME BOY ADVANCE", ".gba", "/roms/gba", "GAME BOY", "ADVANCE", 0xFD20, nullptr},
  {"NES", ".nes", "/roms/nes", "NINTENDO", "NES", 0xFA49, &pocketNes},
  {"MASTER SYSTEM", ".sms", "/roms/sms", "SEGA", "MASTER SYS.", 0x5DFF, &pocketMasterSystem},
  {"SEGA GAME GEAR", ".gg", "/roms/gg", "SEGA", "GAME GEAR", 0xA6EC, &pocketGameGear},
  {"SEGA GENESIS", ".md", "/roms/genesis", "SEGA", "GENESIS", 0x5DFF, &pocketGenesis},
};
constexpr size_t kSystemCount = std::size(kSystems);
constexpr int kMusicMenuItem = kSystemCount;
constexpr int kFlashlightMenuItem = kMusicMenuItem + 1;
constexpr int kSettingsMenuItem = kFlashlightMenuItem + 1;
constexpr int kContinueMenuItem = kSettingsMenuItem + 1;
constexpr int kFavoritesMenuItem = kContinueMenuItem + 1;
constexpr int kRecentMenuItem = kFavoritesMenuItem + 1;
constexpr int kMiniGamesMenuItem = kRecentMenuItem + 1;
constexpr int kStandbyMenuItem = kMiniGamesMenuItem + 1;
constexpr int kPhotosMenuItem = kStandbyMenuItem + 1;
constexpr int kDoomMenuItem = kPhotosMenuItem + 1;
constexpr int kApplicationsMenuItem = kDoomMenuItem + 1;
constexpr int kLastHomeItem = kApplicationsMenuItem;
constexpr int kHomeColumns = 3;
constexpr int kHomeOrder[] = {
  kContinueMenuItem, kFavoritesMenuItem, kRecentMenuItem,
  int(System::GameBoy), int(System::GameBoyColor), int(System::GameBoyAdvance),
  int(System::Nes), int(System::MasterSystem), int(System::GameGear),
  int(System::Genesis), kDoomMenuItem, kMiniGamesMenuItem,
  kMusicMenuItem, kFlashlightMenuItem, kPhotosMenuItem,
  kStandbyMenuItem, kApplicationsMenuItem, kSettingsMenuItem,
};
constexpr int kHomePositions = std::size(kHomeOrder);
constexpr int kHomeRows = (kHomePositions + kHomeColumns - 1) / kHomeColumns;
constexpr int kHomeVisibleRows = std::min(3, kHomeRows);
// The selected app ID is shared by the layouts. Each scrolling layout keeps its own viewport.
enum class HomeStyle : uint8_t { Grid, Carousel, List };
constexpr const char *kHomeStyleNames[] = {"GRID", "CAROUSEL", "LIST"};
static_assert(std::size(kHomeStyleNames) == unsigned(HomeStyle::List) + 1);
HomeStyle homeStyle = HomeStyle::Grid;
int homeFirstRow = 0;
int homeFirstListRow = 0;
// Active foreground view. Settings, About, and diagnostics can retain a paused emulator.
enum class View {
  SystemMenu, GameList, Settings, AboutMenu, About, Diagnostics, Update, Music, NowPlaying, MusicSettings, MusicScreenOff,
  Flashlight, Favorites, RecentlyPlayed, GameOptions, GameProfile, DeviceTools, Bluetooth, MiniGames, Standby, Photos,
  Playing, Paused, WifiTransfer, DoomMenu, DoomPlaying, Applications, Failed
};
constexpr char kDoomBootKey[] = "doomNextBoot";
unsigned doomSelection = 0;
enum class SettingsPage {
  Root, Display, Sound, Emulation, Power, Connectivity, Wifi,
  GameBoy, GameBoyColor, GameBoyAdvance, Nes, MasterSystem, GameGear, Genesis,
};
enum class SettingsItem {
  DisplayMenu, SoundMenu, EmulationMenu, PowerMenu, ConnectivityMenu, WifiMenu,
  GameBoyMenu, GameBoyColorMenu, GameBoyAdvanceMenu, NesMenu, MasterSystemMenu, GameGearMenu, GenesisMenu,
  Sound, Volume, Brightness, Theme, HomeStyle, EmulatorDisplay, GbaDisplay, GbaFrameskip, GbaTransfer, TftClock,
  GenesisFrameskip,
  Sleep, AutoSleep, DeviceTools, Bluetooth, WifiName, WifiPassword, EnableOnRestart, RestartConsole, About, Back
};
enum class WifiField { None, Name, Password };
WifiField wifiField = WifiField::None;

// Emulator page and row IDs follow the same order as the ROM libraries.
constexpr int kFirstEmulatorPage = int(SettingsPage::GameBoy);
constexpr int kFirstEmulatorItem = int(SettingsItem::GameBoyMenu);
static_assert(int(SettingsPage::Genesis) - kFirstEmulatorPage + 1 == kSystemCount);
static_assert(int(SettingsItem::GenesisMenu) - kFirstEmulatorItem + 1 == kSystemCount);

// Keep navigation IDs independent of the rows visible on each settings page.
constexpr SettingsItem kRootSettings[] = {
  SettingsItem::DisplayMenu, SettingsItem::SoundMenu, SettingsItem::EmulationMenu,
  SettingsItem::PowerMenu, SettingsItem::ConnectivityMenu, SettingsItem::DeviceTools, SettingsItem::About, SettingsItem::Back,
};
constexpr SettingsItem kDisplaySettings[] = {
  SettingsItem::Brightness, SettingsItem::Theme, SettingsItem::HomeStyle, SettingsItem::TftClock, SettingsItem::Back,
};
constexpr SettingsItem kSoundSettings[] = {
  SettingsItem::Sound, SettingsItem::Volume, SettingsItem::Back,
};
constexpr SettingsItem kEmulationSettings[] = {
  SettingsItem::GameBoyMenu, SettingsItem::GameBoyColorMenu, SettingsItem::GameBoyAdvanceMenu,
  SettingsItem::NesMenu, SettingsItem::MasterSystemMenu, SettingsItem::GameGearMenu,
  SettingsItem::GenesisMenu, SettingsItem::Back,
};
constexpr SettingsItem kClassicEmulatorSettings[] = {SettingsItem::EmulatorDisplay, SettingsItem::Back};
constexpr SettingsItem kGbaSettings[] = {
  SettingsItem::GbaDisplay, SettingsItem::GbaFrameskip, SettingsItem::GbaTransfer, SettingsItem::Back,
};
constexpr SettingsItem kGenesisSettings[] = {
  SettingsItem::EmulatorDisplay, SettingsItem::GenesisFrameskip, SettingsItem::Back,
};
constexpr SettingsItem kPowerSettings[] = {
  SettingsItem::Sleep, SettingsItem::AutoSleep, SettingsItem::Back,
};
constexpr SettingsItem kConnectivitySettings[] = {
  SettingsItem::Bluetooth, SettingsItem::WifiMenu, SettingsItem::Back,
};
constexpr SettingsItem kWifiSettings[] = {
  SettingsItem::WifiName, SettingsItem::WifiPassword, SettingsItem::EnableOnRestart,
  SettingsItem::RestartConsole, SettingsItem::Back,
};
struct SettingsMenu {
  const char *title;
  const SettingsItem *items;
  int count;
};
constexpr SettingsMenu kSettingsMenus[] = {
  {"SETTINGS", kRootSettings, std::size(kRootSettings)},
  {"DISPLAY", kDisplaySettings, std::size(kDisplaySettings)},
  {"SOUND", kSoundSettings, std::size(kSoundSettings)},
  {"EMULATION", kEmulationSettings, std::size(kEmulationSettings)},
  {"POWER", kPowerSettings, std::size(kPowerSettings)},
  {"CONNECTIVITY", kConnectivitySettings, std::size(kConnectivitySettings)},
  {"WI-FI", kWifiSettings, std::size(kWifiSettings)},
  {kSystems[int(System::GameBoy)].title, kClassicEmulatorSettings, std::size(kClassicEmulatorSettings)},
  {kSystems[int(System::GameBoyColor)].title, kClassicEmulatorSettings, std::size(kClassicEmulatorSettings)},
  {kSystems[int(System::GameBoyAdvance)].title, kGbaSettings, std::size(kGbaSettings)},
  {kSystems[int(System::Nes)].title, kClassicEmulatorSettings, std::size(kClassicEmulatorSettings)},
  {kSystems[int(System::MasterSystem)].title, kClassicEmulatorSettings, std::size(kClassicEmulatorSettings)},
  {kSystems[int(System::GameGear)].title, kClassicEmulatorSettings, std::size(kClassicEmulatorSettings)},
  {kSystems[int(System::Genesis)].title, kGenesisSettings, std::size(kGenesisSettings)},
};
static_assert(std::size(kSettingsMenus) == size_t(SettingsPage::Genesis) + 1);
static_assert([] {
  for (size_t i = 0; i < kSystemCount; ++i) {
    if (int(kEmulationSettings[i]) != kFirstEmulatorItem + int(i) ||
        kSettingsMenus[kFirstEmulatorPage + i].title != kSystems[i].title) return false;
  }
  return true;
}());
enum class ToolScreen { Menu, Buttons, Display, Speaker, Led, Battery };
enum class MiniScreen { Menu, Puzzle, Timing, Snake };
constexpr int kMiniGameCount = int(MiniScreen::Snake);
MiniScreen miniScreen = MiniScreen::Menu;
int miniSelection = 0;
std::array<uint8_t, 9> puzzleTiles{};
int puzzleGap = 8;
unsigned puzzleMoves = 0;
bool puzzleComplete = false;
uint32_t timingStartedAt = 0;
uint32_t timingDrawnAt = 0;
int timingTarget = 0;
int timingResult = -1;
int timingStoppedAt = 0;
unsigned timingHits = 0;
unsigned timingAttempts = 0;
enum class AboutSection { Build, Device, Network, Library, Emulators, Games, Extras, Power, Overview, Developer, Credits };
enum class PauseItem {
  Resume, SaveState, LoadState, GbPalette, DisplaySize, Volume, Brightness, Sleep, Settings, Quit
};
struct AutoSleepOption {
  uint32_t idleMs;
  const char *label;
};
constexpr AutoSleepOption kAutoSleepOptions[] = {
  {0, "OFF"}, {30000, "30 SEC"}, {60000, "1 MIN"}, {120000, "2 MIN"}, {300000, "5 MIN"},
};

enum class FlashlightItem { Power, Color, Brightness, ColorFlash, ColorBreathe, Speed };
enum class FlashlightEffect : uint8_t { None, Flash, Breathe };
constexpr int kFlashlightItemCount = int(FlashlightItem::Speed) + 1;
struct FlashlightSpeed {
  const char *label;
  uint32_t flashCycleMs;
  uint32_t breatheCycleMs;
};
constexpr FlashlightSpeed kFlashlightSpeeds[] = {
  {"SLOW", 1800, 4320},
  {"NORMAL", 1000, 2400},
  {"FAST", 600, 1440},
};
constexpr int kFlashlightSpeedCount = std::size(kFlashlightSpeeds);
constexpr uint8_t kDefaultFlashlightSpeed = 1;
constexpr int kFlashlightBrightnessStep = 5;
constexpr uint8_t kDefaultFlashlightBrightness = 50;
constexpr uint32_t kFlashlightRefreshMs = 60;
constexpr bool kFlashlightSharesTftClock = kFlashlightRgbPin == kTftSck;
struct FlashlightRgb {
  uint8_t red, green, blue;
};
struct FlashlightColor {
  const char *name;
  FlashlightRgb rgb;
};
constexpr FlashlightColor kFlashlightColors[] = {
  {"WHITE",      {255, 255, 255}},
  {"WARM WHITE", {255, 176,  92}},
  {"RED",        {255,   0,   0}},
  {"ORANGE",     {255,  70,   0}},
  {"YELLOW",     {255, 255,   0}},
  {"GREEN",      {  0, 255,   0}},
  {"CYAN",       {  0, 255, 255}},
  {"BLUE",       {  0,   0, 255}},
  {"PURPLE",     {128,   0, 255}},
  {"PINK",       {255,  36, 128}},
  {"RAINBOW",     {}},
};
constexpr FlashlightRgb kFlashlightSpectrum[] = {
  {255, 0, 0}, {255, 255, 0}, {0, 255, 0}, {0, 255, 255}, {0, 0, 255}, {255, 0, 255},
};
constexpr int kFlashlightColorCount = std::size(kFlashlightColors);
constexpr int kFlashlightRainbowIndex = kFlashlightColorCount - 1;

// Session state belongs to the main loop except for the explicitly shared task buffers and counters.
View view = View::SystemMenu;
View settingsReturnView = View::SystemMenu;
SPIClass sdSpi(HSPI);
I2SClass audio;
Preferences settingsStore;
bool audioReady = false;
bool soundEnabled = kEnableAudio;
std::atomic<uint8_t> volumePercent{kDefaultVolumePercent};
uint8_t displayBrightness = 100;
SleepChord sleepChord;
uint8_t autoSleepOption = 0;
uint32_t lastConsoleActivityAt = 0;
bool sdReady = false;
// Keep the GB and Genesis keys so existing values survive a firmware update.
constexpr const char *kDisplayScaleKeys[] = {
  "gbscale", "gbcscale", nullptr, "nesscale", "smsscale", "ggscale", "mdscale",
};
static_assert(std::size(kDisplayScaleKeys) == kSystemCount);
std::array<Screen::GbScale, kSystemCount> emulatorScales{};
// Native GBA output is faster. Full-screen scaling can be enabled from Settings.
bool gbaFullscreen = false;
bool gbaDmaEnabled = true;
uint8_t gbaFrameskipMode = 0;
uint8_t genesisFrameskipMode = 0;
// Editable and active copies are separate. A disabled profile inherits the global settings.
struct GameProfile {
  bool enabled;
  Screen::GbScale gbDisplay;
  bool gbaFull;
  uint8_t frameskip;
  bool sound;
};
GameProfile currentGameProfile{};
GameProfile editedGameProfile{};
enum class GameFileKind { Missing, File, Unavailable };
struct GameFileMove {
  String source;
  String target;
  bool copy;
};

enum class GameOptionsPage { Actions, Rename, Delete };
View gameOptionsReturnView = View::GameList;
GameOptionsPage gameOptionsPage = GameOptionsPage::Actions;
String gameOptionsPath;
int gameOption = 0;
bool confirmGameDelete = false;
String editedText;
size_t textCaret = 0;
int keyboardKey = 0;
bool keyboardUppercase = false;
int profileSelection = 0;
bool profileIsGba = false;
uint8_t tftClockOption = 0;
bool batteryBacked = false;
bool rtcCart = false;
bool gameIsGba = false;
const CartridgeCore *activeCartridgeCore = nullptr;
bool saveRetry = false;
int systemSelection = 0;
System selectedSystem = System::GameBoy;
std::array<int, kSystemCount> gameSelections = {};
bool menuStatusVisible = false;
std::vector<String> favoriteGames;
std::vector<String> recentGames;
int favoritesSelection = 0;
int recentSelection = 0;
View gameReturnView = View::GameList;
constexpr size_t kMaxFavorites = 128;
constexpr size_t kMaxRecentGames = 20;
constexpr char kFavoritesFile[] = "/sd/.pocketplay-favorites";
constexpr char kRecentGamesFile[] = "/sd/.pocketplay-recent";
int pauseChoice = 0;
enum class StatePrompt { None, Save, Load };
StatePrompt statePrompt = StatePrompt::None;
bool statePromptYes = false;
bool statePromptOverwrite = false;
SettingsPage settingsPage = SettingsPage::Root;
int settingsSelection = 0;
int settingsRootSelection = 0;
int aboutSelection = 0;
int aboutMenuSelection = 0;
std::vector<String> updateImages;
int updateSelection = 0;
int updateFirst = 0;
bool updateConfirm = false;
constexpr int kAboutMenuRows = 4;
constexpr int kVisibleUpdateRows = 5;
ToolScreen toolScreen = ToolScreen::Menu;
int toolSelection = 0;
int testColor = 0;
uint16_t testedButtons = 0;
bool testExitHeld = false;
uint32_t testExitPressedAt = 0;
uint32_t toolReadingDrawnAt = 0;
unsigned gbaSkipped = 0;
unsigned genesisSkipped = 0;
uint32_t lastSaveAttempt = 0;
int64_t nextFrame = 0;
uint16_t *framebuffer = nullptr;
uint16_t *videoPresenterBuffer = nullptr;
TaskHandle_t videoPresenterTaskHandle = nullptr;
SemaphoreHandle_t videoPresenterBufferAvailable = nullptr;
bool asyncVideo = false;
bool presenterFrameIsGba = false;
CartridgeFrame presenterCartridgeFrame{};
constexpr size_t kIndexedMaxPixelBytes = 272 * 240;
constexpr size_t kIndexedPaletteBytes = 256 * sizeof(uint16_t);
constexpr size_t kNativeCartridgeBytes = Screen::width * Screen::height * sizeof(uint16_t);
bool presenterGbaFullscreen = false;
Screen::GbScale presenterGbScale = Screen::GbScale::Native;
std::atomic<uint32_t> gameDisplayedFrames{0};  // Written by the active presenter.
// Timing sums cover emulated frames in one serial-report window. Durations use microseconds.
struct GamePerfWindow {
  int64_t startUs;
  int64_t coreUs;
  int64_t drawUs;
  uint32_t emulated;
  uint32_t drawn;
  uint32_t displayBusy;
  uint32_t displayedAtStart;
} gamePerf{};
alignas(4) int16_t samples[2048];
// GBA queues PCM in the same buffer that Gnuboy uses for its callback.
size_t gbaAudioBegin = 0;
size_t gbaAudioEnd = 0;
bool gbaAudioPrimed = false;
uint8_t *ownedRom = nullptr;
std::array<std::vector<String>, kSystemCount> gamesBySystem;
MusicPlayer musicPlayer;
constexpr char kMusicRoot[] = "/music";
// Keep the browsed folder and the playback queue separate. Both reference completed indexes.
struct MusicFolder {
  String path;
  std::vector<String> folders;
  std::vector<String> tracks;
};
std::shared_ptr<const MusicFolder> musicFolder;
std::shared_ptr<const MusicFolder> musicPlaylist;
// The directory handle and mutable index exist only while the foreground browser is loading.
File musicScanDirectory;
std::shared_ptr<MusicFolder> musicScanFolder;
String musicScanHighlight;
int musicSelection = 0;
int musicTrack = -1;
String musicTrackTitle;
View musicTitleView = View::Failed;
uint32_t musicTitleStartedAt = 0;
int musicTitleLastOffset = -1;
int nowPlayingTitleWidth = 0;
bool musicPaused = false;
bool repeatMusicTrack = false;
bool backgroundMusic = false;
bool musicOwnsAudio = false;
bool audioWasReadyBeforeMusic = false;
TaskHandle_t backgroundMusicTask = nullptr;
SemaphoreHandle_t backgroundMusicStopped = nullptr;
std::atomic<bool> backgroundMusicStopRequested{false};
std::atomic<MusicPlayer::Result> backgroundMusicResult{MusicPlayer::Result::Playing};
bool backgroundMusicStartFailed = false;
const char *musicMessage = nullptr;
const char *musicFolderMessage = nullptr;
enum class MusicControl { Previous, Backward, PlayPause, Forward, Next, VolumeDown, VolumeUp, Repeat };
enum class MusicSetting {
  Background, ScreenOff, ScreenTimeout, Volume, Repeat, Order, Visualizer, ReactiveRgb,
  SleepTimer, Stop, Rescan, Navigate, Exit
};
enum class MusicOrder : uint8_t { InOrder, Shuffle, StopAfterTrack };
constexpr int kMusicSettingCount = int(MusicSetting::Exit) + 1;
constexpr uint32_t kMusicScreenTimeouts[] = {0, 30000, 60000, 120000};
constexpr uint32_t kMusicSleepDelays[] = {0, 15 * 60000, 30 * 60000, 60 * 60000};
// Packed PCM peaks published by either playback path. The left level occupies the low byte.
std::atomic<uint16_t> musicAudioLevels{0};
bool musicVisualizerEnabled = true;
bool musicReactiveRgb = false;
bool musicRgbWasActive = false;
uint8_t musicSleepOption = 0;
uint32_t musicSleepStartedAt = 0;
uint32_t musicRgbUpdatedAt = 0;
uint32_t musicVisualizerUpdatedAt = 0;
uint16_t displayedMusicLevels = 0;
MusicOrder musicOrder = MusicOrder::InOrder;
uint8_t musicScreenTimeout = 0;
int musicSettingsSelection = 0;
View musicSettingsReturnView = View::Music;
View musicScreenReturnView = View::Music;
uint32_t musicLastInputAt = 0;
bool musicWakeRequested = false;
int musicTransportSelection = int(MusicControl::PlayPause);
int musicOptionsSelection = int(MusicControl::VolumeDown);
bool musicOptionsFocused = false;
bool musicWasSeeking = false;
uint64_t musicDisplayedSecond = 0;
uint64_t musicDisplayedDuration = 0;
FlashlightRgb lastLedRgb{};
bool flashlightOn = false;
bool flashlightSettingsChanged = false;
int flashlightSelection = 0;
uint8_t flashlightColorIndex = 0;
uint8_t flashlightBrightness = kDefaultFlashlightBrightness;
uint8_t flashlightSpeed = kDefaultFlashlightSpeed;
FlashlightEffect flashlightEffect = FlashlightEffect::None;
uint32_t flashlightEffectStartedAt = 0;
uint32_t flashlightLastUpdateAt = 0;
String gameName;
String lastPlayedPath;
String saveFile;
String stateFile;
constexpr int kSaveSlotCount = 3;
int saveSlot = 0;

// Erase the shared status strip and mark it available for the current page’s controls.
void clearMenuFooter() {
  Screen::fill(0, 219, Screen::width, 21, Screen::theme.background);
  menuStatusVisible = false;
}

// Log a message to serial and show it in the active menu footer.
void showStatus(const char *message) {
  Serial.println(message);
  clearMenuFooter();
  Screen::text(8, 225, message, Screen::theme.muted, Screen::theme.background);
  menuStatusVisible = true;
}

// Enter the error view and show the failure on both TFT and serial.
void fail(const char *message) {
  view = View::Failed;
  Screen::fill(0, 0, Screen::width, Screen::height, Screen::theme.background);
  Screen::text(12, 20, "STARTUP ERROR", Screen::theme.text, Screen::theme.background, 2);
  drawMenuIndicators(Screen::theme.background);
  showStatus(message);
}

// Combine active-low digital buttons and the optional analog D-pad into the shared input mask.
uint16_t readControls() {
  uint16_t keys = 0;
  for (const auto &button : kButtons) {
    if (button.pin >= 0 && (!kAnalogDpad || button.mask >= GB_PAD_A) &&
        digitalRead(button.pin) == LOW) {
      keys |= button.mask;
    }
  }
  if (kAnalogDpad) {
    const int vertical = analogRead(kUp);
    const int horizontal = analogRead(kDown);
    if (vertical >= 1024) {
      keys |= vertical >= 3072 ? GB_PAD_UP : GB_PAD_DOWN;
    }
    if (horizontal >= 1024) {
      keys |= horizontal >= 3072 ? GB_PAD_LEFT : GB_PAD_RIGHT;
    }
  }
  return keys;
}

// Generate up/down repeats after a debounced tap while the same direction stays held.
uint16_t repeatListScroll(uint16_t held, uint16_t raw, uint16_t tapped, uint32_t now) {
  constexpr uint32_t initialDelayMs = 350;
  constexpr uint32_t repeatDelayMs = 120;
  static uint16_t repeatingDirection = 0;
  static uint32_t lastStepAt = 0;
  static uint32_t delayMs = initialDelayMs;

  const uint16_t direction = held & kListScrollButtons;
  if (!direction || direction == kListScrollButtons) {
    repeatingDirection = 0;
    return 0;
  }
  if (tapped & direction) {
    // The regular button press already moves the selection once.
    repeatingDirection = direction;
    lastStepAt = now;
    delayMs = initialDelayMs;
    return 0;
  }
  if (repeatingDirection != direction || (raw & kListScrollButtons) != direction ||
      now - lastStepAt < delayMs) {
    return 0;
  }
  lastStepAt = now;
  delayMs = repeatDelayMs;
  return direction;
}

// Translate a single serial control key to its button mask. Unrecognized keys produce zero.
uint16_t serialControl(int key) {
  switch (key) {
    case 'w': return GB_PAD_UP;
    case 'a': return GB_PAD_LEFT;
    case 's': return GB_PAD_DOWN;
    case 'd': return GB_PAD_RIGHT;
    case 'j': return GB_PAD_A;
    case 'k': return GB_PAD_B;
    case 'u': return GB_PAD_SELECT;
    case 'i': return GB_PAD_START;
    case 'm': return kMenuKey;
    case 'q': return GBA_PAD_L;
    case 'e': return GBA_PAD_R;
    default: return 0;
  }
}

// Return a pointer to the last path component. The caller must keep the original String alive.
const char *fileName(const String &path) {
  const char *slash = strrchr(path.c_str(), '/');
  return slash ? slash + 1 : path.c_str();
}

// Copy a display title without its final extension, with optional byte-length truncation.
String fileTitle(const String &path, size_t maxCharacters = 0) {
  String title = fileName(path);
  const int extension = title.lastIndexOf('.');
  if (extension > 0) {
    title.remove(extension);
  }
  if (maxCharacters && title.length() > maxCharacters) {
    title = title.substring(0, maxCharacters > 2 ? maxCharacters - 2 : maxCharacters);
    title += "..";
  }
  return title;
}

// Compare file names the same way in the boot index and after a rename.
bool gameNameLess(const String &a, const String &b) {
  return strcasecmp(fileName(a), fileName(b)) < 0;
}

// Classify a library path by its case-insensitive ROM extension. Return -1 for other files.
int systemForPath(const String &path) {
  const char *extension = strrchr(path.c_str(), '.');
  if (!extension) {
    return -1;
  }
  for (size_t i = 0; i < kSystemCount; ++i) {
    if (strcasecmp(extension, kSystems[i].suffix) == 0) return int(i);
  }
  if (!strcasecmp(extension, ".gen") || !strcasecmp(extension, ".smd") ||
      (!strcasecmp(extension, ".bin") && !strncasecmp(path.c_str(), "/roms/genesis/", 14))) {
    return int(System::Genesis);
  }
  return -1;
}

WifiTransfer::Destination wifiUploadDestination(const char *name) {
  if (FirmwareUpdate::supportedName(name)) return {FirmwareUpdate::directory, FirmwareUpdate::maxImageBytes};
  if (DoomLauncher::supportedName(name)) return {DoomLauncher::directory, DoomLauncher::maxWadBytes};
  const int type = systemForPath(String(name));
  if (type >= 0) {
    constexpr size_t limits[] = {kMaxRomBytes, kMaxRomBytes, 32 * 1024 * 1024,
                                 4 * 1024 * 1024, 2 * 1024 * 1024 + 512, 2 * 1024 * 1024 + 512,
                                 POCKET_GENESIS_MAX_ROM};
    static_assert(std::size(limits) == kSystemCount);
    return {kSystems[type].directory, limits[type]};
  }
  const char *extension = strrchr(name, '.');
  if (extension && (!strcasecmp(extension, ".mp3") || !strcasecmp(extension, ".wav"))) {
    return {kMusicRoot, 128 * 1024 * 1024};
  }
  if (PhotoImage::supportedName(name)) return {"/photos", 6 * 1024 * 1024};
  return {};
}

// Convert a valid system enum to the index used by per-system arrays.
size_t systemIndex(System system) {
  return static_cast<size_t>(system);
}

// Return the metadata shared by the library, Home tile, and emulator launch path.
const SystemInfo &systemInfo(System system) {
  return kSystems[systemIndex(system)];
}

const char *systemTitle(System system) { return systemInfo(system).title; }
const char *systemExtension(System system) { return systemInfo(system).suffix; }

// The GB palette and Gnuboy-owned state only apply to these two libraries.
bool gameIsGbFamily() { return !gameIsGba && !activeCartridgeCore; }

// Expose the mutable directory index for a particular console.
std::vector<String> &gamesFor(System system) {
  return gamesBySystem[systemIndex(system)];
}

// Return the indexed ROMs for the currently selected console.
const std::vector<String> &currentGames() {
  return gamesFor(selectedSystem);
}

// Return the saved cursor for the selected console so each library keeps its own position.
int &currentGameSelection() {
  return gameSelections[systemIndex(selectedSystem)];
}

// Return the size of the selected console index for cursor and launch checks.
int currentGameCount() {
  return int(currentGames().size());
}

// Find an exact path in the current index and select its console and row on success.
bool selectGameByPath(const String &path) {
  const int type = systemForPath(path);
  if (type < 0) return false;
  const auto system = static_cast<System>(type);
  const auto &games = gamesFor(system);
  const auto game = std::find(games.begin(), games.end(), path);
  if (game == games.end()) return false;
  selectedSystem = system;
  currentGameSelection() = int(game - games.begin());
  return true;
}

// Identify screens that use the shared ROM list and cover-art renderer.
bool isGameListView() {
  return view == View::GameList || view == View::Favorites || view == View::RecentlyPlayed;
}

// Select the list source behind the game browser, favorites, or recent history.
const std::vector<String> &visibleGames() {
  if (view == View::Favorites) return favoriteGames;
  if (view == View::RecentlyPlayed) return recentGames;
  return currentGames();
}

// Return the cursor corresponding to visibleGames().
int &visibleGameSelection() {
  if (view == View::Favorites) return favoritesSelection;
  if (view == View::RecentlyPlayed) return recentSelection;
  return currentGameSelection();
}

// Prefer the newest SD-backed recent game and fall back to the last path saved in Preferences.
const String &continueGamePath() {
  return recentGames.empty() ? lastPlayedPath : recentGames.front();
}

// Check whether a path is present in the in-memory favorite list.
bool isFavorite(const String &path) {
  return std::find(favoriteGames.begin(), favoriteGames.end(), path) != favoriteGames.end();
}

// Map a requested SPI clock to the settings menu index, falling back to the default choice.
uint8_t clockOptionFor(uint32_t clockHz) {
  for (uint8_t i = 0; i < kTftClockOptionCount; ++i) {
    if (kTftClockOptions[i] == clockHz) {
      return i;
    }
  }
  return kTftClockOptionCount - 1;
}

// Return the label for the selected GB/GBC scaling mode.
const char *gbScaleName(Screen::GbScale scale) {
  switch (scale) {
    case Screen::GbScale::Native: return "NATIVE";
    case Screen::GbScale::Fit: return "FIT";
    case Screen::GbScale::Full: return "FULL";
  }
  return "FULL";
}

// Return the GBA viewport label for the current full-screen flag.
const char *gbaScaleName(bool fullscreen) {
  return fullscreen ? "FULL" : "NATIVE";
}

// Label the frameskip mode for menus and serial diagnostics.
const char *frameskipName(uint8_t mode) {
  constexpr const char *names[] = {"AUTO", "NONE", "LIGHT", "HIGH"};
  return names[mode];
}

// Resolve the active game's display scale, honoring existing per-game profiles.
Screen::GbScale gameGbScale() {
  if (currentGameProfile.enabled) return currentGameProfile.gbDisplay;
  return emulatorScales[systemIndex(selectedSystem)];
}

// Resolve the active GBA game's full-screen override against the global setting.
bool gameGbaFullscreen() {
  return currentGameProfile.enabled ? currentGameProfile.gbaFull : gbaFullscreen;
}

// Resolve the GBA frameskip mode for the active game.
uint8_t gameFrameskipMode() {
  return currentGameProfile.enabled ? currentGameProfile.frameskip : gbaFrameskipMode;
}

// Resolve whether emulator sound is enabled for the active game.
bool gameSoundEnabled() {
  return currentGameProfile.enabled ? currentGameProfile.sound : soundEnabled;
}

// Translate a GBA frameskip setting into its maximum consecutive render skips.
unsigned gbaFrameskipLimit() {
  // AUTO skips at most one frame to recover a small timing debt. HIGH is
  // available for games that need a more aggressive visual trade-off.
  constexpr unsigned limits[] = {1, 0, 1, 3};
  return limits[gameFrameskipMode()];
}

// Initialize the shared I2S output on demand using the configured amplifier pins.
bool ensureAudioHardware() {
  if (audioReady) {
    return true;
  }
  audio.setPins(kAudioBclk, kAudioLrclk, kAudioData);
  audioReady = audio.begin(I2S_MODE_STD, kAudioRate,
                           I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  if (!audioReady) {
    Serial.println("I2S unavailable. Sound stays off.");
  }
  return audioReady;
}

// Persist the core emulator and TFT settings in the Pocket Play Preferences namespace.
void saveDeviceSettings() {
  settingsStore.putBool("sound", soundEnabled);
  for (size_t i = 0; i < kSystemCount; ++i) {
    if (const char *key = kDisplayScaleKeys[i]) settingsStore.putUChar(key, uint8_t(emulatorScales[i]));
  }
  settingsStore.putBool("gbafull", gbaFullscreen);
  settingsStore.putBool("gbadma", gbaDmaEnabled);
  settingsStore.putUChar("frameskip", gbaFrameskipMode);
  settingsStore.putUChar("mdskip", genesisFrameskipMode);
  settingsStore.putUChar("clock", tftClockOption);
}

// Restore validated preferences and initialize settings owned by the Clock and diagnostic log.
void loadDeviceSettings() {
  Screen::setLightTheme(settingsStore.getBool("lightTheme", false));
  DeviceLog::begin(settingsStore);
  StandbyDisplay::begin(settingsStore);
  PhotoViewer::begin(settingsStore);
  lastPlayedPath = settingsStore.getString("lastPlayed");
  soundEnabled = settingsStore.getBool("sound", kEnableAudio);
  const uint8_t savedHomeStyle = settingsStore.getUChar("homeStyle", uint8_t(HomeStyle::Grid));
  homeStyle = savedHomeStyle < std::size(kHomeStyleNames)
                  ? static_cast<HomeStyle>(savedHomeStyle) : HomeStyle::Grid;
  const uint8_t savedBrightness = settingsStore.getUChar("backlight", 100);
  if (savedBrightness >= kBrightnessStep && savedBrightness <= 100 &&
      savedBrightness % kBrightnessStep == 0) {
    displayBrightness = savedBrightness;
  }
  volumePercent = std::min<uint8_t>(100, settingsStore.getUChar("volume", kDefaultVolumePercent));
  backgroundMusic = settingsStore.getBool("musicBg", false);
  musicVisualizerEnabled = settingsStore.getBool("musicMeter", true);
  musicReactiveRgb = settingsStore.getBool("musicRgb", false);
  repeatMusicTrack = settingsStore.getBool("musicRepeat", false);
  const uint8_t savedOrder = settingsStore.getUChar("musicOrder", 0);
  if (savedOrder <= uint8_t(MusicOrder::StopAfterTrack)) {
    musicOrder = static_cast<MusicOrder>(savedOrder);
  }
  const uint8_t savedAutoSleep = settingsStore.getUChar("autoSleep", 0);
  if (savedAutoSleep < std::size(kAutoSleepOptions)) {
    autoSleepOption = savedAutoSleep;
  }
  const uint8_t savedTimeout = settingsStore.getUChar("musicScreen", 0);
  if (savedTimeout < std::size(kMusicScreenTimeouts)) {
    musicScreenTimeout = savedTimeout;
  }
  const uint8_t savedEffect = settingsStore.getUChar("lightfx", uint8_t(FlashlightEffect::None));
  if (savedEffect <= uint8_t(FlashlightEffect::Breathe)) {
    flashlightEffect = static_cast<FlashlightEffect>(savedEffect);
  }
  const uint8_t savedColor = settingsStore.getUChar("lightcolor", 0);
  // The previous menu stored the two multicolor effects as color choices 11 and 12.
  if (savedColor == 11 || savedColor == 12) {
    flashlightColorIndex = kFlashlightRainbowIndex;
    flashlightEffect = savedColor == 11 ? FlashlightEffect::Flash : FlashlightEffect::Breathe;
    flashlightSettingsChanged = true;
  } else {
    flashlightColorIndex = savedColor < kFlashlightColorCount ? savedColor : 0;
  }
  flashlightBrightness = std::clamp<uint8_t>(
      settingsStore.getUChar("lightlevel", kDefaultFlashlightBrightness), kFlashlightBrightnessStep, 100);
  const uint8_t savedSpeed = settingsStore.getUChar("lightspeed", kDefaultFlashlightSpeed);
  flashlightSpeed = savedSpeed < kFlashlightSpeedCount ? savedSpeed : kDefaultFlashlightSpeed;
  // Older builds used one setting for GB, GBC, NES, Master System and Game Gear.
  const auto full = uint8_t(Screen::GbScale::Full);
  const uint8_t legacyScale = std::min(settingsStore.getUChar("gbscale", full), full);
  for (size_t i = 0; i < kSystemCount; ++i) {
    if (const char *key = kDisplayScaleKeys[i]) {
      const uint8_t fallback = i == systemIndex(System::Genesis) ? uint8_t(Screen::GbScale::Native) : legacyScale;
      const uint8_t scale = settingsStore.getUChar(key, fallback);
      emulatorScales[i] = static_cast<Screen::GbScale>(scale <= full ? scale : fallback);
    }
  }
  gbaFullscreen = settingsStore.getBool("gbafull", false);
  gbaDmaEnabled = settingsStore.getBool("gbadma", true);
  gbaFrameskipMode = settingsStore.getUChar("frameskip", 0);
  if (gbaFrameskipMode > 3) {
    gbaFrameskipMode = 0;
  }
  genesisFrameskipMode = settingsStore.getUChar("mdskip", 0);
  if (genesisFrameskipMode > 3) genesisFrameskipMode = 0;
  tftClockOption = settingsStore.getUChar("clock", clockOptionFor(kTftClockHz));
  if (tftClockOption >= kTftClockOptionCount) {
    tftClockOption = clockOptionFor(kTftClockHz);
  }
  Screen::setClockHz(kTftClockOptions[tftClockOption]);
}

// Apply the current sound choice and reconfigure the paused GBA core when applicable.
void applySoundSetting() {
  if (soundEnabled && !ensureAudioHardware()) {
    soundEnabled = false;
  }
  if (settingsReturnView == View::Paused && gameIsGba) {
    gbaCoreSetAudioEnabled(gameSoundEnabled() && audioReady);
  }
}

// Apply one signed volume step, clamp it to 0–100%, and persist only a changed value.
void adjustVolume(int direction) {
  const int adjusted = std::clamp(int(volumePercent) + direction * kVolumeStep, 0, 100);
  if (adjusted != volumePercent) {
    volumePercent = static_cast<uint8_t>(adjusted);
    settingsStore.putUChar("volume", volumePercent);
  }
}

// Apply a brightness step through the display driver and persist it after hardware success.
bool adjustDisplayBrightness(int direction) {
  if (kTftBacklight < 0) {
    return false;
  }
  const uint8_t adjusted = static_cast<uint8_t>(std::clamp(
      int(displayBrightness) + direction * kBrightnessStep, int(kBrightnessStep), 100));
  if (adjusted == displayBrightness) {
    return true;
  }
  if (!Screen::setBrightness(adjusted)) {
    return false;
  }
  displayBrightness = adjusted;
  settingsStore.putUChar("backlight", displayBrightness);
  return true;
}

// Wrap the display mode forward or backward through the supported choices.
Screen::GbScale nextDisplayScale(Screen::GbScale scale, int direction) {
  constexpr int count = int(Screen::GbScale::Full) + 1;
  return static_cast<Screen::GbScale>((int(scale) + direction + count) % count);
}

// Adjust a setting and save the changed preference.
bool cycleSettingsValue(SettingsItem item, int direction) {
  switch (item) {
    case SettingsItem::Sound:
      soundEnabled = !soundEnabled;
      applySoundSetting();
      break;
    case SettingsItem::Volume:
      adjustVolume(direction);
      return true;
    case SettingsItem::Brightness:
      return adjustDisplayBrightness(direction);
    case SettingsItem::Theme:
      Screen::setLightTheme(!Screen::lightTheme());
      settingsStore.putBool("lightTheme", Screen::lightTheme());
      return true;
    case SettingsItem::HomeStyle: {
      constexpr int count = std::size(kHomeStyleNames);
      homeStyle = static_cast<HomeStyle>((int(homeStyle) + direction + count) % count);
      settingsStore.putUChar("homeStyle", uint8_t(homeStyle));
      return true;
    }
    case SettingsItem::EmulatorDisplay: {
      auto &scale = emulatorScales[int(settingsPage) - kFirstEmulatorPage];
      scale = nextDisplayScale(scale, direction);
      break;
    }
    case SettingsItem::GbaDisplay:
      gbaFullscreen = !gbaFullscreen;
      break;
    case SettingsItem::GbaFrameskip:
      gbaFrameskipMode = (gbaFrameskipMode + direction + 4) % 4;
      break;
    case SettingsItem::GbaTransfer:
      gbaDmaEnabled = !gbaDmaEnabled;
      break;
    case SettingsItem::GenesisFrameskip:
      genesisFrameskipMode = (genesisFrameskipMode + direction + 4) % 4;
      break;
    case SettingsItem::TftClock:
      tftClockOption = (tftClockOption + direction + kTftClockOptionCount) % kTftClockOptionCount;
      Screen::setClockHz(kTftClockOptions[tftClockOption]);
      break;
    case SettingsItem::AutoSleep: {
      constexpr int count = std::size(kAutoSleepOptions);
      autoSleepOption = (autoSleepOption + direction + count) % count;
      settingsStore.putUChar("autoSleep", autoSleepOption);
      return true;
    }
    default:
      return false;
  }
  saveDeviceSettings();
  return true;
}

// Draw startup progress by phase and optionally show the number of ROMs found in that phase.
void drawBootProgress(BootStage stage, int count = -1, const char *library = nullptr) {
  constexpr const char *labels[] = {
    "STARTING", "READING SD CARD", "INDEXING GAMES", "CHECKING SD ROOT", "RESTORING LIBRARY", "READY",
  };
  static_assert(std::size(labels) == int(BootStage::Ready) + 1);
  char status[40];
  if (library && count >= 0) snprintf(status, sizeof(status), "INDEXING %s %d", library, count);
  else if (library) snprintf(status, sizeof(status), "INDEXING %s", library);
  else if (count < 0) snprintf(status, sizeof(status), "%s", labels[int(stage)]);
  else snprintf(status, sizeof(status), "%s %d", labels[int(stage)], count);
  Screen::fill(0, 164, Screen::width, 8, Screen::black);
  Screen::text((Screen::width - int(strlen(status)) * 8) / 2, 164, status, Screen::darkTheme.muted, Screen::black);

  constexpr int barWidth = 160;
  constexpr int x = (Screen::width - barWidth) / 2;
  const int progress = barWidth * (int(stage) + 1) / int(std::size(labels));
  Screen::fill(x, 181, progress, 3, Screen::darkTheme.accent);
  Screen::fill(x + progress, 181, barWidth - progress, 3, Screen::darkTheme.highlight);
}

// Index supported ROM paths by reading directory entries without opening the ROMs.
void scanDirectory(const char *directory, BootStage stage, bool showBootProgress, const char *library = nullptr) {
  if (showBootProgress) drawBootProgress(stage, -1, library);
  File dir = SD.open(directory);
  if (!dir || !dir.isDirectory()) {
    return;
  }
  int added = 0;
  uint32_t lastDrawAt = millis();
  for (;;) {
    bool isDirectory = false;
    String path = dir.getNextFileName(&isDirectory);
    if (path.isEmpty()) break;
    if (isDirectory) continue;
    const int system = systemForPath(path);
    if (system >= 0) {
      auto &games = gamesBySystem[system];
      if (games.size() < kMaxGamesPerSystem) {
        games.push_back(std::move(path));
        ++added;
      }
    }
    const uint32_t now = millis();
    if (showBootProgress && now - lastDrawAt >= 100) {
      lastDrawAt = now;
      drawBootProgress(stage, added, library);
    }
  }
}

bool mountCard() {
  return SD.begin(kSdCs, sdSpi, 10000000);
}

// Remount the card, rebuild the sorted ROM indexes, and restore favorites and history.
// Callers close active Music files before asking this function to remount the card.
void scanCard(bool showBootProgress = false) {
  GameCover::stop();
  PhotoViewer::stop();
  for (auto &games : gamesBySystem) {
    games.clear();
  }
  gameSelections.fill(0);
  favoriteGames.clear();
  recentGames.clear();
  favoritesSelection = recentSelection = 0;
  if (sdReady) {
    SD.end();
  }
  if (showBootProgress) drawBootProgress(BootStage::Mounting);
  const uint32_t mountStartedAt = millis();
  sdReady = mountCard();
  if (!sdReady) return;

  const uint32_t indexStartedAt = millis();
  DeviceLog::prepareCard();
  for (const SystemInfo &system : kSystems) {
    scanDirectory(system.directory, BootStage::Library, showBootProgress, system.title);
  }
  scanDirectory("/", BootStage::Root, showBootProgress);
  if (showBootProgress) drawBootProgress(BootStage::Restore);
  readGamePaths(kFavoritesFile, favoriteGames, kMaxFavorites);
  readGamePaths(kRecentGamesFile, recentGames, kMaxRecentGames);
  size_t total = 0;
  for (auto &games : gamesBySystem) {
    std::sort(games.begin(), games.end(), gameNameLess);
    total += games.size();
  }
  Serial.printf("SD mounted in %lu ms. Indexed %u games in %lu ms.\n",
                static_cast<unsigned long>(indexStartedAt - mountStartedAt), unsigned(total),
                static_cast<unsigned long>(millis() - indexStartedAt));
}

constexpr uint16_t kMusicAccent = 0x3DFF;
constexpr uint16_t kFlashlightAccent = 0xFFE0;
constexpr int kHomeFirstRowY = 51;
constexpr int kHomeCardHeight = 52;
constexpr int kHomeCardTitleY = 27;
constexpr int kHomeCardDetailY = 43;
constexpr int kHomeRowGap = 4;
constexpr int kHomeCardWidth = 100;
constexpr int kHomeColumnPitch = 104;
constexpr int kHomeListVisibleRows = 8;
constexpr int kHomeListRowPitch = 20;
constexpr int kHomeListDetailX = 184;
constexpr int kHomeListDetailWidth = 120;
constexpr int kHomeCarouselCenterX = Screen::width / 2;
constexpr int kHomeCarouselCardWidth = 172;
constexpr int kHomeCarouselCardY = 62;
constexpr int kHomeCarouselDetailWidth = kHomeCarouselCardWidth - 24;
constexpr int kHomeCarouselDetailY = 156;
static_assert(kHomeFirstRowY + kHomeVisibleRows * (kHomeCardHeight + kHomeRowGap) - kHomeRowGap <= 219);
static_assert(kHomeCardDetailY + 8 <= kHomeCardHeight);
static_assert(kHomeFirstRowY + kHomeListVisibleRows * kHomeListRowPitch <= 219);

struct HomeAppInfo {
  const char *title;
  const char *detail;
  uint16_t accent;
  const char *shortTitle;
};

constexpr HomeAppInfo kHomeApps[] = {
  {"MUSIC", "MP3 / WAV", kMusicAccent, "MUSIC"},
  {"FLASHLIGHT", "ONBOARD RGB", kFlashlightAccent, "LIGHT"},
  {"SETTINGS", "AUDIO / TFT", Screen::darkTheme.accent, "SETUP"},
  {"CONTINUE", "LAST GAME", Screen::darkTheme.accent, "RESUME"},
  {"FAVORITES", "STARRED", 0xFEA8, "STARS"},
  {"RECENTLY", "PLAYED", 0xAE9F, "RECENT"},
  {"MINI GAMES", "3 BUILT IN", 0x9E9F, "MINIS"},
  {"CLOCK", "DATE/TIMER", 0x7DFF, "CLOCK"},
  {"PHOTOS", "SD ALBUMS", 0xB67F, "PHOTOS"},
  {"DOOM", "SD IWADS", 0xFA49, "DOOM"},
  {"APPLICATIONS", "MICROSD APPS", 0x47BA, "APPS"},
};
static_assert(std::size(kHomeApps) == kLastHomeItem - kMusicMenuItem + 1);
constexpr int kMenuValueCenter = 230;

BatteryGauge batteryGauge;

// Average calibrated ADC samples and double the result for the equal-resistor divider.
uint32_t readBatteryMillivolts() {
  if (kBatterySense < 0) return 0;
  constexpr unsigned sampleCount = 8;
  uint32_t total = 0;
  for (unsigned i = 0; i < sampleCount; ++i) {
    total += analogReadMilliVolts(kBatterySense);
  }
  // The equal divider resistors put half of the switched battery voltage on the ADC.
  return (2 * total + sampleCount / 2) / sampleCount;
}

// Sample the battery only when the gauge is due and report whether its displayed value changed.
bool refreshBatteryReading() {
  if (kBatterySense < 0) return false;
  const uint32_t now = millis();
  return batteryGauge.needsSample(now) && batteryGauge.update(now, readBatteryMillivolts());
}

constexpr int kMenuIndicatorRight = Screen::width - 4;

// Paint the cached charge estimate or unavailable icon in the header.
void drawBatteryIndicator(uint16_t background) {
  constexpr int right = kMenuIndicatorRight;
  constexpr int x = right - 4 * 8 - 2 - 12;
  constexpr int y = 13;
  constexpr uint16_t green = 0x47E8;
  constexpr uint16_t red = 0xFA49;
  const int percent = batteryGauge.percent();
  uint16_t color = Screen::theme.muted;
  char label[5] = "--%";
  if (percent >= 0) {
    color = Screen::themeAccent(percent <= BatteryGauge::lowPercent ? red : green);
    snprintf(label, sizeof(label), "%u%%", unsigned(percent));
  }
  Screen::fill(x, y, right - x, 10, background);
  Screen::fill(x, y, 10, 10, color);
  Screen::fill(x + 1, y + 1, 8, 8, background);
  Screen::fill(x + 10, y + 3, 2, 4, color);
  if (percent > 0) {
    Screen::fill(x + 2, y + 2, (percent * 6 + 99) / 100, 6, color);
  }
  Screen::text(right - int(strlen(label)) * 8, y + 1, label, color, background);
}

// Refresh the header clock only when its formatted minute changes or the header is redrawn.
void drawClockIndicator(uint16_t background, bool force) {
  constexpr int columns = 8;  // Space for "12:59 PM".
  static uint32_t lastCheckAt = 0;
  static char lastValue[columns + 1] = {};
  const uint32_t now = millis();
  if (!force && now - lastCheckAt < 250) return;
  lastCheckAt = now;

  char timeLabel[columns + 1] = {};
  StandbyDisplay::formatTime(timeLabel, sizeof(timeLabel));
  if (force || strcmp(timeLabel, lastValue) != 0) {
    char label[columns + 1];
    snprintf(label, sizeof(label), "%8s", timeLabel);
    Screen::text(kMenuIndicatorRight - columns * 8, 26, label, Screen::theme.muted, background);
    strcpy(lastValue, timeLabel);
  }
}

// Paint the battery and clock after a full menu draw using the supplied background color.
void drawMenuIndicators(uint16_t background) {
  refreshBatteryReading();
  drawBatteryIndicator(background);
  drawClockIndicator(background, true);
}

// Refresh menu indicators and continue battery sampling while the Music screen is off.
void serviceMenuIndicators() {
  if (view == View::Playing || AppRuntime::running() ||
      (view == View::DeviceTools && toolScreen == ToolScreen::Display)) {
    return;
  }
  const bool batteryChanged = refreshBatteryReading();
  if (view == View::MusicScreenOff) return;

  const uint16_t background = view == View::Failed ? Screen::theme.background : Screen::theme.header;
  if (batteryChanged) drawBatteryIndicator(background);
  drawClockIndicator(background, false);
}

// Choose the menu accent associated with a console library.
uint16_t systemAccent(System system) {
  return Screen::themeAccent(systemInfo(system).accent);
}

// A mixed library can contain ROMs from any console. Use the highlighted ROM's palette.
uint16_t romAccent(const String &path) {
  const int type = systemForPath(path);
  return type >= 0 ? systemAccent(static_cast<System>(type)) : Screen::theme.accent;
}

// Clear the screen, reset the footer state, and draw the standard menu header.
void drawHeader(const char *title, const char *subtitle, uint16_t accent) {
  Screen::fill(0, 0, Screen::width, Screen::height, Screen::theme.background);
  menuStatusVisible = false;
  Screen::fill(0, 0, Screen::width, 4, accent);
  Screen::fill(0, 4, Screen::width, 41, Screen::theme.header);
  Screen::text(12, 10, title, Screen::theme.text, Screen::theme.header, 2);
  Screen::text(13, 35, subtitle, Screen::theme.muted, Screen::theme.header);
  drawMenuIndicators(Screen::theme.header);
}

// Paint a menu tile with its selected state and accent stripe.
void drawCard(int x, int y, int cardWidth, int height, bool selected, uint16_t accent) {
  const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
  Screen::fill(x, y, cardWidth, height, bg);
  Screen::fill(x, y, selected ? 5 : 2, height, accent);
  if (selected) {
    Screen::fill(x + 5, y, 1, height, Screen::theme.text);
  }
}

// Center a menu value and add arrow markers when the user can adjust it.
void drawMenuValue(int center, int y, const char *value, uint16_t color, uint16_t background,
                   bool adjustable) {
  char arrows[24];
  if (adjustable) {
    snprintf(arrows, sizeof(arrows), "< %s >", value);
    value = arrows;
  }
  Screen::text(center - int(strlen(value)) * 4, y, value, color, background);
}

// Draw the pictogram associated with a non-emulator Home tile.
void drawHomeAppIcon(int item, int x, int y, uint16_t color, uint16_t bg) {
  switch (item) {
    case kMusicMenuItem:
      Screen::fill(x + 8, y + 1, 12, 3, color);
      Screen::fill(x + 8, y + 3, 3, 11, color);
      Screen::fill(x + 17, y + 3, 3, 9, color);
      Screen::fill(x + 4, y + 11, 7, 5, color);
      Screen::fill(x + 13, y + 9, 7, 5, color);
      break;
    case kFlashlightMenuItem:
      Screen::fill(x + 1, y + 2, 6, 2, color);
      Screen::fill(x + 1, y + 13, 6, 2, color);
      Screen::fill(x + 8, y + 4, 7, 10, color);
      Screen::fill(x + 15, y + 6, 9, 6, color);
      break;
    case kSettingsMenuItem:
      Screen::fill(x + 8, y + 1, 9, 15, color);
      Screen::fill(x + 4, y + 5, 17, 7, color);
      Screen::fill(x + 6, y + 3, 13, 11, color);
      Screen::fill(x + 10, y + 6, 5, 5, bg);
      break;
    case kContinueMenuItem:
      for (int row = 0; row < 15; ++row) {
        Screen::fill(x + 7, y + row + 1, 3 + 2 * std::min(row, 14 - row), 1, color);
      }
      break;
    case kFavoritesMenuItem:
      Screen::text(x + 4, y, "*", color, bg, 2);
      break;
    case kRecentMenuItem:
      Screen::fill(x + 5, y + 1, 16, 15, color);
      Screen::fill(x + 7, y + 3, 12, 11, bg);
      Screen::fill(x + 12, y + 4, 2, 6, color);
      Screen::fill(x + 12, y + 9, 5, 2, color);
      break;
    case kMiniGamesMenuItem:
      for (int i = 0; i < 4; ++i) {
        Screen::fill(x + 5 + (i % 2) * 9, y + 1 + (i / 2) * 8, 7, 6, color);
      }
      break;
    case kStandbyMenuItem:
      Screen::fill(x + 3, y + 2, 20, 12, color);
      Screen::fill(x + 5, y + 4, 16, 8, bg);
      Screen::fill(x + 8, y + 6, 2, 4, color);
      Screen::fill(x + 12, y + 7, 2, 2, color);
      Screen::fill(x + 16, y + 6, 2, 4, color);
      Screen::fill(x + 9, y + 15, 8, 1, color);
      break;
    case kPhotosMenuItem:
      Screen::fill(x + 3, y + 2, 20, 14, color);
      Screen::fill(x + 5, y + 4, 16, 10, bg);
      Screen::fill(x + 16, y + 5, 3, 3, color);
      for (int step = 0; step < 5; ++step) {
        Screen::fill(x + 10 - step, y + 9 + step, 1 + 2 * step, 1, color);
      }
      break;
    case kDoomMenuItem:
      Screen::fill(x + 4, y + 1, 16, 13, color);
      Screen::fill(x + 7, y + 5, 3, 3, bg);
      Screen::fill(x + 14, y + 5, 3, 3, bg);
      Screen::fill(x + 8, y + 14, 8, 3, color);
      break;
    case kApplicationsMenuItem:
      Screen::fill(x + 4, y + 1, 18, 15, color);
      Screen::fill(x + 6, y + 5, 14, 9, bg);
      Screen::fill(x + 8, y + 7, 4, 4, color);
      Screen::fill(x + 14, y + 7, 4, 4, color);
      break;
  }
}

// Translate a stable Home item ID into its position in the configurable display order.
int homePosition(int item) {
  return int(std::find(std::begin(kHomeOrder), std::end(kHomeOrder), item) - std::begin(kHomeOrder));
}

// Determine whether the item's row lies in the Grid viewport.
bool homeAppVisible(int item) {
  const int row = homePosition(item) / kHomeColumns;
  return row >= homeFirstRow && row < homeFirstRow + kHomeVisibleRows;
}

// Find the next valid position, wrapping across the ends of Home.
int stepHomePosition(int position, int step) {
  do {
    position = (position + step + kHomePositions) % kHomePositions;
  } while (kHomeOrder[position] < 0);
  return position;
}

// Test an item against the List viewport without changing its scroll offset.
bool homeListItemVisible(int position) {
  return position >= homeFirstListRow && position < homeFirstListRow + kHomeListVisibleRows;
}

// Compute a row's position relative to the Grid viewport.
int homeRowY(int row) {
  return kHomeFirstRowY + (row - homeFirstRow) * (kHomeCardHeight + kHomeRowGap);
}

// Compute the left edge of an item from its Home column.
int homeAppX(int item) {
  return 6 + (homePosition(item) % kHomeColumns) * kHomeColumnPitch;
}

// Calculate the top of a visible List row.
int homeListRowY(int position) {
  return kHomeFirstRowY + (position - homeFirstListRow) * kHomeListRowPitch;
}

// Return the menu accent shared across layouts.
uint16_t homeItemAccent(int item) {
  return item < kMusicMenuItem ? systemAccent(static_cast<System>(item))
                              : Screen::themeAccent(kHomeApps[item - kMusicMenuItem].accent);
}

// Return a one-line name for List and the focused Carousel card.
const char *homeItemTitle(int item) {
  if (item < kMusicMenuItem) return systemTitle(static_cast<System>(item));
  if (item == kRecentMenuItem) return "RECENTLY PLAYED";
  return kHomeApps[item - kMusicMenuItem].title;
}

// Report the console count, Continue title, or static app detail.
String homeItemDetail(int item, size_t maxCharacters) {
  if (item < kMusicMenuItem) {
    const unsigned count = unsigned(gamesFor(static_cast<System>(item)).size());
    char label[16];
    snprintf(label, sizeof(label), "%u GAME%s", count, count == 1 ? "" : "S");
    return String(label);
  }
  if (item == kContinueMenuItem) {
    const String &path = continueGamePath();
    return path.isEmpty() ? String("NO GAME YET") : fileTitle(path, maxCharacters);
  }
  return String(kHomeApps[item - kMusicMenuItem].detail);
}

// Report whether background playback has a track title to display on Home.
bool homeHasMusicTitle() {
  return backgroundMusic && musicPlayer.isOpen() && !musicTrackTitle.isEmpty();
}

// Render a static name or advance its bounded marquee without repainting surrounding content.
void drawMusicTitle(int x, int y, int width, uint16_t fg, uint16_t bg,
                    bool centered, bool largeWhenItFits, bool redraw,
                    Screen::TextFont font = Screen::TextFont::Standard) {
  if (musicTitleView != view) {
    musicTitleView = view;
    musicTitleStartedAt = millis();
    musicTitleLastOffset = -1;
  }
  const bool compact = font == Screen::TextFont::Compact;
  const int textWidth = int(musicTrackTitle.length()) * (compact ? Screen::compactTextAdvance : 8);
  if (textWidth <= width) {
    if (redraw) {
      if (compact) {
        Screen::compactLabel(x + width / 2, y, musicTrackTitle.c_str(), fg, bg);
      } else {
        const int scale = largeWhenItFits && textWidth * 2 <= width ? 2 : 1;
        const int inset = centered ? (width - textWidth * scale) / 2 : 0;
        Screen::text(x + inset, y - (scale - 1) * 4, musicTrackTitle.c_str(), fg, bg, scale);
      }
    }
    return;
  }
  constexpr uint32_t holdMs = 900;
  constexpr uint32_t stepMs = 45;
  const uint32_t cycleMs = holdMs + (textWidth + Screen::marqueeGapPixels) * stepMs;
  const uint32_t phase = (millis() - musicTitleStartedAt) % cycleMs;
  const int offset = phase < holdMs ? 0 : (phase - holdMs) / stepMs;
  if (redraw || offset != musicTitleLastOffset) {
    Screen::marqueeText(x, y, width, musicTrackTitle.c_str(), offset, fg, bg, font);
    musicTitleLastOffset = offset;
  }
}

// Only the focused Carousel card or a visible row can host a live track title.
bool homeMusicVisible() {
  switch (homeStyle) {
    case HomeStyle::Grid: return homeAppVisible(kMusicMenuItem);
    case HomeStyle::Carousel: return systemSelection == kMusicMenuItem;
    case HomeStyle::List: return homeListItemVisible(homePosition(kMusicMenuItem));
  }
  return false;
}

// Draw the Home marquee within the track field for the current layout.
void drawHomeMusicTitle(bool redraw) {
  const bool selected = systemSelection == kMusicMenuItem;
  const uint16_t fg = selected ? Screen::themeAccent(kMusicAccent) : Screen::theme.muted;
  const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
  switch (homeStyle) {
    case HomeStyle::Grid:
      drawMusicTitle(homeAppX(kMusicMenuItem) + 6,
                     homeRowY(homePosition(kMusicMenuItem) / kHomeColumns) + kHomeCardDetailY,
                     kHomeCardWidth - 12, fg, bg, true, false, redraw, Screen::TextFont::Compact);
      break;
    case HomeStyle::Carousel:
      drawMusicTitle(kHomeCarouselCenterX - kHomeCarouselDetailWidth / 2, kHomeCarouselDetailY,
                     kHomeCarouselDetailWidth, fg, bg, true, false, redraw);
      break;
    case HomeStyle::List:
      drawMusicTitle(kHomeListDetailX, homeListRowY(homePosition(kMusicMenuItem)) + 5,
                     kHomeListDetailWidth, fg, bg, true, false, redraw);
      break;
  }
}

// Advance the marquee only when the current screen exposes a track title.
void serviceMusicTitle() {
  if (view == View::SystemMenu && homeMusicVisible() && homeHasMusicTitle()) {
    drawHomeMusicTitle(false);
  } else if (view == View::NowPlaying) {
    drawMusicTitle(12, 58, nowPlayingTitleWidth, Screen::theme.text, Screen::theme.background, false, true, false);
  } else if (view == View::Music && musicPlayer.isOpen() && !musicFolderMessage) {
    drawMusicTitle(18, 74, 288, Screen::theme.text, Screen::theme.panel, false, false, false);
  } else {
    musicTitleView = View::Failed;
  }
}

// Draw a console silhouette inside a Home tile.
void drawHomeConsoleIcon(System system, int center, int y, uint16_t accent) {
  if (system == System::Nes) {
    Screen::fill(center - 18, y + 5, 36, 16, accent);
    Screen::fill(center - 16, y + 8, 32, 10, Screen::theme.header);
    Screen::fill(center - 14, y + 12, 8, 2, accent);
    Screen::fill(center - 11, y + 9, 2, 8, accent);
    Screen::fill(center + 5, y + 12, 4, 4, accent);
    Screen::fill(center + 11, y + 12, 4, 4, accent);
    return;
  }
  if (system == System::MasterSystem || system == System::Genesis) {
    Screen::fill(center - 18, y + 4, 36, 19, accent);
    Screen::fill(center - 15, y + 7, 30, 8, Screen::theme.header);
    Screen::fill(center - 11, y + 9, 22, 3, accent);
    Screen::fill(center + 7, y + 17, 7, 3, Screen::theme.header);
    return;
  }
  if (system == System::GameBoyAdvance || system == System::GameGear) {
    Screen::fill(center - 18, y + 4, 36, 18, accent);
    Screen::fill(center - 9, y + 7, 18, 12, Screen::theme.header);
    Screen::fill(center - 16, y + 12, 5, 1, Screen::theme.header);
    Screen::fill(center - 14, y + 10, 1, 5, Screen::theme.header);
    Screen::fill(center + 13, y + 9, 3, 3, Screen::theme.header);
    Screen::fill(center + 10, y + 14, 3, 3, Screen::theme.header);
    return;
  }

  const int halfWidth = system == System::GameBoy ? 12 : 10;
  Screen::fill(center - halfWidth, y, halfWidth * 2, 23, accent);
  Screen::fill(center - halfWidth + 3, y + 3, halfWidth * 2 - 6, 10, Screen::theme.header);
  Screen::fill(center - 8, y + 16, 6, 2, Screen::theme.header);
  Screen::fill(center - 6, y + 14, 2, 6, Screen::theme.header);
  Screen::fill(center + 5, y + 14, 3, 3, Screen::theme.header);
  Screen::fill(center + 2, y + 18, 3, 3, Screen::theme.header);
}

// Center a label at the given font scale.
void drawCenteredText(int center, int y, const char *label, uint16_t fg, uint16_t bg, int scale = 1) {
  Screen::text(center - int(strlen(label)) * 4 * scale, y, label, fg, bg, scale);
}

// Draw a uniform Grid card with the icon, title, and detail for its Home item.
void drawHomeGridCard(int item) {
  const int x = homeAppX(item);
  const int y = homeRowY(homePosition(item) / kHomeColumns);
  const int center = x + kHomeCardWidth / 2;
  const bool selected = item == systemSelection;
  const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
  const uint16_t fg = selected ? Screen::theme.text : Screen::theme.muted;
  const uint16_t accent = homeItemAccent(item);
  drawCard(x, y, kHomeCardWidth, kHomeCardHeight, selected, accent);
  if (item < kMusicMenuItem) {
    const auto system = static_cast<System>(item);
    const auto &info = systemInfo(system);
    drawHomeConsoleIcon(system, center, y + 2, accent);
    Screen::compactLabel(center, y + kHomeCardTitleY, info.cardTitle, fg, bg);
    if (*info.cardSubtitle) {
      Screen::compactLabel(center, y + kHomeCardTitleY + 8, info.cardSubtitle, fg, bg);
    }
  } else {
    drawHomeAppIcon(item, center - 13, y + 6, accent, bg);
    Screen::compactLabel(center, y + kHomeCardTitleY, kHomeApps[item - kMusicMenuItem].title, fg, bg);
  }
  if (item == kMusicMenuItem && homeHasMusicTitle()) {
    drawHomeMusicTitle(true);
  } else {
    const String detail = homeItemDetail(item, (kHomeCardWidth - 12) / Screen::compactTextAdvance);
    Screen::compactLabel(center, y + kHomeCardDetailY, detail.c_str(), selected ? accent : Screen::theme.muted, bg);
  }
}

// Draw a viewport indicator on the right edge of Home.
void drawHomeScrollbar(int first, int visible, int total) {
  constexpr int trackHeight = 168;
  const int thumbHeight = trackHeight * visible / total;
  Screen::fill(317, 51, 2, trackHeight, Screen::theme.panel);
  Screen::fill(317, 51 + first * trackHeight / total, 2, thumbHeight, Screen::theme.accent);
}

// Keep the focused Grid tile in view and repaint changed cards within that viewport.
void drawHomeGrid(int previousSelection) {
  const bool fullRedraw = previousSelection < 0;
  const int selectedRow = homePosition(systemSelection) / kHomeColumns;
  const int previousFirstRow = homeFirstRow;
  homeFirstRow = std::clamp(homeFirstRow, std::max(0, selectedRow - kHomeVisibleRows + 1),
                            std::min(selectedRow, kHomeRows - kHomeVisibleRows));
  const bool viewportChanged = homeFirstRow != previousFirstRow;
  if (!fullRedraw && viewportChanged) {
    Screen::fill(0, 49, Screen::width, 170, Screen::theme.background);
  }
  if (fullRedraw || viewportChanged) {
    drawHomeScrollbar(homeFirstRow, kHomeVisibleRows, kHomeRows);
  }
  const int end = std::min(kHomePositions, (homeFirstRow + kHomeVisibleRows) * kHomeColumns);
  for (int position = homeFirstRow * kHomeColumns; position < end; ++position) {
    const int item = kHomeOrder[position];
    if (item < 0) continue;
    if (!fullRedraw && !viewportChanged && item != previousSelection && item != systemSelection) {
      continue;
    }
    drawHomeGridCard(item);
  }
}

// Draw only the focused Carousel card so a track change leaves neighboring cards intact.
void drawHomeCarouselCard() {
  const int item = systemSelection;
  const uint16_t accent = homeItemAccent(item);
  constexpr int center = kHomeCarouselCenterX;
  constexpr int y = kHomeCarouselCardY;
  const uint16_t bg = Screen::theme.selected;
  drawCard(center - kHomeCarouselCardWidth / 2, y, kHomeCarouselCardWidth, 125, true, accent);
  Screen::fill(center - 27, y + 9, 54, 35, Screen::theme.header);
  if (item < kMusicMenuItem) {
    const auto system = static_cast<System>(item);
    const auto &info = systemInfo(system);
    drawHomeConsoleIcon(system, center, y + 14, accent);
    drawCenteredText(center, y + 52, info.cardTitle, Screen::theme.text, bg, 2);
    if (*info.cardSubtitle) {
      drawCenteredText(center, y + 71, info.cardSubtitle, Screen::theme.text, bg);
    }
  } else {
    drawHomeAppIcon(item, center - 13, y + 18, accent, Screen::theme.header);
    const char *title = homeItemTitle(item);
    const int scale = strlen(title) <= (kHomeCarouselCardWidth - 16) / 16 ? 2 : 1;
    drawCenteredText(center, y + 62 - 4 * (scale - 1), title, Screen::theme.text, bg, scale);
  }
  Screen::fill(center - 47, kHomeCarouselDetailY - 7, 94, 1, Screen::theme.header);
  if (item == kMusicMenuItem && homeHasMusicTitle()) {
    drawHomeMusicTitle(true);
  } else {
    const String detail = homeItemDetail(item, kHomeCarouselDetailWidth / 8);
    drawCenteredText(center, kHomeCarouselDetailY, detail.c_str(), accent, bg);
  }
}

// Repaint the three fixed Carousel cards in place. The header and footer are owned by Home.
void drawHomeCarousel(int previousSelection) {
  if (previousSelection == systemSelection) return;
  const int position = homePosition(systemSelection);
  for (int step : {-1, 1}) {
    const int neighbor = kHomeOrder[stepHomePosition(position, step)];
    const uint16_t accent = homeItemAccent(neighbor);
    constexpr int y = 87;
    const int x = step < 0 ? 6 : Screen::width - 66;
    const int center = x + 30;
    drawCard(x, y, 60, 74, false, accent);
    drawCenteredText(center, y + 8, step < 0 ? "<" : ">", Screen::theme.text, Screen::theme.panel);
    if (neighbor < kMusicMenuItem) {
      const auto system = static_cast<System>(neighbor);
      drawHomeConsoleIcon(system, center, y + 22, accent);
      char label[5];
      const char *ext = systemExtension(system) + 1;
      snprintf(label, sizeof(label), "%s", ext);
      for (char *letter = label; *letter; ++letter) *letter = char(toupper(*letter));
      drawCenteredText(center, y + 54, label, Screen::theme.muted, Screen::theme.panel);
    } else {
      drawHomeAppIcon(neighbor, center - 13, y + 25, accent, Screen::theme.panel);
      drawCenteredText(center, y + 54, kHomeApps[neighbor - kMusicMenuItem].shortTitle,
                           Screen::theme.muted, Screen::theme.panel);
    }
  }
  drawHomeCarouselCard();
  constexpr int segment = 8;
  const int left = (Screen::width - kHomePositions * segment) / 2;
  for (int i = 0; i < kHomePositions; ++i) {
    Screen::fill(left + i * segment, 202, segment - 2, 3,
                 i == position ? homeItemAccent(systemSelection) : Screen::theme.panel);
  }
}

// Draw a List row and its corresponding count, track title, or app detail.
void drawHomeListRow(int position) {
  const int item = kHomeOrder[position];
  const int y = homeListRowY(position);
  if (item < 0) {
    Screen::fill(6, y, 308, kHomeListRowPitch - 2, Screen::theme.background);
    return;
  }
  const bool selected = systemSelection == item;
  const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
  const uint16_t accent = homeItemAccent(item);
  drawCard(6, y, 308, kHomeListRowPitch - 2, selected, accent);
  Screen::text(18, y + 5, homeItemTitle(item), selected ? Screen::theme.text : Screen::theme.muted, bg);
  if (item == kMusicMenuItem && homeHasMusicTitle()) {
    drawHomeMusicTitle(true);
  } else {
    const String detail = homeItemDetail(item, kHomeListDetailWidth / 8);
    drawCenteredText(kHomeListDetailX + kHomeListDetailWidth / 2, y + 5,
                         detail.c_str(), selected ? accent : Screen::theme.muted, bg);
  }
}

// Repaint the old and new List selection. Scrolling rewrites rows in place without clearing the body.
void drawHomeList(int previousSelection) {
  const int position = homePosition(systemSelection);
  const int previousFirst = homeFirstListRow;
  if (position < homeFirstListRow) homeFirstListRow = position;
  if (position >= homeFirstListRow + kHomeListVisibleRows) {
    homeFirstListRow = position - kHomeListVisibleRows + 1;
  }
  const bool viewportChanged = homeFirstListRow != previousFirst;
  if (previousSelection < 0 || viewportChanged) {
    drawHomeScrollbar(homeFirstListRow, kHomeListVisibleRows, kHomePositions);
  }
  const int end = std::min(homeFirstListRow + kHomeListVisibleRows, kHomePositions);
  for (int i = homeFirstListRow; i < end; ++i) {
    if (previousSelection < 0 || viewportChanged ||
        kHomeOrder[i] == previousSelection || i == position) {
      drawHomeListRow(i);
    }
  }
}

// Restore the visible Music card or row when background playback changes tracks or stops.
void redrawHomeMusic() {
  if (!homeMusicVisible()) return;
  switch (homeStyle) {
    case HomeStyle::Grid: drawHomeGridCard(kMusicMenuItem); break;
    case HomeStyle::Carousel: drawHomeCarouselCard(); break;
    case HomeStyle::List: drawHomeListRow(homePosition(kMusicMenuItem)); break;
  }
}

// Draw or update the selected Home layout while sharing one status header and footer.
void drawSystemMenu(bool listOnSerial, int previousSelection = -1) {
  const bool fullRedraw = previousSelection < 0;
  if (fullRedraw) {
    drawHeader("POCKET PLAY", "CHOOSE AN APP", Screen::theme.accent);
  }
  switch (homeStyle) {
    case HomeStyle::Grid: drawHomeGrid(previousSelection); break;
    case HomeStyle::Carousel: drawHomeCarousel(previousSelection); break;
    case HomeStyle::List: drawHomeList(previousSelection); break;
  }
  if (listOnSerial) {
    Serial.println("\nPocket Play. W/A/S/D moves, J or I opens, R rescans.");
    Serial.println(sdReady ? "microSD mounted." : "No microSD mounted.");
    for (const int item : kHomeOrder) {
      if (item < 0) continue;
      const char marker = item == systemSelection ? '>' : ' ';
      const char *title = homeItemTitle(item);
      if (item < kMusicMenuItem) {
        const auto system = static_cast<System>(item);
        Serial.printf("%c %s (%d %s)\n", marker, title, int(gamesFor(system).size()), systemExtension(system));
      } else {
        Serial.printf("%c %s\n", marker, title);
      }
    }
  }
  if (fullRedraw || menuStatusVisible) {
    if (!sdReady) {
      showStatus("SD card not mounted - SELECT rescans");
      menuStatusVisible = false;  // This is the normal Home footer without an SD card.
    } else {
      if (!fullRedraw) clearMenuFooter();
      Screen::text(12, 224, "D-PAD: MOVE   A: OPEN   SELECT: SD",
                   Screen::theme.muted, Screen::theme.background);
    }
  }
}

// Move in the Grid by row and column, or in shared Home order for Carousel and List.
bool moveHomeSelection(uint16_t pressed) {
  constexpr int rows = kHomeRows;
  const int previous = systemSelection;
  const int position = homePosition(systemSelection);
  if (homeStyle != HomeStyle::Grid) {
    const bool vertical = pressed & (GB_PAD_UP | GB_PAD_DOWN);
    if (vertical || (pressed & (GB_PAD_LEFT | GB_PAD_RIGHT))) {
      const bool backward = pressed & (vertical ? GB_PAD_UP : GB_PAD_LEFT);
      systemSelection = kHomeOrder[stepHomePosition(position, backward ? -1 : 1)];
    }
    return previous != systemSelection;
  }
  const int row = position / kHomeColumns;
  const int column = position % kHomeColumns;
  if (pressed & (GB_PAD_UP | GB_PAD_DOWN)) {
    const int step = pressed & GB_PAD_UP ? rows - 1 : 1;
    const int first = ((row + step) % rows) * kHomeColumns;
    int target = std::min(first + column, kHomePositions - 1);
    while (target > first && kHomeOrder[target] < 0) --target;
    systemSelection = kHomeOrder[target];
  } else if (pressed & (GB_PAD_LEFT | GB_PAD_RIGHT)) {
    const int first = row * kHomeColumns;
    int count = std::min(kHomeColumns, kHomePositions - first);
    while (count > 1 && kHomeOrder[first + count - 1] < 0) --count;
    const int step = pressed & GB_PAD_LEFT ? count - 1 : 1;
    systemSelection = kHomeOrder[first + (column + step) % count];
  }
  return previous != systemSelection;
}

// Recognize the ordered sliding puzzle with its empty slot in the last position.
bool puzzleSolved() {
  for (int i = 0; i < 8; ++i) {
    if (puzzleTiles[i] != i + 1) return false;
  }
  return puzzleTiles[8] == 0;
}

// Shuffle by legal moves from the solved state to keep the resulting puzzle solvable.
void newPuzzle() {
  for (int i = 0; i < 8; ++i) puzzleTiles[i] = i + 1;
  puzzleTiles[8] = 0;
  puzzleGap = 8;
  int previousGap = -1;
  do {
    for (int i = 0; i < 40; ++i) {
      int neighbors[4];
      int count = 0;
      const int row = puzzleGap / 3;
      const int col = puzzleGap % 3;
      if (row > 0 && puzzleGap - 3 != previousGap) neighbors[count++] = puzzleGap - 3;
      if (row < 2 && puzzleGap + 3 != previousGap) neighbors[count++] = puzzleGap + 3;
      if (col > 0 && puzzleGap - 1 != previousGap) neighbors[count++] = puzzleGap - 1;
      if (col < 2 && puzzleGap + 1 != previousGap) neighbors[count++] = puzzleGap + 1;
      previousGap = puzzleGap;
      puzzleGap = neighbors[random(count)];
      std::swap(puzzleTiles[previousGap], puzzleTiles[puzzleGap]);
    }
  } while (puzzleSolved());
  puzzleMoves = 0;
  puzzleComplete = false;
}

// Draw one square of the sliding puzzle, leaving the gap unnumbered.
void drawPuzzleTile(int index) {
  const int x = 81 + index % 3 * 54;
  const int y = 52 + index / 3 * 47;
  const uint8_t tile = puzzleTiles[index];
  const uint16_t color = tile ? Screen::theme.selected : Screen::theme.panel;
  Screen::fill(x, y, 50, 42, color);
  if (tile) {
    char label[] = {char('0' + tile), 0};
    Screen::text(x + 17, y + 13, label, Screen::theme.text, color, 2);
    Screen::fill(x, y + 40, 50, 2, Screen::theme.accent);
  }
}

// Update the puzzle outcome and move count without redrawing the board.
void drawPuzzleResult() {
  char line[35];
  snprintf(line, sizeof(line), "%s   %u MOVE%s", puzzleComplete ? "SOLVED" : "IN PROGRESS",
           unsigned(puzzleMoves), puzzleMoves == 1 ? "" : "S");
  Screen::fill(6, 199, 308, 18, Screen::theme.panel);
  Screen::text(14, 204, line, puzzleComplete ? Screen::theme.accent : Screen::theme.text, Screen::theme.panel);
}

// Map elapsed time within the current round to the back-and-forth cursor position.
int timingPosition(uint32_t now) {
  const unsigned phase = (now - timingStartedAt) % 1800;
  return int((phase < 900 ? phase : 1800 - phase) * 258 / 900);
}

// Choose a new target and reset the marker and result for the next round.
void newTimingRound() {
  timingTarget = 24 + int(random(211));
  timingStartedAt = millis();
  timingDrawnAt = timingStartedAt;
  timingResult = -1;
}

// Draw the target and cursor for the current round, including its stopped position.
void drawTimingTrack() {
  const int position = timingResult < 0 ? timingPosition(millis()) : timingStoppedAt;
  Screen::fill(28, 106, 264, 15, Screen::theme.panel);
  Screen::fill(28 + timingTarget - 13, 106, 27, 15, 0x0449);
  Screen::fill(28 + position, 106, 5, 15, Screen::theme.text);
  timingDrawnAt = millis();
}

// Show the timing result and cumulative score for the current session.
void drawTimingScore() {
  char score[38];
  snprintf(score, sizeof(score), "%s    %u / %u HITS",
           timingResult < 0 ? "READY" : timingResult ? "HIT!" : "MISSED",
           unsigned(timingHits), unsigned(timingAttempts));
  Screen::fill(20, 152, 280, 24, Screen::theme.panel);
  Screen::text(30, 160, score, Screen::theme.text, Screen::theme.panel);
}

// Draw the mini-games menu or the active game and its controls.
void drawMiniGames() {
  if (miniScreen == MiniScreen::Puzzle) {
    drawHeader("TILE PUZZLE", "PUT THE TILES IN ORDER", Screen::theme.accent);
    for (int i = 0; i < 9; ++i) drawPuzzleTile(i);
    drawPuzzleResult();
    Screen::text(10, 224, "D-PAD: MOVE GAP  START: NEW  B: BACK", Screen::theme.muted, Screen::theme.background);
  } else if (miniScreen == MiniScreen::Timing) {
    drawHeader("TIMING", "PRESS A INSIDE THE TARGET", Screen::theme.accent);
    drawTimingTrack();
    drawTimingScore();
    Screen::text(10, 224, "A: STOP / NEXT ROUND    B: BACK", Screen::theme.muted, Screen::theme.background);
  } else if (miniScreen == MiniScreen::Snake) {
    drawHeader("SNAKE", "EAT FRUIT AND AVOID THE WALLS", Screen::theme.accent);
    SnakeGame::draw();
    Screen::text(10, 224, "D-PAD: TURN   START: NEW   B: BACK", Screen::theme.muted, Screen::theme.background);
  } else {
    drawHeader("MINI GAMES", "A: PLAY    B / MENU: HOME", Screen::theme.accent);
    constexpr const char *names[] = {"TILE PUZZLE", "TIMING", "SNAKE"};
    constexpr const char *details[] = {"SLIDE THE TILES INTO ORDER", "STOP THE MARKER IN THE TARGET", "EAT FRUIT AND GROW"};
    static_assert(std::size(names) == kMiniGameCount && std::size(details) == kMiniGameCount);
    for (int i = 0; i < kMiniGameCount; ++i) {
      const bool selected = i == miniSelection;
      const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
      const int y = 54 + i * 53;
      drawCard(6, y, 308, 49, selected, Screen::theme.accent);
      Screen::text(18, y + 9, names[i], selected ? Screen::theme.text : Screen::theme.muted, bg, 2);
      Screen::text(18, y + 32, details[i], Screen::theme.muted, bg);
    }
  }
}

// Enter the mini-games landing page with the first item selected.
void openMiniGames() {
  view = View::MiniGames;
  miniScreen = MiniScreen::Menu;
  miniSelection = 0;
  drawMiniGames();
}

// Route menu navigation and the currently selected mini game.
void handleMiniGames(uint16_t pressed) {
  if (pressed & (GB_PAD_B | kMenuKey)) {
    if (miniScreen == MiniScreen::Menu) {
      view = View::SystemMenu;
      drawSystemMenu(true);
    } else {
      miniScreen = MiniScreen::Menu;
      drawMiniGames();
    }
    return;
  }
  if (miniScreen == MiniScreen::Menu) {
    if (pressed & (GB_PAD_UP | GB_PAD_DOWN | GB_PAD_LEFT | GB_PAD_RIGHT)) {
      const int step = pressed & (GB_PAD_UP | GB_PAD_LEFT) ? kMiniGameCount - 1 : 1;
      miniSelection = (miniSelection + step) % kMiniGameCount;
      drawMiniGames();
    } else if (pressed & (GB_PAD_A | GB_PAD_START)) {
      miniScreen = static_cast<MiniScreen>(miniSelection + 1);
      if (miniScreen == MiniScreen::Puzzle) {
        newPuzzle();
      } else if (miniScreen == MiniScreen::Timing) {
        timingHits = timingAttempts = 0;
        newTimingRound();
      } else {
        SnakeGame::reset(millis());
      }
      drawMiniGames();
    }
    return;
  }
  if (miniScreen == MiniScreen::Snake) {
    SnakeGame::update(millis(), pressed);
    return;
  }
  if (miniScreen == MiniScreen::Timing) {
    if (pressed & GB_PAD_A) {
      if (timingResult < 0) {
        timingStoppedAt = timingPosition(millis());
        timingResult = abs(timingStoppedAt + 2 - timingTarget) <= 13;
        ++timingAttempts;
        timingHits += timingResult;
      } else {
        newTimingRound();
      }
      drawTimingTrack();
      drawTimingScore();
    } else if (timingResult < 0 && millis() - timingDrawnAt >= 33) {
      drawTimingTrack();
    }
    return;
  }
  if ((pressed & GB_PAD_START) || (puzzleComplete && (pressed & GB_PAD_A))) {
    newPuzzle();
    drawMiniGames();
    return;
  }
  if (puzzleComplete) return;
  const int row = puzzleGap / 3;
  const int col = puzzleGap % 3;
  int next = puzzleGap;
  if ((pressed & GB_PAD_UP) && row > 0) next -= 3;
  else if ((pressed & GB_PAD_DOWN) && row < 2) next += 3;
  else if ((pressed & GB_PAD_LEFT) && col > 0) --next;
  else if ((pressed & GB_PAD_RIGHT) && col < 2) ++next;
  if (next != puzzleGap) {
    std::swap(puzzleTiles[puzzleGap], puzzleTiles[next]);
    drawPuzzleTile(puzzleGap);
    puzzleGap = next;
    drawPuzzleTile(puzzleGap);
    ++puzzleMoves;
    puzzleComplete = puzzleSolved();
    drawPuzzleResult();
  }
}

// Validate the RGB GPIO against other assigned pins, allowing the deliberate TFT clock overlap.
bool flashlightPinAvailable() {
  // SCK is intentionally shareable. Any other configured signal keeps its pin.
  const int pin = kFlashlightRgbPin;
  const int occupied[] = {kTftMosi, kTftCs, kTftDc, kTftReset, kTftBacklight,
                          kSdSck, kSdMosi, kSdMiso, kSdCs, kAudioBclk, kAudioLrclk, kAudioData};
  for (const int other : occupied) {
    if (pin == other) {
      return false;
    }
  }
  for (const auto &button : kButtons) {
    if (pin == button.pin) {
      return false;
    }
  }
  return GPIO_IS_VALID_OUTPUT_GPIO(pin);
}

// Send one latched RGB value and restore SPI when the LED shares the TFT clock.
void writeFlashlightLed(const FlashlightRgb &rgb) {
  if (!flashlightPinAvailable()) {
    return;
  }
  if (kFlashlightSharesTftClock && !Screen::suspendSpi()) {
    Serial.println("Flashlight could not release the TFT clock pin.");
    return;
  }

  rgbLedWrite(kFlashlightRgbPin, rgb.red, rgb.green, rgb.blue);
  lastLedRgb = rgb;
  delayMicroseconds(300);  // Keep the WS2812 data line low while it latches the new color.
  rmtDeinit(kFlashlightRgbPin);
  pinMode(kFlashlightRgbPin, OUTPUT);
  digitalWrite(kFlashlightRgbPin, LOW);

  if (kFlashlightSharesTftClock && !Screen::resumeSpi()) {
    Serial.println("Could not restore TFT SPI after the flashlight update.");
  }
}

// Compute the scaled RGB value for a palette, rainbow animation, or brightness effect.
FlashlightRgb flashlightPixel(uint32_t elapsed, bool steady) {
  FlashlightRgb rgb = kFlashlightColors[flashlightColorIndex].rgb;
  if (flashlightColorIndex == kFlashlightRainbowIndex) {
    constexpr size_t spectrumSize = std::size(kFlashlightSpectrum);
    constexpr uint32_t segmentMs = 700;
    const size_t segment = (elapsed / segmentMs) % spectrumSize;
    const uint32_t phase = elapsed % segmentMs;
    const auto &from = kFlashlightSpectrum[segment];
    const auto &to = kFlashlightSpectrum[(segment + 1) % spectrumSize];
    const auto blend = [phase](uint8_t a, uint8_t b) {
      return uint8_t((a * (segmentMs - phase) + b * phase + segmentMs / 2) / segmentMs);
    };
    rgb = {blend(from.red, to.red), blend(from.green, to.green), blend(from.blue, to.blue)};
  }

  unsigned level = flashlightBrightness;
  const auto &speed = kFlashlightSpeeds[flashlightSpeed];
  switch (steady ? FlashlightEffect::None : flashlightEffect) {
    case FlashlightEffect::None:
      break;
    case FlashlightEffect::Flash:
      if (elapsed % speed.flashCycleMs >= speed.flashCycleMs * 3 / 5) {
        level = 0;
      }
      break;
    case FlashlightEffect::Breathe: {
      const uint32_t cycleMs = speed.breatheCycleMs;
      const uint32_t halfCycleMs = cycleMs / 2;
      const uint32_t phase = elapsed % cycleMs;
      const uint32_t ramp = std::min(phase, cycleMs - phase);
      const unsigned envelope = 10 + ramp * 90 / halfCycleMs;
      level = (level * envelope + 50) / 100;
      break;
    }
  }
  const auto scale = [level](uint8_t channel) {
    return uint8_t((channel * level + 50) / 100);
  };
  return {scale(rgb.red), scale(rgb.green), scale(rgb.blue)};
}

// Apply the current flashlight state and remember the time of the hardware update.
void applyFlashlightLed(uint32_t now, bool steady) {
  flashlightLastUpdateAt = now;
  writeFlashlightLed(flashlightOn ? flashlightPixel(now - flashlightEffectStartedAt, steady) : FlashlightRgb{});
}

// Advance LED effects and music-reactive levels when display ownership permits it.
void serviceFlashlight() {
  if (view == View::DeviceTools && toolScreen == ToolScreen::Led) return;
  const uint32_t now = millis();
  if (musicReactiveRgb && musicPlayer.isOpen() && !musicPaused &&
      view != View::Playing && view != View::Failed && !hasPausedGame()) {
    if (!musicRgbWasActive || now - musicRgbUpdatedAt >= 100) {
      const uint16_t levels = musicAudioLevels.load(std::memory_order_relaxed);
      const uint8_t left = (levels & 255) >> 2;
      const uint8_t right = (levels >> 8) >> 2;
      writeFlashlightLed({right, uint8_t(std::min(left, right) / 2), left});
      musicRgbUpdatedAt = now;
      musicRgbWasActive = true;
    }
    return;
  }
  if (musicRgbWasActive && view != View::Playing && view != View::Failed) {
    musicRgbWasActive = false;
    applyFlashlightLed(now, false);
  }
  // Gameplay may own the TFT SPI bus. The LED keeps the last color it received.
  if (!flashlightOn || view == View::Playing || view == View::Failed) {
    return;
  }
  const bool animated = flashlightColorIndex == kFlashlightRainbowIndex || flashlightEffect != FlashlightEffect::None;
  if (!animated && (!kFlashlightSharesTftClock || view == View::Flashlight)) {
    return;
  }
  const uint32_t interval = animated ? kFlashlightRefreshMs : 250;
  if (now - flashlightLastUpdateAt >= interval) {
    applyFlashlightLed(now, false);
  }
}

// Pack RGB channels into an RGB565 value for the flashlight color preview.
uint16_t flashlightSwatch(const FlashlightRgb &rgb) {
  return ((rgb.red & 0xF8) << 8) | ((rgb.green & 0xFC) << 3) | (rgb.blue >> 3);
}

// Draw the flashlight choices, values, and color swatch.
void drawFlashlightControls() {
  const auto &color = kFlashlightColors[flashlightColorIndex];
  char brightness[5];
  snprintf(brightness, sizeof(brightness), "%u%%", unsigned(flashlightBrightness));
  const char *power = flashlightOn ? "ON" : "OFF";
  const bool powerAvailable = flashlightPinAvailable();
  if (!powerAvailable) {
    power = "PIN IN USE";
  }
  const char *labels[] = {"POWER", "COLOR", "BRIGHTNESS", "COLOR FLASH", "COLOR BREATHE", "EFFECT SPEED"};
  const char *values[] = {power, color.name, brightness,
                          flashlightEffect == FlashlightEffect::Flash ? "ON" : "OFF",
                          flashlightEffect == FlashlightEffect::Breathe ? "ON" : "OFF",
                          kFlashlightSpeeds[flashlightSpeed].label};
  static_assert(std::size(labels) == kFlashlightItemCount && std::size(values) == kFlashlightItemCount);

  for (int i = 0; i < kFlashlightItemCount; ++i) {
    const int y = 51 + i * 28;
    const bool selected = i == flashlightSelection;
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    drawCard(6, y, 308, 24, selected, Screen::themeAccent(kFlashlightAccent));
    Screen::text(18, y + 8, labels[i], selected ? Screen::theme.text : Screen::theme.muted, bg);
    // Leave room for the color swatch at the right edge.
    drawMenuValue(212, y + 8, values[i], selected ? Screen::themeAccent(kFlashlightAccent) : Screen::theme.text, bg,
                  i != int(FlashlightItem::Power) || powerAvailable);
    if (i == int(FlashlightItem::Color)) {
      if (flashlightColorIndex != kFlashlightRainbowIndex) {
        Screen::fill(275, y + 5, 24, 14, flashlightSwatch(color.rgb));
      } else {
        int x = 275;
        for (const auto &rgb : kFlashlightSpectrum) {
          Screen::fill(x, y + 5, 4, 14, flashlightSwatch(rgb));
          x += 4;
        }
      }
    }
  }
}

// Draw the flashlight app header, controls, and footer.
void drawFlashlight() {
  drawHeader("FLASHLIGHT", "UP/DOWN: SELECT   LEFT/RIGHT: ADJUST", Screen::themeAccent(kFlashlightAccent));
  drawFlashlightControls();
  Screen::text(12, 225, "A: CHANGE   START: POWER   B: HOME", Screen::theme.muted, Screen::theme.background);
}

// Enter the flashlight app, select its Power row, and update the RGB output.
void openFlashlight() {
  view = View::Flashlight;
  flashlightSelection = int(FlashlightItem::Power);
  drawFlashlight();
  applyFlashlightLed(millis(), false);
  Serial.println("\nFlashlight. W/S select, A/D adjust, I toggles power, K or M returns.");
}

// Save changed flashlight preferences and redisplay Home with the selected light state.
void leaveFlashlight() {
  if (flashlightSettingsChanged) {
    settingsStore.putUChar("lightcolor", flashlightColorIndex);
    settingsStore.putUChar("lightlevel", flashlightBrightness);
    settingsStore.putUChar("lightfx", uint8_t(flashlightEffect));
    settingsStore.putUChar("lightspeed", flashlightSpeed);
    flashlightSettingsChanged = false;
  }
  view = View::SystemMenu;
  drawSystemMenu(true);
  applyFlashlightLed(millis(), false);
}

// Handle flashlight focus, effects, and changes to power, color, brightness, and speed.
void handleFlashlight(uint16_t pressed) {
  if (pressed & (GB_PAD_B | kMenuKey)) {
    leaveFlashlight();
    return;
  }

  bool restartEffect = false;
  if (pressed & (GB_PAD_UP | GB_PAD_DOWN)) {
    const int step = pressed & GB_PAD_UP ? kFlashlightItemCount - 1 : 1;
    flashlightSelection = (flashlightSelection + step) % kFlashlightItemCount;
  } else if (pressed & (GB_PAD_A | GB_PAD_START | GB_PAD_LEFT | GB_PAD_RIGHT)) {
    const auto item = pressed & GB_PAD_START ? FlashlightItem::Power : FlashlightItem(flashlightSelection);
    const int direction = pressed & GB_PAD_LEFT ? -1 : 1;
    switch (item) {
      case FlashlightItem::Power:
        flashlightOn = !flashlightOn && flashlightPinAvailable();
        restartEffect = flashlightOn;
        break;
      case FlashlightItem::Color:
        flashlightColorIndex = (flashlightColorIndex + direction + kFlashlightColorCount) % kFlashlightColorCount;
        flashlightSettingsChanged = true;
        restartEffect = true;
        break;
      case FlashlightItem::Brightness:
        flashlightBrightness = std::clamp(int(flashlightBrightness) + direction * kFlashlightBrightnessStep,
                                          kFlashlightBrightnessStep, 100);
        flashlightSettingsChanged = true;
        break;
      case FlashlightItem::ColorFlash:
      case FlashlightItem::ColorBreathe: {
        const auto effect = item == FlashlightItem::ColorFlash ? FlashlightEffect::Flash : FlashlightEffect::Breathe;
        flashlightEffect = flashlightEffect == effect ? FlashlightEffect::None : effect;
        flashlightSettingsChanged = true;
        restartEffect = true;
        break;
      }
      case FlashlightItem::Speed:
        flashlightSpeed = (flashlightSpeed + direction + kFlashlightSpeedCount) % kFlashlightSpeedCount;
        flashlightSettingsChanged = true;
        break;
    }
  } else {
    return;
  }

  // Update the LED after drawing. On shared boards, the TFT just used its clock.
  drawFlashlightControls();
  const uint32_t now = millis();
  if (restartEffect) {
    flashlightEffectStartedAt = now;
  }
  applyFlashlightLed(now, false);
}

// Keep cover lookups idle while the browser is moving.
constexpr int kCoverX = 222;
constexpr int kCoverY = 75;
constexpr uint32_t kCoverIdleMs = 150;
bool gameCoverPending = false;
bool gameCoverVisible = false;
uint32_t gameCoverQueuedAt = 0;

bool musicKeepsCardBusy() {
  return musicPlayer.isOpen() && !musicPaused;
}

// Draw a note inside the current cover well without leaving text from an earlier result.
void drawGameCoverNote(const char *heading, const char *detail) {
  constexpr int center = kCoverX + GameCover::width / 2;
  Screen::fill(kCoverX, kCoverY, GameCover::width, GameCover::height, Screen::black);
  Screen::text(center - int(strlen(heading)) * 8, kCoverY + 39, heading, Screen::darkTheme.muted, Screen::black, 2);
  Screen::text(center - int(strlen(detail)) * 4, kCoverY + 67, detail, Screen::darkTheme.muted, Screen::black);
  gameCoverVisible = true;
}

// Publish a current result or enqueue a job without reading SD data on the input loop.
void serviceGameCover(bool controlsActive) {
  if (!isGameListView()) {
    gameCoverPending = false;
    GameCover::cancel();
    return;
  }
  if (musicKeepsCardBusy()) {
    // FAT directory lookups and BMP reads share the same card with the decoder.
    if (GameCover::status() != GameCover::Status::Idle) {
      GameCover::cancel();
      gameCoverPending = true;
    }
    if (!controlsActive && gameCoverPending && !gameCoverVisible &&
        millis() - gameCoverQueuedAt >= kCoverIdleMs) {
      drawGameCoverNote("MUSIC", "PLAYING");
    }
    return;
  }
  if (controlsActive) return;

  switch (GameCover::status()) {
    case GameCover::Status::Loading:
      return;
    case GameCover::Status::Ready:
      Screen::drawRgb565(GameCover::pixels(), kCoverX, kCoverY, GameCover::width, GameCover::height);
      gameCoverVisible = true;
      GameCover::cancel();
      return;
    case GameCover::Status::Missing:
      drawGameCoverNote("NO", "COVER");
      GameCover::cancel();
      return;
    case GameCover::Status::Idle:
      break;
  }
  if (!gameCoverPending || millis() - gameCoverQueuedAt < kCoverIdleMs) return;

  gameCoverPending = false;
  const auto &games = visibleGames();
  if (games.empty()) return;
  const String &romPath = games[visibleGameSelection()];
  const String directory = romPath.substring(0, romPath.lastIndexOf('/') + 1) + "imagesets/";
  if (!sdReady || !GameCover::start(directory + fileTitle(romPath) + ".bmp",
                                   directory + fileName(romPath) + ".bmp")) {
    drawGameCoverNote("NO", "COVER");
  }
}

// Render the current library page and schedule a selected-game cover once scrolling settles.
void drawGameList(bool listOnSerial, int previousSelection = -1) {
  const bool mixedLibrary = view != View::GameList;
  const uint16_t accent = mixedLibrary ? Screen::theme.accent : systemAccent(selectedSystem);
  const bool fullRedraw = previousSelection < 0;
  const char *title = view == View::Favorites ? "FAVORITES" :
                      view == View::RecentlyPlayed ? "RECENTLY PLAYED" : systemTitle(selectedSystem);
  if (fullRedraw) {
    drawHeader(title, "A: PLAY   MENU: GAME OPTIONS   B: HOME", accent);
  }
  const auto &games = visibleGames();
  const int count = int(games.size());
  int &selection = visibleGameSelection();
  if (selection >= count) {
    selection = 0;
  }
  const bool coverChanged = fullRedraw || selection != previousSelection || !count;
  if (coverChanged) {
    GameCover::cancel();
    gameCoverPending = count > 0;
    gameCoverQueuedAt = millis();
  }
  const int firstRow = (selection / kRowsPerPage) * kRowsPerPage;
  const int endRow = min(count, firstRow + kRowsPerPage);
  const bool pageChanged = fullRedraw || previousSelection / kRowsPerPage != selection / kRowsPerPage;
  const int drawEnd = fullRedraw ? endRow : firstRow + kRowsPerPage;
  for (int i = firstRow; i < drawEnd; ++i) {
    if (!pageChanged && i != selection && i != previousSelection) {
      continue;
    }
    const int y = 51 + (i - firstRow) * 20;
    if (i >= endRow) {
      Screen::fill(6, y, 205, 18, Screen::theme.background);
      continue;
    }
    const bool selected = i == selection;
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    const auto &path = games[i];
    drawCard(6, y, 205, 18, selected, mixedLibrary ? romAccent(path) : accent);
    if (isFavorite(path)) Screen::text(16, y + 5, "*", Screen::themeAccent(0xFEA8), bg);
    const String gameTitle = fileTitle(path, 22);
    Screen::text(29, y + 5, gameTitle.c_str(), selected ? Screen::theme.text : Screen::theme.muted, bg);
  }
  if (count && fullRedraw) {
    Screen::fill(217, 51, 97, 145, Screen::theme.panel);
    Screen::fill(217, 51, 2, 145, accent);
    Screen::text(226, 58, "COVER ART", Screen::theme.muted, Screen::theme.panel);
  }
  if (coverChanged) {
    // Clear once as scrolling starts. The blank preview remains while newer jobs are queued.
    if (count && (fullRedraw || gameCoverVisible)) {
      Screen::fill(kCoverX, kCoverY, GameCover::width, GameCover::height, Screen::black);
    }
    gameCoverVisible = false;
  }
  if (listOnSerial) {
    Serial.printf("\n%s. W/S select, J or I play, M game options, D stars, K returns, R rescans.\n", title);
    for (int i = firstRow; i < endRow; ++i) {
      Serial.printf("%c %s\n", i == selection ? '>' : ' ', fileName(games[i]));
    }
  }
  if (!sdReady) {
    showStatus("SD card not mounted - SELECT rescans");
  } else if (!count) {
    char empty[48];
    const char *hint = "COPY ROMS TO THE SD CARD";
    if (view == View::Favorites) {
      snprintf(empty, sizeof(empty), "NO FAVORITES YET");
      hint = "PRESS RIGHT ON A GAME TO STAR IT";
    } else if (view == View::RecentlyPlayed) {
      snprintf(empty, sizeof(empty), "NO RECENT GAMES");
      hint = "PLAY A GAME TO ADD IT HERE";
    } else {
      snprintf(empty, sizeof(empty), "NO %s GAMES FOUND", systemExtension(selectedSystem));
    }
    Screen::text(28, 126, empty, Screen::theme.muted, Screen::theme.background);
    Screen::text(28, 154, hint, Screen::theme.muted, Screen::theme.background);
  } else if (pageChanged || menuStatusVisible) {
    const int page = firstRow / kRowsPerPage + 1;
    const int pages = (count + kRowsPerPage - 1) / kRowsPerPage;
    char info[48];
    snprintf(info, sizeof(info), "%d GAMES   PAGE %d/%d   RIGHT: STAR", count, page, pages);
    if (!fullRedraw) {
      // The footer may contain a load or sleep status from the previous action.
      clearMenuFooter();
    }
    Screen::text(12, 224, info, Screen::theme.muted, Screen::theme.background);
  }
}

// Identify a Music folder below the root, where an Up One Folder row is available.
bool musicHasParent() {
  return musicFolder && musicFolder->path != kMusicRoot;
}

// Count folder rows above the first track in the visible Music list.
int musicTrackRowOffset() {
  return musicFolder ? int(musicFolder->folders.size()) + int(musicHasParent()) : 0;
}

// Return the number of selectable Music rows, or zero during an unfinished folder scan.
int musicEntryCount() {
  return musicFolder && !musicScanFolder ? musicTrackRowOffset() + int(musicFolder->tracks.size()) : 0;
}

// Paint the playback, volume, and current track or folder status panel.
void drawMusicStatus() {
  Screen::fill(6, 50, 308, 39, Screen::theme.panel);
  const bool playing = musicPlayer.isOpen();
  const char *state = !playing ? "LIBRARY" : musicPaused ? "PAUSED" : "PLAYING";
  Screen::text(18, 56, state, Screen::themeAccent(kMusicAccent), Screen::theme.panel);
  char volume[5];
  snprintf(volume, sizeof(volume), "%u%%", unsigned(volumePercent));
  Screen::text(204, 56, "VOL", Screen::theme.text, Screen::theme.panel);
  drawMenuValue(269, 56, volume, Screen::theme.text, Screen::theme.panel, true);
  if (musicFolderMessage) {
    Screen::text(18, 74, musicFolderMessage, Screen::theme.muted, Screen::theme.panel);
  } else if (playing) {
    drawMusicTitle(18, 74, 288, Screen::theme.text, Screen::theme.panel, false, false, true);
  } else {
    const char *hint = "No MP3 or WAV files here";
    if (musicFolder && !musicFolder->folders.empty()) {
      hint = "Open a folder or select a track";
    } else if (musicFolder && !musicFolder->tracks.empty()) {
      hint = "Select a track";
    }
    Screen::text(18, 74, musicMessage && musicFolder == musicPlaylist ? musicMessage : hint,
                 Screen::theme.muted, Screen::theme.panel);
  }
}

// Draw the current list page, changed rows, or the loading message while a scan runs.
void drawMusicEntries(int previousSelection = -1) {
  if (musicScanFolder) {
    Screen::fill(6, 93, 308, 136, Screen::theme.background);
    Screen::text(18, 106, "READING FOLDER...", Screen::theme.muted, Screen::theme.background);
    return;
  }
  constexpr int rows = 7;
  const int parentRows = int(musicHasParent());
  const int trackOffset = musicTrackRowOffset();
  const int count = musicEntryCount();
  const int first = (musicSelection / rows) * rows;
  const int end = std::min(count, first + rows);
  const bool redrawPage = previousSelection < 0 || previousSelection / rows != musicSelection / rows;
  for (int row = 0; row < rows; ++row) {
    const int index = first + row;
    if (!redrawPage && index != musicSelection && index != previousSelection) {
      continue;
    }
    const int y = 93 + row * 18;
    if (index >= end) {
      Screen::fill(6, y, 308, 16, Screen::theme.background);
      continue;
    }
    const bool selected = index == musicSelection;
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    const uint16_t fg = selected ? Screen::theme.text : Screen::theme.muted;
    drawCard(6, y, 308, 16, selected, Screen::themeAccent(kMusicAccent));
    if (index < parentRows) {
      Screen::text(18, y + 4, "[..]", Screen::themeAccent(kMusicAccent), bg);
      Screen::text(66, y + 4, "UP ONE FOLDER", fg, bg);
    } else if (index < trackOffset) {
      String label = musicFolder->folders[index - parentRows];
      if (label.length() > 30) {
        label.remove(28);
        label += "..";
      }
      Screen::text(18, y + 4, "[DIR]", Screen::themeAccent(kMusicAccent), bg);
      Screen::text(66, y + 4, label.c_str(), fg, bg);
    } else {
      const int track = index - trackOffset;
      const String title = fileTitle(musicFolder->tracks[track], 33);
      const bool current = musicFolder == musicPlaylist && track == musicTrack && musicPlayer.isOpen();
      Screen::text(18, y + 4, current ? ">" : " ", Screen::themeAccent(kMusicAccent), bg);
      Screen::text(30, y + 4, title.c_str(), fg, bg);
    }
  }

  if (!redrawPage) {
    return;
  }
  const int page = count ? first / rows + 1 : 0;
  const int pages = (count + rows - 1) / rows;
  const int songs = count - trackOffset;
  char info[48];
  snprintf(info, sizeof(info), "%d SONG%s     PAGE %d / %d", songs, songs == 1 ? "" : "S", page, pages);
  Screen::fill(6, 218, 308, 11, Screen::theme.background);
  Screen::text(12, 219, info, Screen::theme.muted, Screen::theme.background);
}

// Use the pending or completed folder path, truncated from the left to fit the header.
String musicFolderLabel() {
  String path = musicScanFolder ? musicScanFolder->path :
                musicFolder ? musicFolder->path : String(kMusicRoot);
  if (path.length() > 38) {
    path = String("..") + path.substring(path.length() - 36);
  }
  return path;
}

// Refresh the Music path, entry rows, and navigation hints below the title.
void drawMusicFolderContent() {
  const String path = musicFolderLabel();
  Screen::fill(13, 35, 307, 8, Screen::theme.header);
  Screen::text(13, 35, path.c_str(), Screen::theme.muted, Screen::theme.header);
  drawMusicEntries();
  Screen::fill(6, 230, 308, 8, Screen::theme.background);
  const char *controls = musicScanFolder ? "B: CANCEL" :
                         musicHasParent() ? "A: OPEN    B: UP      MENU: OPTIONS"
                                          : "A: OPEN    B: HOME    MENU: OPTIONS";
  Screen::text(12, 230, controls, Screen::theme.muted, Screen::theme.background);
}

// Render a complete Music screen, including the playback panel and folder content.
void drawMusicLibrary() {
  drawHeader("MUSIC", "", Screen::themeAccent(kMusicAccent));
  drawMusicStatus();
  drawMusicFolderContent();
}

// Center a fixed-width bitmap label in the supplied pixel span.
void drawCenteredMusicText(int x, int y, int width, const char *text,
                           uint16_t color, uint16_t background, int scale) {
  const int inset = (width - int(strlen(text)) * 8 * scale) / 2;
  Screen::text(x + inset, y, text, color, background, scale);
}

// Return the focused Now Playing control in the transport or options row.
int focusedMusicControl() {
  return musicOptionsFocused ? musicOptionsSelection : musicTransportSelection;
}

// Draw one Now Playing control with current focus, playback, and repeat state.
void drawMusicControl(int index) {
  struct ButtonLayout {
    int x, width;
    const char *label;
    const char *symbol;
  };
  constexpr ButtonLayout buttons[] = {
    {8, 54, "PREV", "|<<"}, {67, 54, "-10s", "<<"},
    {126, 68, "PLAY", ">"}, {199, 54, "+10s", ">>"}, {258, 54, "NEXT", ">>|"},
    {8, 80, "VOL -", nullptr}, {93, 80, "VOL +", nullptr}, {178, 134, "REPEAT OFF", nullptr},
  };
  static_assert(std::size(buttons) == int(MusicControl::Repeat) + 1);
  const auto &button = buttons[index];
  const bool focused = index == focusedMusicControl();
  const bool transport = index <= int(MusicControl::Next);
  const bool pause = index == int(MusicControl::PlayPause) && musicPlayer.isOpen() && !musicPaused;
  const bool repeat = index == int(MusicControl::Repeat) && repeatMusicTrack;
  const uint16_t bg = focused ? Screen::theme.selected : Screen::theme.panel;
  const uint16_t fg = focused ? Screen::theme.text : Screen::theme.muted;
  const int y = transport ? 111 : 180;
  const int height = transport ? 55 : 31;
  const char *label = pause ? "PAUSE" : repeat ? "REPEAT ON" : button.label;
  Screen::fill(button.x, y, button.width, height, bg);
  if (focused) {
    Screen::fill(button.x, y + height - 3, button.width, 3, Screen::themeAccent(kMusicAccent));
  }
  if (transport) {
    drawCenteredMusicText(button.x, y + 9, button.width, pause ? "||" : button.symbol,
                          focused ? Screen::themeAccent(kMusicAccent) : fg, bg, 2);
  }
  drawCenteredMusicText(button.x, y + (transport ? 35 : 11), button.width, label,
                        repeat ? Screen::themeAccent(kMusicAccent) : fg, bg, 1);
}

// Return the playback clock in whole seconds, bounded by duration when known.
uint64_t musicElapsedSeconds() {
  const uint64_t elapsed = musicPlayer.positionMs();
  const uint64_t duration = musicPlayer.durationMs();
  return (duration ? std::min(elapsed, duration) : elapsed) / 1000;
}

// Update track position, duration, and seeking indicators on the playback screen.
void drawNowPlayingStatus() {
  musicDisplayedSecond = musicElapsedSeconds();
  musicDisplayedDuration = musicPlayer.durationMs();
  musicWasSeeking = musicPlayer.isSeeking();
  Screen::fill(8, 78, 304, 28, Screen::theme.panel);
  if (musicPlayer.isOpen()) {
    const char *state = musicWasSeeking ? "SEEKING" : musicPaused ? "PAUSED" : "PLAYING";
    char elapsed[16];
    snprintf(elapsed, sizeof(elapsed), "%lu:%02u",
             static_cast<unsigned long>(musicDisplayedSecond / 60), unsigned(musicDisplayedSecond % 60));
    char timeline[32];
    if (musicDisplayedDuration) {
      const uint64_t total = musicDisplayedDuration / 1000;
      snprintf(timeline, sizeof(timeline), "%s / %lu:%02u", elapsed,
               static_cast<unsigned long>(total / 60), unsigned(total % 60));
    } else {
      snprintf(timeline, sizeof(timeline), "%s / --:--", elapsed);
    }
    Screen::text(18, 82, state, Screen::themeAccent(kMusicAccent), Screen::theme.panel);
    drawCenteredMusicText(8, 94, 304, timeline, Screen::theme.text, Screen::theme.panel, 1);
  } else {
    Screen::text(18, 82, musicMessage ? musicMessage : "STOPPED", Screen::theme.muted, Screen::theme.panel);
  }
  char volume[12];
  snprintf(volume, sizeof(volume), "VOL %u%%", unsigned(volumePercent));
  Screen::text(232, 82, volume, Screen::theme.text, Screen::theme.panel);
}

// Refresh the stereo peak meters using the latest shared audio levels at a bounded rate.
void drawMusicVisualizer(bool force) {
  const uint32_t now = millis();
  if (!force && (!musicVisualizerEnabled || now - musicVisualizerUpdatedAt < 90)) return;
  musicVisualizerUpdatedAt = now;
  const uint16_t levels = musicVisualizerEnabled && musicPlayer.isOpen() && !musicPaused
                              ? musicAudioLevels.load(std::memory_order_relaxed) : 0;
  if (!force && levels == displayedMusicLevels) return;
  displayedMusicLevels = levels;
  constexpr int barWidth = 147;
  for (int channel = 0; channel < 2; ++channel) {
    const int x = 8 + channel * 157;
    const int lit = ((levels >> (channel * 8)) & 255) * barWidth / 255;
    Screen::fill(x, 171, lit, 5, Screen::themeAccent(kMusicAccent));
    Screen::fill(x + lit, 171, barWidth - lit, 5, musicVisualizerEnabled ? Screen::theme.panel : Screen::theme.background);
  }
}

// Build the Now Playing screen with track number, progress, meters, and controls.
void drawNowPlaying() {
  drawHeader("NOW PLAYING", "D-PAD: SELECT     A: USE", Screen::themeAccent(kMusicAccent));
  char trackNumber[16];
  snprintf(trackNumber, sizeof(trackNumber), "%d / %d", musicTrack + 1, int(musicPlaylist->tracks.size()));
  const int numberX = 312 - int(strlen(trackNumber)) * 8;
  nowPlayingTitleWidth = numberX - 20;
  drawMusicTitle(12, 58, nowPlayingTitleWidth, Screen::theme.text, Screen::theme.background, false, true, true);
  Screen::text(numberX, 58, trackNumber, Screen::themeAccent(kMusicAccent), Screen::theme.background);
  drawNowPlayingStatus();
  drawMusicVisualizer(true);
  for (int i = 0; i <= int(MusicControl::Repeat); ++i) {
    drawMusicControl(i);
  }
  Screen::text(12, 218, "MENU: OPTIONS    B: LIBRARY", Screen::theme.muted, Screen::theme.background);
  Screen::text(12, 230, "SELECT: RESTART    START: PLAY/PAUSE", Screen::theme.muted, Screen::theme.background);
}

// Paint a contiguous range of Music options for complete or incremental redraws.
void drawMusicSettingsRows(int first, int count) {
  char volume[8];
  snprintf(volume, sizeof(volume), "%u%%", unsigned(volumePercent));
  const char *const timeouts[] = {"OFF", "30 SEC", "1 MIN", "2 MIN"};
  const char *const orders[] = {"IN ORDER", "SHUFFLE", "STOP AFTER TRACK"};
  const char *const sleeps[] = {"OFF", "15 MIN", "30 MIN", "60 MIN"};
  const char *rootAction = backgroundMusic ? "HIDE MUSIC" : "CLOSE OPTIONS";
  struct Row {
    const char *label;
    const char *value;
  };
  const Row rows[] = {
    {"BACKGROUND PLAYBACK", backgroundMusic ? "ON" : "OFF"},
    {"SCREEN OFF", nullptr},
    {"AUTO SCREEN OFF", timeouts[musicScreenTimeout]},
    {"VOLUME", volume},
    {"REPEAT TRACK", repeatMusicTrack ? "ON" : "OFF"},
    {"PLAYBACK ORDER", orders[int(musicOrder)]},
    {"VISUALIZER", musicVisualizerEnabled ? "ON" : "OFF"},
    {"MUSIC RGB", musicReactiveRgb ? "ON" : "OFF"},
    {"SLEEP TIMER", sleeps[musicSleepOption]},
    {"STOP PLAYBACK", nullptr},
    {"RESCAN LIBRARY", nullptr},
    {musicHasParent() ? "UP ONE FOLDER" : rootAction, nullptr},
    {"EXIT MUSIC", nullptr},
  };
  static_assert(std::size(rows) == kMusicSettingCount);
  static_assert(std::size(timeouts) == std::size(kMusicScreenTimeouts));
  static_assert(std::size(sleeps) == std::size(kMusicSleepDelays));
  for (int i = first; i < first + count; ++i) {
    const int y = 50 + i * 13;
    const bool selected = i == musicSettingsSelection;
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    drawCard(6, y, 308, 12, selected, Screen::themeAccent(kMusicAccent));
    Screen::text(16, y + 2, rows[i].label, selected ? Screen::theme.text : Screen::theme.muted, bg);
    if (rows[i].value) {
      drawMenuValue(kMenuValueCenter, y + 2, rows[i].value,
                    selected ? Screen::themeAccent(kMusicAccent) : Screen::theme.muted, bg, true);
    }
  }
}

// Draw the Music options page and its current playback and display preferences.
void drawMusicSettings() {
  drawHeader("MUSIC OPTIONS", "A: SELECT    LEFT/RIGHT: CHANGE", Screen::themeAccent(kMusicAccent));
  drawMusicSettingsRows(0, kMusicSettingCount);
  Screen::text(12, 230, "B / MENU: BACK", Screen::theme.muted, Screen::theme.background);
}

// Close the directory handle and discard the unfinished index and pending highlight.
void cancelMusicFolderScan() {
  musicScanDirectory.close();
  musicScanFolder.reset();
  musicScanHighlight = "";
}

// Publish a completed folder index and optionally restore the cursor to its child row.
void selectMusicFolder(const std::shared_ptr<const MusicFolder> &folder, const String &highlight) {
  musicFolder = folder;
  musicFolderMessage = nullptr;
  musicSelection = 0;
  if (!highlight.isEmpty()) {
    const auto &folders = folder->folders;
    const auto previous = std::find(folders.begin(), folders.end(), highlight);
    if (previous != folders.end()) {
      musicSelection = int(musicHasParent()) + int(previous - folders.begin());
    }
  }
}

// Reuse the playback queue's folder index or begin a directory scan for a different path.
bool browseMusicFolder(const String &path, const String &highlight) {
  cancelMusicFolderScan();
  if (!sdReady) return false;
  if (musicPlaylist && musicPlaylist->path == path) {
    selectMusicFolder(musicPlaylist, highlight);
    return true;
  }

  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) return false;
  musicScanDirectory = dir;
  musicScanFolder = std::make_shared<MusicFolder>();
  musicScanFolder->path = path;
  musicScanHighlight = highlight;
  musicFolderMessage = nullptr;
  return true;
}

void startBackgroundMusicPump(bool temporary);
void stopBackgroundMusicPump();

// Keep the audio task feeding I2S during a full or partial Music redraw, then join it.
void redrawMusicFolder(bool fullRedraw) {
  // At low TFT clocks a page redraw can take longer than the I2S DMA queue.
  // Keep the existing audio task running only for the duration of the draw.
  startBackgroundMusicPump(true);
  if (fullRedraw) {
    drawMusicLibrary();
  } else {
    drawMusicStatus();
    drawMusicFolderContent();
  }
  stopBackgroundMusicPump();
}

// Sort the pending index, publish it, and repaint the screen to clear the loading message.
void finishMusicFolderScan() {
  const auto byName = [](const String &a, const String &b) {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
  };
  auto &folder = *musicScanFolder;
  std::sort(folder.folders.begin(), folder.folders.end(), byName);
  std::sort(folder.tracks.begin(), folder.tracks.end(), byName);
  selectMusicFolder(musicScanFolder, musicScanHighlight);
  cancelMusicFolderScan();
  redrawMusicFolder(true);
}

// Read a bounded batch of filenames so the main loop can service audio between batches.
void serviceMusicFolderScan() {
  if (view != View::Music || !musicScanFolder) return;

  // Return to the audio loop between small batches, even in a very large folder.
  constexpr size_t kMaxEntriesPerType = 512;
  constexpr int kEntriesPerBatch = 12;
  const int64_t deadline = esp_timer_get_time() + 1500;
  auto &folder = *musicScanFolder;
  for (int i = 0; i < kEntriesPerBatch; ++i) {
    bool isDirectory = false;
    const String path = musicScanDirectory.getNextFileName(&isDirectory);
    if (path.isEmpty()) {
      finishMusicFolderScan();
      return;
    }
    const char *name = fileName(path);
    if (*name && *name != '.') {
      if (isDirectory && folder.folders.size() < kMaxEntriesPerType) {
        folder.folders.emplace_back(name);
      } else if (!isDirectory && folder.tracks.size() < kMaxEntriesPerType) {
        const char *ext = strrchr(name, '.');
        if (ext && (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0)) {
          folder.tracks.emplace_back(name);
        }
      }
    }
    if (folder.folders.size() == kMaxEntriesPerType && folder.tracks.size() == kMaxEntriesPerType) {
      finishMusicFolderScan();
      return;
    }
    if (esp_timer_get_time() >= deadline) return;
  }
}

// Open the requested folder or fall back to the Music root and show an error if both fail.
void loadMusicLibrary(const String &path) {
  if (browseMusicFolder(path, {}) || (path != kMusicRoot && browseMusicFolder(kMusicRoot, {}))) {
    return;
  }
  musicFolder.reset();
  musicSelection = 0;
  musicFolderMessage = sdReady ? "Copy MP3 or WAV to /music" : "SD card not mounted";
}

// Navigate into a folder or up to its parent, remembering the previous child selection.
void openMusicFolderEntry(int index, bool fullRedraw = false) {
  const int parentRows = int(musicHasParent());
  bool opened;
  if (index < parentRows) {
    const String child = fileName(musicFolder->path);
    const String parent = musicFolder->path.substring(0, musicFolder->path.lastIndexOf('/'));
    opened = browseMusicFolder(parent, child);
  } else {
    const String path = musicFolder->path + "/" + musicFolder->folders[index - parentRows];
    opened = browseMusicFolder(path, {});
  }
  if (!opened) musicFolderMessage = "Could not open folder";
  redrawMusicFolder(fullRedraw);
}

// Return the browser to the playing queue and highlight its current track.
void selectMusicPlaylistInLibrary() {
  cancelMusicFolderScan();
  musicFolder = musicPlaylist;
  musicFolderMessage = nullptr;
  musicSelection = musicTrackRowOffset() + musicTrack;
}

// Enter Music at the playing queue or the root of the library, then resume main-loop audio servicing.
void openMusic() {
  if (musicPlayer.isOpen()) {
    selectMusicPlaylistInLibrary();
  } else {
    loadMusicLibrary(kMusicRoot);
  }
  musicLastInputAt = millis();
  view = View::Music;
  drawMusicLibrary();
  Serial.println("\nMusic. W/S browses, J opens a folder or track, A/D adjusts volume, M opens Music Options, K goes up a folder or Home, R rescans.");
  stopBackgroundMusicPump();
}

constexpr int settingsEmulatorIndex(SettingsItem item) {
  const int index = int(item) - kFirstEmulatorItem;
  return index >= 0 && index < int(kSystemCount) ? index : -1;
}

// Map a settings row ID to its visible label.
const char *settingsLabel(SettingsItem item) {
  switch (item) {
    case SettingsItem::DisplayMenu: return "DISPLAY";
    case SettingsItem::SoundMenu: return "SOUND";
    case SettingsItem::EmulationMenu: return "EMULATION";
    case SettingsItem::PowerMenu: return "POWER";
    case SettingsItem::ConnectivityMenu: return "CONNECTIVITY";
    case SettingsItem::WifiMenu: return "WI-FI";
    case SettingsItem::GameBoyMenu:
    case SettingsItem::GameBoyColorMenu:
    case SettingsItem::GameBoyAdvanceMenu:
    case SettingsItem::NesMenu:
    case SettingsItem::MasterSystemMenu:
    case SettingsItem::GameGearMenu:
    case SettingsItem::GenesisMenu:
      return kSystems[settingsEmulatorIndex(item)].title;
    case SettingsItem::Sound: return "GAME SOUND";
    case SettingsItem::Volume: return "VOLUME";
    case SettingsItem::Brightness: return "DISPLAY BRIGHTNESS";
    case SettingsItem::HomeStyle: return "HOME STYLE";
    case SettingsItem::Theme: return "THEME";
    case SettingsItem::EmulatorDisplay:
    case SettingsItem::GbaDisplay: return "DISPLAY SIZE";
    case SettingsItem::GbaFrameskip:
    case SettingsItem::GenesisFrameskip: return "FRAMESKIP";
    case SettingsItem::GbaTransfer: return "TRANSFER";
    case SettingsItem::TftClock: return "TFT SPEED";
    case SettingsItem::Sleep: return "SLEEP CONSOLE";
    case SettingsItem::AutoSleep: return "AUTO SLEEP";
    case SettingsItem::DeviceTools: return "DEVICE TOOLS";
    case SettingsItem::Bluetooth: return "BLUETOOTH";
    case SettingsItem::WifiName: return "NETWORK NAME";
    case SettingsItem::WifiPassword: return "PASSWORD";
    case SettingsItem::EnableOnRestart: return "ENABLE ON RESTART";
    case SettingsItem::RestartConsole: return "RESTART CONSOLE";
    case SettingsItem::About: return "ABOUT";
    case SettingsItem::Back: return "BACK";
  }
  return "";
}

constexpr int kSettingsRowsY = 52;
constexpr int kSettingsSubmenuRowPitch = 23;
constexpr int kSettingsSubmenuRowHeight = 20;

uint16_t settingsAccent() {
  if (settingsPage >= SettingsPage::GameBoy) {
    return systemAccent(static_cast<System>(int(settingsPage) - kFirstEmulatorPage));
  }
  return settingsReturnView == View::Paused ? systemAccent(selectedSystem) : Screen::theme.accent;
}

// Wi-Fi and Bluetooth both show the startup toggle and restart action at all times.
void drawConnectivityStartupRow(SettingsItem item, int y, bool selected,
                                bool pending, bool active = false, const char *inactiveValue = "OFF") {
  const uint16_t accent = settingsAccent();
  const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
  const uint16_t fg = selected ? Screen::theme.text : Screen::theme.muted;
  const uint16_t valueColor = selected ? accent : Screen::theme.text;
  const int textY = y + 6;
  drawCard(6, y, 308, kSettingsSubmenuRowHeight, selected, accent);
  if (item == SettingsItem::EnableOnRestart) {
    Screen::text(19, textY, settingsLabel(active ? SettingsItem::Bluetooth : item), fg, bg);
    drawMenuValue(kMenuValueCenter, textY, pending ? "PENDING" : active ? "ON" : inactiveValue,
                  valueColor, bg, true);
  } else {
    Screen::text(19, textY, settingsLabel(item), fg, bg);
    if (pending) Screen::text(292, textY, ">", valueColor, bg);
  }
}

void drawWifiEditor();

// Draw the current settings page or repaint just the rows touched by an input.
void drawSettings(bool listOnSerial = true, int previousSelection = -1) {
  if (wifiField != WifiField::None) {
    drawWifiEditor();
    return;
  }
  const auto &menu = kSettingsMenus[int(settingsPage)];
  const bool compactRows = settingsPage == SettingsPage::Root || settingsPage == SettingsPage::Emulation;
  const uint16_t accent = settingsAccent();
  const bool fullRedraw = previousSelection < 0;
  if (fullRedraw) {
    drawHeader(menu.title, compactRows ? "UP/DOWN: SELECT   A: OPEN" : "LEFT/RIGHT: CHANGE   A: SELECT",
               accent);
  }
  const int rowPitch = compactRows ? 20 : kSettingsSubmenuRowPitch;
  const int rowHeight = compactRows ? 18 : kSettingsSubmenuRowHeight;
  static_assert([] {
    for (const auto &page : kSettingsMenus) {
      const bool compact = page.items == kRootSettings || page.items == kEmulationSettings;
      if (kSettingsRowsY + (page.count - 1) * (compact ? 20 : kSettingsSubmenuRowPitch) +
          (compact ? 18 : kSettingsSubmenuRowHeight) > 219) return false;
    }
    return true;
  }());
  for (int i = 0; i < menu.count; ++i) {
    if (!fullRedraw && i != previousSelection && i != settingsSelection) {
      continue;
    }
    const SettingsItem item = menu.items[i];
    const bool selected = i == settingsSelection;
    const int y = kSettingsRowsY + i * rowPitch;
    if (item == SettingsItem::EnableOnRestart || item == SettingsItem::RestartConsole) {
      drawConnectivityStartupRow(item, y, selected, WifiTransfer::scheduled());
      continue;
    }
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    const uint16_t fg = selected ? Screen::theme.text : Screen::theme.muted;
    const uint16_t valueColor = selected ? accent : Screen::theme.text;
    const int textY = y + 6;
    drawCard(6, y, 308, rowHeight, selected, accent);
    Screen::text(19, textY, settingsLabel(item), fg, bg);
    char value[18] = "";
    switch (item) {
      case SettingsItem::Sound:
        snprintf(value, sizeof(value), "%s", soundEnabled ? "ON" : "OFF");
        break;
      case SettingsItem::Volume:
        snprintf(value, sizeof(value), "%u%%", unsigned(volumePercent));
        break;
      case SettingsItem::Brightness:
        if (kTftBacklight >= 0) {
          snprintf(value, sizeof(value), "%u%%", unsigned(displayBrightness));
        } else {
          snprintf(value, sizeof(value), "NO BL PIN");
        }
        break;
      case SettingsItem::Theme:
        snprintf(value, sizeof(value), "%s", Screen::lightTheme() ? "LIGHT" : "DARK");
        break;
      case SettingsItem::HomeStyle:
        snprintf(value, sizeof(value), "%s", kHomeStyleNames[unsigned(homeStyle)]);
        break;
      case SettingsItem::EmulatorDisplay:
        snprintf(value, sizeof(value), "%s", gbScaleName(emulatorScales[int(settingsPage) - kFirstEmulatorPage]));
        break;
      case SettingsItem::GbaDisplay:
        snprintf(value, sizeof(value), "%s", gbaScaleName(gbaFullscreen));
        break;
      case SettingsItem::GbaFrameskip:
        snprintf(value, sizeof(value), "%s", frameskipName(gbaFrameskipMode));
        break;
      case SettingsItem::GbaTransfer:
        snprintf(value, sizeof(value), "%s", gbaDmaEnabled ? "DMA" : "ARDUINO SPI");
        break;
      case SettingsItem::GenesisFrameskip:
        snprintf(value, sizeof(value), "%s", frameskipName(genesisFrameskipMode));
        break;
      case SettingsItem::TftClock:
        snprintf(value, sizeof(value), "%lu MHz",
                 static_cast<unsigned long>(kTftClockOptions[tftClockOption] / 1000000));
        break;
      case SettingsItem::Sleep: {
        constexpr int hintX = Screen::width - 10 - (sizeof(kSleepWakeHint) - 1) * 8;
        Screen::text(hintX, textY, kSleepWakeHint, fg, bg);
        continue;
      }
      case SettingsItem::AutoSleep:
        snprintf(value, sizeof(value), "%s", kAutoSleepOptions[autoSleepOption].label);
        break;
      case SettingsItem::WifiName:
        snprintf(value, sizeof(value), "%.14s", WifiTransfer::networkName());
        Screen::text(174, textY, value, valueColor, bg);
        Screen::text(292, textY, ">", valueColor, bg);
        continue;
      case SettingsItem::About:
      case SettingsItem::DisplayMenu:
      case SettingsItem::SoundMenu:
      case SettingsItem::EmulationMenu:
      case SettingsItem::GameBoyMenu:
      case SettingsItem::GameBoyColorMenu:
      case SettingsItem::GameBoyAdvanceMenu:
      case SettingsItem::NesMenu:
      case SettingsItem::MasterSystemMenu:
      case SettingsItem::GameGearMenu:
      case SettingsItem::GenesisMenu:
      case SettingsItem::PowerMenu:
      case SettingsItem::ConnectivityMenu:
      case SettingsItem::WifiMenu:
      case SettingsItem::DeviceTools:
      case SettingsItem::Bluetooth:
      case SettingsItem::WifiPassword:
        Screen::text(292, textY, ">", valueColor, bg);
        continue;
      case SettingsItem::EnableOnRestart:
      case SettingsItem::RestartConsole:
        continue;
      case SettingsItem::Back:
        Screen::text(292, textY, "<", valueColor, bg);
        continue;
    }
    const bool adjustable = item != SettingsItem::Brightness || kTftBacklight >= 0;
    drawMenuValue(kMenuValueCenter, textY, value, valueColor, bg, adjustable);
  }
  if (fullRedraw || menuStatusVisible) {
    if (!fullRedraw) clearMenuFooter();
    const bool navigation = compactRows || settingsPage == SettingsPage::Connectivity || settingsPage == SettingsPage::Wifi;
    Screen::text(12, 224, navigation ? "B / MENU: BACK" : "B / MENU: SETTINGS   AUTO-SAVED",
                 Screen::theme.muted, Screen::theme.background);
  }
  if (listOnSerial) {
    Serial.printf("\n%s. W/S select; J opens or changes; A/D adjust; K or M returns.\n", menu.title);
    for (int i = 0; i < menu.count; ++i) {
      Serial.printf("%c %s\n", i == settingsSelection ? '>' : ' ', settingsLabel(menu.items[i]));
    }
  }
}

struct AboutPage {
  const char *heading;
  const char *lines[7];
};

constexpr AboutPage kAboutPages[] = {
  {"Build version", {}},
  {"Device status", {}},
  {"Network addresses", {}},
  {"Library status", {}},
  {"Emulator cores", {
    "GB and GBC: Gnuboy",
    "GBA: " GPSP_NAME " " GPSP_VERSION,
    "NES: Nofrendo 3.0",
    "Master System and GG: SMS Plus",
    "Genesis and Mega Drive: mdcore",
    "Doom engine: ESP-DOOM",
    "Open GBA BIOS from ReGBA",
  }},
  {"Games and saves", {
    "Libraries by system on microSD",
    "Three save state slots per game",
    "Cartridge saves beside each ROM",
    "Continue from the last used slot",
    "Favorites and recently played",
    "Per-game display and sound",
    "MENU: PAUSE   SELECT: RESCAN",
  }},
  {"Music and tools", {
    "MP3 and 16-bit PCM WAV playback",
    "Repeat, shuffle and playlists",
    "Background play and sleep timer",
    "Photos, clock and countdown",
    "RGB flashlight and mini games",
    "Hardware tests and diagnostic log",
    "Installable Lua apps on microSD",
  }},
  {"Display and power", {
    "320 x 240 ILI9341 display",
    "Per-system game display sizes",
    "Light and dark menu themes",
    "Adjustable TFT and backlight",
    "Battery estimate in menus",
    "Sleep timer in Power settings",
    "Hold START + SELECT to wake",
  }},
  {kConsoleName, {
    "An open-source DIY handheld",
    "Built for ESP32-S3 and Arduino",
    "",
    "Classic games from microSD",
    "Music, photos and a clock",
    "Wi-Fi file transfer and updates",
    "Bluetooth LE controller support",
  }},
  {kConsoleDeveloper, {
    "Creator of Pocket Play",
    "Firmware and handheld integration",
    "",
    "Mobile, IoT and embedded software",
    "",
    "GitHub profile",
    kDeveloperGitHub,
  }},
  {"Project credits", {
    "Gnuboy: Laguna and Gilgamesh",
    "gpSP: Exophase, notaz and libretro",
    "Retro-Go: Alex Duchesne",
    "mdcore: Andrew Nakas",
    "Doom: id Software and port authors",
    "Wiring reference: AshrafHanyy",
    "Full notices in the repository",
  }},
};
constexpr int kAboutPageCount = std::size(kAboutPages);
static_assert(kAboutPageCount == int(AboutSection::Credits) + 1);
static_assert([] {
  for (const auto &page : kAboutPages) {
    if (std::string_view(page.heading).size() > 17) return false;
    for (const char *line : page.lines) {
      if (line && std::string_view(line).size() > 34) return false;
    }
  }
  return true;
}());

// Render a line of About text and mirror it to serial.
void drawAboutLine(int row, const char *text, int spacing = 15) {
  const bool link = text == kDeveloperGitHub;
  Screen::text(20, 103 + row * spacing, text, link ? Screen::theme.accent : Screen::theme.text, Screen::theme.panel);
  if (link) {
    Serial.print("https://");
  }
  Serial.println(text);
}

// Read local MAC addresses without starting either radio.
void drawAboutNetworkAddresses() {
  constexpr struct { const char *label; esp_mac_type_t type; } interfaces[] = {
    {"Factory MAC", ESP_MAC_EFUSE_FACTORY},
    {"Wi-Fi client", ESP_MAC_WIFI_STA},
    {"Wi-Fi hotspot", ESP_MAC_WIFI_SOFTAP},
    {"Bluetooth LE", ESP_MAC_BT},
  };
  for (int row = 0; row < int(std::size(interfaces)); ++row) {
    const auto &interface = interfaces[row];
    uint8_t mac[6];
    char line[36];
    if (esp_read_mac(mac, interface.type) == ESP_OK) {
      snprintf(line, sizeof(line), "%s: " MACSTR, interface.label, MAC2STR(mac));
    } else {
      snprintf(line, sizeof(line), "%s: Unavailable", interface.label);
    }
    Screen::text(20, 103 + row * 22, line, Screen::theme.text, Screen::theme.panel);
  }
}

// Populate live device, library, or build information for the selected About page.
void drawAboutStats(AboutSection section) {
  char lines[kSystemCount + 1][36] = {};
  if (section == AboutSection::Device) {
    snprintf(lines[0], sizeof(lines[0]), "%s  |  %u cores", ESP.getChipModel(), unsigned(ESP.getChipCores()));
    const unsigned revision = ESP.getChipRevision();
    snprintf(lines[1], sizeof(lines[1]), "CPU: %u MHz  |  Chip rev %u.%u",
             unsigned(ESP.getCpuFreqMHz()), revision / 100, revision % 100);
    snprintf(lines[2], sizeof(lines[2]), "Flash: %u MiB", unsigned(ESP.getFlashChipSize() / 1048576));
    snprintf(lines[3], sizeof(lines[3]), "PSRAM: %u MiB", unsigned(ESP.getPsramSize() / 1048576));
    snprintf(lines[4], sizeof(lines[4]), "Free internal RAM: %u KiB",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
    snprintf(lines[5], sizeof(lines[5]), "Free PSRAM: %u KiB", unsigned(ESP.getFreePsram() / 1024));
    snprintf(lines[6], sizeof(lines[6]), "TFT: %d x %d ILI9341", Screen::width, Screen::height);
  } else if (section == AboutSection::Build) {
    snprintf(lines[0], sizeof(lines[0]), "Firmware: %s", kConsoleVersion);
    snprintf(lines[1], sizeof(lines[1]), "Build ID: %.24s", POCKETPLAY_BUILD_ID);
    snprintf(lines[2], sizeof(lines[2]), "Arduino-ESP32 %s", ESP_ARDUINO_VERSION_STR);
    snprintf(lines[3], sizeof(lines[3]), "ESP-IDF %.26s", ESP.getSdkVersion());
  } else {
    unsigned total = 0;
    for (size_t i = 0; i < kSystemCount; ++i) {
      const unsigned count = unsigned(gamesBySystem[i].size());
      snprintf(lines[i + 1], sizeof(lines[i + 1]), "%s: %u", kSystems[i].title, count);
      total += count;
    }
    snprintf(lines[0], sizeof(lines[0]), "microSD: %s  |  %u ROMs",
             sdReady ? "Mounted" : "Off", total);
  }
  const bool library = section == AboutSection::Library;
  for (int row = 0; row < (library ? int(std::size(lines)) : 7); ++row) {
    if (*lines[row]) drawAboutLine(row, lines[row], library ? 12 : 15);
  }
}

// Draw the selected page of About Pocket Play and its serial summary.
void drawAbout(bool firstOpen) {
  if (firstOpen) {
    char subtitle[40];
    snprintf(subtitle, sizeof(subtitle), "%s  /  Version %s", kConsoleName, kConsoleVersion);
    drawHeader("ABOUT", subtitle, Screen::theme.accent);
  }
  const AboutPage &page = kAboutPages[aboutSelection];
  drawCard(8, 53, 304, 158, false, Screen::theme.accent);
  Screen::text(20, 64, page.heading, Screen::theme.text, Screen::theme.panel, 2);
  Screen::fill(20, 90, 276, 1, Screen::theme.highlight);
  Serial.printf("\nAbout %s %s [%d/%d] - %s\n", kConsoleName, kConsoleVersion,
                aboutSelection + 1, kAboutPageCount, page.heading);

  const auto section = static_cast<AboutSection>(aboutSelection);
  if (section == AboutSection::Network) {
    drawAboutNetworkAddresses();
  } else if (section == AboutSection::Device || section == AboutSection::Library || section == AboutSection::Build) {
    drawAboutStats(section);
  } else {
    for (int row = 0; row < int(std::size(page.lines)); ++row) {
      if (page.lines[row] && *page.lines[row]) {
        drawAboutLine(row, page.lines[row]);
      }
    }
  }

  char pageNumber[8];
  snprintf(pageNumber, sizeof(pageNumber), "%d/%d", aboutSelection + 1, kAboutPageCount);
  Screen::text(12, 224, "D-PAD: PAGES   B / MENU: BACK", Screen::theme.muted, Screen::theme.background);
  Screen::text(308 - int(strlen(pageNumber)) * 8, 224, pageNumber, Screen::theme.accent, Screen::theme.background);
}

// Present device information, diagnostics, app installation and firmware updates.
void drawAboutMenu() {
  drawHeader("ABOUT", "A: OPEN   B / MENU: SETTINGS", Screen::theme.accent);
  constexpr const char *labels[] = {"ABOUT POCKET PLAY", "DIAGNOSTIC LOG", "INSTALL APPLICATION", "UPDATE SOFTWARE"};
  constexpr const char *details[] = {"DEVICE, SOFTWARE AND CREDITS", "RECORD OR REVIEW DEVICE ACTIVITY",
                                     "INSTALL A MICROSD APP PACKAGE", "INSTALL FIRMWARE FROM MICROSD"};
  static_assert(std::size(labels) == kAboutMenuRows && std::size(details) == kAboutMenuRows);
  for (int i = 0; i < kAboutMenuRows; ++i) {
    const bool selected = i == aboutMenuSelection;
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    const int y = 51 + i * 42;
    drawCard(8, y, 304, 38, selected, Screen::theme.accent);
    Screen::text(18, y + 6, labels[i], Screen::theme.text, bg);
    Screen::text(18, y + 22, details[i], Screen::theme.muted, bg);
  }
}

// Open the About chooser with the information page selected.
void openAbout() {
  aboutMenuSelection = 0;
  view = View::AboutMenu;
  drawAboutMenu();
}

// Index firmware images from the directory used by browser uploads and the card root.
void scanUpdateImages(const char *directory) {
  File folder = SD.open(directory, FILE_READ);
  if (!folder || !folder.isDirectory()) return;
  unsigned scanned = 0;
  for (File entry = folder.openNextFile(); entry; entry = folder.openNextFile()) {
    if (!entry.isDirectory() && FirmwareUpdate::supportedName(entry.name())) {
      updateImages.emplace_back(entry.path());
    }
    if (++scanned % 32 == 0) delay(1);
  }
}

void drawUpdates(bool full = true) {
  const auto accent = Screen::theme.accent;
  if (updateConfirm) {
    drawHeader("INSTALL UPDATE?", "A: INSTALL   B / MENU: CANCEL", accent);
    drawCard(8, 58, 304, 90, false, accent);
    Screen::text(18, 74, fileTitle(updateImages[updateSelection], 34).c_str(), Screen::theme.text, Screen::theme.panel);
    Screen::text(18, 98, "Keep the console powered.", Screen::theme.muted, Screen::theme.panel);
    Screen::text(18, 122, "Pocket Play restarts after install.", Screen::theme.muted, Screen::theme.panel);
    return;
  }
  if (full) drawHeader("UPDATE", "A: SELECT   B / MENU: BACK", accent);
  if (updateImages.empty()) {
    drawCard(8, 58, 304, 66, false, accent);
    Screen::text(18, 72, sdReady ? "No firmware images found." : "Insert a microSD card.", Screen::theme.text, Screen::theme.panel);
    Screen::text(18, 98, "Place .ino.bin in /updates or root.", Screen::theme.muted, Screen::theme.panel);
    return;
  }
  for (int row = 0; row < kVisibleUpdateRows && updateFirst + row < int(updateImages.size()); ++row) {
    const int index = updateFirst + row;
    const bool selected = index == updateSelection;
    const auto &path = updateImages[index];
    const int y = 51 + row * 33;
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    drawCard(8, y, 304, 30, selected, accent);
    Screen::text(18, y + 3, fileTitle(path, 34).c_str(), Screen::theme.text, bg);
    Screen::text(18, y + 17, path.startsWith("/updates/") ? "/updates" : "Card root", Screen::theme.muted, bg);
  }
}

void openUpdates() {
  updateImages.clear();
  updateSelection = updateFirst = 0;
  updateConfirm = false;
  if (sdReady) {
    stopBackgroundMusicPump();
    scanUpdateImages(FirmwareUpdate::directory);
    scanUpdateImages("/");
    std::sort(updateImages.begin(), updateImages.end(), gameNameLess);
    startBackgroundMusicPump(false);
  }
  view = View::Update;
  drawUpdates();
}

// Keep the update message centered and refresh the bar without clearing the screen.
void drawFirmwareInstall(int percent, bool full, bool automaticRestart) {
  constexpr int center = Screen::width / 2;
  constexpr int barWidth = 232;
  constexpr int barX = center - barWidth / 2;
  constexpr int barY = 128;
  constexpr int barHeight = 12;
  const uint16_t bg = Screen::theme.background;
  const bool ready = percent == 100;
  if (full) {
    Screen::fill(0, 0, Screen::width, Screen::height, bg);
    menuStatusVisible = false;
    Screen::fill(barX, barY, barWidth, barHeight, Screen::theme.highlight);
  }
  if (full || ready) {
    if (!full) Screen::fill(0, 80, Screen::width, 36, bg);
    drawCenteredText(center, 80, ready ? "UPDATE READY" : "INSTALLING UPDATE", Screen::theme.text, bg, 2);
    const char *message = "Keep the console powered.";
    if (ready) message = automaticRestart ? "Restarting Pocket Play..." : "Restart console to apply.";
    drawCenteredText(center, 108, message, Screen::theme.muted, bg);
  }
  const int filled = barWidth * percent / 100;
  if (filled) Screen::fill(barX, barY, filled, barHeight, Screen::theme.accent);
  char label[8];
  snprintf(label, sizeof(label), "%d%%", percent);
  Screen::fill(center - 16, 154, 32, 8, bg);
  drawCenteredText(center, 154, label, Screen::theme.text, bg);
}

void handleUpdates(uint16_t pressed) {
  if (pressed & (GB_PAD_B | kMenuKey)) {
    if (updateConfirm) {
      updateConfirm = false;
      drawUpdates();
    } else {
      updateImages.clear();
      view = View::AboutMenu;
      drawAboutMenu();
    }
    return;
  }
  const int count = int(updateImages.size());
  if (!count) return;
  if (!updateConfirm && (pressed & (GB_PAD_UP | GB_PAD_DOWN))) {
    updateSelection = (updateSelection + (pressed & GB_PAD_UP ? count - 1 : 1)) % count;
    updateFirst = std::clamp(updateFirst, std::max(0, updateSelection - kVisibleUpdateRows + 1),
                            std::min(updateSelection, std::max(0, count - kVisibleUpdateRows)));
    drawUpdates(false);
    return;
  }
  if (!(pressed & (GB_PAD_A | GB_PAD_START))) return;
  if (settingsReturnView == View::Paused || DeviceLog::recording()) {
    showStatus(settingsReturnView == View::Paused ? "Save and quit the game first." : "Stop the diagnostic log first.");
    return;
  }
  if (!updateConfirm) {
    updateConfirm = true;
    drawUpdates();
    return;
  }
  stopBackgroundMusicPump();
  drawFirmwareInstall(0, true, true);
  const auto result = FirmwareUpdate::installFromSd(updateImages[updateSelection].c_str(),
                                                   [](int percent) { drawFirmwareInstall(percent, false, true); });
  using Result = FirmwareUpdate::Result;
  if (result == Result::Ready) {
    Serial.println("Update ready. Restarting...");
    Serial.flush();
    ESP.restart();
    return;
  }
  startBackgroundMusicPump(false);
  updateConfirm = false;
  drawUpdates();
  switch (result) {
    case Result::Unavailable: showStatus("Update unavailable. Check USB setup."); break;
    case Result::TooLarge: showStatus("Firmware exceeds the 3 MiB app slot."); break;
    case Result::WrongImage: showStatus("Invalid firmware or different wiring."); break;
    case Result::Interrupted: showStatus("Could not read the file from microSD."); break;
    case Result::FlashError: showStatus("Could not install firmware."); break;
    case Result::Ready: break;
  }
}

// Enter Settings at the category list and remember the screen to restore on exit.
void openSettings(View returnView) {
  settingsReturnView = returnView;
  settingsPage = SettingsPage::Root;
  settingsSelection = 0;
  wifiField = WifiField::None;
  view = View::Settings;
  drawSettings();
}

// Open a category at its first row and save the category's position for Back.
void openSettingsPage(SettingsPage page) {
  if (settingsPage == SettingsPage::Root) settingsRootSelection = settingsSelection;
  settingsPage = page;
  settingsSelection = 0;
  drawSettings();
}

BluetoothControls::Snapshot bluetoothMenu;
int bluetoothSelection = 0;
constexpr int kBluetoothToggleRow = 0;
constexpr int kBluetoothRestartRow = 1;
constexpr SettingsItem kBluetoothStartupRows[] = {SettingsItem::EnableOnRestart, SettingsItem::RestartConsole};
constexpr int kBluetoothScanRow = std::size(kBluetoothStartupRows);
constexpr int kBluetoothFirstDeviceRow = kBluetoothScanRow + 1;

int bluetoothRows() {
  using BluetoothControls::State;
  if (!bluetoothMenu.enabled || bluetoothMenu.state == State::RestartRequired) return kBluetoothScanRow;
  const bool showDevices = bluetoothMenu.state == State::Ready || bluetoothMenu.state == State::Error ||
                           bluetoothMenu.state == State::Scanning;
  return kBluetoothFirstDeviceRow + (showDevices ? bluetoothMenu.count : 0);
}

// Keep the header and battery/clock indicators in place as scan results arrive.
void drawBluetooth(bool full, int previous = -1) {
  using BluetoothControls::State;
  constexpr int controllerRowsY = kSettingsRowsY + kBluetoothScanRow * kSettingsSubmenuRowPitch;
  constexpr int controllerRowPitch = 16;
  constexpr int controllerRowHeight = 14;
  constexpr int statusY = 200;
  static_assert(controllerRowsY + BluetoothControls::maxDevices * controllerRowPitch +
                controllerRowHeight <= statusY);
  const bool restartPending = bluetoothMenu.state == State::RestartRequired;
  const char *inactiveValue = bluetoothMenu.state == State::StartupFailed ? "FAILED" : "OFF";
  const uint16_t accent = settingsAccent();
  if (full) drawHeader("BLUETOOTH", "LEFT/RIGHT: CHANGE   A: SELECT", accent);
  if (previous < 0 && !full) Screen::fill(0, 49, Screen::width, 170, Screen::theme.background);
  for (int i = 0; i < bluetoothRows(); ++i) {
    if (previous >= 0 && i != previous && i != bluetoothSelection) continue;
    const bool selected = i == bluetoothSelection;
    if (i < kBluetoothScanRow) {
      const int y = kSettingsRowsY + i * kSettingsSubmenuRowPitch;
      drawConnectivityStartupRow(kBluetoothStartupRows[i], y, selected, restartPending,
                                 bluetoothMenu.enabled, inactiveValue);
      continue;
    }
    const int y = controllerRowsY + (i - kBluetoothScanRow) * controllerRowPitch;
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    drawCard(6, y, 308, controllerRowHeight, selected, accent);
    const char *label;
    if (i == kBluetoothScanRow) {
      switch (bluetoothMenu.state) {
        case State::Scanning: label = "CANCEL SCAN"; break;
        case State::Pairing: label = "CANCEL PAIRING"; break;
        case State::Connected: label = "DISCONNECT"; break;
        case State::Starting: label = "STARTING..."; break;
        case State::Disconnecting: label = "DISCONNECTING..."; break;
        default: label = "SCAN FOR CONTROLLERS"; break;
      }
    } else {
      label = bluetoothMenu.devices[i - kBluetoothFirstDeviceRow];
    }
    Screen::text(18, y + 4, label, selected ? Screen::theme.text : Screen::theme.muted, bg);
    if (i >= kBluetoothFirstDeviceRow) {
      Screen::text(292, y + 4, ">", accent, bg);
    }
  }
  if (previous < 0) {
    Screen::text(12, statusY, bluetoothMenu.status, Screen::theme.text, Screen::theme.background);
    if (bluetoothMenu.connected[0]) {
      Screen::text(12, statusY + 11, bluetoothMenu.connected, accent, Screen::theme.background);
    }
  }
  if (full || menuStatusVisible) {
    if (!full) clearMenuFooter();
    Screen::text(12, 231, "CONTROLLERS USE BLUETOOTH LE", Screen::theme.muted, Screen::theme.background);
  }
}

void openBluetooth() {
  bluetoothSelection = 0;
  bluetoothMenu = BluetoothControls::status();
  view = View::Bluetooth;
  drawBluetooth(true);
}

// Restart actions in both connectivity menus use the same checks.
void restartForConnectivity(bool pending, const char *enableHint, const char *restarting) {
  if (!pending) {
    showStatus(enableHint);
  } else if (settingsReturnView == View::Paused) {
    showStatus("Save and quit the game first.");
  } else if (DeviceLog::recording()) {
    showStatus("Stop the diagnostic log first.");
  } else {
    showStatus(restarting);
    Serial.flush();
    ESP.restart();
  }
}

void handleBluetooth(uint16_t pressed) {
  using BluetoothControls::State;
  if (pressed & (GB_PAD_B | kMenuKey)) {
    view = View::Settings;
    drawSettings();
    return;
  }
  const auto status = BluetoothControls::status();
  if (status.revision != bluetoothMenu.revision) {
    bluetoothMenu = status;
    bluetoothSelection = std::min(bluetoothSelection, bluetoothRows() - 1);
    drawBluetooth(false);
  }
  const bool activate = pressed & (GB_PAD_A | GB_PAD_START);
  if (pressed & (GB_PAD_UP | GB_PAD_DOWN)) {
    const int previous = bluetoothSelection;
    const int rows = bluetoothRows();
    bluetoothSelection = (bluetoothSelection + ((pressed & GB_PAD_UP) ? rows - 1 : 1)) % rows;
    if (previous != bluetoothSelection) drawBluetooth(false, previous);
  } else if (bluetoothSelection == kBluetoothToggleRow && (activate || (pressed & (GB_PAD_LEFT | GB_PAD_RIGHT)))) {
    if (bluetoothMenu.enabled || bluetoothMenu.state == State::RestartRequired) BluetoothControls::disable();
    else BluetoothControls::enableOnRestart();
  } else if (bluetoothSelection == kBluetoothRestartRow) {
    if (activate) {
      restartForConnectivity(bluetoothMenu.state == State::RestartRequired,
                             "Enable Bluetooth on restart first.", "Restarting for Bluetooth...");
    }
  } else if (activate || (pressed & GB_PAD_RIGHT)) {
    if (bluetoothSelection == kBluetoothScanRow) {
      if (bluetoothMenu.state == State::Scanning || bluetoothMenu.state == State::Pairing ||
          bluetoothMenu.state == State::Connected) BluetoothControls::disconnect();
      else BluetoothControls::scan();
    } else if (bluetoothSelection >= kBluetoothFirstDeviceRow) {
      BluetoothControls::pair(bluetoothSelection - kBluetoothFirstDeviceRow);
    }
  }
}

void drawPause(int previousSelection = -1, bool slotsChanged = false);

// Return from Settings and redraw its originating Home or pause screen.
void closeSettings() {
  view = settingsReturnView;
  if (view == View::SystemMenu) {
    drawSystemMenu(true);
  } else if (view == View::Paused) {
    drawPause();
  }
}

constexpr const char *kPauseItems[] = {
  "RESUME", "SAVE STATE", "LOAD STATE", "CHANGE GB PALETTE", "CHANGE DISPLAY SIZE",
  "VOLUME", "DISPLAY BRIGHTNESS", "SLEEP CONSOLE", "SETTINGS", "QUIT TO GAME LIST",
};

// Count pause actions, excluding the GB-only palette choice for GBA.
int pauseItemCount() {
  return std::size(kPauseItems) - (gameIsGbFamily() ? 0 : 1);
}

// Translate a visible row to the underlying action after omitting unsupported rows.
PauseItem pauseAction(int item) {
  if (!gameIsGbFamily() && item >= int(PauseItem::GbPalette)) {
    ++item;
  }
  return static_cast<PauseItem>(item);
}

// Mark pause rows that accept left/right input for slots, palette, display, or audio settings.
bool pauseOptionAdjustable(PauseItem item) {
  return item == PauseItem::SaveState || item == PauseItem::LoadState ||
         item == PauseItem::GbPalette || item == PauseItem::DisplaySize || item == PauseItem::Volume ||
         (item == PauseItem::Brightness && kTftBacklight >= 0);
}

// Show what will happen to the selected save slot before applying Save or Load.
void drawStatePrompt(int previous = -1);
void drawStatePrompt(int previous) {
  const uint16_t accent = systemAccent(selectedSystem);
  const bool saving = statePrompt == StatePrompt::Save;
  if (previous < 0) {
    drawHeader(saving ? "SAVE STATE" : "LOAD STATE", "A: CONFIRM   B / MENU: CANCEL", accent);
    char question[24];
    const char *action = saving ? (statePromptOverwrite ? "OVERWRITE" : "SAVE TO") : "LOAD";
    snprintf(question, sizeof(question), "%s SLOT %d?", action, saveSlot + 1);
    Screen::text(12, 57, question, Screen::theme.text, Screen::theme.background);
    if (!saving) {
      Screen::text(12, 75, "LOADING DISCARDS UNSAVED PROGRESS", Screen::theme.muted, Screen::theme.background);
      Screen::text(12, 88, "SINCE THIS STATE WAS SAVED.", Screen::theme.muted, Screen::theme.background);
    } else if (statePromptOverwrite) {
      Screen::text(12, 75, "THIS REPLACES THE EXISTING STATE.", Screen::theme.muted, Screen::theme.background);
    }
  }
  for (int i = 0; i < 2; ++i) {
    const bool selected = i == int(statePromptYes);
    if (previous >= 0 && !selected && i != previous) continue;
    const int y = 110 + i * 27;
    drawCard(6, y, 308, 23, selected, accent);
    Screen::text(18, y + 8, i ? "Yes" : "No", selected ? Screen::theme.text : Screen::theme.muted,
                 selected ? Screen::theme.selected : Screen::theme.panel);
  }
}

// Draw a fresh pause screen on entry, or repaint just the affected rows in place.
void drawPause(int previousSelection, bool slotsChanged) {
  if (statePrompt != StatePrompt::None) {
    drawStatePrompt(previousSelection);
    return;
  }
  const uint16_t accent = systemAccent(selectedSystem);
  const bool fullRedraw = previousSelection < 0;
  if (fullRedraw) {
    const String title = fileTitle(gameName, 37);
    drawHeader("PAUSED", title.c_str(), accent);
    Serial.println("\nPaused. W/S select, J applies, A/D adjust, K or M resumes.");
  }
  if (fullRedraw || menuStatusVisible) {
    if (!fullRedraw) clearMenuFooter();
    Screen::text(12, 225, "B / MENU: RESUME", Screen::theme.muted, Screen::theme.background);
  }
  for (int i = 0; i < pauseItemCount(); ++i) {
    const PauseItem action = pauseAction(i);
    const bool slotRow = action == PauseItem::SaveState || action == PauseItem::LoadState;
    if (!fullRedraw && i != previousSelection && i != pauseChoice && !(slotsChanged && slotRow)) {
      continue;
    }
    const bool selected = i == pauseChoice;
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    const uint16_t fg = selected ? Screen::theme.text : Screen::theme.muted;
    const uint16_t valueColor = selected ? accent : Screen::theme.text;
    const int y = 51 + i * 16;
    drawCard(6, y, 308, 14, selected, accent);
    const char *label = kPauseItems[int(action)];
    char value[16] = "";
    switch (action) {
      case PauseItem::SaveState:
      case PauseItem::LoadState:
        snprintf(value, sizeof(value), "SLOT %d%s", saveSlot + 1,
                 hasSaveSlot(saveSlot) ? "" : " EMPTY");
        break;
      case PauseItem::GbPalette: {
        const int palette = gnuboy_get_palette();
        if (palette >= GB_PALETTE_DMG) {
          snprintf(value, sizeof(value), "DMG");
        } else {
          snprintf(value, sizeof(value), "PALETTE %u", unsigned(palette + 1));
        }
        break;
      }
      case PauseItem::DisplaySize:
        snprintf(value, sizeof(value), "%s", gameIsGba ? gbaScaleName(gameGbaFullscreen()) : gbScaleName(gameGbScale()));
        break;
      case PauseItem::Volume:
        snprintf(value, sizeof(value), "%u%%", unsigned(volumePercent));
        break;
      case PauseItem::Brightness:
        if (kTftBacklight >= 0) {
          snprintf(value, sizeof(value), "%u%%", unsigned(displayBrightness));
        } else {
          snprintf(value, sizeof(value), "NO BL PIN");
        }
        break;
      case PauseItem::Settings:
        snprintf(value, sizeof(value), "OPEN >");
        break;
      default:
        break;
    }
    Screen::text(17, y + 3, label, fg, bg);
    if (action == PauseItem::Sleep) {
      constexpr int hintX = Screen::width - 10 - (sizeof(kSleepWakeHint) - 1) * 8;
      Screen::text(hintX, y + 3, kSleepWakeHint, valueColor, bg);
      Serial.printf("%c %s  |  %s\n", i == pauseChoice ? '>' : ' ', label, kSleepWakeHint);
    } else if (value[0]) {
      const bool adjustable = pauseOptionAdjustable(action);
      drawMenuValue(kMenuValueCenter, y + 3, value, valueColor, bg, adjustable);
      Serial.printf("%c %s  %s%s%s\n", i == pauseChoice ? '>' : ' ', label,
                    adjustable ? "< " : "", value, adjustable ? " >" : "");
    } else {
      Serial.printf("%c %s\n", i == pauseChoice ? '>' : ' ', label);
    }
  }
}

// Test a mounted VFS path with stat().
bool fileExists(const String &path) {
  struct stat info;
  return stat(path.c_str(), &info) == 0;
}

// Prefer the main file and fall back to its backup when the main path is absent.
String readableSavePath(const String &path) {
  return fileExists(path) ? path : path + ".bak";
}

// Promote a completed temporary file and restore the old file if promotion fails.
bool replaceSavedFile(const String &path, const String &temp) {
  const String backup = path + ".bak";
  if (fileExists(path)) {
    remove(backup.c_str());
    if (rename(path.c_str(), backup.c_str()) != 0) {
      remove(temp.c_str());
      return false;
    }
  }
  if (rename(temp.c_str(), path.c_str()) != 0) {
    rename(backup.c_str(), path.c_str());
    remove(temp.c_str());
    return false;
  }
  remove(backup.c_str());
  return true;
}

// Read exactly the expected bytes from a VFS file or its fallback backup.
bool readSavedBytes(const String &path, uint8_t *bytes, size_t size) {
  FILE *file = fopen(readableSavePath(path).c_str(), "rb");
  if (!file) return false;
  const bool read = fread(bytes, 1, size, file) == size && fgetc(file) == EOF && !ferror(file);
  return fclose(file) == 0 && read;
}

// Write a fixed-size record to a temporary file and publish it only after a successful close.
bool writeSavedBytes(const String &path, const uint8_t *bytes, size_t size) {
  const String temp = path + ".tmp";
  FILE *file = fopen(temp.c_str(), "wb");
  if (!file) return false;
  const bool written = fwrite(bytes, 1, size, file) == size;
  if (fclose(file) != 0 || !written) {
    remove(temp.c_str());
    return false;
  }
  return replaceSavedFile(path, temp);
}

// Validate the version and options of a six-byte ROM profile or use global defaults.
GameProfile readGameProfile(const String &path) {
  uint8_t values[6];
  if (readSavedBytes("/sd" + path + ".profile", values, sizeof(values)) &&
      values[0] == 1 && values[1] <= 1 && values[2] <= uint8_t(Screen::GbScale::Full) &&
      values[3] <= 1 && values[4] <= 3 && values[5] <= 1) {
    return {bool(values[1]), static_cast<Screen::GbScale>(values[2]), bool(values[3]),
            values[4], bool(values[5])};
  }
  const int system = systemForPath(path);
  const auto scale = system >= 0 ? emulatorScales[system] : Screen::GbScale::Full;
  return {false, scale, gbaFullscreen, gbaFrameskipMode, soundEnabled};
}

// Serialize a versioned per-game override beside its ROM.
bool writeGameProfile(const String &path, const GameProfile &profile) {
  const uint8_t values[] = {1, uint8_t(profile.enabled), uint8_t(profile.gbDisplay),
                            uint8_t(profile.gbaFull), profile.frameskip, uint8_t(profile.sound)};
  return writeSavedBytes("/sd" + path + ".profile", values, sizeof(values));
}

// Forward declaration so the profile can return to the game options page.
void drawGameOptions(int previous = -1);

// Draw the selected title's override mode and repaint only the affected rows on input.
void drawGameProfile(int previous = -1) {
  const uint16_t accent = romAccent(gameOptionsPath);
  const bool full = previous < 0;
  if (full) {
    drawHeader("GAME SETTINGS", "LEFT/RIGHT: CHANGE   A: SELECT", accent);
    const String title = fileTitle(gameOptionsPath, 36);
    Screen::text(12, 55, title.c_str(), Screen::theme.text, Screen::theme.background);
  }
  const char *labels[] = {"USE PROFILE", "DISPLAY SIZE", "FRAMESKIP", "GAME SOUND", "SAVE PROFILE"};
  const char *values[] = {
    editedGameProfile.enabled ? "ON" : "DEFAULT",
    profileIsGba ? gbaScaleName(editedGameProfile.gbaFull) : gbScaleName(editedGameProfile.gbDisplay),
    frameskipName(editedGameProfile.frameskip),
    editedGameProfile.sound ? "ON" : "OFF",
    "A: SAVE"
  };
  int row = 0;
  for (int i = 0; i < int(std::size(labels)); ++i) {
    if (i == 2 && !profileIsGba) continue;
    const int index = row++;
    const bool selected = index == profileSelection;
    // Changing an override can turn on Use Profile even when another row is selected.
    if (!full && !selected && index != previous && index != 0) continue;
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    const int y = 73 + index * 24;
    drawCard(6, y, 308, 21, selected, accent);
    Screen::text(18, y + 6, labels[i], selected ? Screen::theme.text : Screen::theme.muted, bg);
    drawMenuValue(kMenuValueCenter, y + 6, values[i], selected ? accent : Screen::theme.text, bg, i != 4);
  }
  if (full || menuStatusVisible) {
    if (!full) clearMenuFooter();
    Screen::text(12, 224, "B: CANCEL     SAVES TO SD CARD", Screen::theme.muted, Screen::theme.background);
  }
}

// Verify the ROM path and load its editable settings before opening the profile view.
void openGameProfile() {
  if (!fileExists("/sd" + gameOptionsPath)) {
    showStatus("Game is unavailable.");
    return;
  }
  profileIsGba = systemForPath(gameOptionsPath) == int(System::GameBoyAdvance);
  editedGameProfile = readGameProfile(gameOptionsPath);
  profileSelection = 0;
  view = View::GameProfile;
  drawGameProfile();
}

// Edit the temporary profile and commit it only when the Save Profile action succeeds.
void handleGameProfile(uint16_t pressed) {
  const int count = profileIsGba ? 5 : 4;
  if (pressed & (GB_PAD_B | kMenuKey)) {
    view = View::GameOptions;
    drawGameOptions();
  } else if (pressed & (GB_PAD_UP | GB_PAD_DOWN)) {
    const int previous = profileSelection;
    profileSelection = (profileSelection + (pressed & GB_PAD_UP ? count - 1 : 1)) % count;
    drawGameProfile(previous);
  } else if (pressed & (GB_PAD_A | GB_PAD_LEFT | GB_PAD_RIGHT)) {
    if (profileSelection == count - 1) {
      if (!(pressed & GB_PAD_A)) return;
      if (writeGameProfile(gameOptionsPath, editedGameProfile)) {
        view = View::GameOptions;
        drawGameOptions();
        showStatus("Game profile saved.");
      } else {
        showStatus("Could not save game profile.");
      }
      return;
    }
    const int step = pressed & GB_PAD_LEFT ? -1 : 1;
    if (profileSelection == 0) {
      editedGameProfile.enabled = !editedGameProfile.enabled;
    } else if (profileSelection == 1) {
      if (profileIsGba) {
        editedGameProfile.gbaFull = !editedGameProfile.gbaFull;
      } else {
        editedGameProfile.gbDisplay = nextDisplayScale(editedGameProfile.gbDisplay, step);
      }
      editedGameProfile.enabled = true;
    } else if (profileIsGba && profileSelection == 2) {
      editedGameProfile.frameskip = (editedGameProfile.frameskip + step + 4) % 4;
      editedGameProfile.enabled = true;
    } else {
      editedGameProfile.sound = !editedGameProfile.sound;
      editedGameProfile.enabled = true;
    }
    drawGameProfile(profileSelection);
  }
}

// Read a bounded list of valid, unique ROM paths from a newline-delimited file.
void readGamePaths(const String &path, std::vector<String> &games, size_t limit) {
  FILE *file = fopen(readableSavePath(path).c_str(), "rb");
  if (!file) return;
  char line[1024];
  while (games.size() < limit && fgets(line, sizeof(line), file)) {
    const size_t length = strcspn(line, "\r\n");
    if (line[length] == '\0' && !feof(file)) {
      int byte;
      while ((byte = fgetc(file)) != EOF && byte != '\n') {}
      continue;
    }
    line[length] = '\0';
    if (line[0] == '/' && systemForPath(line) >= 0 &&
        std::find(games.begin(), games.end(), String(line)) == games.end()) {
      games.emplace_back(line);
    }
  }
  fclose(file);
}

// Write one path per line and promote the completed file using the shared save workflow.
bool writeGamePaths(const String &path, const std::vector<String> &games) {
  const String temp = path + ".tmp";
  FILE *file = fopen(temp.c_str(), "wb");
  if (!file) return false;
  bool written = true;
  for (const String &game : games) {
    if (fprintf(file, "%s\n", game.c_str()) < 0) {
      written = false;
      break;
    }
  }
  written = fclose(file) == 0 && written;
  if (!written) {
    remove(temp.c_str());
    return false;
  }
  return replaceSavedFile(path, temp);
}

// Add or remove a star and publish the updated list only after writing it successfully.
bool toggleFavorite(const String &path) {
  auto updated = favoriteGames;
  const auto existing = std::find(updated.begin(), updated.end(), path);
  if (existing == updated.end()) {
    if (updated.size() >= kMaxFavorites) return false;
    updated.push_back(path);
  } else {
    updated.erase(existing);
  }
  if (!writeGamePaths(kFavoritesFile, updated)) return false;
  favoriteGames.swap(updated);
  return true;
}

// Move the new title to the front of the bounded history and persist that history.
void recordRecentlyPlayed(const String &path) {
  if (!recentGames.empty() && recentGames.front() == path) return;
  const auto existing = std::find(recentGames.begin(), recentGames.end(), path);
  if (existing != recentGames.end()) recentGames.erase(existing);
  recentGames.insert(recentGames.begin(), path);
  if (recentGames.size() > kMaxRecentGames) recentGames.pop_back();
  if (!writeGamePaths(kRecentGamesFile, recentGames)) {
    Serial.println("Could not save recently played games.");
  }
}

// Ask the active core to write a temporary save, then publish it with backup recovery.
bool commitSave(const String &path, bool state) {
  const String temp = path + ".tmp";
  bool written;
  if (activeCartridgeCore) {
    written = state ? activeCartridgeCore->writeState(temp.c_str()) : activeCartridgeCore->writeBattery(temp.c_str());
  } else if (gameIsGba) {
    written = state ? gbaCoreWriteState(temp.c_str()) : gbaCoreWriteBattery(temp.c_str());
  } else {
    written = (state ? gnuboy_save_state(temp.c_str()) : gnuboy_save_sram(temp.c_str(), false)) == 0;
  }
  if (!written) {
    remove(temp.c_str());
    return false;
  }
  if (!replaceSavedFile(path, temp)) return false;
  if (!state) {
    if (activeCartridgeCore) activeCartridgeCore->batteryCommitted();
    else if (gameIsGba) gbaCoreBatteryCommitted();
  }
  return true;
}

// Query whether the active emulator has cartridge save changes pending.
bool batteryChanged() {
  if (activeCartridgeCore) return activeCartridgeCore->batteryChanged();
  return gameIsGba ? gbaCoreBatteryChanged() : gnuboy_sram_dirty();
}

// Map the zero-based slot to a VFS filename, preserving the legacy filename for slot 1.
String saveSlotPath(const String &base, int slot) {
  return slot == 0 ? base : base + String(slot + 1);
}

// Check whether the chosen save slot or its backup is available.
bool hasSaveSlot(int slot) {
  return fileExists(readableSavePath(saveSlotPath(stateFile, slot)));
}

// Prefer valid last-slot metadata and otherwise choose the first existing slot.
int latestSaveSlot() {
  uint8_t slot = 0;
  if (readSavedBytes(stateFile + ".latest", &slot, 1) && slot < kSaveSlotCount && hasSaveSlot(slot)) {
    return slot;
  }
  for (int candidate = 0; candidate < kSaveSlotCount; ++candidate) {
    if (hasSaveSlot(candidate)) return candidate;
  }
  return 0;
}

// Write the selected emulator snapshot and record the slot for Continue when possible.
bool saveSelectedState() {
  if (!commitSave(saveSlotPath(stateFile, saveSlot), true)) return false;
  const uint8_t slot = uint8_t(saveSlot);
  if (!writeSavedBytes(stateFile + ".latest", &slot, 1)) {
    Serial.println("State saved but could not update the Continue slot.");
  }
  return true;
}

// Restore the selected snapshot using the active emulator's file format.
bool loadState() {
  const String path = readableSavePath(saveSlotPath(stateFile, saveSlot));
  if (activeCartridgeCore) return activeCartridgeCore->readState(path.c_str());
  return gameIsGba ? gbaCoreReadState(path.c_str()) : gnuboy_load_state(path.c_str()) == 0;
}

// Inspect a card path without treating SD errors or directories as an available filename.
GameFileKind gameFileKind(const String &path) {
  struct stat info;
  if (stat(path.c_str(), &info) == 0) {
    return S_ISREG(info.st_mode) ? GameFileKind::File : GameFileKind::Unavailable;
  }
  return errno == ENOENT ? GameFileKind::Missing : GameFileKind::Unavailable;
}

bool sameGameFile(const String &a, const String &b) {
  return strcasecmp(a.c_str(), b.c_str()) == 0;
}

// A cover with the same stem can also belong to a second ROM in this directory.
bool gameCoverIsShared(const String &rom, const String &directory, const String &coverStem) {
  const String namedRom = directory + coverStem;
  if (!sameGameFile(rom, namedRom) && systemForPath(namedRom) >= 0 && fileExists(namedRom)) return true;
  for (const SystemInfo &system : kSystems) {
    const String sibling = namedRom + system.suffix;
    if (!sameGameFile(rom, sibling) && fileExists(sibling)) return true;
  }
  return false;
}

// Plan every move before touching the card. Reserve destination save names even if the source lacks them.
const char *planGameRename(const String &source, const String &target, std::vector<GameFileMove> &moves) {
  const bool caseOnly = sameGameFile(source, target);
  const GameFileKind targetKind = gameFileKind(target);
  if (!caseOnly && targetKind != GameFileKind::Missing) return "That ROM name is already in use.";

  std::vector<String> sidecars = {".sav", ".profile", ".state.latest"};
  for (int slot = 0; slot < kSaveSlotCount; ++slot) sidecars.push_back(saveSlotPath(".state", slot));
  for (const String &sidecar : sidecars) {
    for (const char *suffix : {"", ".bak", ".tmp"}) {
      const String from = source + sidecar + suffix;
      const String to = target + sidecar + suffix;
      const GameFileKind fromKind = gameFileKind(from);
      if (fromKind == GameFileKind::Unavailable) return "Cannot read existing game data.";
      if (!caseOnly && gameFileKind(to) != GameFileKind::Missing) return "Save data exists for that name.";
      if (fromKind == GameFileKind::File) moves.push_back({from, to, false});
    }
  }

  const String directory = source.substring(0, source.lastIndexOf('/') + 1);
  for (bool fullName : {false, true}) {
    const String fromStem = fullName ? String(fileName(source)) : fileTitle(source);
    const String toStem = fullName ? String(fileName(target)) : fileTitle(target);
    const String from = directory + "imagesets/" + fromStem + ".bmp";
    const String to = directory + "imagesets/" + toStem + ".bmp";
    if (gameFileKind(from) == GameFileKind::File) {
      const bool shared = gameCoverIsShared(source, directory, fromStem);
      if (shared && caseOnly) continue;
      if (!caseOnly && gameFileKind(to) != GameFileKind::Missing) return "Cover art exists for that name.";
      moves.push_back({from, to, shared});
    }
  }
  // Move the ROM last, so failure on a related file leaves the indexed ROM in place.
  moves.push_back({source, target, false});
  return nullptr;
}

// Preserve cover art still used by a neighboring ROM without loading it into memory.
bool copyGameCover(const String &source, const String &target) {
  if (gameFileKind(target) != GameFileKind::Missing) return false;
  FILE *in = fopen(source.c_str(), "rb");
  if (!in) return false;
  FILE *out = fopen(target.c_str(), "wb");
  if (!out) {
    fclose(in);
    return false;
  }
  uint8_t bytes[1024];
  size_t count;
  bool ok = true;
  while ((count = fread(bytes, 1, sizeof(bytes), in)) > 0) {
    if (fwrite(bytes, 1, count, out) != count) {
      ok = false;
      break;
    }
  }
  ok = !ferror(in) && ok;
  fclose(in);
  ok = fclose(out) == 0 && ok;
  if (!ok) remove(target.c_str());
  return ok;
}

// FAT treats names differing only in case as aliases. Use a vacant intermediate name for those moves.
bool moveGameFile(const String &source, const String &target) {
  if (!sameGameFile(source, target)) {
    return gameFileKind(target) == GameFileKind::Missing && rename(source.c_str(), target.c_str()) == 0;
  }
  const String temporary = source.substring(0, source.lastIndexOf('/') + 1) + ".pocketplay-rename.tmp";
  if (gameFileKind(temporary) != GameFileKind::Missing || rename(source.c_str(), temporary.c_str()) != 0) {
    return false;
  }
  if (rename(temporary.c_str(), target.c_str()) == 0) return true;
  if (rename(temporary.c_str(), source.c_str()) != 0) {
    Serial.printf("Could not restore %s from %s\n", source.c_str(), temporary.c_str());
  }
  return false;
}

// Roll back the completed file moves if the card rejects a later move.
bool applyGameRename(const std::vector<GameFileMove> &moves) {
  size_t completed = 0;
  for (const GameFileMove &move : moves) {
    if (move.copy ? copyGameCover(move.source, move.target) : moveGameFile(move.source, move.target)) {
      ++completed;
    } else {
      while (completed) {
        const GameFileMove &previous = moves[--completed];
        const bool restored = previous.copy ? remove(previous.target.c_str()) == 0 :
                                              moveGameFile(previous.target, previous.source);
        if (!restored) Serial.printf("Could not roll back %s\n", previous.target.c_str());
      }
      Serial.printf("Could not rename %s to %s\n", move.source.c_str(), move.target.c_str());
      return false;
    }
  }
  return true;
}

// Replace the selected path in a library or remove it when deleting a ROM.
void replaceGamePath(std::vector<String> &games, const String &oldPath, const String &newPath) {
  const auto found = std::find_if(games.begin(), games.end(), [&](const String &path) {
    return sameGameFile(path, oldPath);
  });
  if (found == games.end()) return;
  const auto matching = [&](const String &path) {
    return sameGameFile(path, oldPath) || sameGameFile(path, newPath);
  };
  auto first = games.begin();
  if (!newPath.isEmpty()) {
    first = std::find_if(games.begin(), games.end(), matching);
    *first++ = newPath;
  }
  games.erase(std::remove_if(first, games.end(), matching), games.end());
}

void replaceGameList(std::vector<String> &games, std::vector<String> updated, int &selection,
                     const String &oldPath, const String &newPath) {
  String selected = selection < int(games.size()) ? games[selection] : String();
  if (sameGameFile(selected, oldPath)) selected = newPath;
  games.swap(updated);
  const auto found = std::find(games.begin(), games.end(), selected);
  selection = found != games.end() ? int(found - games.begin()) : max(0, min(selection, int(games.size()) - 1));
}

// Update on-card history and favorites together with the file operation, publishing indexes after success.
const char *changeGameFile(const String &oldPath, const String &newPath) {
  const int type = systemForPath(oldPath);
  if (!sdReady || type < 0) return "Game is unavailable.";
  const String source = "/sd" + oldPath;
  const bool deleting = newPath.isEmpty();
  if (oldPath == newPath) return nullptr;
  GameCover::stop();
  if (gameFileKind(source) != GameFileKind::File) return "Game is unavailable.";
  std::vector<GameFileMove> moves;
  if (!deleting) {
    if (const char *message = planGameRename(source, "/sd" + newPath, moves)) return message;
  }

  auto favorites = favoriteGames;
  auto recent = recentGames;
  replaceGamePath(favorites, oldPath, newPath);
  replaceGamePath(recent, oldPath, newPath);
  const bool favoritesChanged = favorites != favoriteGames;
  const bool recentChanged = recent != recentGames;
  if (favoritesChanged && !writeGamePaths(kFavoritesFile, favorites)) return "Could not update favorites.";
  const bool recentSaved = !recentChanged || writeGamePaths(kRecentGamesFile, recent);
  const bool changed = recentSaved && (deleting ? remove(source.c_str()) == 0 : applyGameRename(moves));
  if (!changed) {
    if (favoritesChanged && !writeGamePaths(kFavoritesFile, favoriteGames)) {
      Serial.println("Could not restore the favorites list.");
    }
    if (recentChanged && recentSaved && !writeGamePaths(kRecentGamesFile, recentGames)) {
      Serial.println("Could not restore the recent games list.");
    }
    if (!recentSaved) return "Could not update recent games.";
    return deleting ? "Could not delete the ROM." : "Could not rename. Check SD card.";
  }

  auto &library = gamesBySystem[type];
  auto updated = library;
  replaceGamePath(updated, oldPath, newPath);
  if (!deleting) std::sort(updated.begin(), updated.end(), gameNameLess);
  replaceGameList(library, std::move(updated), gameSelections[type], oldPath, newPath);
  replaceGameList(favoriteGames, std::move(favorites), favoritesSelection, oldPath, newPath);
  replaceGameList(recentGames, std::move(recent), recentSelection, oldPath, newPath);
  if (sameGameFile(lastPlayedPath, oldPath)) {
    lastPlayedPath = newPath;
    const bool saved = deleting ? settingsStore.remove("lastPlayed") :
                                  settingsStore.putString("lastPlayed", newPath) != 0;
    if (!saved) Serial.println("Could not update the Continue game in Preferences.");
  }
  return nullptr;
}

constexpr int kGameKeyboardColumns = 10;
constexpr char kGameNameKeys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.()[]&+'!,#";
constexpr int kGameNameKeyCount = sizeof(kGameNameKeys) - 1;
static_assert(kGameNameKeyCount == 5 * kGameKeyboardColumns);
constexpr const char *kGameNameEditActions[] = {"CASE", "< CURSOR", "CURSOR >", "SAVE"};
constexpr int kGameNameCase = kGameNameKeyCount;
constexpr int kGameNamePrev = kGameNameCase + 1;
constexpr int kGameNameNext = kGameNameCase + 2;
constexpr int kGameNameSave = kGameNameCase + 3;
constexpr size_t kMaxGameFilenameBytes = 238; // Leave room for .state.latest.bak in FAT's 255-character name.
constexpr const char *kGameOptionsItems[] = {"GAME SETTINGS", "RENAME GAME", "DELETE ROM"};

// Select an ASCII keyboard key, or an editing action at the bottom of the keyboard.
void drawTextKey(int key, uint16_t accent) {
  const bool action = key >= kGameNameKeyCount;
  const int col = action ? key - kGameNameKeyCount : key % kGameKeyboardColumns;
  const int x = 10 + col * (action ? 75 : 30);
  const int y = action ? 190 : 91 + (key / kGameKeyboardColumns) * 19;
  const int width = action ? 72 : 27;
  const bool selected = key == keyboardKey;
  const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
  const char character[] = {action ? '\0' : char(keyboardUppercase ? kGameNameKeys[key] : tolower(kGameNameKeys[key])), '\0'};
  const char *label = action ? kGameNameEditActions[col] : *character == ' ' ? "SP" : character;
  Screen::fill(x, y, width, 16, bg);
  if (selected) Screen::fill(x, y, 2, 16, accent);
  Screen::text(x + (width - int(strlen(label)) * 8) / 2, y + 4, label, Screen::theme.text, bg);
}

// Keep the caret and its surrounding text visible while editing.
void drawEditedText(uint16_t accent, const char *detail) {
  constexpr size_t visible = 34;
  const size_t first = textCaret >= visible ? textCaret - visible + 1 : 0;
  const String segment = editedText.substring(first, first + visible);
  Screen::fill(10, 52, 300, 30, Screen::theme.panel);
  Screen::text(16, 57, segment.c_str(), Screen::theme.text, Screen::theme.panel);
  Screen::fill(16 + int(textCaret - first) * 8, 67, 7, 2, accent);
  Screen::text(16, 72, detail, Screen::theme.muted, Screen::theme.panel);
}

void drawTextEditor(const char *title, uint16_t accent, const char *detail) {
  drawHeader(title, "D-PAD: MOVE   A: TYPE   MENU: CANCEL", accent);
  drawEditedText(accent, detail);
  for (int key = 0; key <= kGameNameSave; ++key) drawTextKey(key, accent);
  Screen::text(12, 225, "B: ERASE    CASE: UPPER / LOWER", Screen::theme.muted, Screen::theme.background);
}

void openTextEditor(const String &value) {
  editedText = value;
  textCaret = editedText.length();
  keyboardKey = 0;
  keyboardUppercase = false;
}

void drawWifiEditor() {
  const bool editingName = wifiField == WifiField::Name;
  drawTextEditor(editingName ? "NETWORK NAME" : "WI-FI PASSWORD", Screen::theme.accent,
                 editingName ? "1-32 characters" : "8-63 characters");
}

// Change only the selected rows when moving around the game options or confirmation screen.
void drawGameOptions(int previous) {
  const uint16_t accent = romAccent(gameOptionsPath);
  const bool full = previous < 0;
  if (gameOptionsPage == GameOptionsPage::Rename) {
    drawTextEditor("RENAME GAME", accent, strrchr(gameOptionsPath.c_str(), '.'));
    return;
  }

  const bool deleting = gameOptionsPage == GameOptionsPage::Delete;
  if (full) {
    drawHeader(deleting ? "DELETE ROM?" : "GAME OPTIONS", "A: SELECT   B / MENU: BACK", accent);
    const String title = deleting ? String(fileName(gameOptionsPath)) : fileTitle(gameOptionsPath, 36);
    const String first = title.substring(0, 36);
    Screen::text(12, 53, first.c_str(), Screen::theme.text, Screen::theme.background);
    if (deleting) {
      String second = title.substring(36, 72);
      if (title.length() > 72) second = second.substring(0, 34) + "..";
      Screen::text(12, 65, second.c_str(), Screen::theme.text, Screen::theme.background);
      Screen::text(12, 84, "THE ROM WILL BE PERMANENTLY DELETED.", Screen::theme.muted, Screen::theme.background);
      Screen::text(12, 96, "SAVES AND COVER ART STAY ON THE SD.", Screen::theme.muted, Screen::theme.background);
    }
  }
  const int selection = deleting ? int(confirmGameDelete) : gameOption;
  for (int i = 0; i < (deleting ? 2 : int(std::size(kGameOptionsItems))); ++i) {
    if (!full && i != selection && i != previous) continue;
    const bool selected = selection == i;
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    const int y = (deleting ? 117 : 73) + i * 27;
    drawCard(6, y, 308, 23, selected, accent);
    const char *label = deleting ? (i == 0 ? "No, keep ROM" : "Yes, delete ROM") : kGameOptionsItems[i];
    Screen::text(18, y + 8, label, selected ? Screen::theme.text : Screen::theme.muted, bg);
    if (!deleting) Screen::text(293, y + 8, ">", Screen::theme.muted, bg);
  }
}

void openGameOptions(const String &path) {
  gameOptionsReturnView = view;
  gameOptionsPath = path;
  gameOptionsPage = GameOptionsPage::Actions;
  gameOption = 0;
  GameCover::cancel();
  view = View::GameOptions;
  drawGameOptions();
}

void returnFromGameOptions(const char *message) {
  view = gameOptionsReturnView;
  drawGameList(false);
  if (message) showStatus(message);
}

// Move by a full UTF-8 character when editing an existing filename.
size_t moveTextCaret(bool forward) {
  size_t caret = textCaret;
  if (forward) {
    if (caret < editedText.length()) ++caret;
    while (caret < editedText.length() && (uint8_t(editedText[caret]) & 0xC0) == 0x80) ++caret;
  } else {
    while (caret && (uint8_t(editedText[--caret]) & 0xC0) == 0x80) {}
  }
  return caret;
}

void submitGameRename() {
  String stem = editedText;
  stem.trim();
  const String extension = gameOptionsPath.substring(gameOptionsPath.lastIndexOf('.'));
  if (stem.isEmpty() || stem.endsWith(".") || stem.length() + extension.length() > kMaxGameFilenameBytes) {
    showStatus("Enter a valid game name.");
    return;
  }
  const String path = gameOptionsPath.substring(0, gameOptionsPath.lastIndexOf('/') + 1) + stem + extension;
  if (path == gameOptionsPath) {
    returnFromGameOptions(nullptr);
  } else if (const char *message = changeGameFile(gameOptionsPath, path)) {
    showStatus(message);
  } else {
    returnFromGameOptions("Game renamed.");
  }
}

// Return true when the user chooses Save. The caller validates and applies the value.
bool handleTextEditor(uint16_t pressed, size_t maxBytes, uint16_t accent, const char *detail) {
  if (pressed & GB_PAD_B) {
    const size_t previous = moveTextCaret(false);
    editedText.remove(previous, textCaret - previous);
    textCaret = previous;
  } else if (pressed & GB_PAD_A) {
    if (keyboardKey == kGameNameSave) return true;
    if (keyboardKey == kGameNameCase) {
      keyboardUppercase = !keyboardUppercase;
      for (int key = 0; key < kGameNameKeyCount; ++key) drawTextKey(key, accent);
    } else if (keyboardKey >= kGameNamePrev) {
      textCaret = moveTextCaret(keyboardKey == kGameNameNext);
    } else {
      if (editedText.length() >= maxBytes) {
        showStatus("Text is too long.");
        return false;
      }
      const char key = kGameNameKeys[keyboardKey];
      const char character = keyboardUppercase ? key : tolower(key);
      editedText = editedText.substring(0, textCaret) + character + editedText.substring(textCaret);
      ++textCaret;
    }
  } else if (pressed & (kListScrollButtons | GB_PAD_LEFT | GB_PAD_RIGHT)) {
    const int previous = keyboardKey;
    const bool action = previous >= kGameNameKeyCount;
    if (pressed & GB_PAD_UP) {
      if (action) {
        keyboardKey = kGameNameKeyCount - kGameKeyboardColumns + (previous - kGameNameKeyCount) * 3;
      } else if (previous >= kGameKeyboardColumns) {
        keyboardKey -= kGameKeyboardColumns;
      }
    } else if (pressed & GB_PAD_DOWN) {
      if (!action) {
        keyboardKey = min(previous + kGameKeyboardColumns,
                              kGameNameKeyCount + (previous % kGameKeyboardColumns) / 3);
      }
    } else {
      const int width = action ? int(std::size(kGameNameEditActions)) : kGameKeyboardColumns;
      const int start = action ? kGameNameKeyCount : previous - previous % kGameKeyboardColumns;
      const int step = pressed & GB_PAD_LEFT ? width - 1 : 1;
      keyboardKey = start + (previous - start + step) % width;
    }
    drawTextKey(previous, accent);
    if (keyboardKey != previous) drawTextKey(keyboardKey, accent);
    return false;
  } else {
    return false;
  }
  drawEditedText(accent, detail);
  if (menuStatusVisible) {
    clearMenuFooter();
    Screen::text(12, 225, "B: ERASE    CASE: UPPER / LOWER", Screen::theme.muted, Screen::theme.background);
  }
  return false;
}

void handleWifiEditor(uint16_t pressed) {
  const bool editingName = wifiField == WifiField::Name;
  if (!(pressed & kMenuKey)) {
    if (!handleTextEditor(pressed, editingName ? WifiTransfer::maxNameBytes : WifiTransfer::maxPasswordBytes,
                          Screen::theme.accent, editingName ? "1-32 characters" : "8-63 characters")) return;
    const char *error = editingName ? WifiTransfer::saveName(editedText.c_str()) : WifiTransfer::savePassword(editedText.c_str());
    if (error) {
      showStatus(error);
      return;
    }
  }
  editedText = "";
  wifiField = WifiField::None;
  drawSettings(false);
}

void handleGameOptions(uint16_t pressed) {
  if ((pressed & kMenuKey) || ((pressed & GB_PAD_B) && gameOptionsPage != GameOptionsPage::Rename)) {
    if (gameOptionsPage == GameOptionsPage::Actions) {
      returnFromGameOptions(nullptr);
    } else {
      gameOptionsPage = GameOptionsPage::Actions;
      drawGameOptions();
    }
    return;
  }
  if (gameOptionsPage == GameOptionsPage::Rename) {
    const char *extension = strrchr(gameOptionsPath.c_str(), '.');
    const size_t extensionLength = strlen(extension);
    const size_t maxStem = extensionLength < kMaxGameFilenameBytes ? kMaxGameFilenameBytes - extensionLength : 0;
    if (handleTextEditor(pressed, maxStem, romAccent(gameOptionsPath), extension)) submitGameRename();
    return;
  }
  if (gameOptionsPage == GameOptionsPage::Delete) {
    if (pressed & (kListScrollButtons | GB_PAD_LEFT | GB_PAD_RIGHT)) {
      const int previous = int(confirmGameDelete);
      confirmGameDelete = !confirmGameDelete;
      drawGameOptions(previous);
    } else if (pressed & GB_PAD_A) {
      if (!confirmGameDelete) {
        gameOptionsPage = GameOptionsPage::Actions;
        drawGameOptions();
      } else if (const char *message = changeGameFile(gameOptionsPath, String())) {
        confirmGameDelete = false;
        drawGameOptions(1);
        showStatus(message);
      } else {
        returnFromGameOptions("ROM deleted. Saves kept.");
      }
    }
    return;
  }

  if (pressed & kListScrollButtons) {
    const int previous = gameOption;
    constexpr int count = std::size(kGameOptionsItems);
    gameOption = (gameOption + (pressed & GB_PAD_UP ? count - 1 : 1)) % count;
    drawGameOptions(previous);
  } else if (pressed & (GB_PAD_A | GB_PAD_RIGHT)) {
    if (gameOption == 0) {
      openGameProfile();
      return;
    }
    if (gameOption == 1) {
      gameOptionsPage = GameOptionsPage::Rename;
      openTextEditor(fileTitle(gameOptionsPath));
    } else {
      gameOptionsPage = GameOptionsPage::Delete;
      confirmGameDelete = false;
    }
    drawGameOptions();
  }
}

// Interpret the Game Boy ROM size byte, including 72-, 80-, and 96-bank formats.
size_t headerRomSize(uint8_t code) {
  if (code <= 8) {
    return size_t(32768) << code;
  }
  // Gnuboy represents the uncommon 72/80/96-bank cartridges as 128 banks.
  constexpr size_t banks[] = {72, 80, 96};
  return code >= 0x52 && code <= 0x54 ? banks[code - 0x52] * 16384 : 0;
}

// Validate and load a GB/GBC ROM into owned PSRAM, padding unusual bank counts for Gnuboy.
bool loadCardRom(const String &path) {
  File file = SD.open(path, FILE_READ);
  uint8_t header[0x150];
  if (!file || file.read(header, sizeof(header)) != sizeof(header)) {
    showStatus("Could not read ROM header");
    return false;
  }
  uint8_t checksum = 0;
  for (size_t i = 0x134; i < 0x14D; ++i) {
    checksum = checksum - header[i] - 1;
  }
  const size_t romSize = headerRomSize(header[0x148]);
  if (!romSize || checksum != header[0x14D] || file.size() < romSize) {
    showStatus("Invalid or truncated ROM");
    return false;
  }
  if (romSize > kMaxRomBytes) {
    showStatus("ROM exceeds the 4 MiB limit");
    return false;
  }
  const bool padded = header[0x148] >= 0x52;
  const size_t loadSize = padded ? 128 * 16384 : romSize;
  ownedRom = static_cast<uint8_t *>(heap_caps_malloc(loadSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!ownedRom) {
    showStatus("Enable OPI PSRAM in Arduino IDE");
    return false;
  }
  if (!file.seek(0) || file.read(ownedRom, romSize) != romSize) {
    showStatus("Could not read ROM data");
    return false;
  }
  if (padded) {
    memset(ownedRom + romSize, 0xFF, loadSize - romSize);
  }
  if (gnuboy_load_rom(ownedRom, loadSize) != 0) {
    showStatus("Emulator could not load ROM");
    return false;
  }
  return true;
}

// Present one GBA image and increment the displayed-frame counter only on success.
bool presentGbaFrame(const uint16_t *pixels, bool fullScreen) {
  if (!Screen::drawGbaFrame(pixels, fullScreen)) {
    return false;
  }
  gameDisplayedFrames.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// Present cartridge output and count completed RGB565 transfers for performance reports.
void presentCartridgeFrame(const CartridgeFrame &frame, Screen::GbScale scale) {
  if (Screen::drawCartridgeFrame(frame, scale) && frame.rgb565) {
    gameDisplayedFrames.fetch_add(1, std::memory_order_relaxed);
  }
}

// Consume the submitted frame on core 0 and release its buffer after the TFT transfer.
void videoPresenterTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (presenterCartridgeFrame.pixels || presenterCartridgeFrame.rgb565) {
      presentCartridgeFrame(presenterCartridgeFrame, presenterGbScale);
    } else if (presenterFrameIsGba) {
      presentGbaFrame(videoPresenterBuffer, presenterGbaFullscreen);
    } else {
      Screen::drawGameFrame(videoPresenterBuffer, presenterGbScale);
    }
    xSemaphoreGive(videoPresenterBufferAvailable);
  }
}

// Allocate the single shared staging buffer and start the pinned display task if needed.
bool beginVideoPresenter(size_t frameBytes, bool genesis = false) {
  // Pausing keeps this game's buffer. Quitting releases it before switching
  // systems, so the next game can allocate exactly the size it needs.
  if (!videoPresenterBuffer) {
    if (genesis) {
      videoPresenterBuffer = static_cast<uint16_t *>(heap_caps_calloc(
          1, frameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    } else {
      videoPresenterBuffer = static_cast<uint16_t *>(heap_caps_malloc_prefer(
          frameBytes, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!videoPresenterBuffer) {
      return false;
    }
  }
  if (!videoPresenterTaskHandle) {
    videoPresenterBufferAvailable = xSemaphoreCreateBinary();
    const bool taskStarted = videoPresenterBufferAvailable &&
        xTaskCreatePinnedToCore(videoPresenterTask, "game-video", kVideoPresenterStackBytes,
                                nullptr, 2, &videoPresenterTaskHandle, 0) == pdPASS;
    if (!taskStarted) {
      if (videoPresenterBufferAvailable) {
        vSemaphoreDelete(videoPresenterBufferAvailable);
        videoPresenterBufferAvailable = nullptr;
      }
      free(videoPresenterBuffer);
      videoPresenterBuffer = nullptr;
      return false;
    }
    xSemaphoreGive(videoPresenterBufferAvailable);
  }
  asyncVideo = true;
  return true;
}

// Queue one copied frame without waiting. Return false when the presenter still owns the buffer.
bool submitVideoFrame(const void *pixels) {
  if (xSemaphoreTake(videoPresenterBufferAvailable, 0) != pdTRUE) {
    return false;
  }
  presenterCartridgeFrame = {};
  memcpy(videoPresenterBuffer, pixels, gameIsGba ? kGbaFrameBytes : kGbFrameBytes);
  presenterFrameIsGba = gameIsGba;
  presenterGbaFullscreen = gameGbaFullscreen();
  presenterGbScale = gameGbScale();
  xTaskNotifyGive(videoPresenterTaskHandle);
  return true;
}

// Transfer Genesis framebuffer ownership or copy the output from other cartridge cores.
bool submitCartridgeFrame(const CartridgeFrame &frame) {
  if ((!frame.rgb565 && (!frame.pixels || !frame.palette)) || frame.height <= 0 || frame.width <= 0 ||
      frame.pitch < frame.width) return false;
  const size_t pixels = size_t(frame.height - 1) * frame.pitch + frame.width;
  if (pixels > (frame.rgb565 ? kNativeCartridgeBytes / sizeof(uint16_t) : kIndexedMaxPixelBytes) ||
      xSemaphoreTake(videoPresenterBufferAvailable, 0) != pdTRUE) return false;
  presenterCartridgeFrame = frame;
  if (activeCartridgeCore == &pocketGenesis) {
    // The semaphore guarantees that the old front buffer is free for rendering.
    videoPresenterBuffer = pocketGenesisSwapVideoBuffer(videoPresenterBuffer);
  } else if (frame.rgb565) {
    memcpy(videoPresenterBuffer, frame.rgb565, pixels * sizeof(uint16_t));
    presenterCartridgeFrame.rgb565 = videoPresenterBuffer;
  } else {
    auto *indexed = reinterpret_cast<uint8_t *>(videoPresenterBuffer);
    auto *palette = reinterpret_cast<uint16_t *>(indexed + kIndexedMaxPixelBytes);
    memcpy(indexed, frame.pixels, pixels);
    memcpy(palette, frame.palette, kIndexedPaletteBytes);
    presenterCartridgeFrame.pixels = indexed;
    presenterCartridgeFrame.palette = palette;
  }
  presenterGbScale = gameGbScale();
  xTaskNotifyGive(videoPresenterTaskHandle);
  return true;
}

// Accept Gnuboy's video callback and use synchronous or task-based display as configured.
void presentFrame(void *pixels) {
  if (asyncVideo) {
    // Leave a slow TFT transfer on the other core and keep audio on schedule.
    submitVideoFrame(pixels);
  } else {
    Screen::drawGameFrame(pixels, gameGbScale());
  }
}

// Wait for an in-flight transfer and release the GBA DMA backend before drawing menus.
void stopVideoPresenter() {
  if (asyncVideo) {
    // Drain the display task before returning SPI2 to the launcher.
    xSemaphoreTake(videoPresenterBufferAvailable, portMAX_DELAY);
    xSemaphoreGive(videoPresenterBufferAvailable);
  }
  asyncVideo = false;
  Screen::endGbaVideo();
}

// Initialize Gnuboy when a GB or GBC game opens. Keep its frame out of the launcher heap.
bool ensureGbEmulator() {
  static bool initialized = false;
  if (!initialized) {
    if (gnuboy_init(kAudioRate, GB_AUDIO_STEREO_S16, GB_PIXEL_565_LE, presentFrame, playAudio) != 0) {
      return false;
    }
    gnuboy_set_soundbuffer(samples, std::size(samples));
    initialized = true;
  }
  if (!framebuffer) {
    framebuffer = static_cast<uint16_t *>(heap_caps_malloc_prefer(
        kGbFrameBytes, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!framebuffer) return false;
  }
  gnuboy_set_framebuffer(framebuffer);
  return true;
}

// Detach and free the Game Boy frame buffer before leaving the game.
void releaseGbFramebuffer() {
  if (framebuffer) {
    free(framebuffer);
    framebuffer = nullptr;
    gnuboy_set_framebuffer(nullptr);
  }
}

// Choose whether this GBA frame renders based on pacing debt and the configured skip limit.
bool shouldDrawGbaFrame(int64_t frameDebt) {
  if (gameFrameskipMode() == 1) {  // NONE
    return true;
  }
  // AUTO tolerates a tiny scheduling jitter. LIGHT and HIGH react as soon as
  // a deadline is missed, avoiding the long stutter caused by late skips.
  const int64_t tolerance = gameFrameskipMode() == 0 ? kAutoFrameDebtUs : 0;
  return frameDebt <= tolerance || gbaSkipped >= gbaFrameskipLimit();
}

// Use adaptive skipping in Auto or a fixed cadence in Light and High.
bool shouldDrawGenesisFrame(int64_t debtUs) {
  switch (genesisFrameskipMode) {
    case 0:  // AUTO
      return genesisSkipped >= 1 ||
             (debtUs <= kAutoFrameDebtUs &&
              (!asyncVideo || uxSemaphoreGetCount(videoPresenterBufferAvailable) != 0));
    case 2:  // LIGHT
      return genesisSkipped >= 1;
    case 3:  // HIGH
      return genesisSkipped >= 3;
    default:  // NONE
      return true;
  }
}

// Apply the shared volume gain in place to signed PCM samples using fixed-point scaling.
void applyAudioVolume(int16_t *pcm, size_t count) {
  const int volume = volumePercent.load(std::memory_order_relaxed);
  if (volume == 100) {
    return;
  }
  // Compute a Q15 multiplier per batch to avoid division in the sample loop.
  constexpr int32_t unity = 1 << 15;
  const int32_t gain = (volume * unity + 50) / 100;
  for (size_t i = 0; i < count; ++i) {
    pcm[i] = static_cast<int16_t>((pcm[i] * gain + unity / 2) >> 15);
  }
}

// Send a Gnuboy callback's PCM to I2S when game sound and the driver are ready.
void playAudio(void *buffer, size_t count) {
  if (gameSoundEnabled() && audioReady) {
    applyAudioVolume(static_cast<int16_t *>(buffer), count);
    audio.write(static_cast<const uint8_t *>(buffer), count * sizeof(int16_t));
  }
}

// Scale playback PCM and publish coarse stereo peaks for meters and reactive lighting.
void applyMusicVolume(int16_t *pcm, size_t count) {
  applyAudioVolume(pcm, count);
  unsigned left = 0;
  unsigned right = 0;
  for (size_t i = 0; i + 1 < count; i += 8) {
    left = std::max(left, unsigned(abs(int(pcm[i]))));
    right = std::max(right, unsigned(abs(int(pcm[i + 1]))));
  }
  const uint16_t packed = std::min(left >> 5, 255u) | (std::min(right >> 5, 255u) << 8);
  musicAudioLevels.store(packed, std::memory_order_relaxed);
}

// Service the Music decoder and I2S on the other core until asked to stop or playback ends.
void backgroundMusicPump(void *) {
  // The main loop joins this task before changing the decoder, SD mount, or I2S ownership.
  while (!backgroundMusicStopRequested.load()) {
    const auto result = musicPlayer.update(audio, applyMusicVolume, musicPaused);
    if (result != MusicPlayer::Result::Playing) {
      backgroundMusicResult.store(result);
      break;
    }
    vTaskDelay(1);
  }
  xSemaphoreGive(backgroundMusicStopped);
  vTaskDelete(nullptr);
}

// Start the existing playback task when audio is active and background or temporary use is allowed.
void startBackgroundMusicPump(bool temporary) {
  if (backgroundMusicTask || backgroundMusicStartFailed || (!backgroundMusic && !temporary) || !musicOwnsAudio ||
      !musicPlayer.isOpen() || (musicPaused && !musicPlayer.isSeeking()) ||
      backgroundMusicResult.load() != MusicPlayer::Result::Playing) {
    return;
  }
  if (!backgroundMusicStopped) {
    backgroundMusicStopped = xSemaphoreCreateBinary();
  }
  backgroundMusicStopRequested.store(false);
  const BaseType_t core = portNUM_PROCESSORS > 1 ? 1 - xPortGetCoreID() : 0;
  if (!backgroundMusicStopped ||
      xTaskCreatePinnedToCore(backgroundMusicPump, "music-audio", kBackgroundMusicStackBytes,
                              nullptr, 2, &backgroundMusicTask, core) != pdPASS) {
    backgroundMusicStartFailed = true;
    Serial.println("Background music task unavailable; using the main loop.");
  }
}

// Join the audio task before changing shared decoder state, card mounts, or I2S ownership.
void stopBackgroundMusicPump() {
  if (backgroundMusicTask) {
    backgroundMusicStopRequested.store(true);
    xSemaphoreTake(backgroundMusicStopped, portMAX_DELAY);
    backgroundMusicTask = nullptr;
  }
  backgroundMusicStartFailed = false;
}

// Release the I2S driver and handle a channel left disabled by a failed clock change.
void resetAudioHardware() {
  if (audioReady) {
    if (!audio.end()) {
      // A failed clock change can leave the channel disabled. ESP_I2S end()
      // expects a running channel when it releases the pins and DMA buffers.
      i2s_channel_enable(audio.txChan());
      audio.end();
    }
    audioReady = false;
  }
}

// Give Music the shared I2S output and remember whether another feature had already initialized it.
bool acquireMusicAudio() {
  if (!musicOwnsAudio) {
    audioWasReadyBeforeMusic = audioReady;
  }
  if (!ensureAudioHardware()) {
    return false;
  }
  musicOwnsAudio = true;
  return true;
}

// Join background playback and restore I2S to the state needed by emulation and settings.
void releaseMusicAudio() {
  stopBackgroundMusicPump();
  if (musicRgbWasActive) {
    musicRgbWasActive = false;
    applyFlashlightLed(millis(), false);
  }
  if (!musicOwnsAudio) {
    return;
  }
  musicOwnsAudio = false;
  if (audioReady && (backgroundMusicResult.load() == MusicPlayer::Result::OutputError ||
                     !audioWasReadyBeforeMusic || audio.txSampleRate() != kAudioRate)) {
    resetAudioHardware();
  }
  if (audioWasReadyBeforeMusic || soundEnabled) {
    ensureAudioHardware();
  }
}

// Browse installed app packages or select a package staged for installation.
struct ListedApp {
  String path;
  AppPackage::Info info;
};
std::vector<ListedApp> listedApps;
constexpr int kMaxListedApps = 96;
constexpr int kVisibleAppRows = 6;
bool installingApplication = false;
int appSelection = 0;
int appFirst = 0;
unsigned appToneFrames = 0;
unsigned appToneFrequency = 0;
unsigned appTonePhase = 0;
bool appUsedAudio = false;

void scanAppDirectory(const char *path) {
  File folder = SD.open(path, FILE_READ);
  if (!folder || !folder.isDirectory()) return;
  unsigned scanned = 0;
  while (listedApps.size() < kMaxListedApps) {
    File entry = folder.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory() && AppPackage::supportedName(entry.name())) {
      ListedApp package{entry.path(), {}};
      entry.close();
      if (AppPackage::inspect(package.path.c_str(), package.info)) listedApps.push_back(std::move(package));
    }
    if (++scanned % 32 == 0) delay(1);
  }
}

void drawApplications(bool full = true) {
  const uint16_t accent = homeItemAccent(kApplicationsMenuItem);
  if (full) {
    drawHeader(installingApplication ? "INSTALL APP" : "APPLICATIONS",
               installingApplication ? "A: INSTALL   B / MENU: BACK" : "A: OPEN   SELECT: RESCAN   B: HOME", accent);
  }
  if (listedApps.empty()) {
    drawCard(8, 58, 304, 65, false, accent);
    const char *empty = installingApplication ? "No app packages found." : "No apps installed.";
    Screen::text(18, 72, sdReady ? empty : "Insert a microSD card.", Screen::theme.text, Screen::theme.panel);
    Screen::text(18, 99, installingApplication ? "Copy .ppapp to /app-install." : "Go to About > Install Application.",
                 Screen::theme.muted, Screen::theme.panel);
    return;
  }
  for (int row = 0; row < kVisibleAppRows && appFirst + row < int(listedApps.size()); ++row) {
    const int index = appFirst + row;
    const bool selected = index == appSelection;
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    const int y = 52 + row * 26;
    drawCard(8, y, 304, 23, selected, accent);
    Screen::text(18, y + 8, listedApps[index].info.title, selected ? Screen::theme.text : Screen::theme.muted, bg);
    Screen::text(292, y + 8, ">", accent, bg);
  }
}

void openApplications(bool installer) {
  installingApplication = installer;
  listedApps.clear();
  appSelection = appFirst = 0;
  if (sdReady) {
    stopBackgroundMusicPump();
    scanAppDirectory(installer ? AppPackage::inbox : AppPackage::directory);
    if (installer) scanAppDirectory("/");
    std::sort(listedApps.begin(), listedApps.end(), [](const ListedApp &a, const ListedApp &b) {
      return strcasecmp(a.info.title, b.info.title) < 0;
    });
  }
  view = View::Applications;
  drawApplications();
}

bool queueAppTone(unsigned frequency, unsigned duration) {
  if (!soundEnabled || appToneFrames || !ensureAudioHardware() || audio.txSampleRate() != kAudioRate) return false;
  appUsedAudio = true;
  appToneFrequency = frequency;
  appTonePhase = 0;
  appToneFrames = kAudioRate * duration / 1000;
  return true;
}

void serviceAppTone() {
  if (!appToneFrames || !audioReady) return;
  int16_t pcm[240 * 2];
  const unsigned frames = std::min(appToneFrames, unsigned(std::size(pcm) / 2));
  unsigned phase = appTonePhase;
  for (unsigned i = 0; i < frames; ++i) {
    const int step = phase * 256 / kAudioRate;
    pcm[i * 2] = pcm[i * 2 + 1] = int16_t((step < 128 ? step : 255 - step) * 32 - 2048);
    phase = (phase + appToneFrequency) % kAudioRate;
  }
  applyAudioVolume(pcm, frames * 2);
  size_t written = 0;
  const esp_err_t result = i2s_channel_write(audio.txChan(), pcm, frames * 2 * sizeof(*pcm), &written, 0);
  const unsigned sent = written / (2 * sizeof(*pcm));
  appToneFrames -= sent;
  appTonePhase = (appTonePhase + sent * appToneFrequency) % kAudioRate;
  if (result != ESP_OK && result != ESP_ERR_TIMEOUT) appToneFrames = 0;
}

void drawAppFooter() {
  Screen::fill(0, AppSdk::height, Screen::width, Screen::height - AppSdk::height, Screen::theme.header);
  Screen::text(10, 228, "MENU: APPLICATIONS", Screen::theme.muted, Screen::theme.header);
}

void closeApplication(bool failed) {
  if (failed) Serial.printf("Application: %s\n", AppRuntime::error());
  AppRuntime::stop();
  appToneFrames = 0;
  if (appUsedAudio) {
    resetAudioHardware();
    if (soundEnabled) ensureAudioHardware();
    appUsedAudio = false;
  }
  drawApplications();
  if (failed) showStatus("App stopped. See Serial Monitor.");
}

void handleApplications(uint16_t pressed, uint16_t held, uint32_t now) {
  if (AppRuntime::running()) {
    if (pressed & kMenuKey) closeApplication(false);
    else if (!AppRuntime::tick(now, {held, pressed})) closeApplication(true);
    else serviceAppTone();
    return;
  }
  if (pressed & (GB_PAD_B | kMenuKey)) {
    listedApps.clear();
    if (installingApplication) {
      view = View::AboutMenu;
      drawAboutMenu();
    } else {
      view = View::SystemMenu;
      drawSystemMenu(true);
    }
    return;
  }
  if (pressed & GB_PAD_SELECT) {
    openApplications(installingApplication);
    return;
  }
  const int count = int(listedApps.size());
  if (!count) return;
  if (pressed & kListScrollButtons) {
    appSelection = (appSelection + (pressed & GB_PAD_UP ? count - 1 : 1)) % count;
    appFirst = std::clamp(appFirst, std::max(0, appSelection - kVisibleAppRows + 1),
                         std::min(appSelection, std::max(0, count - kVisibleAppRows)));
    drawApplications(false);
    return;
  }
  if (!(pressed & (GB_PAD_A | GB_PAD_START))) return;

  const auto &selected = listedApps[appSelection];
  if (installingApplication) {
    stopBackgroundMusicPump();
    const char *path = selected.path.c_str();
    const char *name = strrchr(path, '/');
    using Result = AppPackage::Result;
    switch (AppPackage::install(path, name ? name + 1 : path)) {
      case Result::Installed: showStatus("Installed. Open Applications on Home."); break;
      case Result::Invalid: showStatus("Invalid app or SDK version."); break;
      case Result::Exists: showStatus("An app with this ID is installed."); break;
      case Result::StorageError: showStatus("Could not save the app to microSD."); break;
    }
  } else {
    releaseMusicAudio();
    menuStatusVisible = false;
    if (AppRuntime::start(selected.path.c_str(), queueAppTone)) drawAppFooter();
    else closeApplication(true);
  }
}

// Close playback, clear its meter and sleep timer, and retain a status message.
void closeMusicTrack(const char *message) {
  releaseMusicAudio();
  musicPlayer.close();
  musicAudioLevels.store(0, std::memory_order_relaxed);
  musicSleepOption = 0;
  backgroundMusicResult.store(MusicPlayer::Result::Playing);
  musicPaused = false;
  musicMessage = message;
}

// Identify Music views, including the screen-off state, that share the Music session.
bool inMusicApp() {
  return view == View::Music || view == View::NowPlaying ||
         view == View::MusicSettings || view == View::MusicScreenOff;
}

// Update playback indicators on the current Music view or on Home with background playback.
void refreshMusicView() {
  if (view == View::NowPlaying) {
    drawNowPlaying();
  } else if (view == View::Music) {
    drawMusicStatus();
    drawMusicEntries();
  } else if (view == View::MusicSettings) {
    drawMusicSettingsRows(int(MusicSetting::SleepTimer), 1);
  } else if (view == View::SystemMenu && backgroundMusic) {
    redrawHomeMusic();
  }
}

// Close playback and discard the playlist, folder indexes, and pending scan.
void clearMusicSession() {
  cancelMusicFolderScan();
  closeMusicTrack(nullptr);
  musicPlaylist.reset();
  musicFolder.reset();
  musicFolderMessage = nullptr;
  musicTrack = -1;
  musicTrackTitle = "";
  musicTitleView = View::Failed;
  musicSelection = 0;
}

// Stop the active track and refresh the view, preserving the in-app queue.
void stopMusicPlayback(const char *message) {
  if (inMusicApp()) {
    closeMusicTrack(message);
    refreshMusicView();
  } else {
    clearMusicSession();
    refreshMusicView();
  }
}

// Open a queue entry, reset playback state, and acquire I2S for the decoder.
void startMusicTrack(int index, bool flushAudio) {
  stopBackgroundMusicPump();
  backgroundMusicResult.store(MusicPlayer::Result::Playing);
  if (flushAudio && musicOwnsAudio) {
    resetAudioHardware();
  }
  musicTrack = index;
  musicTrackTitle = fileTitle(musicPlaylist->tracks[index]);
  musicTitleView = View::Failed;
  musicPaused = false;
  musicAudioLevels.store(0, std::memory_order_relaxed);
  const String path = musicPlaylist->path + "/" + musicPlaylist->tracks[index];
  musicMessage = musicPlayer.open(path.c_str());
  if (musicMessage) {
    stopMusicPlayback(musicMessage);
  } else if (!acquireMusicAudio()) {
    stopMusicPlayback("I2S unavailable");
  } else {
    refreshMusicView();
  }
}

// Enter the playback controls with its main transport button selected.
void focusNowPlaying() {
  view = View::NowPlaying;
  musicTransportSelection = int(MusicControl::PlayPause);
  musicOptionsSelection = int(MusicControl::VolumeDown);
  musicOptionsFocused = false;
}

// Open the selected track or show the current one and its controls without reopening it.
void openNowPlaying(int track) {
  focusNowPlaying();
  if (musicFolder == musicPlaylist && track == musicTrack && musicPlayer.isOpen()) {
    drawNowPlaying();
  } else {
    musicPlaylist = musicFolder;
    startMusicTrack(track, true);
  }
  Serial.println("\nNow Playing. W/A/S/D moves focus, J uses the control, I plays/pauses, U restarts, M opens options, K returns to the library.");
}

// Refresh only the playback-dependent controls and status in the current Music view.
void redrawMusicPlayback() {
  if (view == View::NowPlaying) {
    drawNowPlayingStatus();
    drawMusicControl(int(MusicControl::PlayPause));
  } else if (view == View::Music) {
    drawMusicStatus();
  }
}

// Pause or resume the active decoder, reopening the selected track if playback had stopped.
void toggleMusicPlayback() {
  if (!musicPlayer.isOpen()) {
    startMusicTrack(musicTrack, true);
    return;
  }
  musicPaused = !musicPaused;
  redrawMusicPlayback();
}

// Reset buffered audio before the first seek and reacquire Music's I2S output.
bool prepareMusicSeek() {
  if (!musicPlayer.isSeeking()) {
    resetAudioHardware();
    if (!acquireMusicAudio()) {
      stopMusicPlayback("I2S unavailable");
      return false;
    }
  }
  return true;
}

// Request a relative jump and refresh the playback position indicator.
void seekMusicTrack(int seconds) {
  if (musicPlayer.isOpen() && prepareMusicSeek()) {
    musicPlayer.seekBy(seconds);
    drawNowPlayingStatus();
  }
}

// Rewind the active track and resume playback, or reopen a stopped queue entry.
void restartMusicTrack() {
  if (!musicPlayer.isOpen()) {
    startMusicTrack(musicTrack, true);
  } else if (prepareMusicSeek()) {
    musicPaused = false;
    musicPlayer.seekTo(0);
    redrawMusicPlayback();
  }
}

// Toggle and persist repeating the current track.
void toggleMusicRepeat() {
  repeatMusicTrack = !repeatMusicTrack;
  settingsStore.putBool("musicRepeat", repeatMusicTrack);
}

// Route transport button activation and focus navigation on the Now Playing screen.
void handleNowPlaying(uint16_t pressed) {
  if (pressed & GB_PAD_A) {
    const auto control = static_cast<MusicControl>(focusedMusicControl());
    switch (control) {
      case MusicControl::Previous:
        if (musicTrack > 0) startMusicTrack(musicTrack - 1, true);
        break;
      case MusicControl::Backward: seekMusicTrack(-10); break;
      case MusicControl::PlayPause: toggleMusicPlayback(); break;
      case MusicControl::Forward: seekMusicTrack(10); break;
      case MusicControl::Next:
        if (musicTrack + 1 < int(musicPlaylist->tracks.size())) startMusicTrack(musicTrack + 1, true);
        break;
      case MusicControl::Repeat:
        toggleMusicRepeat();
        drawMusicControl(int(MusicControl::Repeat));
        break;
      case MusicControl::VolumeDown:
      case MusicControl::VolumeUp:
        adjustVolume(control == MusicControl::VolumeDown ? -1 : 1);
        drawNowPlayingStatus();
        break;
    }
  } else {
    const int before = focusedMusicControl();
    if (pressed & GB_PAD_UP) {
      musicOptionsFocused = false;
    } else if (pressed & GB_PAD_DOWN) {
      musicOptionsFocused = true;
    } else if (pressed & (GB_PAD_LEFT | GB_PAD_RIGHT)) {
      const int step = pressed & GB_PAD_LEFT ? -1 : 1;
      if (musicOptionsFocused) {
        musicOptionsSelection = std::clamp(musicOptionsSelection + step,
                                           int(MusicControl::VolumeDown), int(MusicControl::Repeat));
      } else {
        musicTransportSelection = std::clamp(musicTransportSelection + step,
                                             int(MusicControl::Previous), int(MusicControl::Next));
      }
    }
    const int after = focusedMusicControl();
    if (before != after) {
      drawMusicControl(before);
      drawMusicControl(after);
    }
  }
}

// Return to Home, retaining playback only when requested and a track is open.
void leaveMusic(bool keepPlaying) {
  if (!keepPlaying || !musicPlayer.isOpen()) {
    clearMusicSession();
  } else {
    cancelMusicFolderScan();
    musicFolder.reset();
    musicFolderMessage = nullptr;
  }
  view = View::SystemMenu;
  startBackgroundMusicPump(false);
  drawSystemMenu(true);
}

// Pick the next queue index from repeat, shuffle, or in-order playback settings.
int nextMusicTrack() {
  if (repeatMusicTrack) {
    return musicTrack;
  }
  const int count = int(musicPlaylist->tracks.size());
  if (musicOrder == MusicOrder::Shuffle && count > 1) {
    const int pick = int(random(count - 1));
    return pick + (pick >= musicTrack);
  }
  if (musicOrder == MusicOrder::InOrder && musicTrack + 1 < count) {
    return musicTrack + 1;
  }
  return -1;
}

// Detect a paused game even while one of its settings or diagnostic pages is open.
bool hasPausedGame() {
  return view == View::Paused ||
         ((view == View::Settings || view == View::AboutMenu || view == View::About ||
           view == View::Diagnostics || view == View::Update || view == View::DeviceTools || view == View::Bluetooth ||
           (view == View::Applications && installingApplication)) &&
          settingsReturnView == View::Paused);
}

// Feed I2S and advance playback or handle decoder errors when the emulator is idle.
void serviceMusic() {
  if (!musicPlayer.isOpen() || view == View::Playing || view == View::Failed || hasPausedGame() || AppRuntime::running()) {
    return;
  }
  auto result = backgroundMusicResult.load();
  if (backgroundMusicTask) {
    if (result == MusicPlayer::Result::Playing) {
      return;
    }
    stopBackgroundMusicPump();
  }
  if (result == MusicPlayer::Result::Playing) {
    if ((!musicPaused || musicPlayer.isSeeking()) && !musicOwnsAudio && !acquireMusicAudio()) {
      stopMusicPlayback("I2S unavailable");
      return;
    }
    if (!inMusicApp()) {
      startBackgroundMusicPump(false);
      if (backgroundMusicTask) {
        return;
      }
    }
    result = musicPlayer.update(audio, applyMusicVolume, musicPaused);
  }
  if (result == MusicPlayer::Result::Playing) {
    if (view == View::NowPlaying) {
      musicPlayer.updateDuration();
      if (musicDisplayedSecond != musicElapsedSeconds() ||
          musicDisplayedDuration != musicPlayer.durationMs() ||
          musicWasSeeking != musicPlayer.isSeeking()) {
        drawNowPlayingStatus();
      }
    }
    return;
  }
  if (result == MusicPlayer::Result::Finished) {
    const int next = nextMusicTrack();
    if (next >= 0) {
      startMusicTrack(next, false);
      return;
    }
  }
  if (result == MusicPlayer::Result::OutputError) {
    if (musicOwnsAudio) {
      resetAudioHardware();
    }
    stopMusicPlayback("I2S playback error");
  } else if (result == MusicPlayer::Result::BadTrack) {
    stopMusicPlayback("Could not decode track");
  } else {
    stopMusicPlayback(musicOrder == MusicOrder::StopAfterTrack ? "End of track" : "End of library");
  }
}

// Rebuild the active Music panel after leaving options or waking the display.
void redrawMusicScreen() {
  switch (view) {
    case View::Music: drawMusicLibrary(); break;
    case View::NowPlaying: drawNowPlaying(); break;
    case View::MusicSettings: drawMusicSettings(); break;
    default: break;
  }
}

// Turn off the TFT while preserving the previous Music screen and audio session.
void sleepMusicScreen() {
  musicLastInputAt = millis();
  if (Screen::sleepPanel()) {
    musicScreenReturnView = view;
    musicWakeRequested = false;
    view = View::MusicScreenOff;
  }
}

// Wake the TFT on input and restore the previous Music screen after the wake delay.
void handleMusicScreenOff(uint16_t pressed, bool rescan) {
  if (pressed || rescan) {
    musicWakeRequested = true;
    Screen::wakePanel();
  }
  if (musicWakeRequested && Screen::finishPanelWake()) {
    musicWakeRequested = false;
    musicLastInputAt = millis();
    view = musicScreenReturnView;
    redrawMusicScreen();
  }
}

// End the Music session, remount the card, and rescan the previously browsed folder if available.
void rescanMusicLibrary() {
  const String path = musicFolder ? musicFolder->path : String(kMusicRoot);
  clearMusicSession();
  scanCard();
  loadMusicLibrary(path);
  view = View::Music;
  drawMusicLibrary();
}

// Return to the Music view that opened Options.
void closeMusicSettings() {
  view = musicSettingsReturnView;
  redrawMusicScreen();
}

// Apply or dispatch the selected audio, navigation, visualizer, and display options.
void handleMusicSettings(uint16_t pressed) {
  if (pressed & (GB_PAD_UP | GB_PAD_DOWN)) {
    const int previous = musicSettingsSelection;
    const int step = pressed & GB_PAD_UP ? kMusicSettingCount - 1 : 1;
    musicSettingsSelection = (musicSettingsSelection + step) % kMusicSettingCount;
    drawMusicSettingsRows(previous, 1);
    drawMusicSettingsRows(musicSettingsSelection, 1);
    return;
  }
  if (!(pressed & (GB_PAD_A | GB_PAD_LEFT | GB_PAD_RIGHT))) {
    return;
  }
  const bool activate = pressed & GB_PAD_A;
  const int direction = pressed & GB_PAD_LEFT ? -1 : 1;
  switch (static_cast<MusicSetting>(musicSettingsSelection)) {
    case MusicSetting::Background:
      backgroundMusic = !backgroundMusic;
      settingsStore.putBool("musicBg", backgroundMusic);
      if (!musicHasParent()) drawMusicSettingsRows(int(MusicSetting::Navigate), 1);
      break;
    case MusicSetting::ScreenOff:
      if (activate) sleepMusicScreen();
      return;
    case MusicSetting::ScreenTimeout: {
      const int count = int(std::size(kMusicScreenTimeouts));
      musicScreenTimeout = (musicScreenTimeout + count + direction) % count;
      settingsStore.putUChar("musicScreen", musicScreenTimeout);
      break;
    }
    case MusicSetting::Volume:
      adjustVolume(direction);
      break;
    case MusicSetting::Repeat:
      toggleMusicRepeat();
      break;
    case MusicSetting::Order: {
      constexpr int count = int(MusicOrder::StopAfterTrack) + 1;
      musicOrder = static_cast<MusicOrder>((int(musicOrder) + count + direction) % count);
      settingsStore.putUChar("musicOrder", uint8_t(musicOrder));
      break;
    }
    case MusicSetting::Visualizer:
      musicVisualizerEnabled = !musicVisualizerEnabled;
      settingsStore.putBool("musicMeter", musicVisualizerEnabled);
      break;
    case MusicSetting::ReactiveRgb:
      musicReactiveRgb = !musicReactiveRgb;
      settingsStore.putBool("musicRgb", musicReactiveRgb);
      break;
    case MusicSetting::SleepTimer: {
      constexpr int count = std::size(kMusicSleepDelays);
      musicSleepOption = (musicSleepOption + direction + count) % count;
      musicSleepStartedAt = millis();
      break;
    }
    case MusicSetting::Stop:
      if (activate) stopMusicPlayback("Stopped");
      return;
    case MusicSetting::Rescan:
      if (activate) rescanMusicLibrary();
      return;
    case MusicSetting::Navigate:
      if (!activate) return;
      if (musicHasParent()) {
        view = View::Music;
        openMusicFolderEntry(0, true);
      } else if (backgroundMusic) {
        leaveMusic(true);
      } else {
        closeMusicSettings();
      }
      return;
    case MusicSetting::Exit:
      if (activate) leaveMusic(false);
      return;
  }
  drawMusicSettingsRows(musicSettingsSelection, 1);
}

// Show the wired inputs and update highlighted buttons as their held state changes.
void drawButtonTest(uint16_t held, bool all) {
  constexpr const char *labels[] = {"RIGHT", "LEFT", "UP", "DOWN", "A", "B",
                                    "SELECT", "START", "MENU", "L", "R"};
  static_assert(std::size(labels) == std::size(kButtons));
  for (int i = 0; i < int(std::size(kButtons)); ++i) {
    const auto &button = kButtons[i];
    if (!all && !((held ^ testedButtons) & button.mask)) continue;
    const bool down = held & button.mask;
    const int x = 6 + (i % 3) * 104;
    const int y = 55 + (i / 3) * 36;
    const uint16_t bg = down ? Screen::theme.selected : Screen::theme.panel;
    drawCard(x, y, 100, 31, down, Screen::theme.accent);
    Screen::text(x + 10, y + 6, labels[i], down ? Screen::theme.text : Screen::theme.muted, bg);
    char pin[13];
    if (kAnalogDpad && i < 4) {
      snprintf(pin, sizeof(pin), "ADC %d", i < 2 ? kDown : kUp);
    } else if (button.pin < 0) {
      snprintf(pin, sizeof(pin), "UNWIRED");
    } else {
      snprintf(pin, sizeof(pin), "GPIO %d", button.pin);
    }
    Screen::text(x + 10, y + 18, pin, down ? Screen::theme.accent : Screen::theme.muted, bg);
  }
  testedButtons = held;
}

// Show calibrated battery millivolts separately from the smoothed charge estimate.
void drawPowerTestReadings() {
  if (refreshBatteryReading()) drawBatteryIndicator(Screen::theme.header);
  char voltage[32] = "Voltage now: unavailable";
  const uint32_t millivolts = batteryGauge.latestMillivolts();
  if (millivolts) snprintf(voltage, sizeof(voltage), "Voltage now: %u mV", unsigned(millivolts));

  char charge[25] = "Charge estimate: --%";
  const int percent = batteryGauge.percent();
  if (percent >= 0) snprintf(charge, sizeof(charge), "Charge estimate: %d%%", percent);
  Screen::fill(8, 61, 304, 31, Screen::theme.panel);
  Screen::text(16, 66, voltage, Screen::theme.text, Screen::theme.panel);
  Screen::text(16, 79, charge, Screen::theme.muted, Screen::theme.panel);
  toolReadingDrawnAt = millis();
}

// Format a byte count in binary units with two fractional digits for larger values.
void formatStorageCapacity(uint64_t bytes, char *out, size_t outSize) {
  constexpr const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  uint64_t unit = 1;
  size_t index = 0;
  while (index + 1 < std::size(units) && bytes / unit >= 1024) {
    unit *= 1024;
    ++index;
  }
  if (index == 0) {
    snprintf(out, outSize, "%llu B", static_cast<unsigned long long>(bytes));
  } else {
    snprintf(out, outSize, "%llu.%02u %s", static_cast<unsigned long long>(bytes / unit),
             unsigned(bytes % unit * 100 / unit), units[index]);
  }
}

// Draw the diagnostic menu or active test, including SD filesystem free space.
void drawDeviceTools() {
  if (toolScreen == ToolScreen::Display) {
    constexpr uint16_t colors[] = {0xF800, 0x07E0, 0x001F, Screen::white, Screen::black};
    Screen::fill(0, 0, Screen::width, Screen::height, colors[testColor]);
    return;
  }
  if (toolScreen == ToolScreen::Buttons) {
    drawHeader("BUTTON TEST", "HOLD MENU OR B TO RETURN", Screen::theme.accent);
    drawButtonTest(testedButtons, true);
    Screen::text(12, 224, "PRESS BUTTONS TO CHECK THE WIRING", Screen::theme.muted, Screen::theme.background);
    return;
  }
  if (toolScreen == ToolScreen::Battery) {
    drawHeader("BATTERY / SD", "B / MENU: DEVICE TOOLS", Screen::theme.accent);
    drawPowerTestReadings();
    char info[35];
    if (sdReady) {
      char capacity[24];
      formatStorageCapacity(SD.cardSize(), capacity, sizeof(capacity));
      snprintf(info, sizeof(info), "Total capacity: %.19s", capacity);
      Screen::text(16, 107, info, Screen::theme.text, Screen::theme.background);

      uint64_t volumeBytes = 0;
      uint64_t freeBytes = 0;
      const char *mountPoint = SD.mountpoint();
      if (mountPoint && esp_vfs_fat_info(mountPoint, &volumeBytes, &freeBytes) == ESP_OK &&
          freeBytes <= volumeBytes) {
        formatStorageCapacity(freeBytes, capacity, sizeof(capacity));
        snprintf(info, sizeof(info), "Remaining: %.19s", capacity);
      } else {
        snprintf(info, sizeof(info), "Remaining: unavailable");
      }
      Screen::text(16, 126, info, Screen::theme.text, Screen::theme.background);
    } else {
      Screen::text(16, 107, "microSD: not mounted", Screen::theme.text, Screen::theme.background);
    }
    snprintf(info, sizeof(info), "%u GB / %u GBC / %u GBA ROMS",
             unsigned(gamesFor(System::GameBoy).size()), unsigned(gamesFor(System::GameBoyColor).size()),
             unsigned(gamesFor(System::GameBoyAdvance).size()));
    Screen::text(16, 145, info, Screen::theme.muted, Screen::theme.background);
    snprintf(info, sizeof(info), "%u NES / %u SMS / %u GG ROMS",
             unsigned(gamesFor(System::Nes).size()), unsigned(gamesFor(System::MasterSystem).size()),
             unsigned(gamesFor(System::GameGear).size()));
    Screen::text(16, 164, info, Screen::theme.muted, Screen::theme.background);
    snprintf(info, sizeof(info), "%u GENESIS ROMS", unsigned(gamesFor(System::Genesis).size()));
    Screen::text(16, 183, info, Screen::theme.muted, Screen::theme.background);
    return;
  }
  if (toolScreen == ToolScreen::Led) {
    drawHeader("RGB LED TEST", "A: NEXT COLOR    B / MENU: BACK", Screen::theme.accent);
    constexpr const char *colorNames[] = {"RED", "GREEN", "BLUE", "WHITE", "OFF"};
    Screen::text(20, 67, colorNames[testColor], Screen::theme.text, Screen::theme.background, 2);
    Screen::text(20, 110, flashlightPinAvailable() ? "CHECK THE ONBOARD LED" : "RGB PIN UNAVAILABLE",
                 Screen::theme.muted, Screen::theme.background);
    return;
  }
  drawHeader("DEVICE TOOLS", "A: RUN    B / MENU: SETTINGS", Screen::theme.accent);
  constexpr const char *labels[] = {"BUTTON TEST", "DISPLAY COLORS", "SPEAKER TONE",
                                    "ONBOARD RGB", "BATTERY / SD"};
  static_assert(std::size(labels) == int(ToolScreen::Battery));
  for (int i = 0; i < int(std::size(labels)); ++i) {
    const bool selected = i == toolSelection;
    const int y = 54 + i * 31;
    const uint16_t bg = selected ? Screen::theme.selected : Screen::theme.panel;
    drawCard(6, y, 308, 26, selected, Screen::theme.accent);
    Screen::text(18, y + 9, labels[i], selected ? Screen::theme.text : Screen::theme.muted, bg);
  }
}

// Open the test menu with its first diagnostic selected.
void openDeviceTools() {
  view = View::DeviceTools;
  toolScreen = ToolScreen::Menu;
  toolSelection = 0;
  drawDeviceTools();
}

// Send a short test tone through I2S, borrowing the output from Music when necessary.
bool playSpeakerTest() {
  stopBackgroundMusicPump();
  const bool wasReady = audioReady;
  if (!ensureAudioHardware()) return false;
  const uint32_t rate = audio.txSampleRate();
  if (rate < 8000 || rate > 96000) {
    if (!wasReady) resetAudioHardware();
    return false;
  }
  int16_t pcm[256];
  const uint32_t frames = rate / 6;
  bool written = true;
  for (uint32_t sent = 0; sent < frames && written;) {
    const uint32_t count = std::min(frames - sent, uint32_t(std::size(pcm) / 2));
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t phase = ((sent + i) * 440 % rate) * 256 / rate;
      const int sample = (phase < 128 ? int(phase) : 255 - int(phase)) * 32 - 2048;
      pcm[i * 2] = pcm[i * 2 + 1] = int16_t(sample);
    }
    const size_t bytes = count * 2 * sizeof(*pcm);
    written = audio.write(reinterpret_cast<const uint8_t *>(pcm), bytes) == bytes;
    sent += count;
  }
  if (!wasReady) {
    delay(50);
    resetAudioHardware();
  }
  return written;
}

// Route diagnostic selection, input holds, color changes, and live battery updates.
void handleDeviceTools(uint16_t pressed, uint16_t held) {
  if (toolScreen == ToolScreen::Buttons) {
    const uint32_t now = millis();
    if (!(held & (kMenuKey | GB_PAD_B))) {
      testExitHeld = false;
    } else if (!testExitHeld) {
      testExitHeld = true;
      testExitPressedAt = now;
    } else if (now - testExitPressedAt >= 750) {
      toolScreen = ToolScreen::Menu;
      drawDeviceTools();
      return;
    }
    if (held != testedButtons) drawButtonTest(held, false);
    return;
  }
  if (pressed & (GB_PAD_B | kMenuKey)) {
    if (toolScreen == ToolScreen::Menu) {
      view = View::Settings;
      drawSettings();
    } else {
      const bool restoreLed = toolScreen == ToolScreen::Led;
      toolScreen = ToolScreen::Menu;
      drawDeviceTools();
      if (restoreLed) applyFlashlightLed(millis(), false);
    }
    return;
  }
  if (toolScreen == ToolScreen::Battery) {
    if (millis() - toolReadingDrawnAt >= 1000) drawPowerTestReadings();
    return;
  }
  if (toolScreen == ToolScreen::Display || toolScreen == ToolScreen::Led) {
    if (!(pressed & (GB_PAD_A | GB_PAD_LEFT | GB_PAD_RIGHT))) return;
    testColor = (testColor + (pressed & GB_PAD_LEFT ? 4 : 1)) % 5;
  } else if (pressed & (GB_PAD_UP | GB_PAD_DOWN)) {
    constexpr int count = int(ToolScreen::Battery);
    toolSelection = (toolSelection + (pressed & GB_PAD_UP ? count - 1 : 1)) % count;
  } else if (pressed & GB_PAD_A) {
    const auto selected = static_cast<ToolScreen>(toolSelection + 1);
    if (selected == ToolScreen::Speaker) {
      showStatus(playSpeakerTest() ? "Played a 440 Hz tone." : "I2S speaker test unavailable.");
      return;
    }
    toolScreen = selected;
    testedButtons = 0;
    testExitHeld = false;
    testColor = 0;
  } else {
    return;
  }
  drawDeviceTools();
  if (toolScreen == ToolScreen::Led) {
    constexpr FlashlightRgb colors[] = {{64, 0, 0}, {0, 64, 0}, {0, 0, 64}, {48, 48, 48}, {}};
    writeFlashlightLed(colors[testColor]);
  }
}

// Handle Music input, cancelable folder scanning, transport controls, and screen timeout.
void handleMusicApp(uint16_t pressed, bool rescan) {
  if (view == View::MusicScreenOff) {
    handleMusicScreenOff(pressed, rescan);
    return;
  }
  if (pressed || rescan) {
    musicLastInputAt = millis();
  }
  if (view == View::Music && musicScanFolder && !rescan) {
    if (pressed & GB_PAD_B) {
      cancelMusicFolderScan();
      if (musicFolder) {
        redrawMusicFolder(true);
      } else {
        leaveMusic(backgroundMusic);
      }
      return;
    }
    pressed &= GB_PAD_LEFT | GB_PAD_RIGHT | GB_PAD_START | GB_PAD_SELECT;
  }
  if (rescan) {
    rescanMusicLibrary();
  } else if (pressed & kMenuKey) {
    if (view == View::MusicSettings) {
      closeMusicSettings();
    } else {
      musicSettingsReturnView = view;
      musicSettingsSelection = 0;
      view = View::MusicSettings;
      drawMusicSettings();
    }
  } else if (pressed & GB_PAD_SELECT) {
    if (musicTrack >= 0) {
      restartMusicTrack();
    }
  } else if (musicTrack >= 0 && (pressed & GB_PAD_START)) {
    toggleMusicPlayback();
  } else if (pressed & GB_PAD_B) {
    if (view == View::MusicSettings) {
      closeMusicSettings();
    } else if (view == View::NowPlaying) {
      view = View::Music;
      selectMusicPlaylistInLibrary();
      drawMusicLibrary();
    } else if (musicHasParent()) {
      openMusicFolderEntry(0);
    } else {
      leaveMusic(backgroundMusic);
    }
  } else if (view == View::MusicSettings) {
    handleMusicSettings(pressed);
  } else if (view == View::NowPlaying) {
    if (pressed) handleNowPlaying(pressed);
  } else {
    const int count = musicEntryCount();
    if (count && (pressed & kListScrollButtons)) {
      const int previous = musicSelection;
      musicSelection = (musicSelection + ((pressed & GB_PAD_UP) ? count - 1 : 1)) % count;
      drawMusicEntries(previous);
    } else if (pressed & (GB_PAD_LEFT | GB_PAD_RIGHT)) {
      adjustVolume(pressed & GB_PAD_LEFT ? -1 : 1);
      drawMusicStatus();
    } else if (count && (pressed & (GB_PAD_A | GB_PAD_START))) {
      const int trackOffset = musicTrackRowOffset();
      if (musicSelection < trackOffset) {
        openMusicFolderEntry(musicSelection);
      } else {
        openNowPlaying(musicSelection - trackOffset);
      }
    }
  }
  const uint32_t timeout = kMusicScreenTimeouts[musicScreenTimeout];
  if (timeout && inMusicApp() && view != View::MusicScreenOff && musicPlayer.isOpen() && !musicPaused &&
      millis() - musicLastInputAt >= timeout) {
    sleepMusicScreen();
  }
}

// Reset queued PCM and restart DMA on a clean buffer boundary for the GBA core.
void resetGbaAudioOutput() {
  gbaAudioBegin = gbaAudioEnd = 0;
  gbaAudioPrimed = false;
  if (gameSoundEnabled() && audioReady) {
    // Start on a DMA buffer boundary even if the last game used Gnuboy.
    const i2s_chan_handle_t tx = audio.txChan();
    if (i2s_channel_disable(tx) != ESP_OK || i2s_channel_enable(tx) != ESP_OK) {
      audio.end();
      audioReady = false;
      if (!currentGameProfile.enabled) soundEnabled = false;
      gbaCoreSetAudioEnabled(false);
      Serial.println("I2S unavailable. Sound stays off.");
    }
  }
}

// Write whole I2S blocks without blocking, leaving unaccepted samples in the staging buffer.
void flushGbaAudio() {
  const size_t pending = gbaAudioEnd - gbaAudioBegin;
  if (pending < kGbaAudioBlockSamples || (!gbaAudioPrimed && pending < kGbaAudioStartSamples)) {
    return;
  }
  gbaAudioPrimed = true;
  const size_t count = pending - pending % kGbaAudioBlockSamples;
  size_t written = 0;
  // Keep unwritten PCM for the next attempt instead of waiting for DMA.
  i2s_channel_write(audio.txChan(), samples + gbaAudioBegin,
                    count * sizeof(samples[0]), &written, 0);
  gbaAudioBegin += written / sizeof(samples[0]);
}

// Read new samples from gpSP, apply volume, and bound PCM latency before writing to I2S.
void queueGbaAudio() {
  if (!gameSoundEnabled() || !audioReady) {
    return;
  }
  const size_t incoming = gbaCoreAvailableAudio();
  if (!incoming) {
    return;
  }
  if (gbaAudioEnd - gbaAudioBegin + incoming > std::size(samples)) {
    flushGbaAudio();
  }
  // Bound latency when DMA falls behind. Drain the mixer's data this frame
  // so that new mixes cannot wrap around into unread samples.
  const size_t pending = std::min(gbaAudioEnd - gbaAudioBegin, std::size(samples) - incoming);
  if (pending && gbaAudioEnd != pending) {
    memmove(samples, samples + gbaAudioEnd - pending, pending * sizeof(samples[0]));
  }
  gbaAudioBegin = 0;
  const size_t added = gbaCoreReadAudio(samples + pending, incoming);
  applyAudioVolume(samples + pending, added);
  gbaAudioEnd = pending + added;
  flushGbaAudio();
}

// Start a fresh performance sample window with the current displayed-frame counter.
void resetGamePerfWindow(int64_t now, uint32_t displayed) {
  gamePerf = {};
  gamePerf.startUs = now;
  gamePerf.displayedAtStart = displayed;
}

// Report emulation and presentation FPS, core timings, and presenter contention to serial.
void reportGamePerf(const char *system) {
  const int64_t now = esp_timer_get_time();
  const int64_t elapsed = now - gamePerf.startUs;
  if (elapsed < kGameStatsWindowUs) {
    return;
  }
  const uint32_t displayed = gameDisplayedFrames.load(std::memory_order_relaxed);
  const double fpsFactor = 1000000.0 / elapsed;
  const uint32_t skipped = gamePerf.emulated - gamePerf.drawn;
  const double drawMs = gamePerf.drawn ? gamePerf.drawUs / (1000.0 * gamePerf.drawn) : 0;
  const double skipMs = skipped ? (gamePerf.coreUs - gamePerf.drawUs) / (1000.0 * skipped) : 0;
  Serial.printf("%s: %.1f emulated FPS | %.1f display FPS | core %.1f ms/frame | draw %.1f ms | skip %.1f ms | display busy %u/%u\n",
                system, gamePerf.emulated * fpsFactor,
                (displayed - gamePerf.displayedAtStart) * fpsFactor,
                gamePerf.coreUs / (1000.0 * gamePerf.emulated),
                drawMs, skipMs, unsigned(gamePerf.displayBusy), unsigned(gamePerf.drawn));
  resetGamePerfWindow(now, displayed);
}

// Measure the core's execution separately from audio and screen transfer.
void recordGameFrame(bool draw, int64_t coreUs) {
  gamePerf.coreUs += coreUs;
  ++gamePerf.emulated;
  if (draw) {
    gamePerf.drawUs += coreUs;
    ++gamePerf.drawn;
  }
}

// Restore the game display and audio pipelines and restart the emulator frame deadline.
void resumeGame() {
  Screen::fill(0, 0, Screen::width, Screen::height, Screen::black);
  if (flashlightOn) {
    // Latch a lit color before handing the TFT bus to gameplay.
    applyFlashlightLed(millis(), true);
  }
  if (activeCartridgeCore == &pocketGenesis) {
    if (!beginVideoPresenter(kNativeCartridgeBytes, true)) {
      Serial.println("Genesis async video unavailable; using synchronous display.");
    }
    Serial.printf("Genesis settings: CPU %u MHz | TFT %u MHz | %s | frameskip %s | sound %s | Arduino SPI\n",
                  unsigned(ESP.getCpuFreqMHz()), unsigned(Screen::clockHz() / 1000000),
                  gbScaleName(gameGbScale()), frameskipName(genesisFrameskipMode),
                  gameSoundEnabled() && audioReady ? "ON" : "OFF");
    resetGamePerfWindow(esp_timer_get_time(), gameDisplayedFrames.load(std::memory_order_relaxed));
  } else if (activeCartridgeCore) {
    beginVideoPresenter(activeCartridgeCore->frame().rgb565 ? kNativeCartridgeBytes
                                                           : kIndexedMaxPixelBytes + kIndexedPaletteBytes);
  } else if (gameIsGba) {
    const bool displayDma = gbaDmaEnabled && Screen::beginGbaVideo();
    if (!beginVideoPresenter(kGbaFrameBytes)) {
      Serial.println("GBA async video unavailable; using synchronous display.");
    }
    resetGbaAudioOutput();
    Serial.printf("GBA settings: CPU %u MHz | TFT %u MHz | %s | frameskip %s | sound %s | %s\n",
                  unsigned(ESP.getCpuFreqMHz()), unsigned(Screen::clockHz() / 1000000),
                  gbaScaleName(gameGbaFullscreen()), frameskipName(gameFrameskipMode()),
                  gameSoundEnabled() && audioReady ? "ON" : "OFF",
                  displayDma ? "DMA" : "Arduino SPI");
    resetGamePerfWindow(esp_timer_get_time(), gameDisplayedFrames.load(std::memory_order_relaxed));
  } else if (gameGbScale() != Screen::GbScale::Native && !beginVideoPresenter(kGbFrameBytes)) {
    Serial.println("GB/GBC async video unavailable. Using synchronous display.");
  }
  nextFrame = esp_timer_get_time();
  gbaSkipped = 0;
  genesisSkipped = 3;  // Render the first frame in every mode.
  view = View::Playing;
}

// Flush a dirty or previously failed GBA cartridge save outside frame rendering.
bool commitGbaBatteryAtSafePoint() {
  if (!gameIsGba || !batteryBacked || (!saveRetry && !batteryChanged())) {
    return true;
  }
  lastSaveAttempt = millis();
  saveRetry = !commitSave(saveFile, false);
  return !saveRetry;
}

// Check for distinct valid physical Start and Select GPIOs before enabling wake.
bool consoleWakePinsAvailable() {
  return kStart != kSelect && GPIO_IS_VALID_GPIO(kStart) && GPIO_IS_VALID_GPIO(kSelect);
}

// Use GPIO light-sleep wakeups until both released-and-rearmed buttons are held together.
esp_err_t sleepUntilConsoleWake() {
  const auto startPin = static_cast<gpio_num_t>(kStart);
  const auto selectPin = static_cast<gpio_num_t>(kSelect);
  esp_err_t result = esp_sleep_enable_gpio_wakeup();
  if (result != ESP_OK) {
    return result;
  }

  // Require release after requesting sleep by shortcut or menu. The chip can wake on
  // a single pin change, but stays dark and reenters sleep until both are held.
  bool armed = false;
  unsigned rejectedSleeps = 0;
  for (;;) {
    const bool startHeld = gpio_get_level(startPin) == 0;
    const bool selectHeld = gpio_get_level(selectPin) == 0;
    armed |= !startHeld && !selectHeld;
    if (armed && startHeld && selectHeld) {
      delay(kWakeDebounceMs);
      if (gpio_get_level(startPin) == 0 && gpio_get_level(selectPin) == 0) {
        result = ESP_OK;
        break;
      }
      continue;
    }

    // Wake on the opposite level of each button. A held single button can
    // therefore wait for the other button or its own release in light sleep.
    result = gpio_wakeup_enable(startPin, startHeld ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    if (result == ESP_OK) {
      result = gpio_wakeup_enable(selectPin, selectHeld ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    }
    if (result != ESP_OK) {
      break;
    }
    result = esp_light_sleep_start();
    if (result == ESP_OK) {
      rejectedSleeps = 0;
    } else if (result == ESP_ERR_SLEEP_REJECT && ++rejectedSleeps < 20) {
      // A button moved between reading its level and entering sleep.
      delay(2);
    } else {
      break;
    }
  }

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  gpio_wakeup_disable(startPin);
  gpio_wakeup_disable(selectPin);
  return result;
}

void drawDoomMenu();

// Restore the active view and its indicators after the TFT wakes.
void redrawAfterConsoleSleep() {
  switch (view) {
    case View::SystemMenu: drawSystemMenu(false); break;
    case View::GameList:
    case View::Favorites:
    case View::RecentlyPlayed: drawGameList(false); break;
    case View::GameOptions: drawGameOptions(); break;
    case View::GameProfile: drawGameProfile(); break;
    case View::DeviceTools: drawDeviceTools(); break;
    case View::Bluetooth: handleBluetooth(0); drawBluetooth(true); break;
    case View::MiniGames: drawMiniGames(); break;
    case View::Applications:
      if (!AppRuntime::running()) drawApplications();
      else if (AppRuntime::redraw()) drawAppFooter();
      else closeApplication(true);
      break;
    case View::DoomMenu: drawDoomMenu(); break;
    case View::Standby: StandbyDisplay::draw(); break;
    case View::Photos: PhotoViewer::draw(); break;
    case View::Settings: drawSettings(false); break;
    case View::AboutMenu: drawAboutMenu(); break;
    case View::About: drawAbout(true); break;
    case View::Diagnostics: DeviceLog::draw(); break;
    case View::Update: drawUpdates(); break;
    case View::Flashlight: drawFlashlight(); break;
    case View::Paused: drawPause(); break;
    case View::MusicScreenOff:
      view = musicScreenReturnView;
      musicWakeRequested = false;
      [[fallthrough]];
    case View::Music:
    case View::NowPlaying:
    case View::MusicSettings:
      musicLastInputAt = millis();
      redrawMusicScreen();
      break;
    default: break;
  }
}

// Save pending cartridge data, stop display and audio activity, sleep, and restore the console state.
void sleepConsole(bool resumeGameOnWake) {
  const bool playingGame = view == View::Playing;
  if (!consoleWakePinsAvailable()) {
    if (playingGame) Serial.println("Sleep needs physical Start and Select.");
    else showStatus("Sleep needs physical Start and Select.");
    return;
  }
  stopVideoPresenter();
  const bool gameLoaded = playingGame || hasPausedGame();
  if (gameLoaded && batteryBacked && (rtcCart || saveRetry || batteryChanged())) {
    saveRetry = !commitSave(saveFile, false);
    if (saveRetry) {
      if (playingGame) {
        Serial.println("Sleep canceled. Battery save failed.");
        resumeGame();
      } else {
        showStatus("Sleep canceled. Battery save failed.");
      }
      return;
    }
  }

  if (playingGame || view == View::MusicScreenOff || view == View::Failed) {
    Serial.println(kSleepWakeHint);
  } else {
    showStatus(kSleepWakeHint);
  }
  if (flashlightOn || musicRgbWasActive) {
    musicRgbWasActive = false;
    writeFlashlightLed({});
  }
  if (!Screen::sleepPanel(true)) {
    if (playingGame) {
      Serial.println("Could not put display to sleep.");
      resumeGame();
    } else {
      if (flashlightOn) applyFlashlightLed(millis(), false);
      showStatus("Could not put display to sleep.");
    }
    return;
  }
  GameCover::stop();
  PhotoViewer::stop();
  DeviceLog::stop(millis(), sdReady);
  stopBackgroundMusicPump();
  UsbControls::stop();
  BluetoothControls::suspend();
  const bool restartAudio = audioReady || soundEnabled || musicOwnsAudio || (gameLoaded && gameSoundEnabled());
  resetAudioHardware();
  const auto backlightPin = static_cast<gpio_num_t>(kTftBacklight);
  const bool heldBacklight = kTftBacklight >= 0 && gpio_hold_en(backlightPin) == ESP_OK;
  Serial.flush();

  const esp_err_t result = sleepUntilConsoleWake();
  BluetoothControls::resume();
  UsbControls::begin();

  if (heldBacklight) {
    gpio_hold_dis(backlightPin);
  }
  if (restartAudio && !ensureAudioHardware()) {
    musicOwnsAudio = false;
  }
  if (gameLoaded && gameIsGba) {
    gbaCoreSetAudioEnabled(gameSoundEnabled() && audioReady);
  }

  Screen::wakePanel();
  const uint32_t wakeStarted = millis();
  while (!Screen::finishPanelWake()) {
    if (millis() - wakeStarted >= 500) {
      if (!Screen::begin()) {
        fail("Could not restore display after sleep.");
        return;
      }
      break;
    }
    delay(5);
  }

  sleepChord.waitForRelease();
  if (playingGame || (result == ESP_OK && resumeGameOnWake)) {
    resumeGame();
  } else {
    redrawAfterConsoleSleep();
    if (flashlightOn) applyFlashlightLed(millis(), false);
  }
  if (result != ESP_OK) {
    Serial.printf("ESP32 light sleep failed: %s\n", esp_err_to_name(result));
    if (!playingGame) showStatus("ESP32 could not enter light sleep.");
  }
}

// Move one level up in Settings or return to the view that opened Settings.
void backFromSettings() {
  if (settingsPage == SettingsPage::Root) {
    closeSettings();
  } else if (settingsPage >= SettingsPage::GameBoy) {
    settingsSelection = int(settingsPage) - kFirstEmulatorPage;
    settingsPage = SettingsPage::Emulation;
    drawSettings();
  } else if (settingsPage == SettingsPage::Wifi) {
    settingsPage = SettingsPage::Connectivity;
    settingsSelection = 1;  // Wi-Fi row
    drawSettings();
  } else {
    settingsPage = SettingsPage::Root;
    settingsSelection = settingsRootSelection;
    drawSettings();
  }
}

// Route category navigation, option adjustments, and Settings actions.
void handleSettings(uint16_t pressed) {
  if (wifiField != WifiField::None) {
    handleWifiEditor(pressed);
    return;
  }
  if (pressed & (GB_PAD_B | kMenuKey)) {
    backFromSettings();
    return;
  }
  const auto &menu = kSettingsMenus[int(settingsPage)];
  if (pressed & (GB_PAD_UP | GB_PAD_DOWN)) {
    const int previous = settingsSelection;
    const int step = pressed & GB_PAD_UP ? menu.count - 1 : 1;
    settingsSelection = (settingsSelection + step) % menu.count;
    drawSettings(false, previous);
    return;
  }
  if (!(pressed & (GB_PAD_A | GB_PAD_START | GB_PAD_LEFT | GB_PAD_RIGHT))) return;

  const SettingsItem item = menu.items[settingsSelection];
  const bool activate = pressed & (GB_PAD_A | GB_PAD_START);
  const bool enter = activate || (pressed & GB_PAD_RIGHT);
  switch (item) {
    case SettingsItem::DisplayMenu:
      if (enter) openSettingsPage(SettingsPage::Display);
      break;
    case SettingsItem::SoundMenu:
      if (enter) openSettingsPage(SettingsPage::Sound);
      break;
    case SettingsItem::EmulationMenu:
      if (enter) openSettingsPage(SettingsPage::Emulation);
      break;
    case SettingsItem::GameBoyMenu:
    case SettingsItem::GameBoyColorMenu:
    case SettingsItem::GameBoyAdvanceMenu:
    case SettingsItem::NesMenu:
    case SettingsItem::MasterSystemMenu:
    case SettingsItem::GameGearMenu:
    case SettingsItem::GenesisMenu:
      if (enter) openSettingsPage(static_cast<SettingsPage>(kFirstEmulatorPage + settingsEmulatorIndex(item)));
      break;
    case SettingsItem::PowerMenu:
      if (enter) openSettingsPage(SettingsPage::Power);
      break;
    case SettingsItem::ConnectivityMenu:
      if (enter) openSettingsPage(SettingsPage::Connectivity);
      break;
    case SettingsItem::WifiMenu:
      if (enter) openSettingsPage(SettingsPage::Wifi);
      break;
    case SettingsItem::Bluetooth:
      if (enter) openBluetooth();
      break;
    case SettingsItem::WifiName:
    case SettingsItem::WifiPassword:
      if (enter) {
        wifiField = item == SettingsItem::WifiName ? WifiField::Name : WifiField::Password;
        openTextEditor(wifiField == WifiField::Name ? WifiTransfer::networkName() : WifiTransfer::password());
        drawWifiEditor();
      }
      break;
    case SettingsItem::EnableOnRestart:
      if (activate) {
        if (const char *error = WifiTransfer::schedule(!WifiTransfer::scheduled())) showStatus(error);
        else drawSettings(false);
      }
      break;
    case SettingsItem::RestartConsole:
      if (activate) {
        restartForConnectivity(WifiTransfer::scheduled(), "Enable Wi-Fi on restart first.",
                               "Restarting for file transfer...");
      }
      break;
    case SettingsItem::DeviceTools:
      if (enter) openDeviceTools();
      break;
    case SettingsItem::About:
      if (enter) openAbout();
      break;
    case SettingsItem::Sleep:
      if (activate) sleepConsole(false);
      break;
    case SettingsItem::Back:
      backFromSettings();
      break;
    default: {
      const int direction = pressed & GB_PAD_LEFT ? -1 : 1;
      if (cycleSettingsValue(item, direction)) {
        drawSettings(false, item == SettingsItem::Theme ? -1 : settingsSelection);
      } else {
        showStatus("Backlight dimming unavailable.");
      }
      break;
    }
  }
}

// Start light sleep after the configured idle period unless an active feature keeps the console awake.
void serviceAutoSleep(bool inputActive) {
  const uint32_t now = millis();
  const bool playingMusic = musicPlayer.isOpen() && !musicPaused && !hasPausedGame();
  if (inputActive || DeviceLog::recording() || view == View::Playing || view == View::Failed ||
      AppRuntime::running() || playingMusic || flashlightOn ||
      (view == View::DeviceTools && toolScreen == ToolScreen::Led) ||
      (view == View::MiniGames && miniScreen != MiniScreen::Menu) ||
      (view == View::Standby && StandbyDisplay::showingClock())) {
    lastConsoleActivityAt = now;
    return;
  }
  const uint32_t idleMs = kAutoSleepOptions[autoSleepOption].idleMs;
  if (idleMs && now - lastConsoleActivityAt >= idleMs) {
    sleepConsole(false);
    lastConsoleActivityAt = millis();
  }
}

// Load and configure the selected emulator and optionally restore the last save slot for Continue.
bool startGame(bool resumeState = false) {
  if (GameCover::status() != GameCover::Status::Idle) {
    GameCover::cancel();
    // A failed launch leaves the browser open and can retry the same cover.
    gameCoverPending = true;
    gameCoverQueuedAt = millis();
  }
  const int selection = currentGameSelection();
  gameIsGba = selectedSystem == System::GameBoyAdvance;
  activeCartridgeCore = systemInfo(selectedSystem).core;
  batteryBacked = false;
  rtcCart = false;
  const String &path = currentGames()[selection];
  currentGameProfile = readGameProfile(path);
  if (gameSoundEnabled()) ensureAudioHardware();
  gameName = fileName(path);
  const String romPath = "/sd" + path;
  saveFile = romPath + ".sav";
  stateFile = romPath + ".state";
  saveSlot = latestSaveSlot();
  showStatus("Loading game...");
  if (activeCartridgeCore) {
    const String battery = readableSavePath(saveFile);
    const char *error = activeCartridgeCore->open(romPath.c_str(), battery.c_str(), kAudioRate);
    if (error) {
      showStatus(error);
      activeCartridgeCore = nullptr;
      return false;
    }
    batteryBacked = true;
  } else if (gameIsGba) {
    // With no amplifier configured, reserve CPU time for emulation rather
    // than GBA sound synthesis. Enabling kEnableAudio restores normal sound.
    gbaCoreSetAudioEnabled(gameSoundEnabled() && audioReady);
    const String battery = readableSavePath(saveFile);
    const char *error = gbaCoreOpen(romPath.c_str(), "/sd/retro-go/bios/gba_bios.bin", battery.c_str());
    if (error) {
      showStatus(error);
      return false;
    }
    const uint8_t fastMemory = gbaCoreFastMemoryMask();
    Serial.printf("GBA fast RAM: VRAM %s, IWRAM %s, map %s, frame %s\n",
                  fastMemory & GBA_FAST_MEMORY_VRAM ? "internal" : "PSRAM",
                  fastMemory & GBA_FAST_MEMORY_IWRAM ? "internal" : "PSRAM",
                  fastMemory & GBA_FAST_MEMORY_MAP ? "internal" : "PSRAM",
                  fastMemory & GBA_FAST_MEMORY_FRAMEBUFFER ? "internal" : "PSRAM");
    batteryBacked = true;
  } else if (!ensureGbEmulator()) {
    showStatus("Could not allocate GB emulator");
    return false;
  } else if (!loadCardRom(path)) {
    gnuboy_free_rom();
    releaseGbFramebuffer();
    free(ownedRom);
    ownedRom = nullptr;
    Serial.printf("Could not load %s\n", path.c_str());
    return false;
  } else {
    const uint8_t type = ownedRom[0x147];
    rtcCart = type == 0x0F || type == 0x10;
    batteryBacked = rtcCart || type == 0x03 || type == 0x06 || type == 0x09 ||
                    type == 0x0D || type == 0x13 || type == 0x1B || type == 0x1E || type == 0xFF;
  }

  if (gameIsGbFamily()) {
    gnuboy_reset(true);
    gnuboy_set_palette(GB_PALETTE_DMG);
    if (batteryBacked) {
      gnuboy_load_sram(readableSavePath(saveFile).c_str());
    }
  }
  saveRetry = false;
  lastSaveAttempt = millis();
  gameReturnView = isGameListView() ? view : View::GameList;
  recordRecentlyPlayed(path);
  if (gameReturnView == View::RecentlyPlayed) recentSelection = 0;
  if (lastPlayedPath != path) {
    lastPlayedPath = path;
    if (!settingsStore.putString("lastPlayed", path)) Serial.println("Could not save the Continue game.");
  }
  if (resumeState && hasSaveSlot(saveSlot)) {
    if (!loadState()) {
      if (gameIsGbFamily()) {
        gnuboy_reset(true);
        if (batteryBacked) gnuboy_load_sram(readableSavePath(saveFile).c_str());
      }
      view = View::Paused;
      pauseChoice = 0;
      drawPause();
      showStatus("State could not load. A resumes.");
      return true;
    }
    saveRetry = gameIsGbFamily() && batteryBacked;
  }
  Serial.printf("Running %s. M pauses. W/A/S/D, J/K, U/I control the game.\n", gameName.c_str());
  resumeGame();
  return true;
}

// Locate the last title in the SD index, release Music's I2S output, and open the title with resume enabled.
void continueLastGame() {
  const String &path = continueGamePath();
  if (path.isEmpty() || !selectGameByPath(path)) {
    showStatus(path.isEmpty() ? "Play a game first." : "Last game is unavailable.");
    return;
  }
  releaseMusicAudio();
  startGame(true);
}

// Save cartridge state before freeing the current game and returning to its original browser.
void quitGame() {
  stopVideoPresenter();
  if (batteryBacked && (rtcCart || saveRetry || batteryChanged()) &&
      !commitSave(saveFile, false)) {
    saveRetry = true;
    showStatus("Save failed. Check the SD card.");
    return;
  }
  free(videoPresenterBuffer);
  videoPresenterBuffer = nullptr;
  if (activeCartridgeCore) {
    activeCartridgeCore->close();
    activeCartridgeCore = nullptr;
  } else if (gameIsGba) {
    gbaCoreClose();
  } else {
    gnuboy_free_rom();
    releaseGbFramebuffer();
    free(ownedRom);
    ownedRom = nullptr;
  }
  currentGameProfile.enabled = false;
  view = gameReturnView;
  drawGameList(true);
}

void openStatePrompt(PauseItem item) {
  const bool hasState = hasSaveSlot(saveSlot);
  if (item == PauseItem::LoadState && !hasState) {
    showStatus("This slot is empty.");
    return;
  }
  statePrompt = item == PauseItem::SaveState ? StatePrompt::Save : StatePrompt::Load;
  statePromptYes = false;
  statePromptOverwrite = hasState;
  drawStatePrompt();
}

void handleStatePrompt(uint16_t pressed) {
  if (pressed & (GB_PAD_B | kMenuKey)) {
    statePrompt = StatePrompt::None;
    drawPause();
  } else if (pressed & (kListScrollButtons | GB_PAD_LEFT | GB_PAD_RIGHT)) {
    const int previous = int(statePromptYes);
    statePromptYes = !statePromptYes;
    drawStatePrompt(previous);
  } else if (pressed & GB_PAD_A) {
    const StatePrompt confirmed = statePromptYes ? statePrompt : StatePrompt::None;
    statePrompt = StatePrompt::None;
    if (confirmed == StatePrompt::Load && loadState()) {
      saveRetry = gameIsGbFamily() && batteryBacked;
      resumeGame();
      return;
    }
    const bool saved = confirmed == StatePrompt::Save && saveSelectedState();
    drawPause();
    if (confirmed == StatePrompt::Save) showStatus(saved ? "State saved" : "State save failed");
    else if (confirmed == StatePrompt::Load) showStatus("Could not load state");
  }
}

// Apply the selected action or adjust its value, keeping save and emulator lifecycle rules intact.
void applyPauseChoice(PauseItem item, int direction, bool activate) {
  switch (item) {
    case PauseItem::Resume:
      resumeGame();
      break;
    case PauseItem::SaveState:
    case PauseItem::LoadState:
      if (activate) {
        openStatePrompt(item);
      } else {
        saveSlot = (saveSlot + direction + kSaveSlotCount) % kSaveSlotCount;
        drawPause(pauseChoice, true);
      }
      break;
    case PauseItem::GbPalette: {
      constexpr int count = GB_PALETTE_DMG + 1;
      const int palette = std::clamp(gnuboy_get_palette(), 0, int(GB_PALETTE_DMG));
      gnuboy_set_palette(static_cast<gb_palette_t>((palette + direction + count) % count));
      if (activate) resumeGame();
      else drawPause(pauseChoice);
      break;
    }
    case PauseItem::DisplaySize:
      if (currentGameProfile.enabled) {
        GameProfile updated = currentGameProfile;
        if (gameIsGba) {
          updated.gbaFull = !updated.gbaFull;
        } else {
          updated.gbDisplay = nextDisplayScale(updated.gbDisplay, direction);
        }
        if (!writeGameProfile(currentGames()[currentGameSelection()], updated)) {
          showStatus("Could not save game profile.");
          break;
        }
        currentGameProfile = updated;
      } else {
        if (gameIsGba) {
          gbaFullscreen = !gbaFullscreen;
        } else {
          auto &scale = emulatorScales[systemIndex(selectedSystem)];
          scale = nextDisplayScale(scale, direction);
        }
        saveDeviceSettings();
      }
      if (activate) resumeGame();
      else drawPause(pauseChoice);
      break;
    case PauseItem::Volume:
      adjustVolume(direction);
      drawPause(pauseChoice);
      break;
    case PauseItem::Brightness:
      if (adjustDisplayBrightness(direction)) {
        drawPause(pauseChoice);
      } else {
        showStatus("Backlight dimming unavailable.");
      }
      break;
    case PauseItem::Sleep:
      sleepConsole(true);
      break;
    case PauseItem::Settings:
      openSettings(View::Paused);
      break;
    case PauseItem::Quit:
      quitGame();
      break;
  }
}

// Map the current UI view to a stable activity identifier written in diagnostic captures.
DeviceLog::Activity logActivity(View currentView) {
  using Activity = DeviceLog::Activity;
  switch (currentView) {
    case View::SystemMenu: return Activity::Home;
    case View::DoomMenu: return Activity::Home;
    case View::GameList:
    case View::Favorites:
    case View::RecentlyPlayed:
    case View::GameOptions: return Activity::Library;
    case View::Settings: return Activity::Settings;
    case View::AboutMenu:
    case View::Update:
    case View::About: return Activity::About;
    case View::Diagnostics: return Activity::Log;
    case View::Music: return Activity::Music;
    case View::NowPlaying: return Activity::NowPlaying;
    case View::MusicSettings: return Activity::MusicOptions;
    case View::MusicScreenOff: return Activity::ScreenOff;
    case View::Flashlight: return Activity::Flashlight;
    case View::GameProfile: return Activity::GameProfile;
    case View::Bluetooth:
    case View::DeviceTools: return Activity::Tools;
    case View::WifiTransfer: return Activity::Settings;
    case View::MiniGames: return Activity::MiniGames;
    case View::Applications: return AppRuntime::running() ? Activity::Application : Activity::Applications;
    case View::Standby: return Activity::Clock;
    case View::Photos: return Activity::Photos;
    case View::Playing: return Activity::Game;
    case View::DoomPlaying: return Activity::Game;
    case View::Paused: return Activity::Paused;
    case View::Failed: return Activity::Error;
  }
  return Activity::Error;
}

// Sample operating state when recording is due and service saving, display, and serial export.
void serviceDeviceLog() {
  const uint32_t now = millis();
  if (DeviceLog::needsSample(now)) {
    DeviceLog::Sample sample;
    sample.activity = logActivity(view);
    sample.batteryMv = readBatteryMillivolts();
    // Gameplay skips normal gauge sampling. Reuse the capture's ADC reading.
    if (view == View::Playing && kBatterySense >= 0 && batteryGauge.needsSample(now)) {
      batteryGauge.update(now, sample.batteryMv);
    }
    sample.charge = batteryGauge.percent();
    sample.brightness = view == View::MusicScreenOff ? 0 : displayBrightness;
    sample.volume = volumePercent.load(std::memory_order_relaxed);
    sample.flashlight = flashlightOn;
    sample.red = lastLedRgb.red;
    sample.green = lastLedRgb.green;
    sample.blue = lastLedRgb.blue;
    if (musicPlayer.isOpen()) {
      sample.track = musicTrack;
      sample.music = musicPaused ? DeviceLog::Music::Paused :
                     musicOwnsAudio ? DeviceLog::Music::Playing : DeviceLog::Music::Suspended;
      if (sample.music == DeviceLog::Music::Playing) {
        const uint16_t peaks = musicAudioLevels.load(std::memory_order_relaxed);
        sample.peakLeft = peaks & 255;
        sample.peakRight = peaks >> 8;
      }
    }
    if (view == View::Playing || hasPausedGame()) {
      sample.gameSystem = systemIndex(selectedSystem) + 1;
      sample.gameSound = view == View::Playing && gameSoundEnabled() && audioReady;
    }
    DeviceLog::append(now, sample);
  }
  DeviceLog::service(now, sdReady, view == View::Diagnostics, batteryGauge.latestMillivolts());
}

// Draw the fixed Pocket Play branding and version before startup phase updates.
void drawBootSplash() {
  char byline[sizeof(kConsoleDeveloper) + 3];
  char version[sizeof(kConsoleVersion) + 8];
  snprintf(byline, sizeof(byline), "By %s", kConsoleDeveloper);
  snprintf(version, sizeof(version), "Version %s", kConsoleVersion);
  constexpr int glyphSize = 8;
  constexpr int titleScale = 3;
  constexpr int margin = 12;
  constexpr int titleWidth = (sizeof(kConsoleName) - 1) * glyphSize * titleScale;
  constexpr int titleHeight = glyphSize * titleScale;
  constexpr int titleX = (Screen::width - titleWidth) / 2;
  constexpr int titleY = (Screen::height - titleHeight) / 2;
  constexpr int bylineWidth = (sizeof(byline) - 1) * glyphSize;
  constexpr int versionWidth = (sizeof(version) - 1) * glyphSize;

  // Screen::begin() clears to black before this first draw.
  Screen::text(titleX, titleY, kConsoleName, Screen::darkTheme.text, Screen::black, titleScale);
  Screen::text(titleX + titleWidth - bylineWidth, titleY + titleHeight + 10,
               byline, Screen::darkTheme.muted, Screen::black);
  Screen::text(Screen::width - margin - versionWidth, Screen::height - margin - glyphSize,
               version, Screen::darkTheme.muted, Screen::black);
}

// Initialize preferences, GPIO, display, controls, audio, and storage before showing Home.
void drawWifiTransfer() {
  const uint16_t accent = Screen::theme.accent;
  const uint16_t background = Screen::theme.background;
  drawHeader("WI-FI TRANSFER", "CONNECT YOUR COMPUTER TO THIS WI-FI", accent);
  Screen::text(12, 57, "NETWORK NAME", Screen::theme.muted, background);
  Screen::text(12, 72, WifiTransfer::networkName(), Screen::theme.text, background);
  Screen::text(12, 91, "PASSWORD", Screen::theme.muted, background);
  const char *password = WifiTransfer::password();
  char firstLine[33];
  snprintf(firstLine, sizeof(firstLine), "%.32s", password);
  Screen::text(12, 106, firstLine, Screen::theme.text, background);
  if (strlen(password) > 32) Screen::text(12, 118, password + 32, Screen::theme.text, background);
  Screen::text(12, 143, "OPEN IN YOUR BROWSER", Screen::theme.muted, background);
  Screen::text(12, 160, WifiTransfer::address(), accent, background, 2);
  Screen::text(12, 200, "RESTART TO RETURN TO THE LAUNCHER", Screen::theme.text, background);
  Screen::text(12, 224, "B / MENU: RESTART WHEN IDLE", Screen::theme.muted, background);
}

void handleWifiTransfer(uint16_t pressed) {
  if (!WifiTransfer::requestExit(pressed & (GB_PAD_B | kMenuKey))) {
    static int displayedProgress = -1;
    // The HTTP task publishes progress and the main loop owns the display.
    const int percent = WifiTransfer::firmwareProgress();
    if (percent >= 0) {
      if (percent != displayedProgress) drawFirmwareInstall(percent, displayedProgress < 0, false);
    } else {
      if (displayedProgress >= 0) drawWifiTransfer();
      serviceMenuIndicators();
    }
    displayedProgress = percent;
    return;
  }
  showStatus("Restarting...");
  WifiTransfer::stop();
  Serial.flush();
  ESP.restart();
}

void drawDoomMenu() {
  const uint16_t accent = homeItemAccent(kDoomMenuItem);
  drawHeader("DOOM", "SELECT A GAME", accent);
  for (unsigned i = 0; i < std::size(DoomLauncher::files); ++i) {
    const int y = 64 + int(i) * 31;
    const bool present = sdReady && DoomLauncher::available(i);
    const uint16_t background = i == doomSelection ? Screen::theme.panel : Screen::theme.background;
    Screen::fill(12, y, Screen::width - 24, 24, background);
    if (i == doomSelection) Screen::fill(12, y, 3, 24, accent);
    Screen::text(22, y + 8, DoomLauncher::files[i], present ? Screen::theme.text : Screen::theme.muted, background);
    if (!present) Screen::text(190, y + 8, "MISSING", Screen::theme.muted, background);
  }
  Screen::text(12, 177, "COPY YOUR IWAD TO /doom", Screen::theme.muted, Screen::theme.background);
  Screen::text(12, 224, "A: PLAY   B: HOME", Screen::theme.muted, Screen::theme.background);
}

void handleDoomMenu(uint16_t pressed) {
  if (pressed & (GB_PAD_B | kMenuKey)) {
    view = View::SystemMenu;
    drawSystemMenu(true);
  } else if (pressed & (GB_PAD_UP | GB_PAD_DOWN)) {
    const unsigned count = std::size(DoomLauncher::files);
    doomSelection = (doomSelection + (pressed & GB_PAD_UP ? count - 1 : 1)) % count;
    drawDoomMenu();
  } else if (pressed & (GB_PAD_A | GB_PAD_START)) {
    if (!sdReady || !DoomLauncher::available(doomSelection)) {
      showStatus("Copy the IWAD to /doom.");
    } else if (settingsStore.putUChar(kDoomBootKey, doomSelection + 1) != 1) {
      showStatus("Could not start DOOM.");
    } else {
      clearMusicSession();
      Serial.flush();
      ESP.restart();
    }
  }
}

void setup() {
  Serial.begin(115200);
  const bool settingsStarted = settingsStore.begin("pocketplay", false);
  Serial.printf("\nArduino Game Boy | Flash: %u MiB | PSRAM: %u MiB\n",
                unsigned(ESP.getFlashChipSize() / 1048576), unsigned(ESP.getPsramSize() / 1048576));
  const auto reset = esp_reset_reason();
  const char *reason = "other";
  switch (reset) {
    case ESP_RST_POWERON: reason = "power-on"; break;
    case ESP_RST_SW: reason = "software restart"; break;
    case ESP_RST_PANIC: reason = "panic"; break;
    case ESP_RST_INT_WDT: reason = "interrupt watchdog"; break;
    case ESP_RST_TASK_WDT: reason = "task watchdog"; break;
    case ESP_RST_WDT: reason = "watchdog"; break;
    case ESP_RST_BROWNOUT: reason = "brownout"; break;
    default: break;
  }
  Serial.printf("Reset reason: %s (%d)\n", reason, int(reset));
  for (const auto &button : kButtons) {
    if (button.pin >= 0 && (!kAnalogDpad || button.mask >= GB_PAD_A)) {
      pinMode(button.pin, INPUT_PULLUP);
    }
  }
  if (kAnalogDpad) {
    analogReadResolution(12);
  }
  const bool bluetoothRecovery = consoleWakePinsAvailable() &&
                                 digitalRead(kStart) == LOW && digitalRead(kSelect) == LOW;
  if (bluetoothRecovery) sleepChord.waitForRelease();
  const bool transferBoot = WifiTransfer::loadSettings(settingsStore) && !bluetoothRecovery;
  const uint8_t doomSlot = settingsStore.getUChar(kDoomBootKey, 0);
  const bool doomBoot = doomSlot && settingsStore.remove(kDoomBootKey) && !transferBoot && !bluetoothRecovery;
  // Give the radio first access to contiguous internal RAM on launcher boots.
  if (!transferBoot && !doomBoot) BluetoothControls::begin(settingsStore, bluetoothRecovery);
  loadDeviceSettings();
  const bool displayStarted = Screen::begin();
  uint32_t splashStartedAt = 0;
  if (!displayStarted) {
    Serial.println("Display SPI initialization failed");
    FirmwareUpdate::verifyBoot(false);
  } else {
    if (displayBrightness != 100 && !Screen::setBrightness(displayBrightness)) {
      Serial.println("Backlight dimming unavailable. Using full brightness.");
      displayBrightness = 100;
    }
    drawBootSplash();
    drawBootProgress(BootStage::Starting);
    splashStartedAt = millis();
  }
  const char *transferError = nullptr;
  bool sdBusStarted = false;
  // The transfer server owns the card until reboot. Skip audio and library startup on this boot.
  if (transferBoot) {
    sdBusStarted = sdSpi.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
    sdReady = sdBusStarted && mountCard();
    transferError = displayStarted ? WifiTransfer::begin(wifiUploadDestination, sdReady)
                                   : "Wi-Fi transfer needs a display.";
    if (!transferError) {
      UsbControls::begin();
      view = View::WifiTransfer;
      drawWifiTransfer();
      writeFlashlightLed({});
      FirmwareUpdate::verifyBoot(settingsStarted && psramFound());
      Serial.println("Wi-Fi file transfer ready at http://192.168.4.1");
      return;
    }
    BluetoothControls::begin(settingsStore);
  }
  UsbControls::begin();
  const char *doomError = nullptr;
  if (doomBoot && displayStarted) {
    sdBusStarted = sdSpi.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
    sdReady = sdBusStarted && mountCard();
    if (sdReady) {
      drawHeader("DOOM", "LOADING GAME", homeItemAccent(kDoomMenuItem));
      FirmwareUpdate::verifyBoot(settingsStarted && psramFound());
      doomError = DoomLauncher::start(doomSlot - 1, readControls, kMenuKey, soundEnabled, volumePercent.load());
      if (!doomError) {
        view = View::DoomPlaying;
        return;
      }
    } else {
      doomError = "DOOM needs a microSD card.";
    }
  }
  // A failed Doom boot returns to the launcher. Successful Doom boots use physical controls.
  if (doomBoot) BluetoothControls::begin(settingsStore);
  if (soundEnabled && !ensureAudioHardware()) {
    soundEnabled = false;
    saveDeviceSettings();
  }

  if (sdBusStarted || sdSpi.begin(kSdSck, kSdMiso, kSdMosi, kSdCs)) {
    scanCard(displayStarted);
  }
  if (BluetoothControls::status().enabled) {
    Serial.printf("Bluetooth: %u bytes internal heap after SD scan\n",
                  unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  }
  if (displayStarted) {
    drawBootProgress(BootStage::Ready);
    const uint32_t visibleMs = millis() - splashStartedAt;
    if (visibleMs < kBootSplashMinMs) {
      delay(kBootSplashMinMs - visibleMs);
    }
  }
  drawSystemMenu(true);
  if (transferError) showStatus(transferError);
  if (doomError) showStatus(doomError);
  writeFlashlightLed({});
  lastConsoleActivityAt = millis();
  FirmwareUpdate::verifyBoot(settingsStarted && displayStarted && psramFound());
}

// Debounce and dispatch controls, advance emulation or app state, then service shared device work.
void loop() {
  if (view == View::DoomPlaying) {
    DoomLauncher::tick();
    return;
  }
  static uint16_t previousRaw = 0;
  static uint16_t debounced = 0;
  static uint32_t lastButtonChange = 0;
  static uint16_t serialHeld = 0;
  static uint32_t serialAt = 0;
  const uint32_t now = millis();
  const uint16_t raw = readControls();
  uint16_t pressed = 0;
  if (raw != previousRaw) {
    lastButtonChange = now;
    previousRaw = raw;
  }
  if (now - lastButtonChange >= 15) {
    pressed = raw & ~debounced;
    debounced = raw;
  }
  uint16_t held = debounced;
  if (consoleWakePinsAvailable() && sleepChord.update(raw, now, held, pressed)) {
    if (view == View::WifiTransfer) return;
    appToneFrames = 0;
    sleepConsole(false);
    lastConsoleActivityAt = millis();
    return;
  }
  const uint16_t physicalPressed = pressed;
  const ControllerInput usb = UsbControls::takeInput();
  BluetoothControls::service();
  const ControllerInput ble = BluetoothControls::takeInput();
  const uint16_t externalHeld = usb.held | ble.held;
  const uint16_t externalPressed = usb.pressed | ble.pressed;
  pressed |= externalPressed;
  held |= externalHeld;
  if (now - serialAt >= 120) {
    serialHeld = 0;
  }
  static uint8_t logCommand = 0;
  bool rescan = false;
  while (Serial.available()) {
    const int key = tolower(Serial.read());
    if (key == "log"[logCommand]) {
      if (++logCommand == 3) {
        logCommand = 0;
        if (view != View::WifiTransfer) DeviceLog::requestSerial(sdReady);
      }
      continue;
    }
    logCommand = key == 'l' ? 1 : 0;
    rescan |= key == 'r';
    const uint16_t mask = serialControl(key);
    if (mask) {
      serialAt = now;
      serialHeld = mask;
      pressed |= mask;
    }
  }

  // A new control action takes priority over a held list direction.
  const bool scrollAllowed = !rescan && !(pressed & ~kListScrollButtons) &&
                             ((view == View::SystemMenu && homeStyle == HomeStyle::List) ||
                              (isGameListView() && visibleGames().size() > 1) ||
                              (view == View::Music && musicEntryCount() > 1) ||
                              (view == View::Photos && PhotoViewer::canRepeatScroll()) ||
                              (view == View::Update && !updateConfirm && updateImages.size() > 1) ||
                              (view == View::Applications && !AppRuntime::running() && listedApps.size() > 1) ||
                              (view == View::Diagnostics && DeviceLog::canRepeatScroll()));
  const uint16_t repeat = repeatListScroll(scrollAllowed ? debounced | externalHeld : 0,
                                          raw | externalHeld, physicalPressed | externalPressed, now);
  if (!pressed) {
    pressed = repeat;
  }

  switch (view) {
    case View::SystemMenu: {
      const int previous = systemSelection;
      if (rescan || (pressed & GB_PAD_SELECT)) {
        if (musicPlayer.isOpen()) clearMusicSession();
        scanCard();
        drawSystemMenu(true);
      } else if (pressed & kMenuKey) {
        openSettings(View::SystemMenu);
      } else if (moveHomeSelection(pressed)) {
        drawSystemMenu(true, previous);
      } else if (pressed & (GB_PAD_A | GB_PAD_START)) {
        if (systemSelection == kSettingsMenuItem) {
          openSettings(View::SystemMenu);
        } else if (systemSelection == kMusicMenuItem) {
          openMusic();
        } else if (systemSelection == kFlashlightMenuItem) {
          openFlashlight();
        } else if (systemSelection == kContinueMenuItem) {
          continueLastGame();
        } else if (systemSelection == kMiniGamesMenuItem) {
          openMiniGames();
        } else if (systemSelection == kApplicationsMenuItem) {
          openApplications(false);
        } else if (systemSelection == kStandbyMenuItem) {
          view = View::Standby;
          StandbyDisplay::open();
        } else if (systemSelection == kPhotosMenuItem) {
          view = View::Photos;
          PhotoViewer::open(sdReady);
        } else if (systemSelection == kDoomMenuItem) {
          view = View::DoomMenu;
          drawDoomMenu();
        } else if (systemSelection == kFavoritesMenuItem || systemSelection == kRecentMenuItem) {
          view = systemSelection == kFavoritesMenuItem ? View::Favorites : View::RecentlyPlayed;
          drawGameList(true);
        } else {
          selectedSystem = static_cast<System>(systemSelection);
          view = View::GameList;
          drawGameList(true);
        }
      }
      delay(1);
      break;
    }
    case View::MiniGames:
      handleMiniGames(pressed);
      delay(1);
      break;
    case View::Applications:
      handleApplications(pressed, held | serialHeld, now);
      delay(1);
      break;
    case View::DoomMenu:
      handleDoomMenu(pressed);
      delay(1);
      break;
    case View::Standby:
      if (!StandbyDisplay::handle(pressed, pressed & kMenuKey)) {
        view = View::SystemMenu;
        drawSystemMenu(true);
      }
      delay(1);
      break;
    case View::Photos:
      if (!PhotoViewer::handle(pressed, pressed & kMenuKey, rescan)) {
        PhotoViewer::stop();
        view = View::SystemMenu;
        drawSystemMenu(true);
      }
      delay(1);
      break;
    case View::Flashlight:
      handleFlashlight(pressed);
      delay(1);
      break;
    case View::Music:
    case View::NowPlaying:
    case View::MusicSettings:
    case View::MusicScreenOff:
      handleMusicApp(pressed, rescan);
      delay(1);
      break;
    case View::GameList:
    case View::Favorites:
    case View::RecentlyPlayed: {
      const auto &games = visibleGames();
      const int count = int(games.size());
      int &selection = visibleGameSelection();
      if (rescan || (pressed & GB_PAD_SELECT)) {
        if (musicPlayer.isOpen()) clearMusicSession();
        scanCard();
        drawGameList(true);
      } else if (pressed & GB_PAD_B) {
        systemSelection = view == View::Favorites ? kFavoritesMenuItem :
                          view == View::RecentlyPlayed ? kRecentMenuItem : int(selectedSystem);
        view = View::SystemMenu;
        drawSystemMenu(true);
      } else if (pressed & kMenuKey) {
        if (count) openGameOptions(games[selection]);
        else showStatus("Highlight a game first.");
      } else if (count && (pressed & GB_PAD_RIGHT)) {
        if (toggleFavorite(games[selection])) {
          drawGameList(false);
        } else {
          showStatus("Could not update favorites.");
        }
      } else if (count && (pressed & kListScrollButtons)) {
        const int previous = selection;
        selection = (selection + ((pressed & GB_PAD_UP) ? count - 1 : 1)) % count;
        drawGameList(true, previous);
      } else if (count && (pressed & (GB_PAD_A | GB_PAD_START))) {
        if (selectGameByPath(games[selection])) {
          releaseMusicAudio();
          startGame();
        } else {
          showStatus("Game is unavailable.");
        }
      }
      delay(1);
      break;
    }
    case View::GameOptions:
      handleGameOptions(pressed);
      delay(1);
      break;
    case View::GameProfile:
      handleGameProfile(pressed);
      delay(1);
      break;
    case View::Bluetooth:
      handleBluetooth(pressed);
      delay(1);
      break;
    case View::WifiTransfer:
      handleWifiTransfer(pressed);
      delay(1);
      return;
    case View::DeviceTools:
      handleDeviceTools(pressed, held | serialHeld);
      delay(1);
      break;
    case View::Settings:
      handleSettings(pressed);
      delay(1);
      break;
    case View::AboutMenu:
      if (pressed & (GB_PAD_B | kMenuKey)) {
        view = View::Settings;
        drawSettings();
      } else if (pressed & (GB_PAD_UP | GB_PAD_DOWN)) {
        const int step = pressed & GB_PAD_UP ? kAboutMenuRows - 1 : 1;
        aboutMenuSelection = (aboutMenuSelection + step) % kAboutMenuRows;
        drawAboutMenu();
      } else if (pressed & (GB_PAD_A | GB_PAD_START | GB_PAD_RIGHT)) {
        if (aboutMenuSelection == 3) {
          openUpdates();
        } else if (aboutMenuSelection == 2) {
          openApplications(true);
        } else if (aboutMenuSelection == 1) {
          view = View::Diagnostics;
          DeviceLog::open();
        } else {
          view = View::About;
          aboutSelection = 0;
          drawAbout(true);
        }
      }
      delay(1);
      break;
    case View::Update:
      handleUpdates(pressed);
      delay(1);
      break;
    case View::Diagnostics:
      if (!DeviceLog::handle(pressed, pressed & (GB_PAD_B | kMenuKey), sdReady)) {
        view = View::AboutMenu;
        drawAboutMenu();
      }
      delay(1);
      break;
    case View::About: {
      if (pressed & (GB_PAD_B | kMenuKey)) {
        view = View::AboutMenu;
        drawAboutMenu();
      } else if (pressed & (GB_PAD_UP | GB_PAD_DOWN | GB_PAD_LEFT | GB_PAD_RIGHT)) {
        const int step = pressed & (GB_PAD_UP | GB_PAD_LEFT) ? -1 : 1;
        const int next = std::clamp(aboutSelection + step, 0, kAboutPageCount - 1);
        if (next != aboutSelection) {
          aboutSelection = next;
          drawAbout(false);
        }
      }
      delay(1);
      break;
    }
    case View::Playing: {
      if (pressed & kMenuKey) {
        stopVideoPresenter();
        bool gbaSaveFailed = false;
        if (gameIsGba) {
          gbaSaveFailed = !commitGbaBatteryAtSafePoint();
        }
        view = View::Paused;
        pauseChoice = 0;
        drawPause();
        if (gbaSaveFailed) {
          showStatus("Battery save failed. Check the SD card.");
        }
        break;
      }
      const int64_t tick = esp_timer_get_time();
      if (tick < nextFrame) {
        if (gameIsGba) {
          flushGbaAudio();
        }
        delay(1);
        break;
      }
      int64_t frameDebt = tick - nextFrame;
      const int64_t frameUs = !activeCartridgeCore ? kFrameUs
                             : activeCartridgeCore->framePeriodUs ? activeCartridgeCore->framePeriodUs()
                             : 1000000 / activeCartridgeCore->refreshRate();
      const int64_t originalFrameDebt = frameDebt;
      const bool late = frameDebt > frameUs;
      if (frameDebt > 4 * frameUs) {
        nextFrame = tick;
        frameDebt = 0;
      }
      nextFrame += frameUs;
      const uint16_t keys = held | serialHeld;
      if (activeCartridgeCore == &pocketGenesis) {
        const bool draw = shouldDrawGenesisFrame(originalFrameDebt);
        const int64_t coreStart = esp_timer_get_time();
        activeCartridgeCore->run(keys, draw);
        recordGameFrame(draw, esp_timer_get_time() - coreStart);
        genesisSkipped = draw ? 0 : genesisSkipped + 1;
        if (draw) {
          const CartridgeFrame frame = activeCartridgeCore->frame();
          if (asyncVideo) gamePerf.displayBusy += !submitCartridgeFrame(frame);
          else presentCartridgeFrame(frame, gameGbScale());
        }
        if (gameSoundEnabled() && audioReady) {
          playAudio(samples, activeCartridgeCore->audio(samples, std::size(samples)));
        }
        reportGamePerf("Genesis");
      } else if (activeCartridgeCore) {
        const bool draw = !late || gbaSkipped != 0;
        activeCartridgeCore->run(keys, draw);
        if (gameSoundEnabled() && audioReady) {
          playAudio(samples, activeCartridgeCore->audio(samples, std::size(samples)));
        }
        if (draw) {
          const CartridgeFrame frame = activeCartridgeCore->frame();
          if (asyncVideo) submitCartridgeFrame(frame);
          else Screen::drawCartridgeFrame(frame, gameGbScale());
        }
        gbaSkipped = draw ? 0 : 1;
      } else if (gameIsGba) {
        // The core renders into a separate framebuffer. The presenter can
        // finish sending the last frame while the core renders this one.
        const bool draw = shouldDrawGbaFrame(frameDebt);
        const int64_t coreStart = esp_timer_get_time();
        if (!gbaCoreRun(keys, draw)) {
          stopVideoPresenter();
          view = View::Paused;
          pauseChoice = pauseItemCount() - 1;
          drawPause();
          showStatus("GBA ROM read failed. Check SD.");
          break;
        }
        recordGameFrame(draw, esp_timer_get_time() - coreStart);
        if (draw) {
          bool frameSubmitted = true;
          if (asyncVideo) {
            frameSubmitted = submitVideoFrame(gbaCorePixels());
            gamePerf.displayBusy += !frameSubmitted;
          } else {
            frameSubmitted = presentGbaFrame(gbaCorePixels(), gameGbaFullscreen());
          }
          // Only reset the skip counter when display submission succeeds.
          if (frameSubmitted) {
            gbaSkipped = 0;
          } else {
            ++gbaSkipped;
          }
        } else {
          ++gbaSkipped;
        }
        queueGbaAudio();
        reportGamePerf("GBA");
      } else {
        gnuboy_set_pad(keys & 0xFF);
        gnuboy_run(!late);
      }
      // Writing gpSP's fixed 128 KiB battery image to SD causes a visible
      // hitch. GBA saves are committed at pause and quit instead.
      if (batteryBacked && !gameIsGba && now - lastSaveAttempt >= kAutoSaveMs) {
        lastSaveAttempt = now;
        if (saveRetry || batteryChanged()) {
          saveRetry = !commitSave(saveFile, false);
          if (saveRetry) {
            Serial.println("Battery save failed. Check the SD card.");
          }
        }
      }
      break;
    }
    case View::Paused: {
      if (statePrompt != StatePrompt::None) {
        handleStatePrompt(pressed);
      } else if (pressed & (GB_PAD_B | kMenuKey)) {
        resumeGame();
      } else if (pressed & (GB_PAD_UP | GB_PAD_DOWN)) {
        const int previous = pauseChoice;
        const int count = pauseItemCount();
        pauseChoice = (pauseChoice + ((pressed & GB_PAD_UP) ? count - 1 : 1)) % count;
        drawPause(previous);
      } else if (pressed & (GB_PAD_A | GB_PAD_START | GB_PAD_LEFT | GB_PAD_RIGHT)) {
        const PauseItem item = pauseAction(pauseChoice);
        const bool activate = pressed & (GB_PAD_A | GB_PAD_START);
        const bool opensSettings = item == PauseItem::Settings && (pressed & GB_PAD_RIGHT);
        if (activate || pauseOptionAdjustable(item) || opensSettings) {
          applyPauseChoice(item, pressed & GB_PAD_LEFT ? -1 : 1, activate);
        }
      }
      delay(1);
      break;
    }
    case View::Failed:
      delay(50);
      break;
  }
  const uint32_t musicSleepDelay = kMusicSleepDelays[musicSleepOption];
  if (musicSleepDelay && millis() - musicSleepStartedAt >= musicSleepDelay) {
    stopMusicPlayback("Sleep timer ended.");
  }
  if (musicPlayer.isOpen()) {
    serviceMusic();
  }
  serviceMusicFolderScan();
  if (view == View::NowPlaying) drawMusicVisualizer(false);
  serviceMusicTitle();
  const bool controlsActive = raw || externalHeld || pressed || serialHeld || rescan;
  serviceGameCover(controlsActive);
  if (view == View::Photos) PhotoViewer::service(controlsActive, musicKeepsCardBusy());
  serviceFlashlight();
  serviceMenuIndicators();
  serviceDeviceLog();
  serviceAutoSleep(raw || externalHeld || pressed || rescan);
}
