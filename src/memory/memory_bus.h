#pragma once
#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include "mapper/mapper.h"

class CPU;
class PPU;
class APU;

class MemoryBus {
public:
    virtual ~MemoryBus() = default;

    virtual uint8_t read(uint16_t addr, bool readOnly = false);
    virtual void    write(uint16_t addr, uint8_t data);

    bool loadROM(const std::string& path);
    void reset();

    // Интерфейс для PPU (CHR ROM/RAM)
    uint8_t readCHR (uint16_t addr);
    void    writeCHR(uint16_t addr, uint8_t data);

    void connectCPU(CPU* cpu);
    void connectPPU(PPU* ppu);
    void connectAPU(APU* apu);

    // Маппер: IRQ-хуки (scanline = MMC3, cpuClock = FME-7 и др.)
    void mapperScanline();
    void mapperCpuClock();
    bool mapperIrqPending() const;
    void mapperClearIrq();

    uint8_t controller[2] = {0, 0};

    // Доступ к RAM для save state
          std::array<uint8_t, 2048>& getRAM()       { return ram_; }
    const std::array<uint8_t, 2048>& getRAM() const { return ram_; }
    void setRAM(const std::array<uint8_t, 2048>& r) { ram_ = r; }

    // Battery-backed SRAM ($6000–$7FFF)
    bool hasBattery()    const { return hasBattery_; }
    bool isSramDirty()   const { return sramDirty_;  }
    void clearSramDirty()      { sramDirty_ = false; }
    bool saveSram(const std::string& path) const;
    bool loadSram(const std::string& path);

    // Прямой доступ к буферу PRG RAM маппера (для save state v3)
    uint8_t*       mapperPrgRamPtr();
    const uint8_t* mapperPrgRamPtr() const;
    size_t         mapperPrgRamBytes() const;

protected:
    CPU* cpu_ = nullptr;
    PPU* ppu_ = nullptr;
    APU* apu_ = nullptr;

    std::array<uint8_t, 2048> ram_ = {};

    uint8_t controllerShift_[2] = {0, 0};
    uint8_t controllerLatch_    = 0;

    std::unique_ptr<Mapper> mapper_;
    uint8_t lastMirror_ = 0xFF;  // кэш для обнаружения смены зеркалирования
    bool    hasBattery_ = false;  // iNES header[6] бит 1 = battery-backed SRAM
    bool    sramDirty_  = false;  // флаг записи в battery SRAM после последнего сброса

private:
    void syncMirrorMode();  // обновляет PPU если mapper сменил режим
};
