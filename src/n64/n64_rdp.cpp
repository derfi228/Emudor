// n64_rdp.cpp — RDP: команды, растеризация, TMEM, комбайнер и блендер.
#include "n64_rdp.h"
#include "console/state_io.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

inline int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

// Входы комбайнера 9-битные: 0x180..0x1FF — отрицательные. Выход «насыщается»
// так же: отрицательное → 0, переполнение → 255.
inline int32_t ext9(int32_t v)   { v &= 0x1FF; return (v & 0x180) == 0x180 ? v - 0x200 : v; }
inline int32_t sext9(int32_t v)  { v &= 0x1FF; return (v & 0x100) ? v - 0x200 : v; }
inline int32_t clamp9(int32_t v) { v &= 0x1FF; if ((v & 0x180) == 0x180) return 0; return (v & 0x100) ? 255 : v; }

inline void rgba16(uint16_t p, int32_t out[4])
{
    const int32_t r = p >> 11, g = (p >> 6) & 31, b = (p >> 1) & 31;
    out[0] = (r << 3) | (r >> 2);
    out[1] = (g << 3) | (g >> 2);
    out[2] = (b << 3) | (b >> 2);
    out[3] = (p & 1) ? 255 : 0;
}

inline void setRgba(int32_t out[4], uint64_t w)
{
    out[0] = (int32_t)(w >> 24) & 0xFF; out[1] = (int32_t)(w >> 16) & 0xFF;
    out[2] = (int32_t)(w >> 8) & 0xFF;  out[3] = (int32_t)w & 0xFF;
}

// Z-буфер: 18-битная глубина сжимается в 3 бита степени + 11 бит мантиссы
// (мельче шаг — ближе к дальней плоскости), младшие 2 бита — крутизна dz.
struct ZRange { uint32_t base; int shift; };
constexpr ZRange kZ[8] = {
    {0x00000, 6}, {0x20000, 5}, {0x30000, 4}, {0x38000, 3},
    {0x3C000, 2}, {0x3E000, 1}, {0x3F000, 0}, {0x3F800, 0},
};

uint16_t zCompress(uint32_t z)
{
    int e = 7;
    while (e > 0 && z < kZ[e].base) --e;
    return (uint16_t)((e << 13) | ((((z - kZ[e].base) >> kZ[e].shift) & 0x7FF) << 2));
}

uint32_t zDecompress(uint16_t v)
{
    const int e = v >> 13;
    return kZ[e].base + ((uint32_t)((v >> 2) & 0x7FF) << kZ[e].shift);
}

// Упорядоченный дизеринг 4×4 для 16-битного вывода (значения 0..7)
constexpr uint8_t kBayer[16] = { 0, 4, 1, 5, 6, 2, 7, 3, 1, 5, 0, 4, 7, 3, 6, 2 };

} // namespace

// ─── Сброс и приём команд ─────────────────────────────────────────────────────
void Rdp::reset()
{
    cmdLen_ = 0;
    color_ = tex_ = Image{};
    zAddr_ = 0;
    for (auto& t : tiles_) t = Tile{};
    std::memset(tmem_, 0, sizeof tmem_);
    setOtherModes(0);
    combineMode_ = 0;
    fillColor_ = primLodFrac_ = primZ_ = primDz_ = k4_ = k5_ = 0;
    for (int c = 0; c < 4; ++c) fog_[c] = blend_[c] = prim_[c] = env_[c] = keyCenter_[c] = keyScale_[c] = 0;
    scXh_ = scYh_ = 0; scXl_ = 320 << 2; scYl_ = 240 << 2;
    in_ = Inputs{};
    updateCombiner();
}

int Rdp::commandLength(uint32_t cmd)
{
    if (cmd >= 0x08 && cmd <= 0x0F)
        return 4 + ((cmd & 4) ? 8 : 0) + ((cmd & 2) ? 8 : 0) + ((cmd & 1) ? 2 : 0);
    if (cmd == 0x24 || cmd == 0x25) return 2;
    return 1;
}

void Rdp::run(const uint64_t* words, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        cmd_[cmdLen_++] = words[i];
        if (cmdLen_ >= commandLength((uint32_t)(cmd_[0] >> 56) & 0x3F)) {
            execute();
            cmdLen_ = 0;
        }
    }
}

