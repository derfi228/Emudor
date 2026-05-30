#pragma once
#include "mapper.h"
#include <vector>
#include <cstdint>

// Mapper 0 — NROM (32KB или 16KB PRG, до 8KB CHR ROM/RAM)
// Используется: Donkey Kong, Super Mario Bros., Pac-Man и др.
class Mapper0 : public Mapper {
public:
    Mapper0(std::vector<uint8_t> prg, std::vector<uint8_t> chr,
            bool chrIsRam, uint8_t mirror)
        : prg_(std::move(prg)), chr_(std::move(chr)),
          chrIsRam_(chrIsRam), mirror_(mirror) {}

    uint8_t prgRead(uint32_t addr) override {
        uint32_t off = addr - 0x8000;
        if (prg_.size() <= 0x4000) off &= 0x3FFF;   // 16KB — зеркало
        else                        off &= 0x7FFF;   // 32KB
        return (off < prg_.size()) ? prg_[off] : 0;
    }
    void prgWrite(uint32_t, uint8_t) override {}     // ROM, запись игнорируется

    uint8_t chrRead(uint16_t addr) override {
        return (addr < (uint16_t)chr_.size()) ? chr_[addr] : 0;
    }
    void chrWrite(uint16_t addr, uint8_t data) override {
        if (chrIsRam_ && addr < (uint16_t)chr_.size()) chr_[addr] = data;
    }

    uint8_t mirrorMode() const override { return mirror_; }

private:
    std::vector<uint8_t> prg_;
    std::vector<uint8_t> chr_;
    bool    chrIsRam_;
    uint8_t mirror_;  // 0=Horizontal, 1=Vertical
};
