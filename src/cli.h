#pragma once
#include <string>

// ─── CLI-аргументы эмулятора ─────────────────────────────────────────────────
// Парсинг и обработка флагов командной строки. Используется автоматическими
// тест-агентами для прогонки ROM без UI.
struct CliArgs {
    std::string romPath;          // --rom
    std::string consoleName;      // --console NES|SNES|...
    int         frames           = -1;     // --frames (−1 = бесконечно)
    std::string screenshotPath;   // --screenshot
    int         screenshotEvery  = 0;      // --screenshot-every N (0 = выкл)
    bool        headless         = false;  // --headless
    std::string traceLogPath;     // --record-trace

    bool        printHelp        = false;  // --help / -h
    std::string errorMsg;                  // непустое — ошибка парсинга

    // true если задан хотя бы один флаг, требующий headless-обработки
    // (или сам --headless). Используется чтобы решить, запускать App или CLI.
    bool requiresHeadless() const { return headless; }
};

// Парсит argv → CliArgs. Не падает на неизвестных флагах — пишет в errorMsg.
CliArgs parseCli(int argc, char** argv);

// Печатает справку в stdout.
void printCliHelp();

// Запускает эмулятор в headless-режиме.
// Возвращает код возврата процесса:
//   0 — успех
//   1 — ROM не загрузился (или не указан)
//   2 — краш / неперехваченное исключение
int runHeadless(const CliArgs& args);