void Rdp::execute()
{
    const uint64_t w = cmd_[0];
    const uint32_t cmd = (uint32_t)(w >> 56) & 0x3F;
    switch (cmd) {
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        triangle(cmd);
        break;
    case 0x24: case 0x25: {                                          // Texture Rectangle (Flip)
        const uint64_t w1 = cmd_[1];
        rectangle((int32_t)(w >> 12) & 0xFFF, (int32_t)w & 0xFFF,
                  (int32_t)(w >> 44) & 0xFFF, (int32_t)(w >> 32) & 0xFFF,
                  true, (int)(w >> 24) & 7,
                  (int16_t)(w1 >> 48), (int16_t)(w1 >> 32), (int16_t)(w1 >> 16), (int16_t)w1, cmd == 0x25);
        break;
    }
    case 0x2A:                                                       // Set Key GB
        keyCenter_[1] = (int32_t)(w >> 24) & 0xFF; keyScale_[1] = (int32_t)(w >> 16) & 0xFF;
        keyCenter_[2] = (int32_t)(w >> 8) & 0xFF;  keyScale_[2] = (int32_t)w & 0xFF;
        break;
    case 0x2B:                                                       // Set Key R
        keyCenter_[0] = (int32_t)(w >> 8) & 0xFF; keyScale_[0] = (int32_t)w & 0xFF;
        break;
    case 0x2C: k4_ = sext9((int32_t)(w >> 9)); k5_ = sext9((int32_t)w); break;   // Set Convert
    case 0x2D:                                                       // Set Scissor
        scXh_ = (int32_t)(w >> 44) & 0xFFF; scYh_ = (int32_t)(w >> 32) & 0xFFF;
        scXl_ = (int32_t)(w >> 12) & 0xFFF; scYl_ = (int32_t)w & 0xFFF;
        break;
    case 0x2E: primZ_ = (int32_t)(w >> 16) & 0xFFFF; primDz_ = (int32_t)w & 0xFFFF; break;
    case 0x2F: setOtherModes(w); break;
    case 0x30: loadTlut(w); break;
    case 0x32: {                                                     // Set Tile Size
        Tile& t = tiles_[(w >> 24) & 7];
        t.sl = (uint16_t)((w >> 44) & 0xFFF); t.tl = (uint16_t)((w >> 32) & 0xFFF);
        t.sh = (uint16_t)((w >> 12) & 0xFFF); t.th = (uint16_t)(w & 0xFFF);
        break;
    }
    case 0x33: loadBlock(w); break;
    case 0x34: loadTile(w); break;
    case 0x35: {                                                     // Set Tile
        Tile& t = tiles_[(w >> 24) & 7];
        t.fmt = (uint8_t)((w >> 53) & 7);   t.size = (uint8_t)((w >> 51) & 3);
        t.line = (uint16_t)((w >> 41) & 0x1FF); t.tmem = (uint16_t)((w >> 32) & 0x1FF);
        t.palette = (uint8_t)((w >> 20) & 0xF);
        t.ct = (w >> 19) & 1; t.mt = (w >> 18) & 1;
        t.maskT = (uint8_t)((w >> 14) & 0xF); t.shiftT = (uint8_t)((w >> 10) & 0xF);
        t.cs = (w >> 9) & 1;  t.ms = (w >> 8) & 1;
        t.maskS = (uint8_t)((w >> 4) & 0xF);  t.shiftS = (uint8_t)(w & 0xF);
        break;
    }
    case 0x36:                                                       // Fill Rectangle
        rectangle((int32_t)(w >> 12) & 0xFFF, (int32_t)w & 0xFFF,
                  (int32_t)(w >> 44) & 0xFFF, (int32_t)(w >> 32) & 0xFFF, false, 0, 0, 0, 0, 0, false);
        break;
    case 0x37: fillColor_ = (int32_t)(uint32_t)w; break;
    case 0x38: setRgba(fog_, w); break;
    case 0x39: setRgba(blend_, w); break;
    case 0x3A: setRgba(prim_, w); primLodFrac_ = (int32_t)(w >> 32) & 0xFF; break;
    case 0x3B: setRgba(env_, w); break;
    case 0x3C: combineMode_ = w; updateCombiner(); break;
    case 0x3D: case 0x3F: {                                          // Set Texture / Color Image
        Image& img = cmd == 0x3D ? tex_ : color_;
        img.fmt = (uint8_t)((w >> 53) & 7);
        img.size = (uint8_t)((w >> 51) & 3);
        img.width = (uint32_t)((w >> 32) & 0x3FF) + 1;
        img.addr = (uint32_t)w & 0x3FFFFFF;
        break;
    }
    case 0x3E: zAddr_ = (uint32_t)w & 0x3FFFFFF; break;
    default: break;                                                  // NOP и синхронизации
    }
    syncInputs();
}

// Константы комбайнера (цвета prim/env, ключ, конвертация) → его входы.
void Rdp::syncInputs()
{
    for (int c = 0; c < 4; ++c) {
        in_.prim[c] = prim_[c]; in_.env[c] = env_[c];
        in_.primA[c] = prim_[3]; in_.envA[c] = env_[3];
        in_.primLodFrac[c] = primLodFrac_;
        in_.keyCenter[c] = keyCenter_[c]; in_.keyScale[c] = keyScale_[c];
        in_.k4[c] = k4_; in_.k5[c] = k5_;
    }
}

void Rdp::setOtherModes(uint64_t w)
{
    otherModes_ = w;
    m_.cycle     = (uint8_t)((w >> 52) & 3);
    m_.persp     = (w >> 51) & 1;
    m_.texLod    = (w >> 48) & 1;
    m_.tlut      = (w >> 47) & 1;
    m_.tlutIa    = (w >> 46) & 1;
    m_.bilinear  = (w >> 45) & 1;
    m_.rgbDither = (uint8_t)((w >> 38) & 3);
    m_.bP[0] = (uint8_t)((w >> 30) & 3); m_.bP[1] = (uint8_t)((w >> 28) & 3);
    m_.bA[0] = (uint8_t)((w >> 26) & 3); m_.bA[1] = (uint8_t)((w >> 24) & 3);
    m_.bM[0] = (uint8_t)((w >> 22) & 3); m_.bM[1] = (uint8_t)((w >> 20) & 3);
    m_.bB[0] = (uint8_t)((w >> 18) & 3); m_.bB[1] = (uint8_t)((w >> 16) & 3);
    m_.forceBlend    = (w >> 14) & 1;
    m_.alphaCvgSel   = (w >> 13) & 1;
    m_.cvgTimesAlpha = (w >> 12) & 1;
    m_.zMode         = (uint8_t)((w >> 10) & 3);
    m_.cvgDest       = (uint8_t)((w >> 8) & 3);
    m_.imageRead     = (w >> 6) & 1;
    m_.zUpdate       = (w >> 5) & 1;
    m_.zCompare      = (w >> 4) & 1;
    m_.aa            = (w >> 3) & 1;
    m_.zPrim         = (w >> 2) & 1;
    m_.ditherAlpha   = (w >> 1) & 1;
    m_.alphaCompare  = w & 1;
}

