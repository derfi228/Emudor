#include "app.h"
#include "console/nes_console.h"
#include "console/snes_console.h"
#include "console/console_detect.h"
#include "ui_pads.h"
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <SDL2/SDL_image.h>
#include <nlohmann/json.hpp>
#include <tinyfiledialogs.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <utility>
#include <sstream>
#include <cstring>
#include <cstdio>

#ifdef NES_HAVE_CURL
#  include <curl/curl.h>
#endif

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ImU32 (упаковка ImGui) → ImVec4 — используется во всех render*-функциях
// для перевода UiTheme-токенов в цвета ImGuiStyle/PushStyleColor.
static ImVec4 ToVec4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// Instrument Serif (fonts_.display/displayItalic) физически не содержит
// кириллицу (Google Fonts subsets: только latin/latin-ext) — для RU/ZH
// показываем перевод через Inter вместо тофу-квадратов. Названия игр всегда
// латиница (не переводятся), поэтому им этот хелпер не нужен.
static ImFont* displayFontFor(Lang lang, const UiFonts& f) {
    if (lang == Lang::RU || lang == Lang::ZH) return f.uiSemiBold ? f.uiSemiBold : f.ui;
    return f.displayItalic ? f.displayItalic : f.ui;
}

// Вставляет пробел между символами UTF-8 строки (порт letter-spacing:.35em
// для "PAUSED") — по кодовым точкам, а не байтам, чтобы не ломать кириллицу/CJK.
static std::string spaceOutUtf8(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t len = 1;
        unsigned char c = (unsigned char)s[i];
        if      ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (!out.empty()) out += ' ';
        out += s.substr(i, len);
        i += len;
    }
    return out;
}

// ─── Вспомогательные функции для обложек ────────────────────────────────────

// URL-кодирование (пробел → %20, спецсимволы → %XX)
static std::string urlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    char buf[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else if (c == ' ') {
            out += "%20";
        } else {
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Превращаем имя ROM в безопасное имя файла
static std::string safeFilename(const std::string& name) {
    std::string s = name;
    for (auto& c : s)
        if (c == '\\' || c == '/' || c == ':' || c == '*' ||
            c == '?'  || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    return s;
}

// Убираем теги региона/версии: "Donkey Kong (USA)" → "Donkey Kong"
static std::string stripRegion(const std::string& name) {
    std::string s = name;
    // Убираем суффиксы вида " (USA)", " [!]" и т.д.
    for (char open : {'(', '['}) {
        auto pos = s.rfind(open);
        if (pos != std::string::npos && pos > 0) s = s.substr(0, pos);
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    // Заменяем подчёркивания на пробелы
    for (auto& c : s) if (c == '_') c = ' ';
    return s;
}

// Главная функция скачивания (запускается в потоке)
// Возвращает (romIndex, путь к локальному файлу) или (romIndex, "")
static std::pair<int,std::string> downloadCoverTask(int romIndex, std::string gameName,
                                                    std::string consoleId) {
#ifndef NES_HAVE_CURL
    (void)romIndex; (void)gameName; (void)consoleId;
    return {romIndex, ""};
#else
    fs::create_directories("covers");

    // Пробуем несколько вариантов имени: исходное → без региона
    std::vector<std::string> names;
    names.push_back(gameName);
    std::string stripped = stripRegion(gameName);
    if (stripped != gameName) names.push_back(stripped);

    // Системный путь в Libretro thumbnails
    std::string kSystem;
    if      (consoleId == "SNES") kSystem = "Nintendo - Super Nintendo Entertainment System";
    else                          kSystem = "Nintendo - Nintendo Entertainment System";

    for (const auto& n : names) {
        std::string localPath = "covers/" + safeFilename(n) + ".png";

        // Уже скачано?
        if (fs::exists(localPath)) return {romIndex, localPath};

        std::string url = "https://thumbnails.libretro.com/"
                        + urlEncode(kSystem) + "/Named_Boxarts/"
                        + urlEncode(n) + ".png";

        CURL* curl = curl_easy_init();
        if (!curl) continue;

        std::ofstream f(localPath, std::ios::binary);
        if (!f) { curl_easy_cleanup(curl); continue; }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](char* ptr, size_t sz, size_t nmemb, void* ud) -> size_t {
                static_cast<std::ofstream*>(ud)->write(ptr, sz * nmemb);
                return sz * nmemb;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &f);

        CURLcode res = curl_easy_perform(curl);
        long http   = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
        curl_easy_cleanup(curl);
        f.close();

        if (res == CURLE_OK && http == 200)
            return {romIndex, localPath};

        fs::remove(localPath);  // удаляем неудачный файл
    }
    return {romIndex, ""};
#endif
}

// ─── Инициализация ───────────────────────────────────────────────────────────

void App::init() {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);

    window_ = SDL_CreateWindow(
        "Emudor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI
    );
    renderer_ = SDL_CreateRenderer(
        window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    gameTexture_ = SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, 256, 240
    );

    // ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // ── Дизайн-шрифты (design_handoff_emudor_ui): Anton/Oswald/Instrument
    // Serif/Inter из assets/fonts/. Inter Regular 16px становится основным
    // UI-шрифтом ImGui; если загрузка не удалась — старый Segoe UI/Tahoma
    // fallback, чтобы приложение не осталось совсем без текста.
    fonts_ = LoadUiFonts(io);
    bool mainFontLoaded = (fonts_.ui != nullptr);
    if (mainFontLoaded) {
        io.FontDefault = fonts_.ui;
    } else {
        ImFontConfig fc;
        fc.OversampleH = 3;
        fc.OversampleV = 2;
        fc.PixelSnapH  = false;
        const char* fontCandidates[] = {
            "assets/fonts/Cousine-Regular.ttf",  // вендорный
            "C:/Windows/Fonts/segoeui.ttf",      // Windows Segoe UI
            "C:/Windows/Fonts/tahoma.ttf",       // Windows Tahoma (запасной)
        };
        for (const char* fp : fontCandidates) {
            if (fs::exists(fp)) {
                io.Fonts->AddFontFromFileTTF(fp, 18.0f, &fc);
                mainFontLoaded = true;
                break;
            }
        }
        if (!mainFontLoaded)
            io.Fonts->AddFontDefault();
    }

    // Шестерёнка ⚙ (U+2699) из Segoe UI Symbol — только если основной
    // шрифт загружен через TTF (AddFontDefault даёт "implicit ref size",
    // несовместимый с MergeMode + явным SizePixels → imgui_draw.cpp:3116)
    if (mainFontLoaded) {
        const char* segoeSymPath = "C:/Windows/Fonts/seguisym.ttf";
        if (fs::exists(segoeSymPath)) {
            ImFontConfig mc;
            mc.MergeMode     = true;
            mc.OversampleH   = 2;
            mc.GlyphOffset.y = 1.5f;
            static const ImWchar gearRange[] = { 0x2699u, 0x2699u, 0u };
            io.Fonts->AddFontFromFileTTF(segoeSymPath, 18.0f, &mc, gearRange);
            hasGearGlyph_ = true;
        }
    }

    ImGui_ImplSDL2_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer2_Init(renderer_);

    // SDL Audio
    SDL_AudioSpec want{}, have{};
    want.freq = 44100; want.format = AUDIO_F32SYS;
    want.channels = 1; want.samples = 512; want.callback = nullptr;
    audioDevice_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audioDevice_ > 0) SDL_PauseAudioDevice(audioDevice_, 0);

#ifdef NES_HAVE_CURL
    curl_global_init(CURL_GLOBAL_ALL);
#endif

    loadConfig();
    scanRomsFolder();   // сканирует roms/ — для обратной совместимости
    scanRomFolders();   // сканирует пользовательские папки из конфига
    pruneDeletedRoms(); // удаляем несуществующие ROM из списка
    refreshSaveSlots();

    if (themeMode_ == ThemeMode::Light) applyLightTheme();
    else                                applyDarkTheme();

    // Автозапуск ROM из командной строки
    if (!pendingRomPath_.empty()) {
        launchROM(pendingRomPath_);
        pendingRomPath_.clear();
    }
}

void App::shutdown() {
    // Ждём завершения текущей загрузки
    if (coverDlActive_ && coverFuture_.valid())
        coverFuture_.wait();

    for (auto& e : romList_)
        if (e.cover) SDL_DestroyTexture(e.cover);

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (audioDevice_) SDL_CloseAudioDevice(audioDevice_);
    if (gameTexture_) SDL_DestroyTexture(gameTexture_);
    if (renderer_)    SDL_DestroyRenderer(renderer_);
    if (window_)      SDL_DestroyWindow(window_);

    IMG_Quit();
    SDL_Quit();

#ifdef NES_HAVE_CURL
    curl_global_cleanup();
#endif
}

// ─── Главный цикл ────────────────────────────────────────────────────────────

void App::run() {
    init();
    bool running = true;
    while (running) {
        handleEvents(running);
        update();
        render();
    }
    shutdown();
}

// ─── События ─────────────────────────────────────────────────────────────────

void App::handleEvents(bool& running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ImGui_ImplSDL2_ProcessEvent(&e);
        if (e.type == SDL_QUIT) { running = false; return; }

        if (rebindIndex_ >= 0 && e.type == SDL_KEYDOWN) {
            KeyConfig& kc = rebindSnes_ ? snesKeys_ : nesKeys_;
            // Расширенный список: для NES активны 0..7, для SNES — все 12
            SDL_Keycode* keys[12] = {
                &kc.up, &kc.down, &kc.left, &kc.right,
                &kc.a,  &kc.b,   &kc.select, &kc.start,
                &kc.x,  &kc.y,   &kc.l,      &kc.rsh
            };
            int maxIdx = rebindSnes_ ? 12 : 8;
            if (rebindIndex_ < maxIdx) {
                *keys[rebindIndex_] = e.key.keysym.sym;
            }
            rebindIndex_ = -1;
            saveConfig();
            return;
        }

        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_TAB) {
            if      (state_ == AppState::Playing) state_ = AppState::Paused;
            else if (state_ == AppState::Paused)  state_ = AppState::Playing;
        }
    }

    if (state_ == AppState::Playing && console_) {
        const uint8_t* kb = SDL_GetKeyboardState(nullptr);
        bool isSnes = console_->getConsoleName() == "SNES";
        const KeyConfig& kc = isSnes ? snesKeys_ : nesKeys_;

        if (isSnes) {
            // SNES контроллер 16-бит: B(15) Y(14) Sel(13) Start(12) Up(11) Down(10)
            // Left(9) Right(8) A(7) X(6) L(5) R(4) 0(3..0)
            uint16_t ctrl = 0;
            if (kb[SDL_GetScancodeFromKey(kc.b)])      ctrl |= (1u<<15);
            if (kb[SDL_GetScancodeFromKey(kc.y)])      ctrl |= (1u<<14);
            if (kb[SDL_GetScancodeFromKey(kc.select)]) ctrl |= (1u<<13);
            if (kb[SDL_GetScancodeFromKey(kc.start)])  ctrl |= (1u<<12);
            if (kb[SDL_GetScancodeFromKey(kc.up)])     ctrl |= (1u<<11);
            if (kb[SDL_GetScancodeFromKey(kc.down)])   ctrl |= (1u<<10);
            if (kb[SDL_GetScancodeFromKey(kc.left)])   ctrl |= (1u<< 9);
            if (kb[SDL_GetScancodeFromKey(kc.right)])  ctrl |= (1u<< 8);
            if (kb[SDL_GetScancodeFromKey(kc.a)])      ctrl |= (1u<< 7);
            if (kb[SDL_GetScancodeFromKey(kc.x)])      ctrl |= (1u<< 6);
            if (kb[SDL_GetScancodeFromKey(kc.l)])      ctrl |= (1u<< 5);
            if (kb[SDL_GetScancodeFromKey(kc.rsh)])    ctrl |= (1u<< 4);
            console_->setInput(0, ctrl);
        } else {
            // NES 8-бит: A|B|Sel|Sta|Up|Dn|L|R
            uint16_t ctrl = 0;
            if (kb[SDL_GetScancodeFromKey(kc.a)])      ctrl |= (1u<<7);
            if (kb[SDL_GetScancodeFromKey(kc.b)])      ctrl |= (1u<<6);
            if (kb[SDL_GetScancodeFromKey(kc.select)]) ctrl |= (1u<<5);
            if (kb[SDL_GetScancodeFromKey(kc.start)])  ctrl |= (1u<<4);
            if (kb[SDL_GetScancodeFromKey(kc.up)])     ctrl |= (1u<<3);
            if (kb[SDL_GetScancodeFromKey(kc.down)])   ctrl |= (1u<<2);
            if (kb[SDL_GetScancodeFromKey(kc.left)])   ctrl |= (1u<<1);
            if (kb[SDL_GetScancodeFromKey(kc.right)])  ctrl |= (1u<<0);
            console_->setInput(0, ctrl);
        }
    }
}

