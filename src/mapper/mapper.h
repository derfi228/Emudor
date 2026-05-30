#pragma once
#include <cstdint>

// Абстрактный маппер — роутинг PRG/CHR доступа
// mirrorMode() кодировка: 0=Horizontal, 1=Vertical, 2=SingleLo, 3=SingleHi
class Mapper {
public:
    virtual ~Mapper() = default;

    // PRG ROM — полный CPU-адрес ($8000–$FFFF)
    virtual uint8_t prgRead (uint32_t addr) = 0;
    virtual void    prgWrite(uint32_t addr, uint8_t data) = 0;

    // CHR ROM/RAM — адрес в PPU-пространстве ($0000–$1FFF)
    virtual uint8_t chrRead (uint16_t addr) = 0;
    virtual void    chrWrite(uint16_t addr, uint8_t data) = 0;

    // PRG RAM ($6000–$7FFF) — опционально (по умолчанию нет)
    virtual bool    hasPrgRam()                         const { return false; }
    virtual uint8_t prgRamRead (uint16_t /*addr*/)      const { return 0; }
    virtual void    prgRamWrite(uint16_t /*addr*/, uint8_t)   {}

    // Прямой доступ к буферу PRG RAM (для сохранения/загрузки battery SRAM)
    virtual uint8_t*       prgRamPtr()       { return nullptr; }
    virtual const uint8_t* prgRamPtr() const { return nullptr; }
    virtual size_t         prgRamBytes()     const { return 0; }

    // Зеркалирование nametable
    virtual uint8_t mirrorMode() const = 0;

    // IRQ (MMC3 и другие маперы со счётчиком скэнлайнов)
    virtual void    scanline()          {}   // PPU dot 260 каждого видимого скэнлайна
    // IRQ по CPU-циклам (Sunsoft FME-7 и др.)
    virtual void    cpuClock()          {}   // вызывается каждый CPU-такт
    virtual bool    irqPending()  const { return false; }
    virtual void    clearIRQ()          {}
};