// ─── Комбайнер ────────────────────────────────────────────────────────────────
void Rdp::updateCombiner()
{
    const uint64_t w = combineMode_;
    auto f = [&](int shift, uint32_t mask) { return (uint32_t)(w >> shift) & mask; };
    const uint32_t subAR[2] = { f(52, 0xF),  f(37, 0xF) }, mulR[2] = { f(47, 0x1F), f(32, 0x1F) };
    const uint32_t subBR[2] = { f(28, 0xF),  f(24, 0xF) }, addR[2] = { f(15, 0x7),  f(6, 0x7) };
    const uint32_t subAA[2] = { f(44, 0x7),  f(21, 0x7) }, mulA[2] = { f(41, 0x7),  f(18, 0x7) };
    const uint32_t subBA[2] = { f(12, 0x7),  f(3, 0x7) },  addA[2] = { f(9, 0x7),   f(0, 0x7) };

    // Общая часть кодов 0-5: комбинированный, texel0, texel1, prim, shade, env
    auto common = [&](uint32_t code) -> const int32_t* {
        switch (code) {
        case 0: return in_.combined;
        case 1: return in_.texel0;
        case 2: return in_.texel1;
        case 3: return in_.prim;
        case 4: return in_.shade;
        case 5: return in_.env;
        default: return nullptr;
        }
    };
    for (int c = 0; c < 2; ++c) {
        const int32_t* p;
        p = common(subAR[c]);
        ccRgb_[c][0] = p ? p : subAR[c] == 6 ? in_.one : subAR[c] == 7 ? in_.noise : in_.zero;
        p = common(subBR[c]);
        ccRgb_[c][1] = p ? p : subBR[c] == 6 ? in_.keyCenter : subBR[c] == 7 ? in_.k4 : in_.zero;
        switch (mulR[c]) {
        case 6:  p = in_.keyScale; break;
        case 7:  p = in_.combinedA; break;
        case 8:  p = in_.texel0A; break;
        case 9:  p = in_.texel1A; break;
        case 10: p = in_.primA; break;
        case 11: p = in_.shadeA; break;
        case 12: p = in_.envA; break;
        case 13: p = in_.lodFrac; break;
        case 14: p = in_.primLodFrac; break;
        case 15: p = in_.k5; break;
        default: p = mulR[c] < 6 ? common(mulR[c]) : in_.zero; break;
        }
        ccRgb_[c][2] = p;
        p = common(addR[c]);
        ccRgb_[c][3] = p ? p : addR[c] == 6 ? in_.one : in_.zero;

        auto alphaIn = [&](uint32_t code) -> const int32_t* {
            const int32_t* q = common(code);
            return q ? q + 3 : code == 6 ? in_.one + 3 : in_.zero + 3;
        };
        ccAlpha_[c][0] = alphaIn(subAA[c]);
        ccAlpha_[c][1] = alphaIn(subBA[c]);
        switch (mulA[c]) {
        case 0:  ccAlpha_[c][2] = in_.lodFrac; break;
        case 6:  ccAlpha_[c][2] = in_.primLodFrac; break;
        case 7:  ccAlpha_[c][2] = in_.zero; break;
        default: ccAlpha_[c][2] = common(mulA[c]) + 3; break;
        }
        ccAlpha_[c][3] = alphaIn(addA[c]);
    }
}

// (A − B) · C + D в 9-битной арифметике железа; результат остаётся 9-битным
// (вход «комбинированный» второго такта), насыщение — при выводе.
void Rdp::combine(int cycle)
{
    int32_t out[4];
    const int32_t* const* rgb = ccRgb_[cycle];
    for (int ch = 0; ch < 3; ++ch) {
        const int32_t v = (ext9(rgb[0][ch]) - ext9(rgb[1][ch])) * sext9(rgb[2][ch]) + (ext9(rgb[3][ch]) << 8) + 0x80;
        out[ch] = (v >> 8) & 0x1FF;
    }
    const int32_t* const* al = ccAlpha_[cycle];
    const int32_t va = (ext9(*al[0]) - ext9(*al[1])) * sext9(*al[2]) + (ext9(*al[3]) << 8) + 0x80;
    out[3] = (va >> 8) & 0x1FF;
    for (int ch = 0; ch < 4; ++ch) {
        in_.combined[ch] = out[ch];
        in_.combinedA[ch] = out[3];
    }
}

