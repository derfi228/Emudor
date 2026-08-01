// ─── Точка входа Emudor ──────────────────────────────────────────────────────
// Разбирает командную строку. В интерактивном режиме (нет --headless) —
// поднимает App с SDL2+ImGui. В headless-режиме — прогоняет ROM через
// IConsole::runFrame() без UI (для авто-тестов и ИИ-агентов).

#include "app.h"
#include "cli.h"

#include <cstdio>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifdef _WIN32_WINNT
#    undef _WIN32_WINNT
#  endif
#  define _WIN32_WINNT 0x0A00  // Windows 10 — нужен для DPI_AWARENESS_CONTEXT_*
#  include <windows.h>
#endif

int main(int argc, char** argv)
{
#ifdef _WIN32
    // Без этого Windows считает приложение "DPI-unaware" и растрово
    // растягивает окно под масштаб экрана (125%/150%/...) — размытая картинка
    // и вся вёрстка на основе реального winW/winH сбивается с толку. Per-
    // Monitor-V2 — самый современный режим, есть с Windows 10 1703+.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

    CliArgs args = parseCli(argc, argv);

    if (args.printHelp) {
        printCliHelp();
        return 0;
    }
    if (!args.errorMsg.empty()) {
        std::fprintf(stderr, "CLI error: %s\n\n", args.errorMsg.c_str());
        printCliHelp();
        return 1;
    }

    // ── Headless-режим: без окна, без ImGui ──────────────────────────────────
    if (args.requiresHeadless()) {
        return runHeadless(args);
    }

    // ── Интерактивный режим: обычный UI ──────────────────────────────────────
    App app;
    if (!args.romPath.empty()) app.pendingRomPath_ = args.romPath;
    app.run();
    return 0;
}
