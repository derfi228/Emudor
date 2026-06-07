#pragma once
#include <cstdint>

// ─── DSP-1 (NEC uPD77C25) — математический сопроцессор SNES ────────────────────
// Используется в Super Mario Kart, Pilotwings и др. для 16-битной математики
// с фиксированной точкой (умножение, синус/косинус, проекции для Mode-7).
//
// Интерфейс с CPU (маппинг как у Mario Kart):
//   DR (Data Register)   — $6000-$6FFF (банки $00-$3F/$80-$BF): команда/данные.
//   SR (Status Register) — $7000-$7FFF: бит7 (RQM) = готов к обмену.
//
// ВНИМАНИЕ: пока это МИНИМАЛЬНАЯ заглушка — статус всегда «готов», результаты
// команд = 0. Этого достаточно, чтобы DSP-1-игры проходили boot/self-test,
// показывали интро/меню и играли музыку (N-SPC). Полная математика DSP-1
// (нужна для рендера трасс в гонке) — отдельная задача.
class SnesDSP1 {
public:
    void    reset();
    uint8_t readDR();             // чтение Data Register ($6000)
    void    writeDR(uint8_t v);   // запись Data Register ($6000)
    uint8_t readSR() const;       // чтение Status Register ($7000)

private:
    uint8_t  sr_ = 0x80;          // статус (бит7 RQM = готов)
    bool     haveCommand_ = false;
    int      inCount_ = 0;
    int      inIndex_ = 0;
    int      outCount_ = 0;
    int      outIndex_ = 0;
    uint8_t  inBuf_[64]{};
    uint8_t  outBuf_[64]{};

    int  cmdInBytes(uint8_t cmd) const;  // число входных БАЙТ команды
    void exec();                         // вычислить результат
    void setOut(int words);              // подготовить выход (words слов = 0)
};