// ─── Треугольник ──────────────────────────────────────────────────────────────
// Рёбра заданы x на верхней целой строке (xh, xm) и на ym (xl) с наклонами
// dx/dy; строка делится на 4 подстроки. Атрибуты: значение в точке (xh, верх),
// производные по x и вдоль главного ребра (de).
void Rdp::triangle(uint32_t cmd)
{
    const bool shade = cmd & 4, tex = cmd & 2, zbuf = cmd & 1;
    const uint64_t w0 = cmd_[0];
    const bool lft = (w0 >> 55) & 1;
    const int tile = (int)(w0 >> 48) & 7;
    const int32_t yl = sext((uint32_t)(w0 >> 32) & 0x3FFF, 14);
    const int32_t ym = sext((uint32_t)(w0 >> 16) & 0x3FFF, 14);
    const int32_t yh = sext((uint32_t)w0 & 0x3FFF, 14);
    const int32_t xl = (int32_t)(cmd_[1] >> 32), dxl = (int32_t)cmd_[1];
    const int32_t xh = (int32_t)(cmd_[2] >> 32), dxh = (int32_t)cmd_[2];
    const int32_t xm = (int32_t)(cmd_[3] >> 32), dxm = (int32_t)cmd_[3];

    // attr[n] = {значение, d/dx, d/de}; n: 0-3 RGBA, 4-6 S T W, 7 Z
    int32_t attr[8][3]{};
    int32_t dzdy = 0;
    auto fixed = [](uint64_t hi, uint64_t lo, int lane) -> int32_t {
        const int sh = 48 - 16 * lane;
        return (int32_t)(((uint32_t)(hi >> sh) & 0xFFFF) << 16 | ((uint32_t)(lo >> sh) & 0xFFFF));
    };
    int idx = 4;
    if (shade) {
        for (int c = 0; c < 4; ++c) {
            attr[c][0] = fixed(cmd_[idx], cmd_[idx + 2], c);
            attr[c][1] = fixed(cmd_[idx + 1], cmd_[idx + 3], c);
            attr[c][2] = fixed(cmd_[idx + 4], cmd_[idx + 6], c);
        }
        idx += 8;
    }
    if (tex) {
        for (int c = 0; c < 3; ++c) {
            attr[4 + c][0] = fixed(cmd_[idx], cmd_[idx + 2], c);
            attr[4 + c][1] = fixed(cmd_[idx + 1], cmd_[idx + 3], c);
            attr[4 + c][2] = fixed(cmd_[idx + 4], cmd_[idx + 6], c);
        }
        idx += 8;
    }
    if (zbuf) {
        attr[7][0] = (int32_t)(cmd_[idx] >> 32);
        attr[7][1] = (int32_t)cmd_[idx];
        attr[7][2] = (int32_t)(cmd_[idx + 1] >> 32);
        dzdy       = (int32_t)cmd_[idx + 1];
    }
    // Крутизна глубины пикселя (для decal и допуска сравнения), в единицах 18-битной Z
    const int32_t dzPix = m_.zPrim ? primDz_ : (int32_t)(((int64_t)std::abs(attr[7][1]) + std::abs(dzdy)) >> 13);

    const int32_t ystart = yh & ~3;
    const int32_t yTop = std::max(yh, scYh_);
    const int32_t yEnd = std::min(yl, scYl_);                       // не включая
    if (yTop >= yEnd) return;
    const int clipX0 = scXh_ >> 2, clipX1 = scXl_ >> 2;             // x < clipX1

    for (int row = yTop >> 2; row <= (yEnd - 1) >> 2; ++row) {
        int64_t lx[4]{}, rx[4]{};
        bool valid[4];
        int64_t minX = std::numeric_limits<int64_t>::max(), maxX = std::numeric_limits<int64_t>::min();
        for (int i = 0; i < 4; ++i) {
            const int32_t k = row * 4 + i;
            valid[i] = k >= yTop && k < yEnd;
            if (!valid[i]) continue;
            const int64_t major = (int64_t)xh + (((int64_t)dxh * (k - ystart)) >> 2);
            const int64_t minor = k < ym ? (int64_t)xm + (((int64_t)dxm * (k - ystart)) >> 2)
                                         : (int64_t)xl + (((int64_t)dxl * (k - ym)) >> 2);
            lx[i] = lft ? major : minor;
            rx[i] = lft ? minor : major;
            if (lx[i] >= rx[i]) { valid[i] = false; continue; }
            minX = std::min(minX, lx[i]);
            maxX = std::max(maxX, rx[i]);
        }
        if (minX >= maxX) continue;
        const int x0 = std::max<int64_t>(minX >> 16, clipX0);
        const int x1 = std::min<int64_t>((maxX - 1) >> 16, clipX1 - 1);
        if (x0 > x1) continue;

        // Внутренние пиксели (все точки покрыты) — без поточечной проверки
        int64_t maxL = std::numeric_limits<int64_t>::min(), minR = std::numeric_limits<int64_t>::max();
        int fullCvg = 0;
        for (int i = 0; i < 4; ++i) {
            if (!valid[i]) continue;
            maxL = std::max(maxL, lx[i]);
            minR = std::min(minR, rx[i]);
            fullCvg += 2;
        }
        const int64_t in0 = (maxL + 0xFFFF) >> 16;                   // первый x с lx ≤ x
        const int64_t in1 = (minR - 0xC001) >> 16;                   // последний x с x+¾ < rx

        // Атрибуты в пикселе x0, дальше — приращениями d/dx
        const int32_t rows = row - (ystart >> 2);
        const int64_t xhRow = (int64_t)xh + (int64_t)dxh * rows;    // главное ребро на этой строке
        const int64_t dx0 = ((int64_t)x0 << 16) - xhRow;
        int64_t val[8];
        for (int n = 0; n < 8; ++n)
            val[n] = attr[n][0] + (int64_t)attr[n][2] * rows + (((int64_t)attr[n][1] * dx0) >> 16);

        for (int x = x0; x <= x1; ++x) {
            int cvg;
            bool cvbit;
            if (x >= in0 && x <= in1) {
                cvg = fullCvg;
                cvbit = valid[0];
            } else {
                // Покрытие: по 2 точки на подстроку (смещения 0 и ½ или ¼ и ¾)
                cvg = 0;
                cvbit = false;
                const int64_t px16 = (int64_t)x << 16;
                for (int i = 0; i < 4; ++i) {
                    if (!valid[i]) continue;
                    const int64_t s0 = px16 + ((i & 1) ? 0x4000 : 0), s1 = s0 + 0x8000;
                    if (s0 >= lx[i] && s0 < rx[i]) { ++cvg; if (i == 0) cvbit = true; }
                    if (s1 >= lx[i] && s1 < rx[i]) ++cvg;
                }
            }
            if (cvg && (m_.cycle == 3 ? cvbit : true)) {
                if (m_.cycle == 3) {
                    fillPixel(x, row);
                } else {
                    Pixel p;
                    for (int c = 0; c < 4; ++c) p.shade[c] = shade ? clamp9((int32_t)(val[c] >> 16)) : 0;
                    p.s = p.t = 0;
                    if (tex) {
                        const int32_t s = (int32_t)val[4], t = (int32_t)val[5];
                        if (m_.persp) {
                            const double inv = 32768.0 / (double)std::max<int32_t>((int32_t)val[6], 1);
                            p.s = (int32_t)std::clamp<double>(s * inv, -32768.0, 32767.0);
                            p.t = (int32_t)std::clamp<double>(t * inv, -32768.0, 32767.0);
                        } else {
                            p.s = s >> 16;
                            p.t = t >> 16;
                        }
                    }
                    if (m_.zPrim)  p.z = (primZ_ & 0x7FFF) << 3;
                    else if (zbuf) p.z = std::clamp((int32_t)val[7] >> 13, 0, 0x3FFFF);
                    else           p.z = 0;
                    p.dz = dzPix;
                    p.cvg = cvg;
                    p.cvbit = cvbit;
                    drawPixel(x, row, p, tile, tex);
                }
            }
            for (int n = 0; n < 8; ++n) val[n] += attr[n][1];
        }
    }
}

