// ─── Точка входа Emudor ──────────────────────────────────────────────────────
// Разбирает командную строку. В интерактивном режиме (нет --headless) —
// поднимает App с SDL2+ImGui. В headless-режиме — прогоняет ROM через
// IConsole::runFrame() без UI (для авто-тестов и ИИ-агентов).

#include "app.h"
#include "cli.h"

#include <cstdio>

int main(int argc, char** argv)
{
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
