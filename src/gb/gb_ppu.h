#pragma once
#include <array>
#include <cstdint>

// ─── Видеочип Game Boy / Game Boy Color ──────────────────────────────────────
// Экран 160×144. Строка — 456 точек: режим 2 (поиск спрайтов, 80 точек),
// режим 3 (вывод, 172+ точек), режим 0 (HBlank). Строки 144-153 — VBlank.
// Фон и окно — тайлы 8×8 из двух карт 32×32; до 10 спрайтов на строку.
//
// Цветной режим (CGB): второй банк видеопамяти с атрибутами тайлов (палитра,
// банк, отражения, приоритет над спрайтами), по 8 палитр RGB555 для фона и
// для спрайтов.
//
// Строка рисуется целиком в конце режима 3: всё, что игра успела поменять
// в прерывании по LYC/HBlank до этого момента, попадает в кадр.
class GbPpu {
public:
    static constexpr int WIDTH  = 160;
    static constexpr int HEIGHT = 144;

    void reset(bool cgb);
    void connect(uint8_t* ifReg) { if_ = ifReg; }
    void tick(int dots);

    // ── Доступ процессора: видеопамять закрыта в режиме 3, OAM — в 2 и 3 ─────
    uint8_t cpuReadVram(uint16_t addr) const;
    void    cpuWriteVram(uint16_t addr, uint8_t v);
    uint8_t cpuReadOam(uint16_t addr) const;
    void    cpuWriteOam(uint16_t addr, uint8_t v);
    void    dmaWriteOam(uint8_t index, uint8_t v) { oam_[index] = v; }
    void    setDmaActive(bool on) { dmaActive_ = on; }
    void    hdmaWriteVram(uint16_t addr, uint8_t v) { vram_[vbk_][addr & 0x1FFF] = v; }
    uint8_t vramRaw(uint16_t addr) const { return vram_[vbk_][addr & 0x1FFF]; }   // для DMA

    uint8_t readReg(uint16_t addr) const;         // $FF40-$FF4B, $FF4F, $FF68-$FF6C
    void    writeReg(uint16_t addr, uint8_t v);

    // ── События для остальной системы ────────────────────────────────────────
    bool frameDone = false;                        // начался VBlank: кадр готов
    bool takeHblankEvent() { bool e = hblankEvent_; hblankEvent_ = false; return e; }
    bool lcdOn() const { return (lcdc_ & 0x80) != 0; }
    uint8_t mode() const { return mode_; }

    const uint32_t* framebuffer() const { return fb_.data(); }
    uint32_t*       framebuffer()       { return fb_.data(); }

    template<class S> void serialize(S& s);

private:
    uint8_t* if_ = nullptr;                        // регистр IF шины
    bool     cgb_ = false;

    std::array<std::array<uint8_t, 0x2000>, 2> vram_{};
    std::array<uint8_t, 160> oam_{};
    std::array<uint32_t, WIDTH * HEIGHT> fb_{};

    // Регистры
    uint8_t lcdc_ = 0x91, stat_ = 0x00, scy_ = 0, scx_ = 0, ly_ = 0, lyc_ = 0;
    uint8_t bgp_ = 0xFC, obp0_ = 0xFF, obp1_ = 0xFF, wy_ = 0, wx_ = 0;
    uint8_t vbk_ = 0;                              // CGB: банк видеопамяти
    uint8_t bcps_ = 0, ocps_ = 0;                  // CGB: индексы палитр
    uint8_t opri_ = 0;                             // CGB: 1 = приоритет спрайтов как у DMG
    std::array<uint8_t, 64> bgPal_{};
    std::array<uint8_t, 64> objPal_{};

    // Внутреннее состояние
    uint8_t  line_ = 0;                            // строка (LY на строке 153 раньше становится 0)
    uint16_t dot_ = 0;                             // точка в строке, 0..455
    uint16_t mode3End_ = 252;                      // где кончается режим 3 этой строки
    uint8_t  mode_ = 0;
    bool     lycEqual_ = false;
    bool     statLine_ = false;                    // общая линия STAT-прерывания
    bool     windowTriggered_ = false;             // LY совпал с WY в этом кадре
    uint8_t  windowLine_ = 0;                      // внутренний счётчик строк окна
    bool     hblankEvent_ = false;
    bool     dmaActive_ = false;

    // Спрайты текущей строки (индексы OAM), до 10 штук
    uint8_t  spriteCount_ = 0;
    uint8_t  sprites_[10]{};

    void     enterMode(uint8_t m);
    void     updateLyc();
    void     updateStatLine();
    void     startLine();
    void     oamScan();
    uint16_t mode3Length() const;
    void     renderLine();
    uint32_t dmgColor(uint8_t shade) const;
    uint32_t cgbColor(const std::array<uint8_t, 64>& pal, int palette, int index) const;
    void     requestIrq(int bit) { if (if_) *if_ |= (uint8_t)(1 << bit); }
};
