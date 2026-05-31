#include "app.h"
#include "console/nes_console.h"
#include "console/snes_console.h"
#include "console/console_detect.h"
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <SDL2/SDL_image.h>
#include <nlohmann/json.hpp>
#include <tinyfiledialogs.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <cstring>
#include <cstdio>

#ifdef NES_HAVE_CURL
#  include <curl/curl.h>
#endif

namespace fs = std::filesystem;
using json   = nlohmann::json;

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
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
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

    // Основной шрифт: 18px, сглаживание через oversampling
    // Пробуем несколько путей; если ни один не подошёл — AddFontDefault()
    ImFontConfig fc;
    fc.OversampleH = 3;
    fc.OversampleV = 2;
    fc.PixelSnapH  = false;
    const char* fontCandidates[] = {
        "assets/fonts/Cousine-Regular.ttf",  // вендорный
        "C:/Windows/Fonts/segoeui.ttf",      // Windows Segoe UI
        "C:/Windows/Fonts/tahoma.ttf",       // Windows Tahoma (запасной)
    };
    bool mainFontLoaded = false;
    for (const char* fp : fontCandidates) {
        if (fs::exists(fp)) {
            io.Fonts->AddFontFromFileTTF(fp, 18.0f, &fc);
            mainFontLoaded = true;
            break;
        }
    }
    if (!mainFontLoaded)
        io.Fonts->AddFontDefault();

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
    if (inGame)
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    else if (themeMode_ == ThemeMode::Light)
        SDL_SetRenderDrawColor(renderer_, 245, 245, 248, 255);
    else
        SDL_SetRenderDrawColor(renderer_, 18, 18, 24, 255);
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

