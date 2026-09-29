#pragma once
#include <cstdint>

class N64System;

// ─── RDP: растеризатор внутри RCP ─────────────────────────────────────────────
// Получает список 64-битных команд (из RDRAM или, в режиме XBUS, из DMEM) и
// рисует в кадровый буфер в RDRAM: треугольники (затенение Гуро, текстуры с
// перспективой, Z-буфер), прямоугольники, заливки. Текстуры грузятся в 4 КБ
// TMEM (с палитрами в верхней половине). Пиксель проходит комбайнер цвета
// ((A−B)·C+D, один или два такта), проверку альфы и глубины и блендер.
//
// Покрытие считается по 8 точкам на пиксель, как у железа: без сглаживания
// пиксель рисуется, если покрыт его левый верхний угол; со сглаживанием —
// при любом покрытии. Фильтры VI и скрытые биты покрытия RDRAM не моделируются.
class Rdp {
public:
    void connect(N64System* sys, uint8_t* rdram, uint32_t rdramSize, uint8_t* dmem)
    { sys_ = sys; rdram_ = rdram; rdramMask_ = rdramSize - 1; dmem_ = dmem; }
    void reset();

    // Выполнить команды из [start, end). xbus — список лежит в DMEM.
    void process(uint32_t start, uint32_t end, bool xbus);

    template<class S> void serialize(S& s);

private:
    N64System* sys_ = nullptr;
    uint8_t*   rdram_ = nullptr;
    uint32_t   rdramMask_ = 0;
    uint8_t*   dmem_ = nullptr;

    // Незавершённая команда (список может прерваться посреди неё)
    uint64_t cmd_[44]{};
    int      cmdLen_ = 0;

    // ── Состояние ─────────────────────────────────────────────────────────────
    struct Image { uint32_t addr = 0; uint8_t fmt = 0, size = 0; uint32_t width = 1; };
    Image    color_, tex_;
    uint32_t zAddr_ = 0;

    struct Tile {
        uint8_t  fmt = 0, size = 0, palette = 0;
        uint16_t line = 0, tmem = 0;              // в 64-битных словах
        bool     cs = false, ms = false, ct = false, mt = false;
        uint8_t  maskS = 0, shiftS = 0, maskT = 0, shiftT = 0;
        uint16_t sl = 0, tl = 0, sh = 0, th = 0;  // 10.2
    };
    Tile     tiles_[8];
    uint8_t  tmem_[4096]{};

    struct Modes {
        uint8_t cycle = 0;                        // 0 — 1 такт, 1 — 2 такта, 2 — копирование, 3 — заливка
        bool    persp = false, texLod = false, tlut = false, tlutIa = false, bilinear = false;
        uint8_t rgbDither = 0;
        uint8_t bP[2]{}, bA[2]{}, bM[2]{}, bB[2]{};   // входы блендера по тактам
        bool    forceBlend = false, alphaCvgSel = false, cvgTimesAlpha = false;
        uint8_t zMode = 0, cvgDest = 0;
        bool    imageRead = false, zUpdate = false, zCompare = false, aa = false;
        bool    zPrim = false, ditherAlpha = false, alphaCompare = false;
    };
    Modes    m_;
    uint64_t otherModes_ = 0, combineMode_ = 0;

    int32_t  fillColor_ = 0;
    int32_t  fog_[4]{}, blend_[4]{}, prim_[4]{}, env_[4]{};
    int32_t  primLodFrac_ = 0;
    int32_t  primZ_ = 0, primDz_ = 0;
    int32_t  keyCenter_[4]{}, keyScale_[4]{};
    int32_t  k4_ = 0, k5_ = 0;
    int32_t  scXh_ = 0, scYh_ = 0, scXl_ = 0, scYl_ = 0;   // 10.2
    uint32_t noise_ = 0x12345;

    // ── Комбайнер: указатели на входы (как у железа — выбор по полям режима) ──
    struct Inputs {
        int32_t combined[4]{}, texel0[4]{}, texel1[4]{}, shade[4]{}, prim[4]{}, env[4]{};
        int32_t one[4]{256, 256, 256, 256}, zero[4]{};
        int32_t noise[4]{}, keyCenter[4]{}, keyScale[4]{};
        int32_t combinedA[4]{}, texel0A[4]{}, texel1A[4]{}, primA[4]{}, shadeA[4]{}, envA[4]{};
        int32_t lodFrac[4]{}, primLodFrac[4]{}, k4[4]{}, k5[4]{};
    };
    Inputs   in_;
    const int32_t* ccRgb_[2][4]{};                // [такт][A, B, C, D] → массив r,g,b
    const int32_t* ccAlpha_[2][4]{};              // [такт][A, B, C, D] → одно значение
    void     updateCombiner();
    void     syncInputs();
    void     combine(int cycle);

    // ── Команды ───────────────────────────────────────────────────────────────
    static int  commandLength(uint32_t cmd);
    void     execute();
    void     setOtherModes(uint64_t w);
    void     triangle(uint32_t cmd);
    void     rectangle(int32_t xh, int32_t yh, int32_t xl, int32_t yl, bool textured,
                       int tile, int32_t s, int32_t t, int32_t dsdx, int32_t dtdy, bool flip);
    void     loadTile(uint64_t w);
    void     loadBlock(uint64_t w);
    void     loadTlut(uint64_t w);

    // ── Пиксельный конвейер ───────────────────────────────────────────────────
    struct Pixel {
        int32_t shade[4];                         // 0..255 (9-битные, как у железа)
        int32_t s, t;                             // 10.5 после перспективы
        int32_t z, dz;                            // 18 бит
        int     cvg;                              // 0..8 точек покрытия
        bool    cvbit;                            // покрыт левый верхний угол
    };
    void     drawPixel(int x, int y, Pixel& px, int tile, bool textured);
    void     fillPixel(int x, int y);
    void     sampleTexture(int tile, int32_t s, int32_t t, int32_t out[4]);
    void     fetchTexel(const Tile& tl, int32_t s, int32_t t, int32_t out[4]) const;
    void     paletteColor(uint32_t index, int32_t out[4]) const;
    void     readColor(int x, int y, int32_t out[4]) const;
    void     writeColor(int x, int y, const int32_t c[4], int cvg);
    uint32_t readZ(int x, int y) const;
    void     writeZ(int x, int y, uint32_t z, int32_t dz);

    uint8_t  rd8(uint32_t a) const { return rdram_[a & rdramMask_]; }
    uint16_t rd16(uint32_t a) const { return (uint16_t)(rd8(a) << 8 | rd8(a + 1)); }
    void     wr16(uint32_t a, uint16_t v) { rdram_[a & rdramMask_] = (uint8_t)(v >> 8); rdram_[(a + 1) & rdramMask_] = (uint8_t)v; }
};