// ─── Прямоугольники: заливка и текстура ───────────────────────────────────────
// В режимах заливки и копирования правый и нижний край включаются.
void Rdp::rectangle(int32_t xh, int32_t yh, int32_t xl, int32_t yl, bool textured,
                    int tile, int32_t s, int32_t t, int32_t dsdx, int32_t dtdy, bool flip)
{
    const bool inclusive = m_.cycle >= 2;
    const int ux0 = inclusive ? xh >> 2 : (xh + 3) >> 2;
    const int uy0 = inclusive ? yh >> 2 : (yh + 3) >> 2;
    int x1 = inclusive ? xl >> 2 : ((xl + 3) >> 2) - 1;
    int y1 = inclusive ? yl >> 2 : ((yl + 3) >> 2) - 1;
    const int x0 = std::max(ux0, scXh_ >> 2), y0 = std::max(uy0, scYh_ >> 2);
    x1 = std::min(x1, (scXl_ >> 2) - 1);
    y1 = std::min(y1, (scYl_ >> 2) - 1);
    const int sDiv = m_.cycle == 2 ? 7 : 5;                         // копирование: 4 пикселя за такт

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (m_.cycle == 3) { fillPixel(x, y); continue; }
            const int32_t dx = x - ux0, dy = y - uy0;
            const int32_t ss = s + ((dsdx * (flip ? dy : dx)) >> sDiv);
            const int32_t tt = t + ((dtdy * (flip ? dx : dy)) >> 5);
            if (m_.cycle == 2) {                                     // копирование текселей
                int32_t c[4];
                sampleTexture(tile, ss, tt, c);
                if (m_.alphaCompare && c[3] == 0) continue;
                writeColor(x, y, c, c[3] ? 8 : 0);
                continue;
            }
            Pixel p;
            for (int ch = 0; ch < 4; ++ch) p.shade[ch] = 0;
            p.s = ss;
            p.t = tt;
            p.z = (primZ_ & 0x7FFF) << 3;
            p.dz = primDz_;
            p.cvg = 8;
            p.cvbit = true;
            drawPixel(x, y, p, tile, textured);
        }
    }
}

// ─── Пиксель ──────────────────────────────────────────────────────────────────
void Rdp::drawPixel(int x, int y, Pixel& px, int tile, bool textured)
{
    if (textured) {
        sampleTexture(tile, px.s, px.t, in_.texel0);
        if (m_.cycle == 1) sampleTexture((tile + 1) & 7, px.s, px.t, in_.texel1);
        else std::memcpy(in_.texel1, in_.texel0, sizeof in_.texel1);
    }
    noise_ = noise_ * 1103515245u + 12345u;
    for (int c = 0; c < 4; ++c) {
        in_.shade[c] = px.shade[c];
        in_.shadeA[c] = px.shade[3];
        in_.texel0A[c] = in_.texel0[3];
        in_.texel1A[c] = in_.texel1[3];
        in_.noise[c] = (int32_t)((noise_ >> 16) & 0xFF);
    }

    // Комбайнер: в 1-тактном режиме работают настройки второго такта; во
    // втором такте TEXEL0 — это texel1, а TEXEL1 — (почти) следующий texel0.
    if (m_.cycle == 1) {
        combine(0);
        int32_t tmp[4];
        std::memcpy(tmp, in_.texel0, sizeof tmp);
        std::memcpy(in_.texel0, in_.texel1, sizeof tmp);
        std::memcpy(in_.texel1, tmp, sizeof tmp);
        for (int c = 0; c < 4; ++c) { in_.texel0A[c] = in_.texel0[3]; in_.texel1A[c] = in_.texel1[3]; }
        combine(1);
    } else {
        combine(1);
    }
    int32_t color[4];
    for (int c = 0; c < 4; ++c) color[c] = clamp9(in_.combined[c]);

    // Покрытие × альфа, альфа из покрытия, порог альфы
    int cvg = px.cvg;
    if (m_.cvgTimesAlpha) cvg = ((color[3] * cvg + 4) >> 3) >> 5;
    int32_t alpha = color[3];
    if (m_.alphaCvgSel) alpha = std::min(cvg << 5, 255);
    if (m_.alphaCompare) {
        const int32_t threshold = m_.ditherAlpha ? (int32_t)((noise_ >> 8) & 0xFF) : blend_[3];
        if (color[3] < threshold) return;
    }
    // ponytail: сглаживание краёв не моделируется (нужны скрытые биты покрытия
    // RDRAM и фильтр VI) — пиксель рисуется по угловой точке, как без AA;
    // покрытие только отсекает прозрачные тексели (cvg × alpha).
    if (!px.cvbit || (m_.aa && cvg == 0)) return;

    // Глубина
    if (m_.zCompare) {
        const uint32_t zmem = readZ(x, y);
        const int32_t z = px.z;
        bool pass;
        if (m_.zMode == 3) pass = std::abs(z - (int32_t)zmem) <= std::max(px.dz, 16);   // decal
        else if (m_.zMode == 2) pass = z < (int32_t)zmem;                               // прозрачные
        else pass = z <= (int32_t)zmem;
        if (!pass) return;
    }

    // Блендер: P·A + M·B. Второй такт видит результат первого как «пиксель».
    int32_t mem[4] = { 0, 0, 0, 0 };
    if (m_.imageRead) readColor(x, y, mem);
    auto colorIn = [&](uint8_t sel, const int32_t* pix) -> const int32_t* {
        switch (sel) { case 0: return pix; case 1: return mem; case 2: return blend_; default: return fog_; }
    };
    auto alphaIn = [&](uint8_t sel) -> int32_t {
        switch (sel) { case 0: return alpha; case 1: return fog_[3]; case 2: return px.shade[3]; default: return 0; }
    };
    auto betaIn = [&](uint8_t sel, int32_t a) -> int32_t {
        switch (sel) { case 0: return 255 - a; case 1: return mem[3]; case 2: return 255; default: return 0; }
    };
    auto equation = [&](int cyc, const int32_t* pix, bool divide, int32_t out[3]) {
        const int32_t* p = colorIn(m_.bP[cyc], pix);
        const int32_t* m = colorIn(m_.bM[cyc], pix);
        const int32_t a8 = alphaIn(m_.bA[cyc]);
        const int32_t a = a8 >> 3, b = (betaIn(m_.bB[cyc], a8) >> 3) + 1;
        for (int ch = 0; ch < 3; ++ch) {
            const int32_t sum = p[ch] * a + m[ch] * b;
            out[ch] = std::min(divide ? sum / (a + b) : sum >> 5, 255);
        }
    };
    const bool blendEn = m_.forceBlend;
    int32_t out[4] = { 0, 0, 0, alpha };
    if (m_.cycle == 1) {
        int32_t first[4] = { 0, 0, 0, alpha };
        equation(0, color, false, first);
        if (blendEn) equation(1, first, !m_.forceBlend, out);
        else std::memcpy(out, colorIn(m_.bP[1], first), sizeof(int32_t) * 3);
    } else {
        if (blendEn) equation(0, color, !m_.forceBlend, out);
        else std::memcpy(out, colorIn(m_.bP[0], color), sizeof(int32_t) * 3);
    }

    // Дизеринг для 16-битного буфера
    if (color_.size == 2 && m_.rgbDither != 3) {
        const int32_t d = kBayer[((y & 3) << 2) | (x & 3)];
        for (int ch = 0; ch < 3; ++ch) out[ch] = std::min(out[ch] + d, 255);
    }
    writeColor(x, y, out, cvg);
    if (m_.zUpdate) writeZ(x, y, (uint32_t)px.z, px.dz);
}