void App::renderMainMenu() {
    int winW, winH;
    SDL_GetWindowSize(window_, &winW, &winH);

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({(float)winW, (float)winH});
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ── Шапка ──
    const float btnW     = 42.0f;
    const float gap      = 6.0f;
    const float rightPad = 14.0f;
    // 3 кнопки: +, ↻, ⚙
    float       btnX     = (float)winW - rightPad - btnW * 3.0f - gap * 2.0f;
    float       topY     = ImGui::GetCursorPosY();   // Y верхней строки

    // Заголовок слева
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.098f, 0.463f, 0.824f, 1.0f));
    ImGui::SetWindowFontScale(1.3f);
    ImGui::Text("Emudor");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    // Кнопки — явно привязаны к topY, независимо от высоты заголовка
    const char* gearLabel = hasGearGlyph_ ? u8"⚙" : "S";


    // [+] Добавить ROM вручную
    ImGui::SetCursorPos({btnX, topY + 1.0f});
    if (ImGui::Button("+", {btnW, 0})) {
        const char* filters[] = {"*.nes", "*.sfc", "*.smc", "*.fig", "*.swc"};
        const char* p = tinyfd_openFileDialog(
            "Add ROM to Library", "", 5, filters, "ROM files (NES/SNES)", 0);
        if (p) addRomEntry(p);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add ROM manually");

    // [↻] Пересканировать папки
    ImGui::SetCursorPos({btnX + btnW + gap, topY + 1.0f});
    if (ImGui::Button(u8"↻", {btnW, 0})) {
        pruneDeletedRoms();
        scanRomFolders();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rescan ROM folders");

    // [⚙] Настройки
    ImGui::SetCursorPos({btnX + (btnW + gap) * 2.0f, topY + 1.0f});
    if (ImGui::Button(gearLabel, {btnW, 0})) showSettings_ = !showSettings_;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Settings");
    if (showSettings_) renderSettings();

    // Перемещаем курсор ниже заголовка (1.3× высота строки) и ставим разделитель
    ImGui::SetCursorPosY(topY + ImGui::GetTextLineHeightWithSpacing() * 1.4f);
    ImGui::Separator();

    // ── Сетка карточек ──
    const float cardW   = 170.0f;
    const float cardH   = 220.0f;
    const float pad     = 14.0f;
    const float cornerR = 8.0f;

    float x      = pad;
    float startY = ImGui::GetCursorPosY() + 4.0f;
    float areaW  = (float)winW;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (size_t i = 0; i < romList_.size(); i++) {
        auto& entry = romList_[i];
        ImGui::SetCursorPos({x, startY});
        ImGui::PushID((int)i);

        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = {p0.x + cardW, p0.y + cardH};
        bool hovered = ImGui::IsMouseHoveringRect(p0, p1);

        // Клипинг к границам карточки (скрывает выходящие за углы части)
        dl->PushClipRect(p0, p1, true);

        if (entry.cover) {
            // Обложка: crop-to-fill на всю карточку без полей
            int texW = 1, texH = 1;
            SDL_QueryTexture(entry.cover, nullptr, nullptr, &texW, &texH);
            float scaleX = cardW / (float)texW;
            float scaleY = cardH / (float)texH;
            float scale  = scaleX > scaleY ? scaleX : scaleY;  // crop
            float fitW   = texW * scale;
            float fitH   = texH * scale;
            float u0 = (fitW - cardW) / (2.0f * fitW);
            float v0 = (fitH - cardH) / (2.0f * fitH);
            float u1 = 1.0f - u0;
            float v1 = 1.0f - v0;
            dl->AddImageRounded((ImTextureID)(intptr_t)entry.cover,
                p0, p1, {u0, v0}, {u1, v1}, IM_COL32_WHITE, cornerR);
        } else {
            // Заглушка: тёмный фон без иконок
            dl->AddRectFilled(p0, p1, IM_COL32(34, 38, 52, 255), cornerR);
        }

        // Градиент снизу (прозрачный → тёмный) для читаемости текста
        float gradH = 72.0f;
        ImVec2 g0 = {p0.x, p1.y - gradH};
        dl->AddRectFilledMultiColor(g0, p1,
            IM_COL32(0,0,0,  0), IM_COL32(0,0,0,  0),
            IM_COL32(0,0,0,210), IM_COL32(0,0,0,210));

        // Имя игры
        std::string label = entry.name.size() > 20
            ? entry.name.substr(0, 19) + u8"…"
            : entry.name;
        float lineH  = ImGui::GetTextLineHeight();
        float nameY  = p1.y - lineH - 9.0f;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
            {p0.x + 8.0f, nameY},
            entry.cover ? IM_COL32(255,255,255,240) : IM_COL32(195,210,235,230),
            label.c_str());

        // Бейдж консоли (NES / SNES / ...) — над именем
        if (!entry.console.empty()) {
            const char*  badge  = entry.console.c_str();
            const float  bfont  = 11.0f;
            const float  bpadX  = 5.0f;
            const float  bpadY  = 2.0f;
            const float  bh     = bfont + bpadY * 2.0f;
            float btw = ImGui::GetFont()->CalcTextSizeA(bfont, FLT_MAX, 0.0f, badge).x;
            float bw  = btw + bpadX * 2.0f;
            ImVec2 bs = {p0.x + 8.0f, nameY - bh - 4.0f};
            // Цвет бейджа: NES — красный, SNES — фиолетовый, остальные — тёмно-серый
            ImU32 badgeCol;
            if      (entry.console == "NES")  badgeCol = IM_COL32(185,  30,  30, 225);
            else if (entry.console == "SNES") badgeCol = IM_COL32( 85,  30, 175, 225);
            else                              badgeCol = IM_COL32( 60,  60,  60, 225);
            dl->AddRectFilled(bs, {bs.x + bw, bs.y + bh}, badgeCol, 4.0f);
            dl->AddText(ImGui::GetFont(), bfont,
                        {bs.x + bpadX, bs.y + bpadY},
                        IM_COL32(255, 255, 255, 255), badge);
        }

        // Ховер: лёгкий белый оверлей + синяя рамка
        if (hovered) {
            dl->AddRectFilled(p0, p1, IM_COL32(255,255,255,22), cornerR);
            dl->AddRect(p0, p1, IM_COL32(25,118,210,200), cornerR, 0, 2.0f);
        }

        dl->PopClipRect();

        // Кнопка-призрак на всю карточку
        ImGui::SetCursorScreenPos(p0);
        if (ImGui::InvisibleButton("##card", {cardW, cardH}))
            launchROM(entry.path);

        ImGui::PopID();

        x += cardW + pad;
        if (x + cardW + pad > areaW) {
            x = pad;
            startY += cardH + pad;
        }
    }

    if (romList_.empty()) {
        ImGui::SetCursorPosY(startY + 60.0f);
        float tw2 = ImGui::CalcTextSize("No ROMs yet. Press  +  to add a ROM.").x;
        ImGui::SetCursorPosX((winW - tw2) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f,0.5f,0.6f,1.0f));
        ImGui::TextUnformatted("No ROMs yet. Press  +  to add a ROM.");
        ImGui::PopStyleColor();
    }

    ImGui::End();
}

void App::renderGame() {}

// ─── Меню паузы ──────────────────────────────────────────────────────────────

