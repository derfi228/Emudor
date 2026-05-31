// ─── CLI-режим эмулятора Emudor ──────────────────────────────────────────────
// Headless-прогонка ROM из командной строки: без SDL-окна, без ImGui.
// Для систем автоматического тестирования (ИИ-агенты, регрессии).
//
// Зависимости: SDL2 (только core, без VIDEO) + SDL2_image (для IMG_SavePNG).

#include "cli.h"

#include "console/iconsole.h"
#include "console/console_detect.h"
#include "console/nes_console.h"
#include "console/snes_console.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ─── Парсинг командной строки ────────────────────────────────────────────────
namespace {

bool hasValue(int i, int argc) { return (i + 1) < argc; }

ConsoleType consoleFromName(const std::string& name)
{
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c){ return (char)std::toupper(c); });
    if (n == "NES")  return ConsoleType::NES;
    if (n == "SNES") return ConsoleType::SNES;
    return ConsoleType::Unknown;
}

} // namespace

CliArgs parseCli(int argc, char** argv)
{
    CliArgs a;
    for (int i = 1; i < argc; ++i) {
        std::string flag = argv[i];

        auto needVal = [&](const char* name) -> const char* {
            if (!hasValue(i, argc)) {
                a.errorMsg = std::string("missing value for ") + name;
                return nullptr;
            }
            return argv[++i];
        };

        if (flag == "--help" || flag == "-h") {
            a.printHelp = true;
        }
        else if (flag == "--rom") {
            const char* v = needVal("--rom");          if (!v) return a;
            a.romPath = v;
        }
        else if (flag == "--console") {
            const char* v = needVal("--console");      if (!v) return a;
            a.consoleName = v;
        }
        else if (flag == "--frames") {
            const char* v = needVal("--frames");       if (!v) return a;
            try { a.frames = std::stoi(v); }
            catch (...) { a.errorMsg = "invalid --frames value"; return a; }
        }
        else if (flag == "--screenshot") {
            const char* v = needVal("--screenshot");   if (!v) return a;
            a.screenshotPath = v;
        }
        else if (flag == "--screenshot-every") {
            const char* v = needVal("--screenshot-every"); if (!v) return a;
            try { a.screenshotEvery = std::stoi(v); }
            catch (...) { a.errorMsg = "invalid --screenshot-every value"; return a; }
        }
        else if (flag == "--headless") {
            a.headless = true;
        }
        else if (flag == "--input-script") {
            const char* v = needVal("--input-script"); if (!v) return a;
            a.inputScriptPath = v;
        }
        else if (flag == "--record-trace") {
            const char* v = needVal("--record-trace"); if (!v) return a;
            a.traceLogPath = v;
        }
        else if (flag.rfind("--", 0) == 0) {
            a.errorMsg = "unknown flag: " + flag;
            return a;
        }
        else {
            // Позиционный аргумент — путь к ROM (для совместимости)
            if (a.romPath.empty()) a.romPath = flag;
        }
    }
    return a;
}

void printCliHelp()
{
    std::printf(
        "Emudor — мультиконсольный эмулятор (NES + SNES)\n"
        "\n"
        "Использование:\n"
        "  emudor [options] [rom-path]\n"
        "\n"
        "Опции:\n"
        "  --rom <path>            Путь к ROM-файлу\n"
        "  --console <name>        NES, SNES (auto-detect если не указано)\n"
        "  --frames <number>       Сколько кадров эмулировать, потом выйти\n"
        "  --screenshot <path>     Куда сохранить финальный скриншот (PNG)\n"
        "  --screenshot-every <N>  Сохранять скриншот каждые N кадров\n"
        "                          (нумеруются: name_0001.png, name_0002.png, ...)\n"
        "  --headless              Не открывать SDL2 окно и ImGui — только эмуляция\n"
        "  --input-script <path>   Файл с записью нажатий для воспроизведения\n"
        "  --record-trace <path>   Записать лог трассировки CPU в файл\n"
        "  --help, -h              Показать эту справку\n"
        "\n"
        "Формат input-script:\n"
        "  <frame> <button> <action>    # одно событие на строку\n"
        "  frame   — целое число (номер кадра от старта)\n"
        "  button  — UP DOWN LEFT RIGHT A B  (NES)\n"
        "            + X Y L R              (только SNES)\n"
        "            + START SELECT         (обе системы)\n"
        "            (регистр не важен)\n"
        "  action  — press  / release\n"
        "  '#...'  — комментарий, пустые строки игнорируются\n"
        "\n"
        "  Пример:\n"
        "    60  START press\n"
        "    70  START release\n"
        "    90  RIGHT press\n"
        "    130 RIGHT release\n"
        "\n"
        "Коды возврата:\n"
        "  0 — успех\n"
        "  1 — ROM не загрузился\n"
        "  2 — краш / исключение\n"
        "\n"
        "Примеры:\n"
        "  emudor --headless --rom mario.nes --frames 600 --screenshot end.png\n"
        "  emudor --headless --rom zelda.sfc --frames 300 \\\n"
        "         --input-script start.txt --screenshot-every 60\n"
    );
}

