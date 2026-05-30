#pragma once
#include <cstdint>
#include <array>

class MemoryBus;

class PPU {
public:
    // Режим зеркалирования nametable (iNES header[6])
    enum class MirrorMode : uint8_t {
        Horizontal = 0,  // NT0 вверху, NT1 внизу
        Vertical   = 1,  // NT0 слева, NT1 справа
        SingleLo   = 2,  // вся экранная область → NT0
        SingleHi   = 3,  // вся экранная область → NT1
        FourScreen = 4   // 4 независимых NT (нужен дополнительный VRAM)
    };

    bool nmiPending    = false;
    bool frameComplete = false;

    std::array<uint32_t, 256 * 240> screenBuffer = {};

    PPU();
    void connectBus(MemoryBus* bus);

    void    reset();
    void    clock();
    void    setMirrorMode(MirrorMode mode);

    uint8_t readRegister (uint8_t reg);
    void    writeRegister(uint8_t reg, uint8_t data);
    void    writeOAMByte (uint8_t index, uint8_t data);

    int scanline() const { return scanline_; }
    int dot()      const { return dot_;      }

    // Доступ к состоянию для save state
    struct State {
        uint16_t vramAddr, tramAddr;
        uint8_t  fineX;
        uint8_t  ppuCtrl, ppuMask, ppuStatus;
        uint8_t  oamAddr;
        uint8_t  ppuDataBuf;
        bool     addressLatch;
        uint8_t  bgNextNtByte, bgNextAtByte, bgNextPtLo, bgNextPtHi;
        uint16_t bgShiftPatLo, bgShiftPatHi;
        uint16_t bgShiftAttrLo, bgShiftAttrHi;
        int      scanline, dot;
        uint8_t  mirrorMode;
        std::array<uint32_t, 256*240> screenBuffer;
        std::array<uint8_t, 2048>     nameTables;
        std::array<uint8_t, 32>       palette;
        std::array<uint8_t, 256>      oam;
    };
    State getState() const;
    void  setState(const State& s);

private:
    MemoryBus* bus_ = nullptr;

    // Loopy-регистры: единый 15-битный формат
    union LoopyReg {
        struct {
            uint16_t coarseX   : 5;
            uint16_t coarseY   : 5;
            uint16_t nametable : 2;
            uint16_t fineY     : 3;
        };
        uint16_t reg = 0;
    };

    LoopyReg vram_;
    LoopyReg tram_;
    uint8_t  fineX_    = 0;
    bool     addrLatch_ = false;

    union PPUCTRL {
        struct {
            uint8_t nametable   : 2;
            uint8_t increment   : 1;
            uint8_t spriteTable : 1;
            uint8_t bgTable     : 1;
            uint8_t spriteSize  : 1;
            uint8_t masterSlave : 1;
            uint8_t nmiEnable   : 1;
        };
        uint8_t reg = 0;
    } ctrl_;

    union PPUMASK {
        struct {
            uint8_t grayscale   : 1;
            uint8_t showBgLeft  : 1;
            uint8_t showSpLeft  : 1;
            uint8_t showBg      : 1;
            uint8_t showSprites : 1;
            uint8_t emphRed     : 1;
            uint8_t emphGreen   : 1;
            uint8_t emphBlue    : 1;
        };
        uint8_t reg = 0;
    } mask_;

    union PPUSTATUS {
        struct {
            uint8_t unused         : 5;
            uint8_t spriteOverflow : 1;
            uint8_t sprite0Hit     : 1;
            uint8_t vblank         : 1;
        };
        uint8_t reg = 0;
    } status_;

    uint8_t oamAddr_    = 0;
    uint8_t ppuDataBuf_ = 0;

    // Внутренняя VRAM: 2 nametable + 32 байта палитры + OAM
    std::array<uint8_t, 2048> nameTables_ = {};
    std::array<uint8_t, 32>   palette_    = {};
    std::array<uint8_t, 256>  oam_        = {};

    // BG pipeline
    uint8_t  bgNextNtByte_  = 0;
    uint8_t  bgNextAtByte_  = 0;
    uint8_t  bgNextPtLo_    = 0;
    uint8_t  bgNextPtHi_    = 0;

    uint16_t bgShiftPatLo_  = 0;
    uint16_t bgShiftPatHi_  = 0;
    uint16_t bgShiftAttrLo_ = 0;
    uint16_t bgShiftAttrHi_ = 0;

    int scanline_ = 0;
    int dot_      = 0;

    MirrorMode mirrorMode_ = MirrorMode::Horizontal;

    // Буфер спрайтов для текущего скэнлайна (не более 8)
    struct SpriteBuf {
        uint8_t x;
        uint8_t attr;
        uint8_t patLo;
        uint8_t patHi;
        bool    isSprite0;
        bool    active;
    };
    SpriteBuf spriteBuf_[8] = {};
    int       spriteCount_  = 0;

    // NTSC палитра 64 цвета (ARGB8888)
    static const std::array<uint32_t, 64> kPalette;

    // VRAM helpers
    uint16_t mirrorNametableAddr(uint16_t addr) const;
    uint8_t  readVRAM (uint16_t addr) const;
    void     writeVRAM(uint16_t addr, uint8_t data);
    uint32_t getColor(uint8_t paletteIdx, uint8_t pixel) const;

    // Спрайты
    void evaluateSprites();

    // BG pipeline helpers
    void loadBgShifters();
    void shiftBgRegisters();
    void incrementScrollX();
    void incrementScrollY();
    void transferAddressX();
    void transferAddressY();
};
