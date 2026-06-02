#pragma once
#include <cstdint>
#include <array>
#include <vector>
#include <string>
#include "superfx.h"

class SnesPPU;
class SnesAPU;

// ─── SNES шина памяти ────────────────────────────────────────────────────────
// 24-битное адресное пространство: банк $00–$FF × $0000–$FFFF.
// Поддерживает LoROM, HiROM, ExHiROM.
class SnesBus {
public:
    enum class MapMode : uint8_t { Unknown, LoROM, HiROM, ExHiROM };

    virtual ~SnesBus() = default;

    // Загрузка ROM из файла; возвращает false при ошибке
    bool loadROM(const std::string& path);
    void reset();

    // Чтение/запись (виртуальные — мок может переопределить в тестах)
    virtual uint8_t read (uint32_t addr);
    virtual void    write(uint32_t addr, uint8_t data);

    // Подключение PPU и APU для перенаправления регистров
    void connectPPU(SnesPPU* ppu) { ppu_ = ppu; }
    void connectAPU(SnesAPU* apu) { apu_ = apu; }

    // Battery SRAM
    bool hasBattery()    const { return hasBattery_;  }
    bool isSramDirty()   const { return sramDirty_;   }
    void clearSramDirty()      { sramDirty_ = false;  }
    bool saveSram(const std::string& path) const;
    bool loadSram(const std::string& path);

    // Прямой доступ для SnesConsole (save state / DMA)
    uint8_t* wram()                { return wram_.data(); }
    const uint8_t* wram()    const { return wram_.data(); }
    static constexpr size_t WRAM_SIZE = 131072;  // 128 KB

    // Тип маппинга
    MapMode mapMode() const { return mapMode_; }

    // Контроллеры (опрашиваются SnesConsole)
    uint16_t controller[2] = {0, 0};

    // Открытая шина (заглушка для неизвестных адресов)
    uint8_t openBus_ = 0;

    // Для тестов — прямая инъекция ROM (публично, используется в юнит-тестах)
    void loadROMDirect(std::vector<uint8_t> data, MapMode mode, bool battery = false);

    // DMA: вызывается из SnesConsole после CPU-шага (H-DMA) и по $420B (G-DMA)
    void runHDMA();      // вызывать раз в сканлайн (в начале HBlank)
    void resetHDMA();    // реинициализировать HDMA в начале каждого VBlank

    // ── Управление NMI/VBlank (вызывается из SnesConsole) ────────────────────
    void setNmiFlag(bool v)       { nmiFlag_ = v; }
    void setVBlankActive(bool v)  { vblankActive_ = v; }
    bool nmiEnabled() const       { return (nmitimen_ & 0x80) != 0; }
    // IRQ-управление
    uint8_t irqMode() const       { return (uint8_t)((nmitimen_ >> 4) & 3); } // bits 5:4 (H/V enable)
    uint16_t hTarget() const      { return hTarget_; }
    uint16_t vTarget() const      { return vTarget_; }
    bool consumeIrq() {
        bool p = irqPending_;
        // флаг IRQ автоматически висит до чтения $4211
        return p;
    }
    void raiseIrq()               { irqPending_ = true; }
    // Авто-опрос джойпада ($4218/$421A): вызывается в начале VBlank
    void latchAutoJoy() {
        autoJoy_[0] = controller[0];
        autoJoy_[1] = controller[1];
    }

private:
    std::array<uint8_t, WRAM_SIZE> wram_{};  // $7E0000–$7FFFFF
    std::vector<uint8_t> rom_;
    std::vector<uint8_t> sram_;   // battery-backed SRAM
    MapMode  mapMode_    = MapMode::Unknown;
    bool     hasBattery_ = false;
    bool     sramDirty_  = false;

    // ─── SuperFX / GSU ────────────────────────────────────────────────────────
    bool     hasSuperFX_ = false;
    SuperFX  gsu_;