void Rdp::fillPixel(int x, int y)
{
    const uint32_t i = (uint32_t)y * color_.width + (uint32_t)x;
    switch (color_.size) {
    case 1: rdram_[(color_.addr + i) & rdramMask_] = (uint8_t)(fillColor_ >> (8 * (3 - (x & 3)))); break;
    case 2: wr16(color_.addr + i * 2, (uint16_t)((x & 1) ? fillColor_ : fillColor_ >> 16)); break;
    case 3: {
        const uint32_t a = color_.addr + i * 4;
        wr16(a, (uint16_t)(fillColor_ >> 16));
        wr16(a + 2, (uint16_t)fillColor_);
        break;
    }
    default: break;
    }
}

void Rdp::readColor(int x, int y, int32_t out[4]) const
{
    const uint32_t i = (uint32_t)y * color_.width + (uint32_t)x;
    if (color_.size == 3) {
        const uint32_t a = color_.addr + i * 4;
        out[0] = rd8(a); out[1] = rd8(a + 1); out[2] = rd8(a + 2); out[3] = rd8(a + 3);
    } else if (color_.size == 2) {
        const uint16_t p = rd16(color_.addr + i * 2);
        rgba16(p, out);
        out[3] = (p & 1) ? 0xE0 : 0;                                  // покрытие в памяти
    } else {
        out[0] = out[1] = out[2] = rd8(color_.addr + i);
        out[3] = 0xE0;
    }
}

void Rdp::writeColor(int x, int y, const int32_t c[4], int cvg)
{
    const uint32_t i = (uint32_t)y * color_.width + (uint32_t)x;
    switch (color_.size) {
    case 1: rdram_[(color_.addr + i) & rdramMask_] = (uint8_t)c[0]; break;
    case 2:
        wr16(color_.addr + i * 2, (uint16_t)(((c[0] >> 3) << 11) | ((c[1] >> 3) << 6) | ((c[2] >> 3) << 1) | (cvg > 0 ? 1 : 0)));
        break;
    case 3: {
        const uint32_t a = color_.addr + i * 4;
        rdram_[a & rdramMask_] = (uint8_t)c[0];
        rdram_[(a + 1) & rdramMask_] = (uint8_t)c[1];
        rdram_[(a + 2) & rdramMask_] = (uint8_t)c[2];
        rdram_[(a + 3) & rdramMask_] = (uint8_t)std::min(cvg << 5, 255);
        break;
    }
    default: break;
    }
}

uint32_t Rdp::readZ(int x, int y) const
{
    return zDecompress(rd16(zAddr_ + ((uint32_t)y * color_.width + (uint32_t)x) * 2));
}

void Rdp::writeZ(int x, int y, uint32_t z, int32_t dz)
{
    int bits = 0;
    while (bits < 15 && (1 << (bits + 1)) <= dz) ++bits;
    wr16(zAddr_ + ((uint32_t)y * color_.width + (uint32_t)x) * 2, (uint16_t)(zCompress(z) | (bits >> 2)));
}

