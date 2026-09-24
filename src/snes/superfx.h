#pragma once
#include <cstdint>
#include <vector>
#include <array>

// ─── SuperFX / GSU (Graphics Support Unit) ───────────────────────────────────
// Копроцессор Nintendo для Star Fox, Stunt Race FX, Yoshi's Island и др.
// RISC-ядро с 16 регистрами R0-R15, своим доступом к ПЗУ/ОЗУ картриджа и
// аппаратным плоттером пикселей в тайловый формат SNES.
//
// Модель повторяет железо:
//  • однобайтовый КОНВЕЙЕР: пока исполняется инструкция, следующий байт уже
//    выбран. Поэтому инструкция сразу после перехода (delay slot) исполняется
//    ДО прыжка — на этом построен весь код GSU;
//  • кэш команд 512 байт от CBR (его же CPU видит в $3100-$32FF);
//  • буфер чтения ПЗУ: запись в R14 запускает чтение байта ROMBR:R14,
//    а GETB/GETC его забирают; отложенная запись в ОЗУ;
//  • плоттер PLOT/RPIX с двумя 8-пиксельными кэшами строк;
//  • пока GSU работает, он владеет шиной ПЗУ/ОЗУ (SCMR.RON/RAN): CPU вместо
//    ПЗУ читает таблицу векторов в WRAM, а вместо ОЗУ — открытую шину.
//
// Время считается в мастер-тактах SNES (21.477 МГц): байт из кэша стоит 2 такта
// (1 при CLSR=1), байт из ПЗУ/ОЗУ — 6 (5).
class SuperFX {
public:
    // ── Подключение ──────────────────────────────────────────────────────────
    // rom — ПЗУ картриджа; ramBytes — размер game-pak RAM (округляется до 2^n).
    void connect(const std::vector<uint8_t>* rom, uint32_t ramBytes);
    void reset();

    // ── Доступ CPU к регистрам $3000-$34FF ────────────────────────────────────
    uint8_t readIO(uint16_t addr);
    void    writeIO(uint16_t addr, uint8_t data);

    // ── Доступ CPU к ОЗУ картриджа (смещение маскируется размером ОЗУ) ───────
    uint8_t  readRam(uint32_t offset) const { return ram_[offset & ramMask_]; }
    void     writeRam(uint32_t offset, uint8_t data) { ram_[offset & ramMask_] = data; }
    uint32_t ramSize() const { return (uint32_t)ram_.size(); }

    // ── Кто владеет шиной картриджа ──────────────────────────────────────────
    // Пока GSU работает и держит шину, CPU не видит ни ПЗУ, ни ОЗУ.
    bool romLocked() const { return (sfr_ & SFR_G) && (scmr_ & SCMR_RON); }
    bool ramLocked() const { return (sfr_ & SFR_G) && (scmr_ & SCMR_RAN); }
    // Что CPU читает вместо ПЗУ, пока оно занято: векторы прерываний
    // указывают в WRAM ($0100-$010F), туда игры кладут обработчики.
    static uint8_t lockedRomByte(uint32_t addr);

    // ── Исполнение ────────────────────────────────────────────────────────────
    // Дать чипу masterClocks мастер-тактов времени (долг переносится дальше).
    void run(uint32_t masterClocks);
    bool running() const { return (sfr_ & SFR_G) != 0; }
    // Линия IRQ к CPU: взводится STOP'ом (если CFGR не маскирует), снимается
    // чтением $3031.
    bool irqLine() const { return (sfr_ & SFR_IRQ) != 0; }

    // ── Отладка/тесты ────────────────────────────────────────────────────────
    uint16_t dbgR(int i) const { return r_[i & 15]; }
    uint16_t dbgSFR() const { return sfr_; }
    uint8_t  dbgPBR() const { return pbr_; }
    uint64_t dbgInstructions() const { return instructions_; }

    // ── Флаги SFR ─────────────────────────────────────────────────────────────
    enum : uint16_t {
        SFR_Z    = 0x0002,  // ноль
        SFR_CY   = 0x0004,  // перенос
        SFR_S    = 0x0008,  // знак
        SFR_OV   = 0x0010,  // переполнение
        SFR_G    = 0x0020,  // GO: чип работает
        SFR_R    = 0x0040,  // идёт чтение ПЗУ по R14
        SFR_ALT1 = 0x0100,
        SFR_ALT2 = 0x0200,
        SFR_IL   = 0x0400,
        SFR_IH   = 0x0800,
        SFR_B    = 0x1000,  // префикс WITH
        SFR_IRQ  = 0x8000,
    };
    enum : uint8_t {
        SCMR_RAN = 0x08,    // ОЗУ отдано GSU
        SCMR_RON = 0x10,    // ПЗУ отдано GSU
    };

private:
    // ── Регистры ─────────────────────────────────────────────────────────────
    uint16_t r_[16]{};      // R0-R15 (R15 — счётчик команд)
    uint16_t sfr_   = 0;    // флаги/состояние
    uint8_t  pbr_   = 0;    // банк программы
    uint8_t  rombr_ = 0;    // банк данных ПЗУ
    uint8_t  rambr_ = 0;    // банк данных ОЗУ (0/1)
    uint16_t cbr_   = 0;    // база кэша команд
    uint8_t  scbr_  = 0;    // база экрана (в КБ)
    uint8_t  scmr_  = 0;    // режим экрана: глубина цвета, высота, RON/RAN
    uint8_t  colr_  = 0;    // текущий цвет PLOT
    uint8_t  por_   = 0;    // опции PLOT (CMODE)
    uint8_t  bramr_ = 0;
    uint8_t  vcr_   = 0x04; // версия чипа
    uint8_t  cfgr_  = 0;    // бит 7 — маска IRQ, бит 5 — быстрое умножение
    uint8_t  clsr_  = 0;    // 0 = 10.7 МГц, 1 = 21.4 МГц

