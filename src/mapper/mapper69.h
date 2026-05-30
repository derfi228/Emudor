#pragma once
#include "mapper.h"
#include <vector>
#include <cstdint>

// Sunsoft FME-7 / Sunsoft 5B (iNES mapper 69)
// Используется в Batman: Return of the Joker, Hebereke и др.
//
// PRG-расклад:
//   $6000-$7FFF  — команда $08: PRG ROM банк или PRG RAM (бит 6 = RAM)
//   $8000-$9FFF  — команда $09: переключаемый PRG ROM банк
//   $A000-$BFFF  — команда $0A: переключаемый PRG ROM банк
//   $C000-$DFFF  — команда $0B: переключаемый PRG ROM банк
//   $E000-$FFFF  — ФИКСИРОВАНО = последний 8KB банк (всегда)
//
// CHR: 8 × 1KB слотов (команды $00-$07).
// IRQ: 16-bit countdown на CPU-тактах (команды $0D-$0F).
class Mapper69 : public Mapper {
public:
    Mapper69(std::vector<uint8_t> prg,
             std::vector<uint8_t> chr,
             bool                 chrIsRam,
             uint8_t              mirrorBit);

    uint8_t prgRead (uint32_t addr)               override;
    void    prgWrite(uint32_t addr, uint8_t data)  override;
    uint8_t chrRead (uint16_t addr)               override;
    void    chrWrite(uint16_t addr, uint8_t data)  override;

    bool    hasPrgRam()                            const override { return true; }
    uint8_t prgRamRead (uint16_t addr)             const override;
    void    prgRamWrite(uint16_t addr, uint8_t data)     override;

    uint8_t*       prgRamPtr()       override { return prgRam_; }
    const uint8_t* prgRamPtr() const override { return prgRam_; }
    size_t         prgRamBytes() const override { return sizeof(prgRam_); }

    uint8_t mirrorMode() const override { return mirror_; }

    // IRQ по CPU-циклам
    void    cpuClock()         override;
    bool    irqPending() const override { return irqPending_; }
    void    clearIRQ()         override { irqPending_ = false; }

private:
    std::vector<uint8_t> prg_;
    std::vector<uint8_t> chr_;
    uint8_t              prgRam_[8192] = {};
    bool                 chrIsRam_;
    uint8_t              mirror_ = 0;

    uint8_t  cmdReg_        = 0;    // текущая команда ($8000)
    uint8_t  chrBank_[8]    = {};   // команды 0–7

    // PRG банки: индексы соответствуют диапазонам
    //   [0] = $8000-$9FFF (команда $09)
    //   [1] = $A000-$BFFF (команда $0A)
    //   [2] = $C000-$DFFF (команда $0B)
    //   [3] = $6000-$7FFF (команда $08, может быть RAM)
    uint8_t  prgBank_[4]    = {};
    bool     slot0Ram_      = false;// команда $08 бит6: слот $6000-$7FFF = RAM

    // IRQ (команды $D–$F)
    bool     irqCountEn_    = false;// команда $D бит7
    bool     irqEnable_     = false;// команда $D бит0
    uint16_t irqCounter_    = 0;
    bool     irqPending_    = false;
};