// ─── Создание консоли по типу ────────────────────────────────────────────────
namespace {

std::unique_ptr<IConsole> makeConsole(ConsoleType t)
{
    switch (t) {
    case ConsoleType::NES:  return std::make_unique<NesConsole>();
    case ConsoleType::SNES: return std::make_unique<SnesConsole>();
    default:                return nullptr;
    }
}

// ─── Скриншот в PNG ──────────────────────────────────────────────────────────
// Framebuffer консоли — ARGB8888 little-endian (0xAARRGGBB), один uint32_t/пиксель.
// Превращаем в SDL_Surface и сохраняем через IMG_SavePNG.
bool savePng(const IConsole& con, const std::string& path)
{
    int w = con.getFrameWidth();
    int h = con.getFrameHeight();
    const uint32_t* fb = con.getFramebuffer();
    if (!fb || w <= 0 || h <= 0) return false;

    // SDL_PIXELFORMAT_ARGB8888 = на little-endian читается как [B,G,R,A] байтами,
    // а как uint32_t = 0xAARRGGBB. Это формат, который ожидается от консолей.
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
        const_cast<uint32_t*>(fb),
        w, h,
        32,                                  // bits/pixel
        (int)(w * sizeof(uint32_t)),         // pitch (байт на строку)
        SDL_PIXELFORMAT_ARGB8888);
    if (!surf) {
        std::fprintf(stderr, "savePng: SDL_CreateRGBSurfaceWithFormatFrom: %s\n",
                     SDL_GetError());
        return false;
    }
    int rc = IMG_SavePNG(surf, path.c_str());
    SDL_FreeSurface(surf);
    if (rc != 0) {
        std::fprintf(stderr, "savePng: IMG_SavePNG(%s): %s\n",
                     path.c_str(), IMG_GetError());
        return false;
    }
    return true;
}

// ─── Нумерация скриншотов: foo.png → foo_0042.png ───────────────────────────
std::string makeNumberedPath(const std::string& tpl, int frame)
{
    // Берём расширение от точки в конце; если нет — добавляем .png
    auto dot = tpl.rfind('.');
    std::string stem, ext;
    if (dot == std::string::npos) { stem = tpl;             ext = ".png"; }
    else                          { stem = tpl.substr(0,dot); ext = tpl.substr(dot); }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "_%04d", frame);
    return stem + buf + ext;
}

// ─── Input-script ────────────────────────────────────────────────────────────
// Формат: '<frame> <BUTTON> press|release'
//   frame   — целое число (номер кадра от старта)
//   button  — UP DOWN LEFT RIGHT A B X Y L R START SELECT (case-insensitive)
//   action  — press или release
//   '#...'  — комментарий, пустые строки игнорируются
//
// Кнопки накапливаются в running state: press ставит бит, release снимает.
// В каждом кадре все события на этот frame применяются ДО runFrame().
struct InputEvent {
    int      frame;
    uint16_t buttonMask;  // одна кнопка — ровно один бит
    bool     isPress;     // true=press, false=release
};

