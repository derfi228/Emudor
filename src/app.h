#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <deque>
#include <future>
#include <memory>
#include <cstdint>

#include "console/iconsole.h"
#include "ui_theme.h"

enum class AppState  { MainMenu, Playing, Paused };
enum class ColorMode { Normal, Inverted, BlackWhite };
enum class ThemeMode { Light, Dark };

struct RomEntry {
    std::string  path;
    std::string  name;
    std::string  console;            // "NES", "SNES" и т.д.
    SDL_Texture* cover   = nullptr;
    bool         custom  = false;   // добавлен через [+], не из roms/
};

// Раскладки клавиш. Для NES активны up/down/left/right/a/b/select/start.
// Для SNES дополнительно используются x, l, r (X-кнопка и шифты L/R).
struct KeyConfig {
    SDL_Keycode up     = SDLK_w;
    SDL_Keycode down   = SDLK_s;
    SDL_Keycode left   = SDLK_a;
    SDL_Keycode right  = SDLK_d;
    SDL_Keycode a      = SDLK_2;
    SDL_Keycode b      = SDLK_1;
    SDL_Keycode select = SDLK_e;
    SDL_Keycode start  = SDLK_r;
    // Только SNES:
    SDL_Keycode x      = SDLK_3;
    SDL_Keycode y      = SDLK_4;
    SDL_Keycode l      = SDLK_q;
    SDL_Keycode rsh    = SDLK_t;   // R shoulder (rsh — чтобы не путать с right)
};

struct SaveSlot {
    int         index    = 0;
    std::string label    = "";
    std::string filename = "";
    bool        hasData  = false;
    bool        isAuto   = false;  // true = авто-слот из внутриигрового save
};

// Задание на скачивание обложки
struct CoverJob {
    int         romIndex;
    std::string gameName;   // для URL (имя файла без расширения)
    std::string console;    // "NES", "SNES" и т.д. — для выбора системы в URL
};

class App {
public:
    App()  = default;
    ~App() = default;
    void run();

    // Автозапуск ROM из командной строки (до run())
    std::string pendingRomPath_;

private:
    SDL_Window*        window_      = nullptr;
    SDL_Renderer*      renderer_    = nullptr;
    SDL_Texture*       gameTexture_ = nullptr;
    SDL_AudioDeviceID  audioDevice_ = 0;

    // Активная консоль (NesConsole, SnesConsole, …); nullptr вне игры
    std::unique_ptr<IConsole> console_;

    AppState              state_       = AppState::MainMenu;
    std::vector<RomEntry> romList_;
    std::vector<std::string> romFolders_;   // папки для авто-сканирования
    KeyConfig             nesKeys_;
    KeyConfig             snesKeys_;
    ThemeMode             themeMode_   = ThemeMode::Light;
    ColorMode             colorMode_   = ColorMode::Normal;
    int                   rebindIndex_   = -1;     // 0..11 — индекс кнопки
    bool                  rebindSnes_    = false;  // false = NES, true = SNES
    bool                  romLoaded_      = false;
    std::string           currentRomName_;        // безопасное имя текущей игры (для папки сохранений)
    bool                  hasGearGlyph_   = false;  // загружен ⚙ из системного шрифта

    // ─── Дизайн UI (design_handoff_emudor_ui) ─────────────────────────────────
    UiTheme  theme_;                 // текущая палитра (пересчитывается в applyXTheme)
    UiFonts  fonts_;                 // Anton/Oswald/Instrument Serif/Inter
    char     searchBuf_[128] = "";   // текст в поле поиска библиотеки
    float    refreshSpinT_   = 0.0f; // 0..1 — прогресс анимации спина кнопки ↻

    // Save/Load
    bool                  showSaveModal_  = false;
    bool                  showLoadModal_  = false;
    bool                  showSettings_   = false;
    std::vector<SaveSlot> saveSlots_;

    // Авто-сохранение при внутриигровом save (battery SRAM)
    // Отсчёт: 180 кадров (~3 с) тишины после последней записи в SRAM
    int  sramDirtyCountdown_ = 0;

    // Фоновая загрузка обложек
    std::deque<CoverJob>                          coverQueue_;
    std::future<std::pair<int,std::string>>       coverFuture_;
    bool                                          coverDlActive_ = false;

    void init();
    void shutdown();
    void handleEvents(bool& running);
    void update();
    void render();

    void renderMainMenu();
    void renderGame();
    void renderPauseOverlay();
    void renderSaveModal();
    void renderLoadModal();
    void renderSettings();

    // ROM
    void addRomEntry      (const std::string& path);
    void launchROM        (const std::string& path);
    void scanRomsFolder   ();                          // устаревший, для совместимости
    void scanRomFolders   ();                          // сканирует все romFolders_
    void pruneDeletedRoms ();                          // удаляет несуществующие файлы

    // Обложки
    void enqueueCoverFetch(int romIndex);
    void processCoverQueue();

    // Save/Load
    void saveState(int slot);
    void loadState(int slot);
    void saveStateToFile(const std::string& path);
    void loadStateFromFile(const std::string& path);
    void triggerAutoSave();       // battery SRAM → sram.bin + autosave.sav
    void refreshSaveSlots();

    // Темы
    void applyLightTheme();
    void applyDarkTheme();

    // Конфиг
    void loadConfig();
    void saveConfig();
};
