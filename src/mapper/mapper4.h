#pragma once
#include "mapper.h"
#include <vector>
#include <cstdint>

// MMC3 (iNES mapper 4)
// Поддержка: 8KB PRG банки (4 слота), 1KB/2KB CHR банки, счётчик IRQ
class Mapper4 : public Mapper {
public:
    Mapper4(std::vector<uint8_t> prg,
            std::vector<uint8_t> chr,
            bool                 chrIsRam,
            uint8_t              mirrorBit);

    uint8_t prgRead (uint32_t addr)              override;
    void    prgWrite(uint32_t addr, uint8_t data) override;
    uint8_t chrRead (uint16_t addr)              override;
    void    chrWrite(uint16_t addr, uint8_t data) override;

    bool    hasPrgRam()                            const override { return true; }
    uint8_t prgRamRead (uint16_t addr)             const override;
    void    prgRamWrite(uint16_t addr, uint8_t data)     override;

    uint8_t*       prgRamPtr()       override { return prgRam_; }
    const uint8_t* prgRamPtr() const override { return prgRam_; }
    size_t         prgRamBytes() const override { return sizeof(prgRam_); }

    uint8_t mirrorMode() const override { return mirror_; }

    // IRQ: вызвать из PPU на dot 260 видимых скэнлайнов
    void    scanline()         override;
    bool    irqPending() const override { return irqPending_; }
    void    clearIRQ()         override { irqPending_ = false; }

private:
    std::vector<uint8_t> prg_;
    std::vector<uint8_t> chr_;
    uint8_t              prgRam_[8192] = {};
    bool                 chrIsRam_;
    uint8_t              mirror_ = 0;

    // Регистры R0–R7 (CHR×6 + PRG×2)
    uint8_t  reg_[8]     = {};
    uint8_t  bankSel_    = 0;   // $8000: выбор банка + режим CHR/PRG

    // IRQ
    uint8_t  irqLatch_   = 0;
    uint8_t  irqCounter_ = 0;
    bool     irqEnable_  = false;
    bool     irqPending_ = false;
    bool     irqReload_  = false;

    uint32_t chrBankAddr(uint16_t addr) const;
};