// Возвращает битовую маску одной кнопки.
// Для NES кнопки X/Y/L/R недоступны — возвращаем 0 с предупреждением.
uint16_t buttonMaskOf(const std::string& name, bool isSnes, int lineNo)
{
    // SNES: B(15) Y(14) SELECT(13) START(12) UP(11) DOWN(10) LEFT(9) RIGHT(8)
    //       A(7)  X(6)  L(5)       R(4)
    // NES:  A(7)  B(6)  SELECT(5)  START(4)  UP(3)  DOWN(2)  LEFT(1) RIGHT(0)
    static const std::unordered_map<std::string, std::pair<uint16_t,uint16_t>> table = {
    //  name      NES-mask    SNES-mask
        {"UP",    {1u<<3,  1u<<11}},
        {"DOWN",  {1u<<2,  1u<<10}},
        {"LEFT",  {1u<<1,  1u<< 9}},
        {"RIGHT", {1u<<0,  1u<< 8}},
        {"A",     {1u<<7,  1u<< 7}},
        {"B",     {1u<<6,  1u<<15}},
        {"SELECT",{1u<<5,  1u<<13}},
        {"START", {1u<<4,  1u<<12}},
        // SNES-only:
        {"X",     {0,      1u<< 6}},
        {"Y",     {0,      1u<<14}},
        {"L",     {0,      1u<< 5}},
        {"R",     {0,      1u<< 4}},
    };
    auto it = table.find(name);
    if (it == table.end()) {
        std::fprintf(stderr, "input-script:%d: unknown button '%s'\n", lineNo, name.c_str());
        return 0;
    }
    uint16_t mask = isSnes ? it->second.second : it->second.first;
    if (mask == 0 && !isSnes) {
        std::fprintf(stderr, "input-script:%d: button '%s' not available on NES\n",
                     lineNo, name.c_str());
    }
    return mask;
}

bool loadInputScript(const std::string& path, bool isSnes,
                     std::vector<InputEvent>& out)
{
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "input-script: cannot open '%s'\n", path.c_str());
        return false;
    }
    std::string line;
    int lineNo = 0;
    int loaded = 0;
    while (std::getline(f, line)) {
        ++lineNo;
        // Убираем \r (Windows CRLF) и ведущие пробелы
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;   // пустая строка
        if (line[start] == '#') continue;            // комментарий

        std::istringstream iss(line.substr(start));
        int frame;
        std::string btnName, action;
        if (!(iss >> frame >> btnName >> action)) {
            std::fprintf(stderr, "input-script:%d: expected '<frame> <button> press|release'\n",
                         lineNo);
            continue;
        }
        // Нормализуем к upper
        std::transform(btnName.begin(), btnName.end(), btnName.begin(),
                       [](unsigned char c){ return (char)std::toupper(c); });
        std::transform(action.begin(), action.end(), action.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });

        if (action != "press" && action != "release") {
            std::fprintf(stderr, "input-script:%d: action must be 'press' or 'release', got '%s'\n",
                         lineNo, action.c_str());
            continue;
        }

        uint16_t mask = buttonMaskOf(btnName, isSnes, lineNo);
        if (mask == 0) continue;  // ошибка уже напечатана

        out.push_back({ frame, mask, (action == "press") });
        ++loaded;
    }

    // Сортируем по кадру (стабильно — сохраняем порядок событий одного кадра)
    std::stable_sort(out.begin(), out.end(),
                     [](const InputEvent& a, const InputEvent& b){ return a.frame < b.frame; });

    std::fprintf(stdout, "input-script: loaded %d events from '%s'\n", loaded, path.c_str());
    return true;
}

} // namespace

