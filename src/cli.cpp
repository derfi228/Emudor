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
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
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
        else if (flag == "--record-trace") {
            const char* v = needVal("--record-trace"); if (!v) return a;
            a.traceLogPath = v;
        }
        else if (flag == "--input-script") {
            const char* v = needVal("--input-script"); if (!v) return a;
            a.inputScriptPath = v;
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
        "  --record-trace <path>   Записать лог трассировки CPU в файл\n"
        "  --help, -h              Показать эту справку\n"
        "\n"
        "Коды возврата:\n"
        "  0 — успех\n"
        "  1 — ROM не загрузился\n"
        "  2 — краш / исключение\n"
        "\n"
        "Примеры:\n"
        "  emudor --headless --rom mario.nes --frames 600 --screenshot end.png\n"
        "  emudor --headless --rom zelda.sfc --frames 300 --screenshot-every 60\n"
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

// ─── Input-script (debug-инструмент для воспроизведения нажатий) ─────────────
// Формат: одна строка на событие "<frame> <BUTTON> <press|release>".
// '#' — комментарий, пустые строки игнорируются.
// Кнопки: UP DOWN LEFT RIGHT A B X Y L R START SELECT (X/Y/L/R только SNES).
// Состояние накапливается: press ставит бит, release снимает, держится до смены.
struct InputEvent { int frame; uint16_t mask; bool press; };

uint16_t buttonMaskOf(const std::string& nameUpper, bool isSnes)
{
    // SNES 16-бит: B(15) Y(14) Sel(13) Start(12) Up(11) Dn(10) Lt(9) Rt(8) A(7) X(6) L(5) R(4)
    // NES  8-бит:  A(7) B(6) Sel(5) Start(4) Up(3) Dn(2) Lt(1) Rt(0)
    if (isSnes) {
        if (nameUpper=="B")      return 1u<<15;
        if (nameUpper=="Y")      return 1u<<14;
        if (nameUpper=="SELECT") return 1u<<13;
        if (nameUpper=="START")  return 1u<<12;
        if (nameUpper=="UP")     return 1u<<11;
        if (nameUpper=="DOWN")   return 1u<<10;
        if (nameUpper=="LEFT")   return 1u<<9;
        if (nameUpper=="RIGHT")  return 1u<<8;
        if (nameUpper=="A")      return 1u<<7;
        if (nameUpper=="X")      return 1u<<6;
        if (nameUpper=="L")      return 1u<<5;
        if (nameUpper=="R")      return 1u<<4;
    } else {
        if (nameUpper=="A")      return 1u<<7;
        if (nameUpper=="B")      return 1u<<6;
        if (nameUpper=="SELECT") return 1u<<5;
        if (nameUpper=="START")  return 1u<<4;
        if (nameUpper=="UP")     return 1u<<3;
        if (nameUpper=="DOWN")   return 1u<<2;
        if (nameUpper=="LEFT")   return 1u<<1;
        if (nameUpper=="RIGHT")  return 1u<<0;
    }
    return 0;
}

std::vector<InputEvent> loadInputScript(const std::string& path, bool isSnes)
{
    std::vector<InputEvent> events;
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "input-script: cannot open '%s'\n", path.c_str());
        return events;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(f, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos || line[s] == '#') continue;

        std::istringstream iss(line.substr(s));
        int frame; std::string btn, act;
        if (!(iss >> frame >> btn >> act)) {
            std::fprintf(stderr, "input-script:%d: expected '<frame> <button> press|release'\n", lineNo);
            continue;
        }
        std::transform(btn.begin(), btn.end(), btn.begin(),
                       [](unsigned char c){ return (char)std::toupper(c); });
        std::transform(act.begin(), act.end(), act.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        uint16_t mask = buttonMaskOf(btn, isSnes);
        if (mask == 0) { std::fprintf(stderr, "input-script:%d: bad button '%s'\n", lineNo, btn.c_str()); continue; }
        bool press = (act == "press");
        events.push_back({ frame, mask, press });
    }
    std::stable_sort(events.begin(), events.end(),
                     [](const InputEvent& a, const InputEvent& b){ return a.frame < b.frame; });
    std::fprintf(stdout, "input-script: loaded %zu events from '%s'\n", events.size(), path.c_str());
    return events;
}

} // namespace

// ─── Перехват крашей: переводим всё в rc=2 ──────────────────────────────────
// На Windows MinGW abort() / SIGABRT / SIGSEGV дают exit code 3 (signal-based)
// или 0xC0000005 (access violation). Спецификация CLI говорит «rc=2 = краш».
// Чтобы tester мог надёжно отличать "штатный fail" от "что-то очень плохое",
// перехватываем всё что можно перехватить C++/POSIX-средствами.
namespace {

[[noreturn]] void crashExit(const char* what)
{
    std::fputs("Crash: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::_Exit(2);
}

extern "C" void signalCrashHandler(int sig)
{
    const char* name = "unknown signal";
    switch (sig) {
        case SIGABRT: name = "SIGABRT (abort/assert)"; break;
        case SIGSEGV: name = "SIGSEGV (segmentation fault)"; break;
        case SIGFPE:  name = "SIGFPE (floating-point exception)"; break;
        case SIGILL:  name = "SIGILL (illegal instruction)"; break;
    }
    crashExit(name);
}

void terminateHandler()
{
    // Сюда попадаем при uncaught C++ exception или std::terminate()
    try {
        if (auto ep = std::current_exception()) std::rethrow_exception(ep);
        crashExit("std::terminate() called");
    } catch (const std::exception& e) {
        crashExit(e.what());
    } catch (...) {
        crashExit("unknown C++ exception");
    }
}

void installCrashHandlers()
{
    std::set_terminate(terminateHandler);
    std::signal(SIGABRT, signalCrashHandler);
    std::signal(SIGSEGV, signalCrashHandler);
    std::signal(SIGFPE,  signalCrashHandler);
    std::signal(SIGILL,  signalCrashHandler);
}

} // namespace

// ─── Headless-прогонка ───────────────────────────────────────────────────────
int runHeadless(const CliArgs& args)
{
    // Ставим перехватчики крашей ДО любого взаимодействия с эмуляцией.
    // Любой signal или uncaught exception → rc=2 (вместо системного rc=3).
    installCrashHandlers();

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

    // ── Input-script (опционально, debug) ────────────────────────────────────
    const bool isSnes = (con->getConsoleName() == "SNES");
    std::vector<InputEvent> events;
    if (!args.inputScriptPath.empty())
        events = loadInputScript(args.inputScriptPath, isSnes);
    size_t evIdx = 0;
    uint16_t btnState = 0;  // накапливаемое состояние кнопок

    // ── Основной цикл ────────────────────────────────────────────────────────
    int totalFrames = (args.frames > 0) ? args.frames : 600;  // дефолт для headless
    int rc = 0;
    try {
        for (int frame = 0; frame < totalFrames; ++frame) {
            // Применяем события input-script на этот кадр
            while (evIdx < events.size() && events[evIdx].frame <= frame) {
                if (events[evIdx].press) btnState |=  events[evIdx].mask;
                else                     btnState &= ~events[evIdx].mask;
                ++evIdx;
            }
            if (!events.empty()) con->setInput(0, btnState);

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
