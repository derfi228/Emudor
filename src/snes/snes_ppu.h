#pragma once
#include <cstdint>
#include <array>

class SnesBus;

// ─── SNES PPU (Ricoh 5C77/5C78) ──────────────────────────────────────────────
// 256×224 (224 активных строк + режим 239 строк) + 8 пустых строк сверху
// BG-режимы 0–7, 128 спрайтов, цветовая математика.
class SnesPPU {
public:
    static constexpr int WIDTH  = 256;
    static constexpr int HEIGHT = 224;  // стандартные NTSC-строки (SNES без overscan)

    bool frameComplete = false;
    bool nmiPending    = false;

    void connectBus(SnesBus* bus) { bus_ = bus; }
    void reset();
    void clock();

    // Регистры PPU ($2100–$213F), вызывается из SnesBus
    void    writeReg(uint16_t addr, uint8_t data);
    uint8_t readReg (uint16_t addr);

    using Framebuffer = std::array<uint32_t, WIDTH * HEIGHT>;
    const Framebuffer& framebuffer() const { return fb_; }
          Framebuffer& framebuffer()       { return fb_; }

    struct State {
        uint16_t scanline;
        uint16_t dot;
        uint8_t  regs[0x40];
    };
    State getState()           const;
    void  setState(const State& s);

private:
    SnesBus* bus_ = nullptr;
    Framebuffer fb_{};

    // Регистры $2100–$213F
    uint8_t regs_[0x40]{};

    uint16_t scanline_ = 0;
    uint16_t dot_      = 0;

    // VRAM (64 KB = 32K слов)
    std::array<uint16_t, 0x8000> vram_{};
    // CGRAM (256 цветов BGR555)
    std::array<uint16_t, 256>    cgram_{};
    // OAM (512 основных байт + 32 байта расширения)
    std::array<uint8_t, 544>     oam_{};

    // Внутренние счётчики записи VRAM ($2115 VMAIN)
    uint16_t vramAddr_    = 0;
    bool     vramHiLatch_ = false;   // bit7 $2115: true=инкр. на $2119/$213A, false=на $2118/$2139
    uint8_t  vramStep_    = 1;       // биты 1:0 $2115: 1/32/128/128
    uint8_t  vramRemap_   = 0;       // биты 3:2 $2115: 0=нет, 1=8-bit, 2=9-bit, 3=10-bit
    uint8_t  vramPrefetchLo_ = 0;
    uint8_t  vramPrefetchHi_ = 0;

    uint16_t cgramAddr_   = 0;       // текущая позиция CGRAM (в half-words)
    bool     cgramHalf_   = false;   // false=lo, true=hi

    uint16_t oamAddr_     = 0;
    bool     oamFirstWrite_= false;
    uint8_t  oamLatch_    = 0;

    // Scroll-регистры: используют 2-байтовую запись с общим вторым байтом
    uint8_t scrollLatch_ = 0;        // предыдущее значение M7/BG scroll
    uint16_t bgHscroll_[4]{};
    uint16_t bgVscroll_[4]{};

    // Регистры Mode 7
    int16_t  m7A_=0, m7B_=0, m7C_=0, m7D_=0;
    int16_t  m7X_=0, m7Y_=0;
    int16_t  m7HOFS_=0, m7VOFS_=0;  // $210D/$210E (Mode 7 13-битный скролл)
    uint8_t  m7Latch_ = 0;          // защёлка для 16-бит записи

    // $2132 COLDATA — фиксированный цвет для color math (15-битный BGR555)
    uint16_t coldata_ = 0;

    // ─── Рендеринг ────────────────────────────────────────────────────────────
    void renderScanline(int y);

    // Получить пиксель BG на позиции (screenX, scanlineY) с заданным числом bpp
    // Возвращает (colorIndex, priority, paletteBase); colorIndex=0 → прозрачно
    struct BgPixel { uint8_t color; bool priority; };

    BgPixel getBGPixel(int bgIdx, int screenX, int screenY) const;
    BgPixel getMode7Pixel(int screenX, int screenY) const;

    // Спрайты: возвращает цвет спрайта для данного X на текущем сканлайне
    // Возвращает (colorARGB, priority 0-3); color=0 → прозрачно
    struct ObjPixel { uint16_t color; uint8_t priority; };
    ObjPixel getObjPixel(int screenX, int screenY) const;

    // Вспомогательные методы
    int  bgBpp(int bgIdx) const;     // битов на пиксель для данного BG
    int  bgPaletteOffset(int bgIdx, uint8_t palNum) const;
    // Эффективный адрес VRAM с учётом ремаппинга $2115 bits 3:2
    uint16_t vramEffAddr() const;

    // Конвертация CGRAM-цвета BGR555 → RGBA8888
    static uint32_t cgToRGBA(uint16_t bgr555, uint8_t brightness);
    uint8_t         brightness() const { return (uint8_t)(regs_[0x00] & 0x0F); }
    bool            displayEnabled() const { return !(regs_[0x00] & 0x80); }
};
