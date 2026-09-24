#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "gb_apu.h"
#include "gb_cart.h"
#include "gb_ppu.h"

// ─── Шина Game Boy: карта памяти, таймер, джойпад, DMA ───────────────────────
// $0000-$7FFF ПЗУ картриджа, $8000-$9FFF видеопамять, $A000-$BFFF ОЗУ
// картриджа, $C000-$DFFF рабочая память (у CGB — 8 банков по 4 КБ), $E000-$FDFF
// её зеркало, $FE00-$FE9F OAM, $FF00-$FF7F порты, $FF80-$FFFE HRAM, $FFFF IE.
//
// Каждое обращение процессора — один M-цикл: шина двигает таймер (в такте
// процессора), OAM DMA, видео и звук (в такте экрана: при двойной скорости
// CGB — вдвое меньше точек на M-цикл).
class GbBus {
public:
    GbPpu ppu;
    GbApu apu;

    void init(GbCart* cart, bool cgb);        // после загрузки картриджа
    bool cgb() const { return cgb_; }
    bool doubleSpeed() const { return doubleSpeed_; }

    // ── Для процессора: каждое обращение — один M-цикл ───────────────────────
    uint8_t cpuRead(uint16_t addr)             { tick(); return read(addr); }
    void    cpuWrite(uint16_t addr, uint8_t v) { tick(); write(addr, v); }
    void    cpuIdle()                          { tick(); }
    bool    consumeStall();                    // HDMA держит процессор

    uint8_t read(uint16_t addr);               // без хода времени
    void    write(uint16_t addr, uint8_t v);

    // ── Прерывания: 0 VBlank, 1 STAT, 2 таймер, 3 порт, 4 джойпад ────────────
    uint8_t pendingInterrupts() const { return (uint8_t)(ie_ & if_ & 0x1F); }
    void    ackInterrupt(int bit)     { if_ &= (uint8_t)~(1 << bit); }
    void    requestInterrupt(int bit) { if_ |= (uint8_t)(1 << bit); }

    // STOP: у CGB с подготовленным KEY1 переключает скорость (true).
    bool    stopInstruction();

    // ── Кнопки: бит 0 → Right, 1 Left, 2 Up, 3 Down, 4 A, 5 B, 6 Select, 7 Start
    void    setButtons(uint8_t pressed);
    bool    anyButtonPressed() const { return buttons_ != 0; }

    uint64_t dots() const { return dots_; }    // точек экрана с момента старта
    // Что игра отправила в последовательный порт (тестовые ROM печатают туда
    // результат). Хранятся последние 1024 байта.
    const std::string& serialLog() const { return serialLog_; }

    template<class S> void serialize(S& s);

private:
    GbCart* cart_ = nullptr;
    bool    cgb_ = false;
    bool    doubleSpeed_ = false;
    uint8_t key1_ = 0;

    std::array<uint8_t, 0x8000> wram_{};
    std::array<uint8_t, 0x7F>   hram_{};
    uint8_t svbk_ = 1;
    uint8_t ie_ = 0, if_ = 0xE1;

    // Таймер: 16-битный делитель, TIMA растёт по спаду выбранного бита
    uint16_t div_ = 0;
    uint8_t  tima_ = 0, tma_ = 0, tac_ = 0xF8;
    bool     timaReload_ = false;              // переполнение: перезагрузка через M-цикл

    // Джойпад
    uint8_t  p1_ = 0x30;                       // выбор групп (биты 5-4)
    uint8_t  buttons_ = 0;

    // Последовательный порт (без партнёра)
    uint8_t  sb_ = 0, sc_ = 0;
    int32_t  serialTimer_ = 0;

    // OAM DMA
    bool     dmaActive_ = false;
    uint8_t  dmaReg_ = 0xFF;
    uint16_t dmaSrc_ = 0;
    uint8_t  dmaIndex_ = 0;
    uint8_t  dmaDelay_ = 0;

    // CGB HDMA
    uint16_t hdmaSrc_ = 0, hdmaDst_ = 0;
    uint8_t  hdmaRemain_ = 0;                  // блоков по 16 байт
    bool     hdmaActive_ = false;              // режим HBlank
    uint32_t stall_ = 0;                       // M-циклов, пока процессор стоит

    uint64_t dots_ = 0;
    std::string serialLog_;

    void     tick();
    void     timerStep();
    void     divChanged(uint16_t oldDiv, uint16_t newDiv);
    bool     timerSignal(uint16_t div, uint8_t tac) const;
    uint8_t  joypadLines() const;
    uint8_t  readIO(uint16_t addr);
    void     writeIO(uint16_t addr, uint8_t v);
    uint8_t  dmaSourceRead(uint16_t addr);
    void     hdmaBlock();
};
