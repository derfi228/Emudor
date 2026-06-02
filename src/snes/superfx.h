#pragma once
#include <cstdint>
#include <vector>
#include <array>

// ─── SuperFX / GSU (Graphics Support Unit) ───────────────────────────────────
// Копроцессор Nintendo для Star Fox, Yoshi's Island и др. RISC-подобное ядро
// с 16 регистрами R0-R15, доступом к game-pak ROM/RAM и аппаратным плоттингом
// пикселей в bitplane-формат SNES.
//
// Регистры доступны CPU через $3000-$32FF (банки $00-$3F / $80-$BF).
// GSU исполняет код из ROM/RAM; CPU запускает его установкой SFR.GO (через R15).
//
// Реализация — интерпретатор. Опирается на доступ к ROM/RAM через колбэки шины.
class SuperFX {
public:
    // ── Конфигурация памяти ──────────────────────────────────────────────────
    // rom  — указатель на game-pak ROM (LoROM-маппинг)
    // ramKB — размер game-pak RAM в КБ (обычно 32/64/128)
    void connect(const std::vector<uint8_t>* rom, uint32_t ramKB);
    void reset();

    // ── Доступ CPU к регистрам $3000-$32FF ───────────────────────────────────
    uint8_t  readReg (uint16_t addr);
    void     writeReg(uint16_t addr, uint8_t data);

    // ── Доступ CPU к game-pak RAM (банки $70-$71 / $F0-... в LoROM SuperFX) ───
    uint8_t  readRam (uint32_t offset) const;
    void     writeRam(uint32_t offset, uint8_t data);
    uint32_t ramSize() const { return (uint32_t)ram_.size(); }

    // ── Доступ CPU к ROM, когда GSU им владеет (для маппинга шины) ────────────
    bool     cpuOwnsRom() const { return (sfr_ & SFR_GO) == 0; }
    bool     cpuOwnsRam() const { return true; } // упрощённо: RAM всегда доступна CPU

    // ── Исполнение ────────────────────────────────────────────────────────────
    // Выполнить до n тактов GSU (если запущен). Возвращает true пока работает.
    void run(int cycles);
    bool running() const { return (sfr_ & SFR_GO) != 0; }

    // IRQ к главному CPU (SFR бит 15 / при STOP)
    bool irqPending() const { return irq_; }
    void clearIrq() { irq_ = false; }

    // Для отладки
    uint16_t dbgR(int i) const { return r_[i & 15]; }
    uint16_t dbgSFR() const { return sfr_; }

private:
    // ── Регистры ───────────────────────────────────────────────────────────────
    uint16_t r_[16]{};          // R0-R15 (R15 = PC)
    uint16_t sfr_   = 0;        // Status/Flag Register
    uint8_t  pbr_   = 0;        // Program Bank Register
    uint8_t  rombr_ = 0;        // ROM Bank Register
    uint8_t  rambr_ = 0;        // RAM Bank Register
    uint16_t cbr_   = 0;        // Cache Base Register
    uint8_t  scbr_  = 0;        // Screen Base Register
    uint8_t  scmr_  = 0;        // Screen Mode Register
    uint8_t  colr_  = 0;        // COLour Register (для PLOT)
    uint8_t  por_   = 0;        // Plot Option Register
    uint8_t  bramr_ = 0;        // Backup RAM enable
    uint8_t  cfgr_  = 0;        // Config
    uint8_t  clsr_  = 0;        // Clock select (0=10.7MHz,1=21.4MHz)
    uint8_t  vcr_   = 0x04;     // Version

    // Префиксы/состояние конвейера
    uint8_t  sreg_ = 0;         // Source register index (от FROM)
    uint8_t  dreg_ = 0;         // Dest register index (от TO)
    uint8_t  alt_  = 0;         // ALT1/ALT2 prefix state (биты 0,1)
    bool     b_    = false;     // WITH/B prefix (sreg==dreg)
    uint16_t romBuffer_ = 0;    // буфер ROM (LDB/GETB)
    uint8_t  romBufByte_ = 0;
    uint8_t  ramBuffer_[2]{};   // буфер RAM
    bool     irq_  = false;

    // Кэш-RAM (512 байт) для исполнения из кэша
    std::array<uint8_t, 512> cache_{};
    std::array<bool, 32>     cacheValid_{};

    // Пиксельный кэш PLOT (8 пикселей одной колонки)
    uint8_t  pixCache_[8]{};
    uint8_t  pixValid_ = 0;     // битовая маска валидных пикселей
    uint16_t pixX_ = 0xFFFF, pixY_ = 0xFFFF;  // позиция текущего кэша

    // Память
    const std::vector<uint8_t>* rom_ = nullptr;
    std::vector<uint8_t>        ram_;

    // ── SFR флаги ───────────────────────────────────────────────────────────────
    enum : uint16_t {
        SFR_Z   = 0x0002,  // Zero
        SFR_CY  = 0x0004,  // Carry
        SFR_S   = 0x0008,  // Sign
        SFR_OV  = 0x0010,  // Overflow
        SFR_GO  = 0x0020,  // GSU работает
        SFR_R   = 0x0040,  // ROM read in progress
        SFR_ALT1= 0x0100,
        SFR_ALT2= 0x0200,
        SFR_IL  = 0x0400,  // immediate lower
        SFR_IH  = 0x0800,  // immediate upper
        SFR_B   = 0x1000,  // WITH/B prefix
        SFR_IRQ = 0x8000,
    };

    // ── Внутреннее ───────────────────────────────────────────────────────────
    uint8_t  fetchOpcode();           // чтение байта кода по PBR:R15++
    uint8_t  readCode(uint32_t pbrPC);
    uint8_t  romReadByte(uint32_t addr) const;

    uint16_t& src();                  // регистр-источник
    uint16_t& dst();                  // регистр-приёмник
    void      writeDst(uint16_t v);   // запись в dst + сброс префиксов
    void      setZS16(uint16_t v);
    void      resetPrefix();          // после исполнения опкода

    void      step();                 // один опкод
    void      plot();                 // PLOT
    uint8_t   rpix();                 // RPIX
    void      flushPixCache();

    uint8_t   ramReadByte(uint32_t addr) const;
    void      ramWriteByte(uint32_t addr, uint8_t v);
};