// ─── Headless-прогонка ───────────────────────────────────────────────────────
int runHeadless(const CliArgs& args)
{
    // SDL без видео — нужен только для IMG_SavePNG (и SDL_CreateRGBSurfaceWithFormatFrom).
    if (SDL_Init(0) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 2;
    }
    // IMG_Init для PNG — поднимает libpng (если SDL2_image собран с поддержкой)
    int imgFlags = IMG_INIT_PNG;
    if ((IMG_Init(imgFlags) & imgFlags) != imgFlags) {
        std::fprintf(stderr, "IMG_Init(PNG) warning: %s\n", IMG_GetError());
        // не критично — IMG_SavePNG может работать и без IMG_Init
    }

    // ── Определяем тип консоли ───────────────────────────────────────────────
    ConsoleType type = ConsoleType::Unknown;
    if (!args.consoleName.empty()) {
        type = consoleFromName(args.consoleName);
        if (type == ConsoleType::Unknown) {
            std::fprintf(stderr, "Unknown --console: %s (NES|SNES)\n",
                         args.consoleName.c_str());
            IMG_Quit(); SDL_Quit();
            return 1;
        }
    } else {
        if (args.romPath.empty()) {
            std::fprintf(stderr, "Error: --rom required in headless mode\n");
            IMG_Quit(); SDL_Quit();
            return 1;
        }
        type = detectConsole(args.romPath);
        if (type == ConsoleType::Unknown) {
            std::fprintf(stderr, "Cannot detect console type for %s (use --console)\n",
                         args.romPath.c_str());
            IMG_Quit(); SDL_Quit();
            return 1;
        }
    }

    // ── Загружаем ROM ────────────────────────────────────────────────────────
    auto con = makeConsole(type);
    if (!con) {
        std::fprintf(stderr, "Console type not implemented\n");
        IMG_Quit(); SDL_Quit();
        return 1;
    }
    if (!con->loadROM(args.romPath)) {
        std::fprintf(stderr, "Failed to load ROM: %s\n", args.romPath.c_str());
        IMG_Quit(); SDL_Quit();
        return 1;
    }
    const bool isSnes = (con->getConsoleName() == "SNES");

    // ── Input-script (опционально) ───────────────────────────────────────────
    std::vector<InputEvent> events;
    if (!args.inputScriptPath.empty()) {
        loadInputScript(args.inputScriptPath, isSnes, events);
        // Ошибка открытия — продолжаем без ввода; отдельные строки с ошибками — пропущены
    }
    size_t evIdx = 0;
    uint16_t scriptButtons = 0;  // накапливаемое состояние: press ставит бит, release снимает

    // ── Trace log (опционально) ──────────────────────────────────────────────
    std::ofstream traceOut;
    if (!args.traceLogPath.empty()) {
        traceOut.open(args.traceLogPath, std::ios::trunc);
        if (!traceOut) {
            std::fprintf(stderr, "Cannot open trace log: %s\n",
                         args.traceLogPath.c_str());
        } else {
            traceOut << "# Emudor trace log (per-frame CPU state)\n";
            traceOut << "# console=" << con->getConsoleName() << "\n";
            traceOut << "# rom=" << args.romPath << "\n";
        }
    }

    // ── Основной цикл ────────────────────────────────────────────────────────
    int totalFrames = (args.frames > 0) ? args.frames : 600;  // дефолт для headless
    int rc = 0;
    try {
        for (int frame = 0; frame < totalFrames; ++frame) {
            // Применяем все события скрипта на этот кадр.
            // press → устанавливаем бит, release → снимаем.
            // Состояние накапливается: кнопка остаётся зажатой до release.
            bool inputChanged = false;
            while (evIdx < events.size() && events[evIdx].frame <= frame) {
                const InputEvent& ev = events[evIdx];
                if (ev.isPress)
                    scriptButtons |=  ev.buttonMask;
                else
                    scriptButtons &= ~ev.buttonMask;
                inputChanged = true;
                ++evIdx;
            }
            // Передаём текущее состояние кнопок в консоль.
            // В headless-режиме реальная клавиатура не читается, только скрипт.
            if (inputChanged || !events.empty()) {
                con->setInput(0, scriptButtons);
            }

            con->runFrame();
            con->clearAudioSamples();   // не накапливать в headless

            // Трасса (минимальная — frame # на каждый кадр)
            if (traceOut) {
                traceOut << "frame=" << frame << "\n";
            }

            // Промежуточные скриншоты
            if (args.screenshotEvery > 0 &&
                !args.screenshotPath.empty() &&
                ((frame + 1) % args.screenshotEvery == 0))
            {
                std::string p = makeNumberedPath(args.screenshotPath, frame + 1);
                if (!savePng(*con, p))
                    std::fprintf(stderr, "frame %d: screenshot %s failed\n", frame, p.c_str());
            }
        }

        // Финальный скриншот
        if (!args.screenshotPath.empty() && args.screenshotEvery == 0) {
            if (!savePng(*con, args.screenshotPath)) {
                std::fprintf(stderr, "Final screenshot failed: %s\n",
                             args.screenshotPath.c_str());
                rc = 2;
            }
        }
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Crash: %s\n", e.what());
        rc = 2;
    }
    catch (...) {
        std::fprintf(stderr, "Crash: unknown exception\n");
        rc = 2;
    }

    if (traceOut) traceOut.close();
    IMG_Quit();
    SDL_Quit();
    return rc;
}