// ─── Текстуры ─────────────────────────────────────────────────────────────────
// s, t — 10.5. Сдвиг тайла, отсчёт от его угла, зажим/зеркало/маска, затем
// точка или трёхточечный фильтр N64.
void Rdp::sampleTexture(int tileIdx, int32_t s, int32_t t, int32_t out[4])
{
    const Tile& tl = tiles_[tileIdx & 7];
    auto shift = [](int32_t c, uint8_t sh) { return sh < 11 ? c >> sh : (int32_t)((uint32_t)c << (16 - sh)); };
    s = shift(s, tl.shiftS) - ((int32_t)tl.sl << 3);
    t = shift(t, tl.shiftT) - ((int32_t)tl.tl << 3);
    const int32_t maxS = std::max(((int32_t)tl.sh - (int32_t)tl.sl) << 3, 0);
    const int32_t maxT = std::max(((int32_t)tl.th - (int32_t)tl.tl) << 3, 0);
    const bool clampS = tl.cs || !tl.maskS, clampT = tl.ct || !tl.maskT;
    if (clampS) s = std::clamp(s, 0, maxS);
    if (clampT) t = std::clamp(t, 0, maxT);

    auto wrap = [](int32_t c, bool mirror, uint8_t mask) -> int32_t {
        if (!mask) return c;
        mask = std::min<uint8_t>(mask, 10);
        if (mirror && ((c >> mask) & 1)) c = ~c;
        return c & ((1 << mask) - 1);
    };
    const int32_t si = s >> 5, ti = t >> 5;
    if (!m_.bilinear || m_.cycle == 2) {
        fetchTexel(tl, wrap(si, tl.ms, tl.maskS), wrap(ti, tl.mt, tl.maskT), out);
        return;
    }
    const int32_t sf = s & 31, tf = t & 31;
    int32_t s1 = si + 1, t1 = ti + 1;
    if (clampS && s1 > (maxS >> 5)) s1 = maxS >> 5;
    if (clampT && t1 > (maxT >> 5)) t1 = maxT >> 5;
    const int32_t sw0 = wrap(si, tl.ms, tl.maskS), sw1 = wrap(s1, tl.ms, tl.maskS);
    const int32_t tw0 = wrap(ti, tl.mt, tl.maskT), tw1 = wrap(t1, tl.mt, tl.maskT);
    int32_t c10[4], c01[4], c[4];
    fetchTexel(tl, sw1, tw0, c10);
    fetchTexel(tl, sw0, tw1, c01);
    if (sf + tf < 32) {                                              // верхний левый треугольник
        fetchTexel(tl, sw0, tw0, c);
        for (int ch = 0; ch < 4; ++ch)
            out[ch] = c[ch] + (((c10[ch] - c[ch]) * sf + (c01[ch] - c[ch]) * tf + 16) >> 5);
    } else {                                                         // нижний правый
        fetchTexel(tl, sw1, tw1, c);
        for (int ch = 0; ch < 4; ++ch)
            out[ch] = c[ch] + (((c01[ch] - c[ch]) * (32 - sf) + (c10[ch] - c[ch]) * (32 - tf) + 16) >> 5);
    }
}

void Rdp::paletteColor(uint32_t index, int32_t out[4]) const
{
    const uint32_t a = (0x800 + (index & 0xFF) * 8) & 0xFFF;
    const uint16_t e = (uint16_t)(tmem_[a] << 8 | tmem_[a + 1]);
    if (m_.tlutIa) { out[0] = out[1] = out[2] = e >> 8; out[3] = e & 0xFF; }
    else rgba16(e, out);
}

// Тексель из TMEM. Нечётные строки лежат с переставленными 32-битными
// половинами слов (так их кладут загрузки) — адрес исправляется XOR 4.
void Rdp::fetchTexel(const Tile& tl, int32_t s, int32_t t, int32_t out[4]) const
{
    const uint32_t base = (uint32_t)tl.tmem * 8 + (uint32_t)t * tl.line * 8;
    const uint32_t swap = (t & 1) ? 4 : 0;
    const uint32_t amask = m_.tlut ? 0x7FF : 0xFFF;                  // палитра занимает верхнюю половину
    switch (tl.size) {
    case 0: {                                                        // 4 бита
        const uint8_t b = tmem_[((base + ((uint32_t)s >> 1)) ^ swap) & amask];
        const uint32_t n = (s & 1) ? b & 0xF : b >> 4;
        if (m_.tlut) { paletteColor(((uint32_t)tl.palette << 4) | n, out); return; }
        if (tl.fmt == 3) {                                           // IA4: 3 бита яркости, 1 бит альфы
            const int32_t i = (int32_t)(n >> 1);
            out[0] = out[1] = out[2] = (i << 5) | (i << 2) | (i >> 1);
            out[3] = (n & 1) ? 255 : 0;
        } else {                                                     // I4 (и CI без палитры)
            out[0] = out[1] = out[2] = out[3] = (int32_t)(n * 17);
        }
        return;
    }
    case 1: {                                                        // 8 бит
        const uint8_t b = tmem_[((base + (uint32_t)s) ^ swap) & amask];
        if (m_.tlut) { paletteColor(b, out); return; }
        if (tl.fmt == 3) {                                           // IA8
            out[0] = out[1] = out[2] = (b >> 4) * 17;
            out[3] = (b & 0xF) * 17;
        } else {                                                     // I8
            out[0] = out[1] = out[2] = out[3] = b;
        }
        return;
    }
    case 2: {                                                        // 16 бит
        const uint32_t a = ((base + (uint32_t)s * 2) ^ swap) & 0xFFF;
        const uint16_t v = (uint16_t)(tmem_[a] << 8 | tmem_[(a + 1) & 0xFFF]);
        if (m_.tlut) { paletteColor(v >> 8, out); return; }
        if (tl.fmt == 3) { out[0] = out[1] = out[2] = v >> 8; out[3] = v & 0xFF; }   // IA16
        else rgba16(v, out);                                         // RGBA16 (YUV — приближённо)
        return;
    }
    default: {                                                       // 32 бита: RG внизу, BA вверху
        const uint32_t a = ((base + (uint32_t)s * 2) ^ swap) & 0x7FF;
        out[0] = tmem_[a];
        out[1] = tmem_[(a + 1) & 0x7FF];
        out[2] = tmem_[a | 0x800];
        out[3] = tmem_[((a + 1) & 0x7FF) | 0x800];
        return;
    }
    }
}