    // ── Состояние конвейера и префиксов ──────────────────────────────────────
    uint8_t  pipeline_ = 0x01;  // уже выбранный байт следующей команды
    uint8_t  sreg_ = 0, dreg_ = 0;  // FROM / TO
    bool     r14Mod_ = false;   // R14 переписан → запустить чтение ПЗУ
    bool     r15Mod_ = false;   // R15 переписан → не инкрементировать
    uint16_t ramaddr_ = 0;      // последний адрес ОЗУ (для SBK)

    // ── Буферы ПЗУ/ОЗУ ───────────────────────────────────────────────────────
    uint32_t romcl_ = 0;        // тактов до готовности чтения ПЗУ
    uint8_t  romdr_ = 0;        // прочитанный байт ПЗУ
    uint32_t ramcl_ = 0;        // тактов до завершения записи в ОЗУ
    uint16_t ramar_ = 0;
    uint8_t  ramdr_ = 0;

    // ── Кэш команд ───────────────────────────────────────────────────────────
    std::array<uint8_t, 512> cache_{};
    std::array<bool, 32>     cacheValid_{};

    // ── Пиксельные кэши плоттера (8 пикселей строки тайла) ───────────────────
    struct PixelCache {
        uint16_t offset  = 0xFFFF;  // (y << 5) + (x >> 3)
        uint8_t  bitpend = 0;       // какие из 8 пикселей заданы
        uint8_t  data[8]{};
    };
    PixelCache pixcache_[2];

    // ── Память ────────────────────────────────────────────────────────────────
    const std::vector<uint8_t>* rom_ = nullptr;
    std::vector<uint8_t> ram_ = std::vector<uint8_t>(1, 0);
    uint32_t ramMask_ = 0;

    // ── Время ─────────────────────────────────────────────────────────────────
    int64_t  budget_ = 0;           // сколько мастер-тактов ещё можно потратить
    uint32_t clocks_ = 0;           // потрачено текущей инструкцией
    uint64_t instructions_ = 0;

    // ── Внутреннее ───────────────────────────────────────────────────────────
    uint8_t  busRead(uint32_t addr) const;
    void     busWrite(uint32_t addr, uint8_t data);
    uint8_t  romByte(uint32_t offset) const;
    void     tick(uint32_t clocks);

    uint8_t  readOpcode(uint16_t addr);
    uint8_t  pipe();
    void     flushCache() { cacheValid_.fill(false); }

    void     updateRomBuffer();
    void     syncRomBuffer()  { if (romcl_) tick(romcl_); }
    uint8_t  readRomBuffer()  { syncRomBuffer(); return romdr_; }
    void     syncRamBuffer()  { if (ramcl_) tick(ramcl_); }
    uint8_t  readRamBuffer(uint16_t addr);
    void     writeRamBuffer(uint16_t addr, uint8_t data);

    uint16_t sr() const { return r_[sreg_]; }
    void     setR(int n, uint16_t v);
    void     setDr(uint16_t v) { setR(dreg_, v); }
    void     setFlag(uint16_t f, bool on) { if (on) sfr_ |= f; else sfr_ &= (uint16_t)~f; }
    void     setSZ(uint16_t v) { setFlag(SFR_S, (v & 0x8000) != 0); setFlag(SFR_Z, v == 0); }
    bool     alt1() const { return (sfr_ & SFR_ALT1) != 0; }
    bool     alt2() const { return (sfr_ & SFR_ALT2) != 0; }
    void     resetPrefix();

    void     step();
    void     execute(uint8_t op);
    void     branch(bool take);

    uint8_t  color(uint8_t source) const;
    uint32_t charAddress(uint8_t x, uint8_t y) const;
    uint32_t bitsPerPixel() const;
    void     plot(uint8_t x, uint8_t y);
    uint8_t  rpix(uint8_t x, uint8_t y);
    void     flushPixelCache(PixelCache& cache);
};