// ─── Update ──────────────────────────────────────────────────────────────────

void App::update() {
    // Обрабатываем очередь скачивания обложек каждый кадр
    processCoverQueue();

    // ── Frame throttling: синхронизация по аудио или по таймеру ──────────────
    {
        static uint32_t lastFrameTime  = 0;
        static uint32_t frameCount     = 0;
        static uint32_t fpsTimer       = 0;
        static uint32_t displayedFps   = 0;

        // Если аудио играет — синхронизируемся по нему (заполненность очереди).
        // Это устраняет крекинг (под/перепроизводство аудио) и заодно
        // даёт ровный 60 FPS, потому что аудио идёт с фиксированной частотой.
        bool audioSync = false;
        if (audioDevice_ > 0 && state_ == AppState::Playing) {
            // Цель: ~50 мс в очереди. Если больше — ждём, чтобы дать аудио
            // выйграться. Если меньше — продолжаем без задержки.
            const uint32_t targetQ = 44100 * sizeof(float) / 20;
            int spinGuard = 200;  // защита от вечного сна
            while (SDL_GetQueuedAudioSize(audioDevice_) > targetQ && spinGuard-- > 0) {
                SDL_Delay(1);
            }
            audioSync = true;
        }

        uint32_t now = SDL_GetTicks();

        // Если аудио недоступно — таймер с целью ~16.67 мс/кадр
        if (!audioSync) {
            constexpr uint32_t TARGET_MS = 16;
            if (lastFrameTime != 0) {
                uint32_t elapsed = now - lastFrameTime;
                if (elapsed < TARGET_MS) {
                    SDL_Delay(TARGET_MS - elapsed);
                    now = SDL_GetTicks();
                }
            }
        }
        lastFrameTime = now;

        // Счётчик FPS в заголовке окна (обновляем раз в секунду)
        ++frameCount;
        if (now - fpsTimer >= 1000u) {
            displayedFps = frameCount;
            frameCount   = 0;
            fpsTimer     = now;
            if (state_ == AppState::Playing && console_) {
                std::string title = std::string("Emudor — ")
                    + console_->getConsoleName()
                    + " | " + std::to_string(displayedFps) + " FPS";
                SDL_SetWindowTitle(window_, title.c_str());
            }
        }
    }

    if (state_ != AppState::Playing || !romLoaded_ || !console_) return;

    // ── Авто-сохранение при внутриигровом save (battery SRAM) ─────────────────
    if (console_->hasBattery()) {
        if (console_->isSramDirty()) {
            console_->clearSramDirty();
            sramDirtyCountdown_ = 180;  // перезапустить таймер (~3 с)
        } else if (sramDirtyCountdown_ > 0) {
            if (--sramDirtyCountdown_ == 0) {
                triggerAutoSave();
            }
        }
    }

    // ── Один кадр эмуляции ────────────────────────────────────────────────────
    console_->runFrame();

    // ── Аудио → SDL очередь ───────────────────────────────────────────────────
    // Throttling по очереди (выше) уже синхронизирует скорость, так что здесь
    // мы редко превышаем порог. Порог сделан большим (200 мс), чтобы случайные
    // долгие кадры не теряли семплы (это основная причина крекинга).
    if (audioDevice_ > 0) {
        const auto& samples = console_->getAudioSamples();
        if (!samples.empty()) {
            const uint32_t maxQ = 44100 * sizeof(float) / 5;  // 200 мс
            if (SDL_GetQueuedAudioSize(audioDevice_) < maxQ)
                SDL_QueueAudio(audioDevice_,
                               samples.data(),
                               (uint32_t)(samples.size() * sizeof(float)));
        }
        console_->clearAudioSamples();
    }

    // ── Постобработка цвета (инверсия / ч/б) ─────────────────────────────────
    if (colorMode_ != ColorMode::Normal) {
        uint32_t* fb    = console_->getFramebuffer();
        int       count = console_->getFrameWidth() * console_->getFrameHeight();
        for (int i = 0; i < count; i++) {
            uint32_t& px = fb[i];
            uint8_t r = (uint8_t)((px >> 16) & 0xFF);
            uint8_t g = (uint8_t)((px >>  8) & 0xFF);
            uint8_t b = (uint8_t)( px        & 0xFF);
            if (colorMode_ == ColorMode::Inverted) {
                r = (uint8_t)(255-r); g = (uint8_t)(255-g); b = (uint8_t)(255-b);
            } else {
                uint8_t gr = (uint8_t)(0.299f*r + 0.587f*g + 0.114f*b);
                r = g = b = gr;
            }
            px = (px & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }

    SDL_UpdateTexture(gameTexture_, nullptr,
                      console_->getFramebuffer(),
                      console_->getFrameWidth() * (int)sizeof(uint32_t));
}

// ─── Рендеринг ───────────────────────────────────────────────────────────────

void App::render() {
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    switch (state_) {
    case AppState::MainMenu: renderMainMenu(); break;
    case AppState::Playing:  renderGame();     break;
    case AppState::Paused:
        renderGame();
        renderPauseOverlay();
        renderSaveModal();
        renderLoadModal();
        break;
    }

    ImGui::Render();

    // Фон: чёрный во время игры (для рамок), иначе — цвет темы
    bool inGame = (state_ == AppState::Playing || state_ == AppState::Paused);
    if (inGame) {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    } else {
        ImVec4 bg = ImGui::ColorConvertU32ToFloat4(theme_.bg);
        SDL_SetRenderDrawColor(renderer_,
            (Uint8)(bg.x*255), (Uint8)(bg.y*255), (Uint8)(bg.z*255), 255);
    }
    SDL_RenderClear(renderer_);

    if (inGame && console_ && gameTexture_) {
        int winW, winH;
        SDL_GetWindowSize(window_, &winW, &winH);

        int srcW = console_->getFrameWidth();
        int srcH = console_->getFrameHeight();

        // SNES: пиксельное соотношение 8:7 → физически шире
        // NES:  пиксельное соотношение 8:7 тоже (NTSC), но для простоты 1:1
        bool isSNES = (console_->getConsoleName() == "SNES");
        // Логическое соотношение: для SNES display width = srcW * 8/7
        int logW = isSNES ? (srcW * 8 / 7) : srcW;
        int logH = srcH;

        // Масштаб fit-in-window
        float scaleX = (float)winW / (float)logW;
        float scaleY = (float)winH / (float)logH;
        float scale  = std::min(scaleX, scaleY);
        if (scale > 1.0f && !isSNES) scale = (float)(int)scale;  // целые пиксели для NES

        int tw = (int)(logW * scale);
        int th = (int)(logH * scale);
        SDL_Rect dst = { (winW - tw) / 2, (winH - th) / 2, tw, th };
        SDL_RenderCopy(renderer_, gameTexture_, nullptr, &dst);
    }

    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer_);
    SDL_RenderPresent(renderer_);
}

// ─── Главный экран ────────────────────────────────────────────────────────────

// Пара цветов градиента-заглушки для игр без обложки — детерминированно из
// имени (хэш), чтобы у разных игр были разные, но стабильные между кадрами
// оттенки (как c1/c2 в GAMES[] макета).
static void placeholderGradient(const std::string& name, ImU32& c1, ImU32& c2) {
    uint32_t h = 2166136261u;
    for (unsigned char c : name) { h ^= c; h *= 16777619u; }
    float hue = (float)(h % 360u);
    ImVec4 top, bot;
    ImGui::ColorConvertHSVtoRGB(hue/360.0f, 0.55f, 0.62f, top.x, top.y, top.z);
    ImGui::ColorConvertHSVtoRGB(hue/360.0f, 0.65f, 0.24f, bot.x, bot.y, bot.z);
    c1 = ImGui::ColorConvertFloat4ToU32({top.x,top.y,top.z,1.0f});
    c2 = ImGui::ColorConvertFloat4ToU32({bot.x,bot.y,bot.z,1.0f});
}

static const UiTheme::PadTint& padTintFor(const UiTheme& t, const std::string& console) {
    if (console == "NES")  return t.padNes;
    if (console == "SNES") return t.padSnes;
    if (console == "GB")   return t.padGb;
    if (console == "GBA")  return t.padGba;
    if (console == "N64")  return t.padN64;
    if (console == "PS1")  return t.padPs1;
    return t.padNes;
}

void App::renderMainMenu() {
    int winW, winH;
    SDL_GetWindowSize(window_, &winW, &winH);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(theme_.bg));
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({(float)winW, (float)winH});
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const I18nStrings& tr = GetI18n(lang_);

    // ── Фон: мягкое диагональное свечение (порт body::before radial-gradient).
    // Плоская заливка читается как «неоформленная»; лёгкий градиент от bg-2 в
    // верхнем правом углу к bg внизу-слева даёт глубину, не отвлекая от карточек.
    dl->AddRectFilledMultiColor({0.0f, 0.0f}, {(float)winW, (float)winH},
        theme_.bg, theme_.bg2, theme_.bg, theme_.bg);
    dl->AddRectFilledMultiColor({winW * 0.45f, 0.0f}, {(float)winW, winH * 0.5f},
        theme_.accentGlow & 0x00FFFFFFu, (theme_.accentGlow & 0x00FFFFFFu) | 0x14000000u,
        theme_.accentGlow & 0x00FFFFFFu, theme_.accentGlow & 0x00FFFFFFu);

    // ── Шапка (header.app-bar): [+] [↻]   EMUDOR   [поиск] [⚙] ────────────────
    const float padX    = 40.0f;
    const float topY    = 22.0f;
    const float btnSize = 44.0f;
    const float btnGap  = 10.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);

    // ── [+] Добавить ROM ─────────────────────────────────────────────────────
    // Главное действие в шапке, поэтому рисуем вручную: акцентное свечение,
    // внутренний блик сверху и векторный плюс (текстовый глиф "+" в кнопке
    // ImGui сидел не по центру и выглядел тонким).
    {
        ImVec2 bp0 = {padX, topY};
        ImGui::SetCursorScreenPos(bp0);
        bool addClicked = ImGui::InvisibleButton("##addrom", {btnSize, btnSize});
        bool hov = ImGui::IsItemHovered();
        bool act = ImGui::IsItemActive();

        float lift = (hov && !act) ? -1.0f : 0.0f;   // приподнимается под курсором
        ImVec2 q0 = {bp0.x, bp0.y + lift};
        ImVec2 q1 = {q0.x + btnSize, q0.y + btnSize};
        const float rad = 13.0f;

        // Внешнее свечение акцентом (мягкая тень цвета кнопки)
        int layers = hov ? 6 : 3;
        for (int gi = layers; gi >= 1; --gi) {
            float g  = (float)gi * 1.9f;
            int   al = (hov ? 34 : 20) - gi * 3;
            if (al <= 0) continue;
            dl->AddRect({q0.x - g, q0.y - g + 2.0f}, {q1.x + g, q1.y + g + 2.0f},
                        (theme_.accent & 0x00FFFFFFu) | ((ImU32)al << 24),
                        rad + g, 0, 1.7f);
        }

        // Заливка: при нажатии темнее, при наведении чуть насыщеннее
        ImU32 fill = act ? theme_.accentDeep : theme_.accent;
        dl->AddRectFilled(q0, q1, fill, rad);
        // Блик по верхней половине + светлая кромка — объём без градиента
        dl->AddRectFilled(q0, {q1.x, q0.y + btnSize * 0.46f},
                          IM_COL32(255, 255, 255, act ? 18 : 38),
                          rad, ImDrawFlags_RoundCornersTop);
        dl->AddLine({q0.x + rad * 0.7f, q0.y + 1.5f}, {q1.x - rad * 0.7f, q0.y + 1.5f},
                    IM_COL32(255, 255, 255, 90), 1.2f);
        dl->AddRect(q0, q1, theme_.accentDeep, rad, 0, 1.0f);

        // Векторный плюс
        {
            ImVec2 c = {(q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f};
            float  a = btnSize * 0.26f;   // половина длины штриха
            ImU32  ink = IM_COL32(255, 248, 237, 255);
            dl->AddLine({c.x - a, c.y}, {c.x + a, c.y}, ink, 2.8f);
            dl->AddLine({c.x, c.y - a}, {c.x, c.y + a}, ink, 2.8f);
        }

        if (addClicked) {
            const char* filters[] = {"*.nes", "*.sfc", "*.smc", "*.fig", "*.swc"};
            const char* romPath = tinyfd_openFileDialog(
                "Add ROM to Library", "", 5, filters, "ROM files (NES/SNES)", 0);
            if (romPath) addRomEntry(romPath);
        }
        if (hov) ImGui::SetTooltip("%s", tr.addRom);
    }

    // [↻] Пересканировать папки — обычная (surface) кнопка
    ImGui::SetCursorPos({padX + btnSize + btnGap, topY});
    if (ImGui::Button(u8"↻", {btnSize, btnSize})) {
        pruneDeletedRoms();
        scanRomsFolder();
        scanRomFolders();
        saveConfig();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr.rescan);

    // Логотип EMUDOR — Anton с эффектом объёма (стек смещённых обводок,
    // порт --logo-extrude-1..4 из CSS text-shadow).
    {
        const char* logoText = "EMUDOR";
        ImFont* lf = fonts_.logo ? fonts_.logo : ImGui::GetFont();
        // Порт CSS clamp(48px, 5.4vw, 76px) — растёт с шириной окна, раньше
        // было жёстко зашито 46px (логотип выглядел мелким на широких окнах).
        float   logoSize = winW * 0.054f;
        if (logoSize < 48.0f) logoSize = 48.0f;
        if (logoSize > 76.0f) logoSize = 76.0f;
        ImVec2  tsz = lf->CalcTextSizeA(logoSize, FLT_MAX, 0.0f, logoText);
        ImVec2  base = { winW * 0.5f - tsz.x * 0.5f, topY - 3.0f };
        float   k = logoSize / 76.0f;   // масштаб относительно эталонных 76px из CSS
        struct { float d; ImU32 c; } layers[] = {
            {6.0f, theme_.logoExtrude4}, {5.0f, theme_.logoExtrude3},
            {4.0f, theme_.logoExtrude2}, {3.0f, theme_.logoExtrude1},
            {2.0f, theme_.logoExtrude1},
        };
        for (auto& L : layers)
            dl->AddText(lf, logoSize, {base.x + L.d*k, base.y + L.d*k}, L.c, logoText);
        dl->AddText(lf, logoSize, base, theme_.logoColor, logoText);
    }

    // [⚙] Настройки + поле поиска — справа
    float gearX   = winW - padX - btnSize;
    float searchW = 260.0f;
    float searchX = gearX - btnGap - searchW;

    ImVec2 sp0 = {searchX, topY}, sp1 = {searchX + searchW, topY + btnSize};
    dl->AddRectFilled(sp0, sp1, theme_.surface, btnSize * 0.5f);
    dl->AddRect(sp0, sp1, theme_.line, btnSize * 0.5f, 0, 1.0f);
    // Лупа — рисуем вручную (emoji-глиф 🔍 не покрыт шрифтом Inter → tofu-box)
    {
        ImVec2 c = {sp0.x + 20.0f, sp0.y + btnSize * 0.5f};
        float  r = 5.0f;
        dl->AddCircle(c, r, theme_.ink3, 16, 1.6f);
        ImVec2 dir = {0.7071f, 0.7071f};
        dl->AddLine({c.x+dir.x*r, c.y+dir.y*r}, {c.x+dir.x*r*1.9f, c.y+dir.y*r*1.9f}, theme_.ink3, 1.8f);
    }

    // Центрируем ПО ВЫСОТЕ РЕАЛЬНОГО ВИДЖЕТА (текст + FramePadding.y*2), а не
    // только по высоте текста — раньше не учитывали FramePadding, из-за чего
    // поле съезжало вниз внутри пилюли.
    float fieldH = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::SetCursorPos({searchX + 34.0f, topY + (btnSize - fieldH) * 0.5f});
    ImGui::SetNextItemWidth(searchW - 48.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0,0,0,0));
    ImGui::InputTextWithHint("##search", tr.search,
                              searchBuf_, sizeof(searchBuf_));
    ImGui::PopStyleColor(4);

    ImGui::SetCursorPos({gearX, topY});
    if (ImGui::Button("##gear", {btnSize, btnSize})) showSettings_ = !showSettings_;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr.settingsTip);
    // Шестерёнка — рисуем вручную (Segoe UI Symbol ⚙ мёржился не в тот шрифт)
    {
        ImVec2 c = {gearX + btnSize*0.5f, topY + btnSize*0.5f};
        float  r = 9.0f;
        ImU32  col = theme_.ink2;
        for (int i = 0; i < 8; ++i) {
            float a = (float)i / 8.0f * 2.0f * 3.14159265f;
            ImVec2 dir = {std::cos(a), std::sin(a)};
            dl->AddLine({c.x+dir.x*r*0.68f, c.y+dir.y*r*0.68f},
                        {c.x+dir.x*r*1.05f, c.y+dir.y*r*1.05f}, col, 2.4f);
        }
        dl->AddCircle(c, r*0.68f, col, 20, 1.8f);
        dl->AddCircleFilled(c, r*0.28f, col);
    }

    ImGui::PopStyleVar(); // FrameRounding

    // ── Тулбар: "N games in your library" + тонкая линия ──────────────────────
    float toolbarY = topY + btnSize + 24.0f;
    std::string filterLower = searchBuf_;
    for (auto& c : filterLower) c = (char)tolower((unsigned char)c);
    std::vector<size_t> visible;
    for (size_t i = 0; i < romList_.size(); ++i) {
        if (filterLower.empty()) { visible.push_back(i); continue; }
        std::string n = romList_[i].name;
        for (auto& c : n) c = (char)tolower((unsigned char)c);
        if (n.find(filterLower) != std::string::npos) visible.push_back(i);
    }
    {
        ImFont* df = displayFontFor(lang_, fonts_);
        std::string count = std::to_string(visible.size());
        ImFont* uf = fonts_.uiSemiBold ? fonts_.uiSemiBold : ImGui::GetFont();
        ImVec2 p = {padX, toolbarY};
        ImVec2 cw = uf->CalcTextSizeA(22.0f, FLT_MAX, 0.0f, count.c_str());
        dl->AddText(uf, 22.0f, p, theme_.ink, count.c_str());
        std::string suffix = std::string(" ") + tr.gamesInLib;
        dl->AddText(df, 22.0f, {p.x + cw.x + 6.0f, p.y}, theme_.ink2, suffix.c_str());
    }
    float lineY = toolbarY + 32.0f;
    dl->AddLine({padX, lineY}, {winW - padX, lineY}, theme_.line, 1.0f);

    // ── Скроллируемая область сетки (порт main{overflow-y:auto}) ──────────────
    // Шапка/тулбар выше остаются неподвижны — скроллится только сама библиотека.
    // Сетка рисуется прямо в главном окне со своим клипом и прокруткой.
    // Раньше здесь был BeginChild, но в развёрнутом окне его draw list
    // переставал рендериться целиком (ImGui считал окно видимым, вершины
    // набирались, а на экран не попадало ничего — включая тестовый маркер),
    // и библиотека выглядела пустой. Ручной клип надёжнее и проще.
    const float gridTop = lineY + 1.0f;
    ImDrawList* gdl = dl;
    gdl->PushClipRect({0.0f, gridTop}, {(float)winW, (float)winH}, true);
    ImVec2 gridOrigin = {0.0f, gridTop - libScroll_};

    // ── Сетка карточек ──────────────────────────────────────────────────────
    const float gapX    = 24.0f;
    const float gapY    = 30.0f;
    const float minCardW= 230.0f;
    const float padStripH = 60.0f;
    float areaW = winW - padX * 2.0f;
    int   cols  = std::max(1, (int)((areaW + gapX) / (minCardW + gapX)));
    float cardW = (areaW - (cols - 1) * gapX) / (float)cols;
    float coverH = cardW * 4.0f / 3.0f;
    float cardH  = coverH + padStripH;
    const float cornerR = theme_.radius;

    float x = gridOrigin.x + padX, y = gridOrigin.y + 24.0f;
    int col = 0;

    for (size_t vi = 0; vi < visible.size(); ++vi) {
        size_t i = visible[vi];
        auto& entry = romList_[i];
        ImGui::PushID((int)i);

        ImVec2 p0 = {x, y};
        ImVec2 p1 = {p0.x + cardW, p0.y + cardH};
        // Пропускаем ряды вне видимой области: иначе их InvisibleButton
        // остаётся кликабельным под шапкой (клип на draw list кнопки не влияет)
        if (p1.y < gridTop || p0.y > (float)winH) {
            ImGui::PopID();
            x += cardW + gapX;
            if (++col >= cols) { col = 0; x = gridOrigin.x + padX; y += cardH + gapY; }
            continue;
        }
        bool hovered = ImGui::IsMouseHoveringRect(p0, p1);
        // Лёгкий "подъём" при наведении (упрощение CSS translateY(-4px)).
        if (hovered) { p0.y -= 4.0f; p1.y -= 4.0f; }
        ImVec2 coverP0 = p0, coverP1 = {p1.x, p0.y + coverH};
        ImVec2 padP0   = {p0.x, coverP1.y}, padP1 = p1;

        // ── Тень под карточкой (порт --shadow-card; ImGui не умеет box-shadow,
        // поэтому кладём 4 расширяющихся контура с падающей альфой). На ховере
        // тень растёт и окрашивается акцентом — это и есть --accent-glow.
        {
            ImU32 shadowBase = hovered ? theme_.accent : IM_COL32(60, 36, 18, 255);
            int   layers     = hovered ? 7 : 4;
            for (int s = layers; s >= 1; --s) {
                float grow  = (float)s * 1.6f;
                int   alpha = (hovered ? 26 : 15) - s * 2;
                if (alpha <= 0) continue;
                gdl->AddRect({p0.x - grow, p0.y - grow + 2.0f},
                             {p1.x + grow, p1.y + grow + 3.0f},
                             (shadowBase & 0x00FFFFFFu) | ((ImU32)alpha << 24),
                             cornerR + grow, 0, 1.6f);
            }
        }

        gdl->PushClipRect(p0, p1, true);
        // Подложка карточки — чтобы скруглённые углы не показывали фон сетки
        gdl->AddRectFilled(p0, p1, theme_.surface, cornerR);

        // ── Обложка ──
        if (entry.cover) {
            int texW = 1, texH = 1;
            SDL_QueryTexture(entry.cover, nullptr, nullptr, &texW, &texH);
            float scaleX = (coverP1.x-coverP0.x) / (float)texW;
            float scaleY = (coverP1.y-coverP0.y) / (float)texH;
            float scale  = scaleX > scaleY ? scaleX : scaleY;
            float fitW   = texW * scale, fitH = texH * scale;
            float u0 = (fitW - (coverP1.x-coverP0.x)) / (2.0f * fitW);
            float v0 = (fitH - (coverP1.y-coverP0.y)) / (2.0f * fitH);
            // Зажимаем UV в [0, 0.5]: погрешность float даёт значения вроде
            // -1e-8, а SDL_RenderGeometryRaw при UV вне [0,1] отвергает ВЕСЬ
            // батч геометрии кадра — пропадал не только этот кадр обложки,
            // а весь интерфейс целиком ("Values of 'uv' out of bounds").
            u0 = u0 < 0.0f ? 0.0f : (u0 > 0.5f ? 0.5f : u0);
            v0 = v0 < 0.0f ? 0.0f : (v0 > 0.5f ? 0.5f : v0);
            // Скругляем ТОЛЬКО верхние углы — низ упирается в полосу геймпада
            gdl->AddImageRounded((ImTextureID)(intptr_t)entry.cover,
                coverP0, coverP1, {u0, v0}, {1.0f-u0, 1.0f-v0},
                IM_COL32_WHITE, cornerR, ImDrawFlags_RoundCornersTop);
        } else {
            ImU32 c1, c2;
            placeholderGradient(entry.name, c1, c2);
            gdl->PushClipRect(coverP0, coverP1, true);
            gdl->AddRectFilled(coverP0, coverP1, c1, cornerR, ImDrawFlags_RoundCornersTop);
            gdl->AddRectFilledMultiColor({coverP0.x, coverP0.y + cornerR}, coverP1, c1, c1, c2, c2);
            gdl->PopClipRect();
            ImFont* df = fonts_.displayItalic ? fonts_.displayItalic : ImGui::GetFont();
            ImFont* lf = fonts_.label ? fonts_.label : ImGui::GetFont();
            std::string title = entry.name;
            float tsize = 22.0f;
            ImVec2 tsz = df->CalcTextSizeA(tsize, coverP1.x-coverP0.x-40.0f, 0.0f, title.c_str());
            ImVec2 tp = {(coverP0.x+coverP1.x)*0.5f - tsz.x*0.5f, (coverP0.y+coverP1.y)*0.5f - tsz.y};
            gdl->AddText(df, tsize, tp, IM_COL32(255,255,255,240), title.c_str());
            std::string sub = entry.console;
            float ssize = 12.0f;
            ImVec2 ssz = lf->CalcTextSizeA(ssize, FLT_MAX, 0.0f, sub.c_str());
            gdl->AddText(lf, ssize, {(coverP0.x+coverP1.x)*0.5f - ssz.x*0.5f, tp.y+tsz.y+10.0f},
                        IM_COL32(255,255,255,200), sub.c_str());
        }

        // Градиент снизу обложки + название (title-overlay)
        float gradH = 46.0f;
        gdl->AddRectFilledMultiColor({coverP0.x, coverP1.y-gradH}, coverP1,
            IM_COL32(0,0,0,0), IM_COL32(0,0,0,0), IM_COL32(0,0,0,190), IM_COL32(0,0,0,190));
        {
            ImFont* uf = fonts_.uiSemiBold ? fonts_.uiSemiBold : ImGui::GetFont();
            std::string label = entry.name.size() > 26 ? entry.name.substr(0,25)+u8"…" : entry.name;
            gdl->AddText(uf, 16.0f, {coverP0.x+10.0f, coverP1.y-26.0f}, IM_COL32(255,255,255,245), label.c_str());
        }

        // ── Полоса геймпада (скругление только по низу карточки) ──
        const auto& tint = padTintFor(theme_, entry.console);
        gdl->AddRectFilled(padP0, padP1, tint.bg, cornerR, ImDrawFlags_RoundCornersBottom);
        gdl->AddLine(padP0, {padP1.x, padP0.y}, theme_.line, 1.0f);
        DrawControllerPad(gdl, {padP0.x, padP0.y}, {padP1.x-padP0.x, padP1.y-padP0.y},
                           entry.console, tint.fg, fonts_.label);

        // Ховер: тёплое свечение поверх обложки (порт .card::after radial-gradient)
        if (hovered)
            gdl->AddRectFilledMultiColor(coverP0, {coverP1.x, coverP0.y + coverH*0.6f},
                (theme_.accentGlow & 0x00FFFFFFu) | 0x22000000u,
                (theme_.accentGlow & 0x00FFFFFFu) | 0x22000000u,
                theme_.accentGlow & 0x00FFFFFFu,
                theme_.accentGlow & 0x00FFFFFFu);

        // Рамка карточки + внутренний блик по верхней кромке (inset highlight
        // из --shadow-card: он и создаёт ощущение приподнятой поверхности)
        ImU32 borderCol = hovered ? theme_.accentSoft : theme_.line;
        gdl->AddRect(p0, p1, borderCol, cornerR, 0, hovered ? 1.6f : 1.0f);
        gdl->AddLine({p0.x + cornerR, p0.y + 1.0f}, {p1.x - cornerR, p0.y + 1.0f},
                     IM_COL32(255, 255, 255, 60), 1.0f);

        gdl->PopClipRect();

        ImGui::SetCursorScreenPos(p0);
        if (ImGui::InvisibleButton("##card", {p1.x-p0.x, p1.y-p0.y}))
            launchROM(entry.path);

        ImGui::PopID();

        x += cardW + gapX;
        if (++col >= cols) { col = 0; x = gridOrigin.x + padX; y += cardH + gapY; }
    }

    if (romList_.empty()) {
        ImGui::SetCursorScreenPos({gridOrigin.x + padX, y + 40.0f});
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(theme_.ink3));
        ImGui::TextUnformatted("No ROMs yet. Press  +  to add a ROM.");
        ImGui::PopStyleColor();
    }

    gdl->PopClipRect();

    // ── Прокрутка сетки колесом ──────────────────────────────────────────────
    // y после цикла указывает на низ последнего ряда (в координатах с учётом
    // текущего скролла), поэтому полную высоту берём относительно gridOrigin.
    {
        float contentH = (y + cardH) - gridOrigin.y;
        float viewH    = (float)winH - gridTop;
        float maxScroll = contentH > viewH ? (contentH - viewH) : 0.0f;
        ImGuiIO& io = ImGui::GetIO();
        if (!showSettings_ && io.MousePos.y >= gridTop && io.MouseWheel != 0.0f)
            libScroll_ -= io.MouseWheel * 80.0f;
        if (libScroll_ > maxScroll) libScroll_ = maxScroll;
        if (libScroll_ < 0.0f)      libScroll_ = 0.0f;
    }

    ImGui::End();
    ImGui::PopStyleColor(); // WindowBg

    if (showSettings_) renderSettings();
}