void App::renderPauseOverlay() {
    int winW, winH;
    SDL_GetWindowSize(window_, &winW, &winH);

    // Тонкий затемнитель
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({(float)winW, (float)winH});
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##ov", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::GetWindowDrawList()->AddRectFilled(
        {0,0}, {(float)winW,(float)winH}, IM_COL32(0,0,0,30));
    ImGui::End();

    const float menuW = 300.0f, menuH = 240.0f;
    ImGui::SetNextWindowPos({(winW-menuW)*0.5f,(winH-menuH)*0.5f});
    ImGui::SetNextWindowSize({menuW, menuH});
    ImGui::Begin("##pause", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.098f,0.463f,0.824f,1.0f));
    ImGui::SetWindowFontScale(1.1f);
    float tw = ImGui::CalcTextSize("PAUSED").x;
    ImGui::SetCursorPosX((menuW - tw) * 0.5f);
    ImGui::TextUnformatted("PAUSED");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::Separator(); ImGui::Spacing();

    float bw = menuW - 24.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 8.0f});
    if (ImGui::Button("Continue",     {bw, 38})) state_ = AppState::Playing;
    if (ImGui::Button("Save",         {bw, 38})) { showSaveModal_ = true; showLoadModal_ = false; }
    if (ImGui::Button("Load",         {bw, 38})) { showLoadModal_ = true; showSaveModal_ = false; }
    if (ImGui::Button("Exit to Menu", {bw, 38})) {
        if (console_) console_->reset();
        romLoaded_ = false;
        state_     = AppState::MainMenu;
        SDL_SetWindowTitle(window_, "Emudor");
    }
    ImGui::PopStyleVar();
    ImGui::End();
}

// ─── Save modal ──────────────────────────────────────────────────────────────

