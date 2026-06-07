// snes_dsp1.cpp — эмуляция DSP-1 на уровне команд
#include "snes_dsp1.h"
#include <cstring>

void SnesDSP1::reset()
{
    sr_ = 0x80;            // готов к обмену
    haveCommand_ = false;
    inCount_ = inIndex_ = 0;
    outCount_ = outIndex_ = 0;
    std::memset(inBuf_, 0, sizeof inBuf_);
    std::memset(outBuf_, 0, sizeof outBuf_);
}

// ─── Status Register ($7000): бит7 RQM = всегда готов (мгновенные вычисления) ──
uint8_t SnesDSP1::readSR() const
{
    return sr_;
}

// ─── Чтение Data Register ($6000): отдаём байты результата ────────────────────
uint8_t SnesDSP1::readDR()
{
    if (outIndex_ < outCount_) {
        uint8_t v = outBuf_[outIndex_++];
        if (outIndex_ >= outCount_) {
            // результат отдан полностью — ждём новую команду
            outIndex_ = outCount_ = 0;
            haveCommand_ = false;
        }
        return v;
    }
    return 0x00;
}

// ─── Запись Data Register ($6000): команда, затем параметры ───────────────────
void SnesDSP1::writeDR(uint8_t v)
{
    if (!haveCommand_) {
        haveCommand_ = true;
        inIndex_ = 0;
        outIndex_ = outCount_ = 0;
        inCount_ = cmdInBytes(v);
        if (inCount_ == 0) exec();   // команда без параметров — выполняем сразу
        return;
    }
    if (inIndex_ < (int)sizeof(inBuf_)) inBuf_[inIndex_++] = v;
    if (inIndex_ >= inCount_) exec();
}

// ─── [ЗАГЛУШКА для теста] размер входа команды ────────────────────────────────
int SnesDSP1::cmdInBytes(uint8_t /*cmd*/) const
{
    return 0;   // временно: команды без параметров, выполняем сразу
}

// ─── [ЗАГЛУШКА для теста] вычисление: отдаём нули ─────────────────────────────
void SnesDSP1::exec()
{
    setOut(1);  // 1 слово результата = 0
}

void SnesDSP1::setOut(int words)
{
    outCount_ = words * 2;
    outIndex_ = 0;
    for (int i = 0; i < outCount_ && i < (int)sizeof(outBuf_); ++i) outBuf_[i] = 0;
}