void App::renderGame() {}

// ─── Меню паузы ──────────────────────────────────────────────────────────────

void App::renderPauseOverlay() {
    int winW, winH;
    SDL_GetWindowSize(window_, &winW, &winH);
    const I18nStrings& tr = GetI18n(lang_);

    // Тонкий затемнитель
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({(float)winW, (float)winH});
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##ov", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::GetWindowDrawList()->AddRectFilled(
        {0,0}, {(float)winW,(float)winH}, IM_COL32(0,0,0,60));
    ImGui::End();

    const float menuW = 340.0f, menuH = 300.0f;
    ImGui::SetNextWindowPos({(winW-menuW)*0.5f,(winH-menuH)*0.5f});
    ImGui::SetNextWindowSize({menuW, menuH});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(theme_.surface));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme_.radius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20.0f, 22.0f});
    ImGui::Begin("##pause", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // "PAUSED" — Oswald, разрежённые буквы (порт letter-spacing:.35em),
    // акцентный цвет.
    {
        ImFont* lf = fonts_.labelSm ? fonts_.labelSm : ImGui::GetFont();
        if (lf) ImGui::PushFont(lf);
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(theme_.accent));
        ImGui::SetWindowFontScale(lf ? 1.7f : 1.1f);
        std::string txt = spaceOutUtf8(tr.paused);
        float tw = ImGui::CalcTextSize(txt.c_str()).x;
        ImGui::SetCursorPosX((menuW - 40.0f - tw) * 0.5f);
        ImGui::TextUnformatted(txt.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        if (lf) ImGui::PopFont();
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Separator, ToVec4(theme_.line));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing(); ImGui::Spacing();

    float bw = menuW - 40.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 10.0f});
    if (ImGui::Button(tr.continueBtn, {bw, 40})) state_ = AppState::Playing;
    if (ImGui::Button(tr.save,        {bw, 40})) { showSaveModal_ = true; showLoadModal_ = false; }
    if (ImGui::Button(tr.load,        {bw, 40})) { showLoadModal_ = true; showSaveModal_ = false; }
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(theme_.accent));
    if (ImGui::Button(tr.exit, {bw, 40})) {
        if (console_) console_->reset();
        romLoaded_ = false;
        state_     = AppState::MainMenu;
        SDL_SetWindowTitle(window_, "Emudor");
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ─── Save modal ──────────────────────────────────────────────────────────────

void App::renderSaveModal() {
    if (!showSaveModal_) return;
    const I18nStrings& tr = GetI18n(lang_);
    std::string title = std::string(tr.saveGame) + "##modal";
    ImGui::OpenPopup(title.c_str());
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, {0.5f,0.5f});
    ImGui::SetNextWindowSize({340.0f, 0.0f});

    if (ImGui::BeginPopupModal(title.c_str(), &showSaveModal_,
        ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", tr.chooseSlot);
        ImGui::Spacing();
        for (auto& slot : saveSlots_) {
            if (slot.isAuto) continue;  // авто-слот нельзя перезаписать вручную
            std::string lbl = slot.label;
            if (slot.hasData) lbl += "  \xe2\x80\xa2 saved";  // • saved
            lbl += "##sv" + std::to_string(slot.index);

            ImGui::PushStyleColor(ImGuiCol_Text,
                slot.hasData ? ToVec4(theme_.accent)
                             : ImGui::GetStyleColorVec4(ImGuiCol_Text));
            if (ImGui::Button(lbl.c_str(), {310.0f, 36.0f})) {
                saveStateToFile(slot.filename);
                slot.hasData   = true;
                showSaveModal_ = false;
                state_         = AppState::Playing;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
        }
        ImGui::Spacing(); ImGui::Separator();
        std::string cancelId = std::string(tr.cancel) + "##sv";
        if (ImGui::Button(cancelId.c_str(), {310.0f, 34.0f})) {
            showSaveModal_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ─── Load modal ──────────────────────────────────────────────────────────────

void App::renderLoadModal() {
    if (!showLoadModal_) return;
    const I18nStrings& tr = GetI18n(lang_);
    std::string title = std::string(tr.loadGame) + "##modal";
    ImGui::OpenPopup(title.c_str());
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, {0.5f,0.5f});
    ImGui::SetNextWindowSize({340.0f, 0.0f});

    if (ImGui::BeginPopupModal(title.c_str(), &showLoadModal_,
        ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", tr.chooseSlot);
        ImGui::Spacing();
        bool firstAuto = true;
        for (auto& slot : saveSlots_) {
            // Разделитель перед ручными слотами (после авто-слота)
            if (!slot.isAuto && firstAuto && console_ && console_->hasBattery()) {
                ImGui::Separator();
                firstAuto = false;
            }

            std::string idSuffix = "##ld" + std::to_string(slot.index);
            if (!slot.hasData) {
                ImGui::PushStyleColor(ImGuiCol_Text,         ToVec4(theme_.ink3));
                ImGui::PushStyleColor(ImGuiCol_Button,       ToVec4(theme_.surface2));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ToVec4(theme_.surface2));
                std::string empty = slot.label + "  [empty]" + idSuffix;
                ImGui::Button(empty.c_str(), {310.0f, 36.0f});
                ImGui::PopStyleColor(3);
            } else {
                // Авто-слот выделяем зелёным
                if (slot.isAuto) {
                    ImGui::PushStyleColor(ImGuiCol_Button,
                        ImVec4(0.10f, 0.55f, 0.20f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        ImVec4(0.15f, 0.70f, 0.28f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                }
                std::string lbl = slot.label + idSuffix;
                if (ImGui::Button(lbl.c_str(), {310.0f, 36.0f})) {
                    loadStateFromFile(slot.filename);
                    showLoadModal_ = false;
                    state_         = AppState::Playing;
                    ImGui::CloseCurrentPopup();
                }
                if (slot.isAuto) ImGui::PopStyleColor(3);
            }
        }
        ImGui::Spacing(); ImGui::Separator();
        std::string cancelId = std::string(tr.cancel) + "##ld";
        if (ImGui::Button(cancelId.c_str(), {310.0f, 34.0f})) {
            showLoadModal_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ─── Настройки ────────────────────────────────────────────────────────────────

void App::renderSettings() {
    int winW, winH;
    SDL_GetWindowSize(window_, &winW, &winH);

    // Затемняющий/размывающий скрим (упрощённо — без blur, ImGui его не умеет)
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize({(float)winW, (float)winH});
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##settingsOverlay", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoInputs   | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::GetWindowDrawList()->AddRectFilled(
        {0.0f, 0.0f}, {(float)winW, (float)winH}, IM_COL32(0, 0, 0, 80));
    ImGui::End();

    // ── Правая выезжающая панель (.drawer) — 420px, во всю высоту ─────────────
    float drawerW = std::min(420.0f, winW * 0.92f);
    ImGui::SetNextWindowPos({(float)winW - drawerW, 0.0f});
    ImGui::SetNextWindowSize({drawerW, (float)winH});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(theme_.bg2));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {28.0f, 22.0f});
    if (!ImGui::Begin("##settingsDrawer", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        return;
    }
    const I18nStrings& tr = GetI18n(lang_);

    // Заголовок "Settings" (курсив, Instrument Serif — для RU/ZH откат на Inter,
    // см. displayFontFor) + [X] закрыть
    {
        ImFont* df = displayFontFor(lang_, fonts_);
        if (df) ImGui::PushFont(df);
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(theme_.accent));
        ImGui::SetWindowFontScale(df ? 1.0f : 1.4f);
        ImGui::TextUnformatted(tr.settingsTip);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        if (df) ImGui::PopFont();
    }
    ImGui::SameLine(drawerW - 28.0f*2.0f - 30.0f);
    if (ImGui::Button(u8"\xc3\x97##closeSettings", {30.0f, 30.0f})) showSettings_ = false;
    ImGui::Spacing(); ImGui::Spacing();

    ImDrawList* sdl = ImGui::GetWindowDrawList();

    // Section label ALL-CAPS mono + затухающая вправо линия (порт .section-label)
    auto sectionLabel = [&](const char* text) {
        ImGui::Spacing();
        ImFont* mf = fonts_.mono ? fonts_.mono : ImGui::GetFont();
        float   fsz = 13.0f;
        ImVec2  tsz = mf->CalcTextSizeA(fsz, FLT_MAX, 0.0f, text);
        ImVec2  p0  = ImGui::GetCursorScreenPos();
        sdl->AddText(mf, fsz, p0, theme_.ink3, text);
        float lineY  = p0.y + tsz.y * 0.5f;
        float lineX0 = p0.x + tsz.x + 10.0f;
        float lineX1 = p0.x + ImGui::GetContentRegionAvail().x;
        if (lineX1 > lineX0)
            sdl->AddRectFilledMultiColor({lineX0, lineY-0.5f}, {lineX1, lineY+0.5f},
                theme_.line, theme_.line & 0x00FFFFFFu,
                theme_.line & 0x00FFFFFFu, theme_.line);
        ImGui::Dummy({1.0f, tsz.y + 6.0f});
        ImGui::Spacing();
    };

    // Choice-чип с dot-индикатором (точный порт .choice/.dot)
    auto chip = [&](const char* label, bool selected) -> bool {
        ImFont* uf  = fonts_.ui ? fonts_.ui : ImGui::GetFont();
        float   fsz = 16.0f;
        ImVec2  tsz = uf->CalcTextSizeA(fsz, FLT_MAX, 0.0f, label);
        float   dotR = 7.0f, padX = 14.0f, padY = 8.0f, gap = 8.0f;
        float   w = padX*2.0f + dotR*2.0f + gap + tsz.x;
        float   h = tsz.y + padY*2.0f;
        ImVec2  p0 = ImGui::GetCursorScreenPos();
        ImVec2  p1 = {p0.x + w, p0.y + h};
        ImGui::InvisibleButton(label, {w, h});
        bool hovered = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked();
        ImU32 bg     = (selected || hovered) ? theme_.accentSoft : theme_.surface;
        ImU32 border = selected ? theme_.accent : theme_.line;
        ImU32 txtCol = selected ? theme_.ink : theme_.ink2;
        sdl->AddRectFilled(p0, p1, bg, h*0.5f);
        sdl->AddRect(p0, p1, border, h*0.5f, 0, 1.0f);
        ImVec2 dotC = {p0.x + padX + dotR, p0.y + h*0.5f};
        sdl->AddCircle(dotC, dotR, selected ? theme_.accent : theme_.ink3, 16, 1.5f);
        if (selected) sdl->AddCircleFilled(dotC, dotR*0.42f, theme_.accent);
        ImVec2 tp = {dotC.x + dotR + gap, p0.y + (h - tsz.y)*0.5f};
        sdl->AddText(uf, fsz, tp, txtCol, label);
        return clicked;
    };
    // Ряд чипов с явным ручным переносом строки (порт .choices{flex-wrap:wrap}).
    // Не полагается на ImGui SameLine-семантику — она плохо сочеталась с чипами
    // на сырых ImDrawList-примитивах (виджеты "терялись" за краем окна).
    ImFont* chipFont = fonts_.ui ? fonts_.ui : ImGui::GetFont();
    float   rowRight = ImGui::GetWindowPos().x + drawerW - 28.0f;
    auto chipRow = [&](const std::vector<std::pair<const char*,bool>>& items) -> int {
        float startX = ImGui::GetCursorScreenPos().x;
        float x = startX, y = ImGui::GetCursorScreenPos().y;
        float rowH = 0.0f;
        int clickedIdx = -1;
        const float gapBetween = 10.0f;
        for (size_t i = 0; i < items.size(); ++i) {
            const char* label = items[i].first;
            // ВАЖНО: размер шрифта здесь должен совпадать с fsz внутри chip(),
            // иначе расчёт ширины для переноса разойдётся с реальной отрисовкой.
            ImVec2 tsz = chipFont->CalcTextSizeA(16.0f, FLT_MAX, 0.0f, label);
            float w = 14.0f*2.0f + 7.0f*2.0f + 8.0f + tsz.x;
            float h = tsz.y + 8.0f*2.0f;
            if (x > startX && x + w > rowRight) { x = startX; y += h + gapBetween; }
            ImGui::SetCursorScreenPos({x, y});
            if (chip(label, items[i].second)) clickedIdx = (int)i;
            rowH = h;
            x += w + gapBetween;
        }
        ImGui::SetCursorScreenPos({startX, y + rowH + 4.0f});
        return clickedIdx;
    };

    sectionLabel(tr.uiTheme);
    switch (chipRow({{tr.light, themeMode_==ThemeMode::Light}, {tr.dark, themeMode_==ThemeMode::Dark}})) {
        case 0: themeMode_=ThemeMode::Light; applyLightTheme(); saveConfig(); break;
        case 1: themeMode_=ThemeMode::Dark;  applyDarkTheme();  saveConfig(); break;
    }

    sectionLabel(tr.colorMode);
    switch (chipRow({{tr.normal, colorMode_==ColorMode::Normal},
                      {tr.inverted, colorMode_==ColorMode::Inverted},
                      {tr.bw, colorMode_==ColorMode::BlackWhite}})) {
        case 0: colorMode_=ColorMode::Normal;     saveConfig(); break;
        case 1: colorMode_=ColorMode::Inverted;   saveConfig(); break;
        case 2: colorMode_=ColorMode::BlackWhite; saveConfig(); break;
    }

    // ── Язык ──────────────────────────────────────────────────────────────────
    sectionLabel(tr.language);
    // 中文 намеренно не в списке — нет CJK-шрифта, весь текст превращался
    // бы в тофу-квадраты (и сам ярлык языка, и всё остальное после выбора).
    // Lang::ZH остаётся в коде (GetI18n/конфиг) на случай будущей поддержки.
    static const Lang kLangs[4] = { Lang::EN, Lang::RU, Lang::ES, Lang::FR };
    std::vector<std::pair<const char*,bool>> langItems;
    for (Lang lg : kLangs) langItems.push_back({LangNativeName(lg), lang_ == lg});
    int langIdx = chipRow(langItems);
    if (langIdx >= 0) { lang_ = kLangs[langIdx]; saveConfig(); }

    // ── Список биндов (без шапки таблицы — точный порт .binds/.row/kbd) ───────
    // snesContext=true → ребиндим snesKeys_, иначе nesKeys_.
    auto bindsList = [&](bool snesContext, KeyConfig& kc, int count, const char* const* names) {
        SDL_Keycode* keys[12] = {
            &kc.up, &kc.down, &kc.left, &kc.right,
            &kc.a,  &kc.b,   &kc.select, &kc.start,
            &kc.x,  &kc.y,   &kc.l,      &kc.rsh
        };
        const float rowH = 34.0f;
        float  boxW = ImGui::GetContentRegionAvail().x;
        ImVec2 b0   = ImGui::GetCursorScreenPos();
        ImVec2 b1   = {b0.x + boxW, b0.y + rowH * count};
        sdl->AddRectFilled(b0, b1, theme_.surface, theme_.radiusSm);
        sdl->AddRect(b0, b1, theme_.line, theme_.radiusSm, 0, 1.0f);

        ImFont* nameFont = fonts_.uiMedium ? fonts_.uiMedium : ImGui::GetFont();
        ImFont* mf       = fonts_.mono     ? fonts_.mono     : ImGui::GetFont();
        const float rebindW = 62.0f, rebindH = 24.0f;

        for (int i = 0; i < count; ++i) {
            float ry = b0.y + i * rowH;
            if (i > 0) sdl->AddLine({b0.x+1.0f, ry}, {b1.x-1.0f, ry}, theme_.lineSoft, 1.0f);

            sdl->AddText(nameFont, 16.0f, {b0.x + 14.0f, ry + (rowH-19.0f)*0.5f}, theme_.ink, names[i]);

            const char* keyName = SDL_GetKeyName(*keys[i]);
            ImVec2 ksz  = mf->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, keyName);
            float  kbdW = (ksz.x + 16.0f) > 30.0f ? (ksz.x + 16.0f) : 30.0f;
            float  kbdH = 20.0f;
            float  kbdX = b1.x - 14.0f - rebindW - 10.0f - kbdW;
            ImVec2 kp0  = {kbdX, ry + (rowH-kbdH)*0.5f};
            ImVec2 kp1  = {kbdX + kbdW, kp0.y + kbdH};
            sdl->AddRectFilled(kp0, kp1, theme_.surface2, 5.0f);
            sdl->AddRect(kp0, kp1, theme_.line, 5.0f, 0, 1.0f);
            sdl->AddText(mf, 12.0f, {kp0.x + (kbdW-ksz.x)*0.5f, kp0.y + (kbdH-ksz.y)*0.5f}, theme_.ink, keyName);

            ImGui::PushID(i);
            ImGui::SetCursorScreenPos({b1.x - 14.0f - rebindW, ry + (rowH-rebindH)*0.5f});
            bool isMe = (rebindIndex_ == i && rebindSnes_ == snesContext);
            if (isMe) {
                ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(theme_.accent));
                ImGui::TextUnformatted("...");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
                ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(theme_.ink2));
                if (ImGui::Button("Rebind", {rebindW, rebindH})) {
                    rebindIndex_ = i;
                    rebindSnes_  = snesContext;
                }
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar();
            }
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos({b0.x, b1.y + 14.0f});
    };

    // ── NES раскладка ─────────────────────────────────────────────────────────
    ImGui::PushID("nes_keytable");
    sectionLabel("CONTROLS (NES)");
    static const char* kNesNames[8] = {
        "Up", "Down", "Left", "Right", "A", "B", "Select", "Start"
    };
    bindsList(false, nesKeys_, 8, kNesNames);
    ImGui::PopID();

    // ── SNES раскладка ────────────────────────────────────────────────────────
    ImGui::PushID("snes_keytable");
    sectionLabel("CONTROLS (SNES)");
    static const char* kSnesNames[12] = {
        "Up", "Down", "Left", "Right",
        "A", "B", "Select", "Start",
        "X", "Y", "L", "R"
    };
    bindsList(true, snesKeys_, 12, kSnesNames);
    ImGui::PopID();

    // ── Папки с ROM ──────────────────────────────────────────────────────────
    sectionLabel(tr.romFolders);
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(theme_.ink3));
    ImGui::TextWrapped("%s", tr.romHelper);
    ImGui::PopStyleColor();
    ImGui::Spacing();

    for (int fi = 0; fi < (int)romFolders_.size(); ++fi) {
        ImGui::PushID(fi);
        ImGui::Text("%s", romFolders_[fi].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            romFolders_.erase(romFolders_.begin() + fi);
            saveConfig();
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }

    if (ImGui::Button(tr.addFolder)) {
        // Запоминаем cwd до диалога — tinyfd на Windows иногда меняет его
        fs::path savedCwd;
        try { savedCwd = fs::current_path(); } catch (...) {}

        const char* p = tinyfd_selectFolderDialog("Select ROM folder", "");

        // Восстанавливаем cwd чтобы все последующие пути работали
        if (!savedCwd.empty()) {
            try { fs::current_path(savedCwd); } catch (...) {}
        }

        if (p) {
            std::string folder(p);
            bool dup = false;
            for (auto& f : romFolders_) if (f == folder) { dup = true; break; }
            if (!dup) {
                romFolders_.push_back(folder);
                scanRomFolders();   // сразу подхватываем игры из папки
                saveConfig();       // сохраняем папку И найденные игры
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);   // WindowRounding, WindowPadding
    ImGui::PopStyleColor();  // WindowBg
}

// ─── ROM управление ──────────────────────────────────────────────────────────

// Ищет обложку: сначала рядом с ROM, потом в covers/ кэше
static SDL_Texture* findLocalCover(SDL_Renderer* r, const std::string& romPath, const std::string& name) {
    // 1) Рядом с ROM файлом (то же имя)
    fs::path base(romPath);
    for (const char* ext : {".png",".jpg",".jpeg"}) {
        fs::path cp = base; cp.replace_extension(ext);
        if (fs::exists(cp))
            return IMG_LoadTexture(r, cp.string().c_str());
    }
    // 2) В кэше covers/
    fs::path cached = fs::path("covers") / (safeFilename(name) + ".png");
    if (fs::exists(cached))
        return IMG_LoadTexture(r, cached.string().c_str());
    // 3) Вариант без региона
    std::string stripped = stripRegion(name);
    if (stripped != name) {
        fs::path c2 = fs::path("covers") / (safeFilename(stripped) + ".png");
        if (fs::exists(c2))
            return IMG_LoadTexture(r, c2.string().c_str());
    }
    return nullptr;
}

void App::addRomEntry(const std::string& path) {
    for (const auto& e : romList_)
        if (e.path == path) return;   // дубль

    RomEntry rom;
    rom.path    = path;
    rom.name    = fs::path(path).stem().string();
    rom.custom  = true;
    // Определяем тип консоли
    switch (detectConsole(path)) {
        case ConsoleType::NES:  rom.console = "NES";  break;
        case ConsoleType::SNES: rom.console = "SNES"; break;
        default: {
            auto ext = fs::path(path).extension().string();
            rom.console = ext.size() > 1 ? ext.substr(1) : "";
            break;
        }
    }
    rom.cover  = findLocalCover(renderer_, path, rom.name);

    int idx = (int)romList_.size();
    romList_.push_back(std::move(rom));

    if (!romList_[idx].cover)
        enqueueCoverFetch(idx);

    saveConfig();
}

void App::launchROM(const std::string& path) {
    // Определяем тип консоли по расширению
    ConsoleType ct = detectConsole(path);

    std::unique_ptr<IConsole> newConsole;
    switch (ct) {
        case ConsoleType::NES:
            newConsole = std::make_unique<NesConsole>();
            break;
        case ConsoleType::SNES:
            newConsole = std::make_unique<SnesConsole>();
            break;
        default:
            return;  // Неподдерживаемый формат
    }

    if (!newConsole->loadROM(path)) return;

    console_ = std::move(newConsole);

    // Пересоздаём текстуру под разрешение этой консоли
    if (gameTexture_) SDL_DestroyTexture(gameTexture_);
    gameTexture_ = SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        console_->getFrameWidth(), console_->getFrameHeight());

    // Заголовок окна
    SDL_SetWindowTitle(window_,
        (std::string("Emudor — ") + console_->getConsoleName() +
         ": " + fs::path(path).stem().string()).c_str());

    // Очищаем SDL-очередь, чтобы не было хвоста от предыдущей игры
    if (audioDevice_ > 0) SDL_ClearQueuedAudio(audioDevice_);

    romLoaded_           = true;
    sramDirtyCountdown_  = 0;
    currentRomName_      = safeFilename(fs::path(path).stem().string());

    // Загрузить battery-backed SRAM если ранее сохранялся
    if (console_->hasBattery()) {
        std::string sramPath = "saves/" + currentRomName_ + "/sram.bin";
        if (fs::exists(sramPath)) console_->loadSram(sramPath);
    }

    refreshSaveSlots();
    state_ = AppState::Playing;
}

// Находит папку roms/: сначала относительно cwd, затем относительно exe
// (на случай запуска release двойным кликом — cwd там build/release).
static std::string findRomsDir() {
    namespace fs = std::filesystem;
    try { if (fs::exists("roms") && fs::is_directory("roms")) return "roms"; } catch (...) {}
    char* base = SDL_GetBasePath();
    if (base && *base) {
        fs::path bp = base; SDL_free(base);
        fs::path cands[] = { bp/"roms", bp.parent_path()/"roms",
                             bp.parent_path().parent_path()/"roms" };
        for (const auto& c : cands) {
            try { if (fs::exists(c) && fs::is_directory(c)) return c.string(); } catch (...) {}
        }
    }
    return std::string();
}

void App::scanRomsFolder() {
    std::string romsDir = findRomsDir();
    if (romsDir.empty()) return;
    // РЕКУРСИВНО: игры лежат в подпапках roms/snes, roms/nes и т.д.
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(romsDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        if (!entry.is_regular_file(ec)) continue;
        auto ct = detectConsole(entry.path().string());
        if (ct == ConsoleType::Unknown) continue;
        std::string p = entry.path().string();
        bool dup = false;
        for (const auto& e : romList_) if (e.path == p) { dup=true; break; }
        if (dup) continue;

        RomEntry rom;
        rom.path    = p;
        rom.name    = entry.path().stem().string();
        rom.custom  = false;
        switch (ct) {
            case ConsoleType::NES:  rom.console = "NES";  break;
            case ConsoleType::SNES: rom.console = "SNES"; break;
            default:                rom.console = "";      break;
        }
        rom.cover  = findLocalCover(renderer_, p, rom.name);

        int idx = (int)romList_.size();
        romList_.push_back(std::move(rom));

        if (!romList_[idx].cover)
            enqueueCoverFetch(idx);
    }
}

// Удаляем из списка ROM, файлы которых больше не существуют
void App::pruneDeletedRoms()
{
    for (auto& e : romList_) {
        if (!fs::exists(e.path) && e.cover) {
            SDL_DestroyTexture(e.cover);
            e.cover = nullptr;
        }
    }
    romList_.erase(
        std::remove_if(romList_.begin(), romList_.end(),
            [](const RomEntry& e){ return !fs::exists(e.path); }),
        romList_.end());
}

// Сканируем все папки из romFolders_ и добавляем новые ROM
void App::scanRomFolders()
{
    for (const auto& folder : romFolders_) {
        if (!fs::exists(folder)) continue;
        // РЕКУРСИВНО: пользователь может указать папку-контейнер (например roms/),
        // где игры лежат в подпапках по консолям (snes/, nes/ …).
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(folder, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const auto& entry = *it;
            if (!entry.is_regular_file(ec)) continue;
            auto ct = detectConsole(entry.path().string());
            if (ct == ConsoleType::Unknown) continue;
            std::string p = entry.path().string();
            bool dup = false;
            for (const auto& e : romList_) if (e.path == p) { dup = true; break; }
            if (dup) continue;

            RomEntry rom;
            rom.path    = p;
            rom.name    = entry.path().stem().string();
            rom.custom  = false;
            switch (ct) {
                case ConsoleType::NES:  rom.console = "NES";  break;
                case ConsoleType::SNES: rom.console = "SNES"; break;
                default:                rom.console = "";      break;
            }
            rom.cover = findLocalCover(renderer_, p, rom.name);

            int idx = (int)romList_.size();
            romList_.push_back(std::move(rom));
            if (!romList_[idx].cover) enqueueCoverFetch(idx);
        }
    }
}

// ─── Загрузка обложек ─────────────────────────────────────────────────────────

void App::enqueueCoverFetch(int romIndex) {
    if (romIndex < 0 || romIndex >= (int)romList_.size()) return;
    coverQueue_.push_back({romIndex, romList_[romIndex].name, romList_[romIndex].console});
}

void App::processCoverQueue() {
    // Проверяем завершение текущей загрузки
    if (coverDlActive_ && coverFuture_.valid() &&
        coverFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto [idx, localPath] = coverFuture_.get();
        if (!localPath.empty() && idx < (int)romList_.size() && !romList_[idx].cover)
            romList_[idx].cover = IMG_LoadTexture(renderer_, localPath.c_str());
        coverDlActive_ = false;
    }

    // Запускаем следующую загрузку
    if (!coverDlActive_ && !coverQueue_.empty()) {
        auto job      = coverQueue_.front();
        coverQueue_.pop_front();
        coverDlActive_ = true;
        coverFuture_   = std::async(std::launch::async,
                                    downloadCoverTask, job.romIndex, job.gameName, job.console);
    }
}

// ─── Save / Load State ────────────────────────────────────────────────────────

void App::refreshSaveSlots() {
    saveSlots_.clear();
    std::string dir = currentRomName_.empty()
        ? "saves"
        : "saves/" + currentRomName_;

    // Авто-слот (только для игр с battery SRAM)
    if (console_ && console_->hasBattery()) {
        SaveSlot a;
        a.index    = -1;
        a.label    = "Auto Save  \xf0\x9f\x8e\xae";  // 🎮 (UTF-8)
        a.filename = dir + "/autosave.sav";
        a.hasData  = fs::exists(a.filename);
        a.isAuto   = true;
        saveSlots_.push_back(a);
    }

    // Ручные слоты 1–5
    for (int i = 1; i <= 5; i++) {
        SaveSlot s;
        s.index    = i;
        s.label    = "Save file " + std::to_string(i);
        s.filename = dir + "/save_" + std::to_string(i) + ".sav";
        s.hasData  = fs::exists(s.filename);
        saveSlots_.push_back(s);
    }
}

void App::saveStateToFile(const std::string& path) {
    if (!romLoaded_ || !console_) return;
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    console_->saveState(f);
}

void App::loadStateFromFile(const std::string& path) {
    if (!console_) return;
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    console_->loadState(f);
}

void App::saveState(int slot) {
    if (!romLoaded_) return;
    std::string dir = currentRomName_.empty() ? "saves" : "saves/" + currentRomName_;
    fs::create_directories(dir);
    saveStateToFile(dir + "/save_" + std::to_string(slot) + ".sav");
}

void App::loadState(int slot) {
    std::string dir = currentRomName_.empty() ? "saves" : "saves/" + currentRomName_;
    loadStateFromFile(dir + "/save_" + std::to_string(slot) + ".sav");
}

void App::triggerAutoSave() {
    if (!romLoaded_ || !console_) return;
    std::string dir = currentRomName_.empty() ? "saves" : "saves/" + currentRomName_;
    fs::create_directories(dir);
    // Сохранить raw SRAM (для совместимости с другими эмуляторами / следующего запуска)
    console_->saveSram(dir + "/sram.bin");
    // Сохранить полный снапшот в авто-слот
    saveStateToFile(dir + "/autosave.sav");
    refreshSaveSlots();
}

// ─── Темы ─────────────────────────────────────────────────────────────────────

// Применяет UiTheme (design_handoff_emudor_ui) к ImGuiStyle — используется
// стандартными виджетами ImGui (Settings-таблицы, Save/Load модалки).
static void applyThemeToImGuiStyle(const UiTheme& t) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = s.ChildRounding = s.PopupRounding = t.radius;
    s.FrameRounding  = s.GrabRounding  = s.TabRounding    = t.radiusSm;
    s.WindowBorderSize = s.FrameBorderSize = 1.0f;
    s.ItemSpacing  = {8.0f, 8.0f};
    s.FramePadding = {10.0f, 6.0f};

    ImVec4 accent  = ToVec4(t.accent);
    ImVec4 accentD = ToVec4(t.accentDeep);
    s.Colors[ImGuiCol_WindowBg]           = ToVec4(t.bg2);
    s.Colors[ImGuiCol_ChildBg]            = ToVec4(t.surface);
    s.Colors[ImGuiCol_PopupBg]            = ToVec4(t.surface);
    s.Colors[ImGuiCol_Text]               = ToVec4(t.ink);
    s.Colors[ImGuiCol_TextDisabled]       = ToVec4(t.ink3);
    s.Colors[ImGuiCol_Border]             = ToVec4(t.line);
    s.Colors[ImGuiCol_FrameBg]            = ToVec4(t.surface2);
    s.Colors[ImGuiCol_FrameBgHovered]     = ToVec4(t.accentSoft);
    s.Colors[ImGuiCol_TitleBg]            = ToVec4(t.bg2);
    s.Colors[ImGuiCol_TitleBgActive]      = ToVec4(t.bg2);
    s.Colors[ImGuiCol_Button]             = ToVec4(t.surface);
    s.Colors[ImGuiCol_ButtonHovered]      = accent;
    s.Colors[ImGuiCol_ButtonActive]       = accentD;
    s.Colors[ImGuiCol_Header]             = ToVec4(t.accentSoft);
    s.Colors[ImGuiCol_HeaderHovered]      = accent;
    s.Colors[ImGuiCol_Separator]          = ToVec4(t.line);
    s.Colors[ImGuiCol_CheckMark]          = accent;
    s.Colors[ImGuiCol_SliderGrab]         = accent;
    s.Colors[ImGuiCol_ScrollbarGrab]      = ToVec4(t.line);
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = accent;
    s.Colors[ImGuiCol_TableBorderLight]   = ToVec4(t.line);
    s.Colors[ImGuiCol_TableBorderStrong]  = ToVec4(t.ink3);
    s.Colors[ImGuiCol_TableRowBgAlt]      = ToVec4(t.surface2);
}

void App::applyLightTheme() {
    theme_ = MakeLightTheme();
    applyThemeToImGuiStyle(theme_);
}

void App::applyDarkTheme() {
    theme_ = MakeDarkTheme();
    applyThemeToImGuiStyle(theme_);
}

// ─── Конфиг ──────────────────────────────────────────────────────────────────

// Путь к config.json в ОБЩЕМ для всех сборок месте (%APPDATA%/Emudor на Windows,
// ~/.local/share на Linux) через SDL_GetPrefPath. Раньше конфиг лежал рядом с exe
// (SDL_GetBasePath) — из-за этого debug и release имели РАЗНЫЕ библиотеки/папки,
// и добавленные в одной сборке игры не появлялись в другой. Теперь — единый конфиг.
// При первом запуске мигрируем старый конфиг рядом с exe, если он есть.
static std::string configPath() {
    static std::string cached;
    if (!cached.empty()) return cached;

    char* pref = SDL_GetPrefPath("Emudor", "Emudor");
    if (pref && *pref) {
        cached = std::string(pref) + "config.json";
        SDL_free(pref);
        // Одноразовая миграция старого конфига (рядом с exe или в соседней
        // build-папке debug↔release), чтобы ранее добавленная библиотека не пропала.
        try {
            if (!fs::exists(cached)) {
                char* base = SDL_GetBasePath();
                if (base && *base) {
                    fs::path bp = base;
                    fs::path candidates[] = {
                        bp / "config.json",
                        bp.parent_path() / "debug"   / "config.json",
                        bp.parent_path() / "release" / "config.json",
                    };
                    for (const auto& c : candidates) {
                        if (fs::exists(c)) { fs::copy_file(c, cached); break; }
                    }
                    SDL_free(base);
                }
            }
        } catch (...) {}
        return cached;
    }

    // Fallback: рядом с exe, иначе cwd
    char* base = SDL_GetBasePath();
    if (base && *base) {
        cached = std::string(base) + "config.json";
        SDL_free(base);
    } else {
        try { cached = (fs::current_path() / "config.json").string(); }
        catch (...) { cached = "config.json"; }
    }
    return cached;
}

void App::loadConfig() {
    std::ifstream f(configPath());
    if (!f) return;
    try {
        json j; f >> j;

        if (j.contains("theme"))
            themeMode_ = (j["theme"].get<std::string>()=="dark")
                         ? ThemeMode::Dark : ThemeMode::Light;

        if (j.contains("color_mode")) {
            auto cm = j["color_mode"].get<std::string>();
            if      (cm=="inverted") colorMode_ = ColorMode::Inverted;
            else if (cm=="bw")       colorMode_ = ColorMode::BlackWhite;
            else                     colorMode_ = ColorMode::Normal;
        }

        if (j.contains("lang")) {
            auto lg = j["lang"].get<std::string>();
            if      (lg=="ru") lang_ = Lang::RU;
            else if (lg=="es") lang_ = Lang::ES;
            // "zh" намеренно не восстанавливаем — 中文 убран из выбора
            // (нет CJK-шрифта), а если оставить его тут, старый конфиг с
            // ранее сохранённым zh навсегда запер бы юзера в тофу-тексте
            // без возможности переключиться обратно через UI.
            else if (lg=="fr") lang_ = Lang::FR;
            else                lang_ = Lang::EN;
        }

        // Загрузка раскладки из секции (поддерживает старый формат "keys" как NES)
        auto loadKc = [&](const json& k, KeyConfig& kc) {
            auto getKey = [&](const char* n, SDL_Keycode d) -> SDL_Keycode {
                if (!k.contains(n)) return d;
                SDL_Keycode v = SDL_GetKeyFromName(k[n].get<std::string>().c_str());
                return (v==SDLK_UNKNOWN) ? d : v;
            };
            kc.up     = getKey("up",     kc.up);
            kc.down   = getKey("down",   kc.down);
            kc.left   = getKey("left",   kc.left);
            kc.right  = getKey("right",  kc.right);
            kc.a      = getKey("a",      kc.a);
            kc.b      = getKey("b",      kc.b);
            kc.select = getKey("select", kc.select);
            kc.start  = getKey("start",  kc.start);
            kc.x      = getKey("x",      kc.x);
            kc.y      = getKey("y",      kc.y);
            kc.l      = getKey("l",      kc.l);
            kc.rsh    = getKey("r",      kc.rsh);
        };
        // Старый формат "keys" — это NES-раскладка (обратная совместимость)
        if (j.contains("keys")) loadKc(j["keys"], nesKeys_);
        if (j.contains("nes_keys"))  loadKc(j["nes_keys"],  nesKeys_);
        if (j.contains("snes_keys")) loadKc(j["snes_keys"], snesKeys_);

        // Custom ROM пути
        if (j.contains("custom_roms") && j["custom_roms"].is_array()) {
            for (auto& item : j["custom_roms"]) {
                std::string p = item.get<std::string>();
                if (fs::exists(p)) addRomEntry(p);
            }
        }

        // Папки для сканирования
        if (j.contains("rom_folders") && j["rom_folders"].is_array()) {
            romFolders_.clear();
            for (auto& item : j["rom_folders"]) {
                // Сохраняем папку даже если она временно недоступна
                // (диск отключён, путь с акцентом и т.д.)
                romFolders_.push_back(item.get<std::string>());
            }
        }

        // Имена слотов сохранений
        if (j.contains("save_slots") && j["save_slots"].is_object()) {
            for (auto& slot : saveSlots_) {
                std::string key = std::to_string(slot.index);
                if (j["save_slots"].contains(key))
                    slot.label = j["save_slots"][key].get<std::string>();
            }
        }
    } catch (...) {}
}

void App::saveConfig() {
    json j;
    j["theme"] = (themeMode_==ThemeMode::Dark) ? "dark" : "light";
    j["color_mode"] = (colorMode_==ColorMode::Inverted)  ? "inverted"
                    : (colorMode_==ColorMode::BlackWhite) ? "bw" : "normal";
    j["lang"] = lang_==Lang::RU ? "ru" : lang_==Lang::ES ? "es"
              : lang_==Lang::ZH ? "zh" : lang_==Lang::FR ? "fr" : "en";
    auto dumpKc = [](json& out, const KeyConfig& kc) {
        out["up"]     = SDL_GetKeyName(kc.up);
        out["down"]   = SDL_GetKeyName(kc.down);
        out["left"]   = SDL_GetKeyName(kc.left);
        out["right"]  = SDL_GetKeyName(kc.right);
        out["a"]      = SDL_GetKeyName(kc.a);
        out["b"]      = SDL_GetKeyName(kc.b);
        out["select"] = SDL_GetKeyName(kc.select);
        out["start"]  = SDL_GetKeyName(kc.start);
        out["x"]      = SDL_GetKeyName(kc.x);
        out["y"]      = SDL_GetKeyName(kc.y);
        out["l"]      = SDL_GetKeyName(kc.l);
        out["r"]      = SDL_GetKeyName(kc.rsh);
    };
    dumpKc(j["nes_keys"],  nesKeys_);
    dumpKc(j["snes_keys"], snesKeys_);

    json customRoms = json::array();
    for (const auto& e : romList_)
        if (e.custom) customRoms.push_back(e.path);
    j["custom_roms"] = customRoms;

    json romFoldersJ = json::array();
    for (const auto& f : romFolders_) romFoldersJ.push_back(f);
    j["rom_folders"] = romFoldersJ;

    for (const auto& slot : saveSlots_)
        j["save_slots"][std::to_string(slot.index)] = slot.label;

    std::ofstream out(configPath());
    out << j.dump(4);
}