void App::renderSaveModal() {
    if (!showSaveModal_) return;
    ImGui::OpenPopup("Save Game##modal");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, {0.5f,0.5f});
    ImGui::SetNextWindowSize({340.0f, 0.0f});

    if (ImGui::BeginPopupModal("Save Game##modal", &showSaveModal_,
        ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Choose a slot:");
        ImGui::Spacing();
        for (auto& slot : saveSlots_) {
            if (slot.isAuto) continue;  // авто-слот нельзя перезаписать вручную
            std::string lbl = slot.label;
            if (slot.hasData) lbl += "  \xe2\x80\xa2 saved";  // • saved
            lbl += "##sv" + std::to_string(slot.index);

            ImGui::PushStyleColor(ImGuiCol_Text,
                slot.hasData ? ImVec4(0.7f,0.4f,0.1f,1.0f)
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
        if (ImGui::Button("Cancel##sv", {310.0f, 34.0f})) {
            showSaveModal_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ─── Load modal ──────────────────────────────────────────────────────────────

void App::renderLoadModal() {
    if (!showLoadModal_) return;
    ImGui::OpenPopup("Load Game##modal");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, {0.5f,0.5f});
    ImGui::SetNextWindowSize({340.0f, 0.0f});

    if (ImGui::BeginPopupModal("Load Game##modal", &showLoadModal_,
        ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Choose a slot:");
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
                ImGui::PushStyleColor(ImGuiCol_Text,         ImVec4(0.55f,0.55f,0.58f,1.0f));
                ImGui::PushStyleColor(ImGuiCol_Button,       ImVec4(0.90f,0.90f,0.93f,1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.90f,0.90f,0.93f,1.0f));
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
        if (ImGui::Button("Cancel##ld", {310.0f, 34.0f})) {
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

    // Полупрозрачный затемняющий оверлей
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

    // Центрированное неподвижное окно настроек
    ImGui::SetNextWindowSizeConstraints({440.0f, 10.0f}, {440.0f, (float)winH * 0.9f});
    ImGui::SetNextWindowPos(
        ImVec2((float)winW * 0.5f, (float)winH * 0.5f),
        ImGuiCond_Always, {0.5f, 0.5f});
    if (!ImGui::Begin("Settings", &showSettings_,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    // ── Универсальная таблица биндов ──────────────────────────────────────────
    // Рисует одну раскладку: имена кнопок + клавиша + Rebind.
    // snesContext=true → ребиндим snesKeys_, иначе nesKeys_.
    auto drawBindTable = [&](const char* tableId, bool snesContext,
                             KeyConfig& kc, int count,
                             const char* const* names) {
        SDL_Keycode* keys[12] = {
            &kc.up, &kc.down, &kc.left, &kc.right,
            &kc.a,  &kc.b,   &kc.select, &kc.start,
            &kc.x,  &kc.y,   &kc.l,      &kc.rsh
        };
        if (ImGui::BeginTable(tableId, 3,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("Button");
            ImGui::TableSetupColumn("Key");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();
            for (int i = 0; i < count; ++i) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%s", names[i]);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%s", SDL_GetKeyName(*keys[i]));
                ImGui::TableSetColumnIndex(2);
                ImGui::PushID(i);
                bool isMe = (rebindIndex_ == i && rebindSnes_ == snesContext);
                if (isMe) ImGui::TextDisabled("[press key...]");
                else if (ImGui::SmallButton("Rebind")) {
                    rebindIndex_ = i;
                    rebindSnes_  = snesContext;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    };

    // ── NES раскладка ─────────────────────────────────────────────────────────
    ImGui::SeparatorText("Controls (NES)");
    static const char* kNesNames[8] = {
        "Up", "Down", "Left", "Right", "A", "B", "Select", "Start"
    };
    drawBindTable("nes_keytable", false, nesKeys_, 8, kNesNames);

    // ── SNES раскладка ────────────────────────────────────────────────────────
    ImGui::SeparatorText("Controls (SNES)");
    static const char* kSnesNames[12] = {
        "Up", "Down", "Left", "Right",
        "A", "B", "Select", "Start",
        "X", "Y", "L", "R"
    };
    drawBindTable("snes_keytable", true, snesKeys_, 12, kSnesNames);

    ImGui::SeparatorText("UI Theme");
    if (ImGui::RadioButton("Light", themeMode_==ThemeMode::Light)) {
        themeMode_=ThemeMode::Light; applyLightTheme(); saveConfig();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Dark", themeMode_==ThemeMode::Dark)) {
        themeMode_=ThemeMode::Dark; applyDarkTheme(); saveConfig();
    }

    ImGui::SeparatorText("Game Color Mode");
    if (ImGui::RadioButton("Normal##cm",  colorMode_==ColorMode::Normal))     { colorMode_=ColorMode::Normal;     saveConfig(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Inverted##cm",colorMode_==ColorMode::Inverted))   { colorMode_=ColorMode::Inverted;   saveConfig(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("B&&W##cm",    colorMode_==ColorMode::BlackWhite)) { colorMode_=ColorMode::BlackWhite; saveConfig(); }

    // ── Папки с ROM ──────────────────────────────────────────────────────────
    ImGui::SeparatorText("ROM Folders");
    ImGui::TextDisabled("Folders scanned automatically on startup and by the  button.");
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

    if (ImGui::Button("Add Folder...")) {
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
                saveConfig();
                scanRomFolders();
            }
        }
    }

    ImGui::End();
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

void App::scanRomsFolder() {
    if (!fs::exists("roms")) return;
    for (auto& entry : fs::directory_iterator("roms")) {
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

// Сканируем все папки из romFolders_ и добавляем новые ROM (рекурсивно)
void App::scanRomFolders()
{
    for (const auto& folder : romFolders_) {
        if (!fs::exists(folder)) continue;
        // recursive_directory_iterator обходит всё дерево подпапок
        std::error_code ec;
        for (auto& entry : fs::recursive_directory_iterator(folder,
                fs::directory_options::skip_permission_denied, ec)) {
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

void App::applyLightTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsLight(&s);
    s.WindowRounding = s.ChildRounding = s.FrameRounding =
    s.PopupRounding  = s.GrabRounding  = s.TabRounding   = 7.0f;
    s.WindowBorderSize = s.FrameBorderSize = 1.0f;
    s.ItemSpacing  = {8.0f, 6.0f};
    s.FramePadding = {10.0f, 5.0f};

    auto accent  = ImVec4(0.098f, 0.463f, 0.824f, 1.0f);
    auto accentD = ImVec4(0.050f, 0.310f, 0.620f, 1.0f);
    s.Colors[ImGuiCol_WindowBg]           = ImVec4(0.965f,0.965f,0.980f,1.0f);
    s.Colors[ImGuiCol_ChildBg]            = ImVec4(1.000f,1.000f,1.000f,1.0f);
    s.Colors[ImGuiCol_PopupBg]            = ImVec4(0.990f,0.990f,1.000f,1.0f);
    s.Colors[ImGuiCol_Text]               = ImVec4(0.130f,0.130f,0.150f,1.0f);
    s.Colors[ImGuiCol_TextDisabled]       = ImVec4(0.620f,0.620f,0.650f,1.0f);
    s.Colors[ImGuiCol_Border]             = ImVec4(0.840f,0.840f,0.880f,1.0f);
    s.Colors[ImGuiCol_FrameBg]            = ImVec4(0.930f,0.930f,0.950f,1.0f);
    s.Colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.870f,0.920f,1.000f,1.0f);
    s.Colors[ImGuiCol_TitleBg]            = ImVec4(0.940f,0.940f,0.960f,1.0f);
    s.Colors[ImGuiCol_TitleBgActive]      = ImVec4(0.870f,0.920f,1.000f,1.0f);
    s.Colors[ImGuiCol_Button]             = ImVec4(0.920f,0.925f,0.945f,1.0f);
    s.Colors[ImGuiCol_ButtonHovered]      = accent;
    s.Colors[ImGuiCol_ButtonActive]       = accentD;
    s.Colors[ImGuiCol_Header]             = ImVec4(0.870f,0.920f,1.000f,1.0f);
    s.Colors[ImGuiCol_HeaderHovered]      = accent;
    s.Colors[ImGuiCol_Separator]          = ImVec4(0.840f,0.840f,0.880f,1.0f);
    s.Colors[ImGuiCol_CheckMark]          = accent;
    s.Colors[ImGuiCol_SliderGrab]         = accent;
    s.Colors[ImGuiCol_ScrollbarGrab]      = ImVec4(0.750f,0.780f,0.850f,1.0f);
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = accent;
    s.Colors[ImGuiCol_TableBorderLight]   = ImVec4(0.840f,0.840f,0.880f,1.0f);
    s.Colors[ImGuiCol_TableBorderStrong]  = ImVec4(0.720f,0.720f,0.780f,1.0f);
    s.Colors[ImGuiCol_TableRowBgAlt]      = ImVec4(0.930f,0.940f,0.980f,0.6f);
}

void App::applyDarkTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = s.FrameRounding = s.GrabRounding =
    s.TabRounding    = s.PopupRounding = 7.0f;
    auto accent = ImVec4(0.29f,0.62f,1.0f,1.0f);
    s.Colors[ImGuiCol_WindowBg]         = ImVec4(0.078f,0.078f,0.102f,1.0f);
    s.Colors[ImGuiCol_Text]             = ImVec4(0.922f,0.929f,0.949f,1.0f);
    s.Colors[ImGuiCol_Button]           = ImVec4(0.14f, 0.14f, 0.18f, 1.0f);
    s.Colors[ImGuiCol_ButtonHovered]    = accent;
    s.Colors[ImGuiCol_ButtonActive]     = ImVec4(0.19f,0.47f,0.83f,1.0f);
    s.Colors[ImGuiCol_Header]           = ImVec4(0.16f,0.16f,0.22f,1.0f);
    s.Colors[ImGuiCol_HeaderHovered]    = accent;
    s.Colors[ImGuiCol_CheckMark]        = accent;
    s.Colors[ImGuiCol_ScrollbarGrab]    = accent;
    s.Colors[ImGuiCol_SliderGrab]       = accent;
    s.Colors[ImGuiCol_FrameBg]          = ImVec4(0.12f,0.12f,0.16f,1.0f);
    s.Colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.16f,0.16f,0.22f,1.0f);
    s.Colors[ImGuiCol_TitleBg]          = ImVec4(0.08f,0.08f,0.10f,1.0f);
    s.Colors[ImGuiCol_TitleBgActive]    = ImVec4(0.10f,0.10f,0.14f,1.0f);
    s.Colors[ImGuiCol_Separator]        = ImVec4(0.22f,0.22f,0.28f,1.0f);
    s.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.22f,0.22f,0.28f,1.0f);
    s.Colors[ImGuiCol_TableBorderStrong]= ImVec4(0.30f,0.30f,0.38f,1.0f);
    s.Colors[ImGuiCol_TableRowBgAlt]    = ImVec4(0.10f,0.10f,0.14f,0.5f);
}

// ─── Конфиг ──────────────────────────────────────────────────────────────────

// Абсолютный путь к config.json рядом с исполняемым файлом.
// SDL_GetBasePath возвращает директорию exe — устойчиво к изменению cwd
// (например, после tinyfd-диалога выбора папки). Кешируем, потому что:
//   1) SDL_GetBasePath требует SDL_Init и в редких случаях возвращает nullptr;
//   2) tinyfd_selectFolderDialog меняет cwd процесса — если бы мы каждый раз
//      вычисляли путь от относительного, файл уезжал бы в случайные места.
static std::string configPath() {
    static std::string cached;
    if (!cached.empty()) return cached;
    char* base = SDL_GetBasePath();
    if (base && *base) {
        cached = std::string(base) + "config.json";
        SDL_free(base);
    } else {
        // Fallback: рядом с текущим cwd на момент первого вызова
        try {
            cached = (fs::current_path() / "config.json").string();
        } catch (...) { cached = "config.json"; }
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