    SnesPPU* ppu_ = nullptr;
    SnesAPU* apu_ = nullptr;

public:
    // Доступ для SnesConsole: прогон GSU и наличие чипа
    bool     hasSuperFX() const { return hasSuperFX_; }
    void     runSuperFX(int cycles) { if (hasSuperFX_) gsu_.run(cycles); }
private:

    // Контроллер: регистры $4016/$4017 (16-бит для полного SNES-геймпада)
    uint16_t ctrlShift_[2]  = {0, 0};
    uint8_t  ctrlStrobe_    = 0;

    // Авто-опрос джойпада $4218–$421B
    uint16_t autoJoy_[2] = {0, 0};

    // Детект маппинга из заголовка ROM
    static MapMode detectMapMode(const std::vector<uint8_t>& rom);
    static bool    verifyHeader (const std::vector<uint8_t>& rom, uint32_t base);

    // Диспетчеры для каждого типа карточки
    uint8_t readLoROM (uint8_t bank, uint16_t addr);
    uint8_t readHiROM (uint8_t bank, uint16_t addr);
    uint8_t readExHiROM(uint8_t bank, uint16_t addr);

    void    writeLoROM (uint8_t bank, uint16_t addr, uint8_t data);
    void    writeHiROM (uint8_t bank, uint16_t addr, uint8_t data);
    void    writeExHiROM(uint8_t bank, uint16_t addr, uint8_t data);

    // Регистры контроллера/CPU
    uint8_t readIO (uint16_t addr);
    void    writeIO(uint16_t addr, uint8_t data);

    // ─── CPU/NMI/IRQ I/O ─────────────────────────────────────────────────────
    uint8_t  nmitimen_      = 0;    // $4200: NMI/IRQ/joypad enable
    bool     nmiFlag_       = false; // $4210: NMI-флаг (устанавливается при VBlank)
    bool     vblankActive_  = false; // $4212: признак VBlank
    uint32_t wramPort_      = 0;    // $2181–$2183: адрес WRAM-порта

    // IRQ по H/V-счётчику ($4207–$420A target, $4211 — флаг)
    uint16_t hTarget_       = 0x1FF;
    uint16_t vTarget_       = 0x1FF;
    bool     irqPending_    = false;

    // ─── Аппаратное умножение/деление ($4202–$4217) ───────────────────────────
    uint8_t  wrmpya_ = 0xFF;   // $4202: множитель A
    uint16_t wrdiv_  = 0xFFFF; // $4204:$4205: делимое
    uint16_t rddiv_  = 0;      // $4214:$4215: частное
    uint16_t rdmpy_  = 0;      // $4216:$4217: произведение / остаток

    // ─── DMA ──────────────────────────────────────────────────────────────────
    struct DmaChannel {
        uint8_t  dmap   = 0;     // $43x0: direction, mode
        uint8_t  bbad   = 0;     // $43x1: B-bus адрес (регистр PPU/APU)
        uint16_t a1t    = 0;     // $43x2–$43x3: A-bus адрес
        uint8_t  a1b    = 0;     // $43x4: A-bus банк
        uint16_t das    = 0;     // $43x5–$43x6: размер/счётчик
        uint8_t  dasb   = 0;     // $43x7: косвенный банк (HDMA)
        uint16_t a2a    = 0;     // $43x8–$43x9: HDMA таблица
        uint8_t  ntrl   = 0;     // $43xA: HDMA line counter
        bool     hdmaFinished = false;
    };
    DmaChannel  dma_[8];
    uint8_t     mdmaen_ = 0;     // $420B: маска активных GPDMA-каналов
    uint8_t     hdmaen_ = 0;     // $420C: маска активных HDMA-каналов
    bool        hdmaInit_ = false;
    uint8_t     memsel_ = 0;     // $420D bit 0: 0=SlowROM, 1=FastROM (хранение для readback)

    void runGDMA (uint8_t channels);
    void execGDMACh(int ch);
    void initHDMA ();
    void execHDMACh(int ch);
};
