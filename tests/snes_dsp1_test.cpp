#include <gtest/gtest.h>
#include "snes/snes_dsp1.h"
#include <cmath>
#include <cstdint>
#include <vector>

// ─── Обмен с DSP-1 как у CPU: команда, слова параметров, слова результата ────
namespace {

std::vector<int16_t> run(SnesDSP1& dsp, uint8_t cmd, std::vector<int16_t> in, int outWords)
{
    dsp.writeDR(cmd);
    for (int16_t w : in) {
        dsp.writeDR((uint8_t)(w & 0xFF));
        dsp.writeDR((uint8_t)((uint16_t)w >> 8));
    }
    std::vector<int16_t> out;
    for (int i = 0; i < outWords; ++i) {
        uint8_t lo = dsp.readDR();
        uint8_t hi = dsp.readDR();
        out.push_back((int16_t)(lo | (hi << 8)));
    }
    return out;
}

constexpr int16_t kAzs = 13312;                       // ~73°, как у Mario Kart
const double kPi = 3.14159265358979323846;
double rad(int16_t a) { return a * (2.0 * kPi / 65536.0); }

} // namespace

// Parameter: при Fz = 0 центр экрана смотрит ровно в F, горизонт выше центра
// на Les·ctg(Azs).
TEST(SnesDSP1, Parameter_CentreIsBasePointAndHorizonAbove)
{
    SnesDSP1 dsp; dsp.reset();
    auto r = run(dsp, 0x02, { 3808, 3000, 0, 256, 256, 192, kAzs }, 4);
    EXPECT_EQ(r[0], 0);                                // Vof
    double vva = -256.0 * std::cos(rad(kAzs)) / std::sin(rad(kAzs));
    EXPECT_NEAR(r[1], vva, 1.0);                       // Vva
    EXPECT_NEAR(r[2], 3808, 1.0);                      // Cx
    EXPECT_NEAR(r[3], 3000, 1.0);                      // Cy
}

// Raster: в центре экрана масштаб Lfe/Les, по вертикали — делённый на cos(Azs).
// Mario Kart строит таблицу матриц камерой Lfe = 64, Les = 256 → 0.25.
TEST(SnesDSP1, Raster_CentreScaleIsLfeOverLes)
{
    SnesDSP1 dsp; dsp.reset();
    run(dsp, 0x02, { 0, 0, 0, 64, 256, 0, kAzs }, 4);
    auto m = run(dsp, 0x0A, { 0 }, 4);
    EXPECT_NEAR(m[0], 64, 1);                          // An = 0.25 в 8.8
    EXPECT_NEAR(m[1], 0, 1);                           // Bn
    EXPECT_NEAR(m[2], 0, 1);                           // Cn
    EXPECT_NEAR(m[3], 64.0 / std::cos(rad(kAzs)), 1);  // Dn
}

// Project: точка, куда смотрит камера, — в центре экрана с масштабом Les/Lfe;
// точка впереди — выше центра и меньше.
TEST(SnesDSP1, Project_BasePointAtCentreAndFarPointHigher)
{
    SnesDSP1 dsp; dsp.reset();
    run(dsp, 0x02, { 3808, 3024, 0, 256, 256, 0, kAzs }, 4);
    auto p = run(dsp, 0x06, { 3808, 3024, 0 }, 3);
    EXPECT_EQ(p[0], 0);
    EXPECT_EQ(p[1], 0);
    EXPECT_NEAR(p[2], 256, 1);

    // При Aas = 0 камера смотрит в сторону уменьшения Y.
    auto q = run(dsp, 0x06, { 3808, 2832, 0 }, 3);
    EXPECT_EQ(q[0], 0);
    EXPECT_LT(q[1], -20);
    EXPECT_LT(q[2], 256);
    EXPECT_GT(q[2], 0);
}

// Спрайт и земля от одной камеры сходятся: карт в 192 единицах впереди
// проецируется на ту же строку, где таблица (Lfe = 64, мир = 4 тексела)
// показывает землю в 48 текселях впереди.
TEST(SnesDSP1, ProjectMatchesRasterGround)
{
    SnesDSP1 dsp; dsp.reset();
    run(dsp, 0x02, { 3808, 3024, 0, 256, 256, 0, kAzs }, 4);
    int16_t v = run(dsp, 0x06, { 3808, 3024 - 192, 0 }, 3)[1];

    run(dsp, 0x02, { 0, 0, 0, 64, 256, 0, kAzs }, 4);
    int16_t d = run(dsp, 0x0A, { v }, 4)[3];
    double groundTexels = d / 256.0 * v;               // смещение строки по карте
    EXPECT_NEAR(groundTexels, -48.0, 2.0);
}