// ─── Загрузки в TMEM ──────────────────────────────────────────────────────────
void Rdp::loadTile(uint64_t w)
{
    Tile& tl = tiles_[(w >> 24) & 7];
    tl.sl = (uint16_t)((w >> 44) & 0xFFF); tl.tl = (uint16_t)((w >> 32) & 0xFFF);
    tl.sh = (uint16_t)((w >> 12) & 0xFFF); tl.th = (uint16_t)(w & 0xFFF);
    const uint32_t sl = tl.sl >> 2, tt = tl.tl >> 2, sh = tl.sh >> 2, th = tl.th >> 2;
    const uint32_t lineBytes = (uint32_t)tl.line * 8;
    for (uint32_t y = tt; y <= th; ++y) {
        const uint32_t row = y - tt;
        const uint32_t swap = (row & 1) ? 4 : 0;
        const uint32_t rowBase = (uint32_t)tl.tmem * 8 + row * lineBytes;
        for (uint32_t x = sl; x <= sh; ++x) {
            const uint32_t col = x - sl;
            const uint32_t texel = y * tex_.width + x;
            switch (tex_.size) {
            case 0: {
                const uint8_t b = rd8(tex_.addr + texel / 2);
                const uint8_t n = (texel & 1) ? b & 0xF : b >> 4;
                uint8_t& d = tmem_[((rowBase + col / 2) ^ swap) & 0xFFF];
                d = (col & 1) ? (uint8_t)((d & 0xF0) | n) : (uint8_t)((d & 0x0F) | (n << 4));
                break;
            }
            case 1:
                tmem_[((rowBase + col) ^ swap) & 0xFFF] = rd8(tex_.addr + texel);
                break;
            case 2: {
                const uint32_t d = (rowBase + col * 2) ^ swap;
                tmem_[d & 0xFFF] = rd8(tex_.addr + texel * 2);
                tmem_[(d + 1) & 0xFFF] = rd8(tex_.addr + texel * 2 + 1);
                break;
            }
            default: {
                const uint32_t d = ((rowBase + col * 2) ^ swap) & 0x7FF;
                const uint32_t src = tex_.addr + texel * 4;
                tmem_[d] = rd8(src);
                tmem_[(d + 1) & 0x7FF] = rd8(src + 1);
                tmem_[d | 0x800] = rd8(src + 2);
                tmem_[((d + 1) & 0x7FF) | 0x800] = rd8(src + 3);
                break;
            }
            }
        }
    }
}

// Блок подряд идущих текселей; dxt (1.11) считает строки, чтобы переставить
// половины слов нечётных строк.
void Rdp::loadBlock(uint64_t w)
{
    Tile& tl = tiles_[(w >> 24) & 7];
    const uint32_t sl = (uint32_t)(w >> 44) & 0xFFF, tt = (uint32_t)(w >> 32) & 0xFFF;
    const uint32_t sh = (uint32_t)(w >> 12) & 0xFFF, dxt = (uint32_t)w & 0xFFF;
    tl.sl = (uint16_t)sl; tl.tl = (uint16_t)tt; tl.sh = (uint16_t)sh; tl.th = (uint16_t)dxt;
    if (sh < sl) return;
    const uint32_t count = sh - sl + 1;
    const uint32_t bits = 4u << tex_.size;
    const uint32_t src = tex_.addr + (tt * tex_.width + sl) * bits / 8;
    if (tex_.size == 3) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t swap = ((((i >> 1) * dxt) >> 11) & 1) ? 4 : 0;
            const uint32_t d = (((uint32_t)tl.tmem * 8 + i * 2) ^ swap) & 0x7FF;
            const uint32_t a = src + i * 4;
            tmem_[d] = rd8(a);
            tmem_[(d + 1) & 0x7FF] = rd8(a + 1);
            tmem_[d | 0x800] = rd8(a + 2);
            tmem_[((d + 1) & 0x7FF) | 0x800] = rd8(a + 3);
        }
        return;
    }
    const uint32_t words = ((count * bits + 7) / 8 + 7) / 8;
    for (uint32_t wd = 0; wd < words; ++wd) {
        const uint32_t swap = (((wd * dxt) >> 11) & 1) ? 4 : 0;
        for (uint32_t b = 0; b < 8; ++b)
            tmem_[(((uint32_t)tl.tmem * 8 + wd * 8 + b) ^ swap) & 0xFFF] = rd8(src + wd * 8 + b);
    }
}

// Палитра: каждый 16-битный элемент повторяется 4 раза (по банкам TMEM).
void Rdp::loadTlut(uint64_t w)
{
    const Tile& tl = tiles_[(w >> 24) & 7];
    const uint32_t sl = ((uint32_t)(w >> 44) & 0xFFF) >> 2, tt = ((uint32_t)(w >> 32) & 0xFFF) >> 2;
    const uint32_t sh = ((uint32_t)(w >> 12) & 0xFFF) >> 2;
    if (sh < sl) return;
    const uint32_t src = tex_.addr + (tt * tex_.width + sl) * 2;
    for (uint32_t i = 0; i <= sh - sl; ++i) {
        const uint16_t e = rd16(src + i * 2);
        const uint32_t d = (uint32_t)tl.tmem * 8 + i * 8;
        for (uint32_t k = 0; k < 4; ++k) {
            tmem_[(d + k * 2) & 0xFFF] = (uint8_t)(e >> 8);
            tmem_[(d + k * 2 + 1) & 0xFFF] = (uint8_t)e;
        }
    }
}

// ─── Save state ───────────────────────────────────────────────────────────────
template<class S> void Rdp::serialize(S& s)
{
    for (auto& w : cmd_) s.io(w);
    s.io(cmdLen_);
    s.io(color_); s.io(tex_); s.io(zAddr_);
    for (auto& t : tiles_) s.io(t);
    for (auto& b : tmem_) s.io(b);
    s.io(otherModes_); s.io(combineMode_);
    s.io(fillColor_);
    for (int c = 0; c < 4; ++c) {
        s.io(fog_[c]); s.io(blend_[c]); s.io(prim_[c]); s.io(env_[c]);
        s.io(keyCenter_[c]); s.io(keyScale_[c]);
    }
    s.io(primLodFrac_); s.io(primZ_); s.io(primDz_); s.io(k4_); s.io(k5_);
    s.io(scXh_); s.io(scYh_); s.io(scXl_); s.io(scYl_); s.io(noise_);
    if (S::reading) {
        setOtherModes(otherModes_);
        updateCombiner();
        syncInputs();
    }
}

template void Rdp::serialize<StateWriter>(StateWriter&);
template void Rdp::serialize<StateReader>(StateReader&);
