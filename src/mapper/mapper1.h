#pragma once
#include "mapper.h"
#include <vector>
#include <array>
#include <cstdint>

// Mapper 1 — MMC1 / SxROM
// Используется: The Legend of Zelda, Mega Man 2, Metroid и др.
//
// Механизм записи: 5 последовательных записей в $8000–$FFFF
// (каждый раз бит 0 сдвигается в регистр), затем значение
// применяется к одному из четырёх регистров в зависимости от адреса.
class Mapper1 : public Mapper {
public:
    Mapper1(std::vector<uint8_t> prg, std::vector<uint8_t> chr,
            bool chrIsRam, uint8_t initMirror);

    uint8_t prgRead (uint32_t  addr) override;
    void    prgWrite(uint32_t  addr, uint8_t data) override;
    uint8_t chrRead (uint16_t  addr) override;
    void    chrWrite(uint16_t  addr, uint8_t data) override;

    bool    hasPrgRam()                       const override { return true; }
    uint8_t prgRamRead (uint16_t addr)        const override;
    void    prgRamWrite(uint16_t addr, uint8_t data) override;

    uint8_t*       prgRamPtr()       override { return prgRam_.data(); }
    const uint8_t* prgRamPtr() const override { return prgRam_.data(); }
    size_t         prgRamBytes() const override { return prgRam_.size(); }

    uint8_t mirrorMode() const override;

private:
    void shiftWrite(uint32_t addr, uint8_t data);
    void applyRegister(uint32_t addr, uint8_t value);

    std::vector<uint8_t>      prg_;
    std::vector<uint8_t>      chr_;
    std::array<uint8_t, 8192> prgRam_ = {};
    bool chrIsRam_;

    // MMC1 сдвиговый регистр (sentinel в bit4)
    uint8_t shiftReg_ = 0x10;

    // Четыре внутренних регистра
    // ctrlReg_ bits: [4]=CHR mode, [3:2]=PRG mode, [1:0]=mirror
    uint8_t ctrlReg_  = 0x1C;   // PRG mode=3 (fix last bank), CHR mode=0
    uint8_t chrBank0_ = 0;
    uint8_t chrBank1_ = 0;
    uint8_t prgBank_  = 0;

    uint8_t numPrg16_;  // число 16KB PRG банков
    uint8_t numChr4_;   // число 4KB CHR банков (0 = CHR RAM)
};
