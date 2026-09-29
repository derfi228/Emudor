#include <gtest/gtest.h>
#include "console/n64_console.h"
#include "n64/n64_system.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <vector>

// ─── Синтетический картридж: код лежит на месте IPL3 (ПЗУ 0x40) ───────────────
// HLE-загрузчик PIF копирует его в DMEM и прыгает на 0xA4000040.
namespace {

enum Reg { R0 = 0, AT = 1, V0 = 2, V1 = 3, A0 = 4, A1 = 5, A2 = 6, A3 = 7,
           T0 = 8, T1 = 9, T2 = 10, T3 = 11, T4 = 12, T5 = 13, T6 = 14, T7 = 15,
           S0 = 16, S1 = 17, S2 = 18, S3 = 19, K0 = 26, K1 = 27, RA = 31 };

struct Asm {
    std::vector<uint32_t> w;
    void op(uint32_t v) { w.push_back(v); }
    void r(uint32_t funct, int rs, int rt, int rd, int sa = 0)
    { op(((uint32_t)rs << 21) | ((uint32_t)rt << 16) | ((uint32_t)rd << 11) | ((uint32_t)sa << 6) | funct); }
    void i(uint32_t o, int rs, int rt, uint32_t imm)
    { op((o << 26) | ((uint32_t)rs << 21) | ((uint32_t)rt << 16) | (imm & 0xFFFF)); }
    void li(int rt, uint32_t v) { i(0x0F, 0, rt, v >> 16); i(0x0D, rt, rt, v & 0xFFFF); }
    void nop() { op(0); }
    void sw(int rt, int16_t off, int base) { i(0x2B, base, rt, (uint16_t)off); }
    void sd(int rt, int16_t off, int base) { i(0x3F, base, rt, (uint16_t)off); }
    void lw(int rt, int16_t off, int base) { i(0x23, base, rt, (uint16_t)off); }
    void halt() { i(0x04, 0, 0, 0xFFFF); nop(); }        // beq r0,r0,. — стоп
};

struct Rig {
    std::unique_ptr<N64System> sys = std::make_unique<N64System>();
    Asm a;
    std::vector<uint8_t> rom = std::vector<uint8_t>(0x101000, 0);

    Rig() { put(0, 0x80371240); rom[0x3E] = 'E'; }
    void put(uint32_t off, uint32_t v) { N64System::put32(&rom[off], v); }
    void boot()
    {
        for (size_t k = 0; k < a.w.size(); ++k) put(0x40 + (uint32_t)k * 4, a.w[k]);
        ASSERT_TRUE(sys->loadRom(rom));
    }
    void run(int frames = 1) { for (int f = 0; f < frames; ++f) sys->runFrame(); }
    uint32_t mem32(uint32_t pa) const { return N64System::get32(sys->rdram() + pa); }
    uint64_t mem64(uint32_t pa) const { return ((uint64_t)mem32(pa) << 32) | mem32(pa + 4); }
};

constexpr uint32_t OUT = 0xA0100000;     // результаты — в RDRAM по 0x100000 (некэшируемо)

} // namespace

// ─── Процессор ────────────────────────────────────────────────────────────────
TEST(N64Cpu, ArithmeticSignExtendsAndDivideByZero)
{
    Rig g;
    g.a.li(S0, OUT);
    g.a.li(T0, 0x7FFFFFFF);
    g.a.i(0x09, T0, T1, 1);                  // addiu t1,t0,1 → 0xFFFFFFFF80000000
    g.a.sd(T1, 0, S0);
    g.a.li(T2, 5);
    g.a.r(0x1A, T2, R0, 0);                  // div t2,r0
    g.a.r(0x12, 0, 0, T3);                   // mflo t3 → -1
    g.a.r(0x10, 0, 0, T4);                   // mfhi t4 → 5
    g.a.sd(T3, 8, S0);
    g.a.sd(T4, 16, S0);
    g.a.li(T5, 0xFFFFFFFF);
    g.a.r(0x19, T5, T5, 0);                  // multu → 0xFFFFFFFE00000001
    g.a.r(0x10, 0, 0, T6);
    g.a.r(0x12, 0, 0, T7);
    g.a.sd(T6, 24, S0);
    g.a.sd(T7, 32, S0);
    g.a.halt();
    g.boot();
    g.run();
    EXPECT_EQ(g.mem64(0x100000), 0xFFFFFFFF80000000ull);
    EXPECT_EQ(g.mem64(0x100008), ~0ull);
    EXPECT_EQ(g.mem64(0x100010), 5ull);
    EXPECT_EQ(g.mem64(0x100018), 0xFFFFFFFFFFFFFFFEull);   // hi = sext(0xFFFFFFFE)
    EXPECT_EQ(g.mem64(0x100020), 1ull);
}

TEST(N64Cpu, UnalignedLoadsAndStores)
{
    Rig g;
    g.a.li(S0, OUT);
    g.a.li(T0, 0x11223344); g.a.sw(T0, 0, S0);
    g.a.li(T0, 0x55667788); g.a.sw(T0, 4, S0);
    g.a.li(T1, 0);
    g.a.i(0x22, S0, T1, 1);                  // lwl t1,1(s0)
    g.a.i(0x26, S0, T1, 4);                  // lwr t1,4(s0) → 0x22334455
    g.a.sw(T1, 8, S0);
    g.a.li(T2, 0xAABBCCDD);
    g.a.i(0x2A, S0, T2, 13);                 // swl t2,13(s0): байты 13..15 ← AA BB CC
    g.a.sw(R0, 16, S0);
    g.a.i(0x2E, S0, T2, 17);                 // swr t2,17(s0): байты 16..17 ← CC DD
    g.a.halt();
    g.boot();
    g.run();
    EXPECT_EQ(g.mem32(0x100008), 0x22334455u);
    EXPECT_EQ(g.mem32(0x10000C) & 0x00FFFFFF, 0x00AABBCCu);
    EXPECT_EQ(g.mem32(0x100010), 0xCCDD0000u);
}

TEST(N64Cpu, BranchLikelyNullifiesDelaySlot)
{
    Rig g;
    g.a.li(S0, OUT);
    g.a.li(T0, 0);
    g.a.i(0x15, R0, R0, 1);                  // bnel r0,r0 (не взят) — слот не исполняется
    g.a.i(0x09, T0, T0, 1);                  //   addiu t0,1
    g.a.i(0x05, R0, R0, 1);                  // bne r0,r0 (не взят) — слот исполняется
    g.a.i(0x09, T0, T0, 2);                  //   addiu t0,2
    g.a.sw(T0, 0, S0);
    g.a.halt();
    g.boot();
    g.run();
    EXPECT_EQ(g.mem32(0x100000), 2u);
}

// SYSCALL в слоте задержки: EPC — адрес перехода, BD=1, вектор 0x80000180.
TEST(N64Cpu, ExceptionInDelaySlotSetsBdAndEpc)
{
    Rig g;
    g.a.li(S0, OUT);
    g.a.i(0x04, R0, R0, 2);                  // beq r0,r0,+2 (адрес 0xA4000048)
    g.a.r(0x0C, 0, 0, 0);                    //   syscall в слоте
    g.a.nop();
    g.a.halt();
    g.boot();
    // Обработчик: сохранить EPC и Cause, стоп.
    Asm h;
    h.op(0x40000000 | (K0 << 16) | (14 << 11));   // mfc0 k0, EPC
    h.op(0x40000000 | (K1 << 16) | (13 << 11));   // mfc0 k1, Cause
    h.sw(K0, 0, S0);
    h.sw(K1, 4, S0);
    h.halt();
    for (size_t k = 0; k < h.w.size(); ++k) N64System::put32(g.sys->rdram() + 0x180 + k * 4, h.w[k]);
    g.run();
    EXPECT_EQ(g.mem32(0x100000), 0xA4000048u);
    EXPECT_EQ(g.mem32(0x100004) & 0x8000007C, 0x80000000u | (8u << 2));
}

TEST(N64Cpu, FpuAddCompareAndConvert)
{
    Rig g;
    g.a.li(S0, OUT);
    g.a.li(T0, 0x3FC00000);                  // 1.5f
    g.a.op(0x44800000 | (T0 << 16) | (0 << 11));   // mtc1 t0,f0
    g.a.op(0x44800000 | (T0 << 16) | (1 << 11));   // mtc1 t0,f1
    g.a.op(0x46000000 | (1 << 16) | (0 << 11) | (2 << 6) | 0x00);   // add.s f2,f0,f1 → 3.0
    g.a.op(0x46000000 | (2 << 11) | (3 << 6) | 0x0D);               // trunc.w.s f3,f2 → 3
    g.a.op(0x46000000 | (2 << 16) | (0 << 11) | 0x3C);              // c.lt.s f0,f2 → true
    g.a.op(0x44000000 | (T1 << 16) | (2 << 11));   // mfc1 t1,f2
    g.a.op(0x44000000 | (T2 << 16) | (3 << 11));   // mfc1 t2,f3
    g.a.op(0x44400000 | (T3 << 16) | (31 << 11));  // cfc1 t3,fcr31
    g.a.sw(T1, 0, S0);
    g.a.sw(T2, 4, S0);
    g.a.sw(T3, 8, S0);
    g.a.halt();
    g.boot();
    g.run();
    EXPECT_EQ(g.mem32(0x100000), 0x40400000u);
    EXPECT_EQ(g.mem32(0x100004), 3u);
    EXPECT_TRUE(g.mem32(0x100008) & (1u << 23));
}

// Промах TLB при 64-битной адресации ядра (так оставляет PIF) — вектор XTLB.
TEST(N64Cpu, TlbMissUsesXtlbVector)
{
    Rig g;
    g.a.li(S0, OUT);
    g.a.li(T0, 0x00000010);
    g.a.lw(T1, 0, T0);
    g.a.halt();
    g.boot();
    Asm h;
    h.li(T2, 0x0080CAFE);
    h.sw(T2, 0, S0);
    h.halt();
    for (size_t k = 0; k < h.w.size(); ++k) N64System::put32(g.sys->rdram() + 0x80 + k * 4, h.w[k]);
    g.run();
    EXPECT_EQ(g.mem32(0x100000), 0x0080CAFEu);
}

// ─── Шина ─────────────────────────────────────────────────────────────────────
TEST(N64System, DetectsRegionCicAndByteOrder)
{
    std::vector<uint8_t> rom(0x2000, 0);
    N64System::put32(&rom[0], 0x80371240);
    rom[0x3B] = 'N'; rom[0x3C] = 'Z'; rom[0x3D] = 'L'; rom[0x3E] = 'P';
    // .v64: байты попарно переставлены
    std::vector<uint8_t> v64 = rom;
    for (size_t k = 0; k < v64.size(); k += 2) std::swap(v64[k], v64[k + 1]);
    auto sys = std::make_unique<N64System>();
    ASSERT_TRUE(sys->loadRom(v64));
    EXPECT_EQ(sys->gameCode(), "NZLP");
    EXPECT_EQ(sys->tvType(), N64System::Tv::PAL);
    EXPECT_EQ(sys->saveType(), N64System::Save::Sram);
    EXPECT_EQ(sys->cpu.gpr[20], 0u);                  // s4 = PAL для IPL3
    EXPECT_EQ(N64System::get32(sys->rdram() + 0x318), N64System::RDRAM_SIZE);
}

TEST(N64System, PiDmaCopiesRomAndRaisesInterrupt)
{
    Rig g;
    for (int k = 0; k < 16; ++k) g.rom[0x2000 + k] = (uint8_t)(0xA0 + k);
    g.a.li(S0, 0xA4600000);                  // PI
    g.a.li(T0, 0x00200000); g.a.sw(T0, 0x00, S0);   // PI_DRAM_ADDR
    g.a.li(T0, 0x10002000); g.a.sw(T0, 0x04, S0);   // PI_CART_ADDR
    g.a.li(T0, 15);         g.a.sw(T0, 0x0C, S0);   // PI_WR_LEN → 16 байт
    g.a.lw(T1, 0x10, S0);                    // PI_STATUS: занят
    g.a.li(S1, OUT);
    g.a.sw(T1, 0, S1);
    g.a.halt();
    g.boot();
    g.run();
    EXPECT_EQ(g.mem32(0x100000) & 1, 1u);
    EXPECT_EQ(g.mem32(0x200000), 0xA0A1A2A3u);
    EXPECT_EQ(g.mem32(0x20000C), 0xACADAEAFu);
    EXPECT_EQ(g.sys->read32(0x04300008) & N64System::MI_PI, N64System::MI_PI);
}

// Джойбас: запрос «прочитать кнопки» для порта 0, ответ после DMA из PIF.
TEST(N64System, JoybusReadsController)
{
    Rig g;
    const uint8_t block[64] = { 0xFF, 0x01, 0x04, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE };
    for (int k = 0; k < 64; k += 4) g.put(0x3000 + k, N64System::get32(&block[k]));
    g.rom[0x3000 + 63] = 0x01;               // выполнить команды
    g.a.li(S0, 0xA4600000);                  // PI: блок из ПЗУ в RDRAM 0x300000
    g.a.li(T0, 0x00300000); g.a.sw(T0, 0x00, S0);
    g.a.li(T0, 0x10003000); g.a.sw(T0, 0x04, S0);
    g.a.li(T0, 63);         g.a.sw(T0, 0x0C, S0);
    g.a.li(S1, 0xA4800000);                  // SI
    g.a.li(T0, 0x00300000); g.a.sw(T0, 0x00, S1);
    g.a.li(T0, 0x1FC007C0); g.a.sw(T0, 0x10, S1);   // RDRAM → PIF
    g.a.li(T0, 0x00300040); g.a.sw(T0, 0x00, S1);
    g.a.li(T0, 0x1FC007C0); g.a.sw(T0, 0x04, S1);   // PIF → RDRAM
    g.a.halt();
    g.boot();
    g.sys->setController(0, 0x9000, 40, -20);       // A + Start, стик
    g.run();
    EXPECT_EQ(g.mem32(0x300040 + 4), 0x90002800u | (uint8_t)-20);
}

TEST(N64System, ViShows16BitFramebuffer)
{
    Rig g;
    g.a.li(S0, 0xA4400000);                  // VI: 320×240, 16 бит
    g.a.li(T0, 0x00003002); g.a.sw(T0, 0x00, S0);   // CTRL: тип 2
    g.a.li(T0, 0x00100000); g.a.sw(T0, 0x04, S0);   // ORIGIN
    g.a.li(T0, 320);        g.a.sw(T0, 0x08, S0);   // WIDTH
    g.a.li(T0, 0x006C02EC); g.a.sw(T0, 0x24, S0);   // H_VIDEO (640 точек)
    g.a.li(T0, 0x002501FF); g.a.sw(T0, 0x28, S0);   // V_VIDEO (237 строк)
    g.a.li(T0, 0x00000200); g.a.sw(T0, 0x30, S0);   // X_SCALE 0.5
    g.a.li(T0, 0x00000400); g.a.sw(T0, 0x34, S0);   // Y_SCALE 1.0
    g.a.li(S1, 0xA0100000);
    g.a.li(T0, 0xF801F801); g.a.sw(T0, 0, S1);      // два красных пикселя
    g.a.halt();
    g.boot();
    g.run(2);
    EXPECT_EQ(g.sys->frameWidth(), 320);
    EXPECT_EQ(g.sys->frameHeight(), 237);
    EXPECT_EQ(g.sys->frame()[0], 0xFFFF0000u);
    EXPECT_EQ(g.sys->frame()[1], 0xFFFF0000u);
    EXPECT_EQ(g.sys->frame()[2], 0xFF000000u);
}

// ─── RSP ──────────────────────────────────────────────────────────────────────
// Программа в IMEM: два вектора из DMEM → VADD и VMULF → обратно в DMEM, BREAK.
TEST(N64Rsp, VectorAddMultiplyAndBreak)
{
    Rig g;
    g.a.halt();
    g.boot();
    uint8_t* dmem = g.sys->spMem();
    const int16_t a[8] = { 100, -200, 32767, -32768, 0x4000, 0x4000, 1, 0 };
    const int16_t b[8] = { 50, 50, 1, -1, 0x4000, -0x4000, 0, 0 };
    for (int n = 0; n < 8; ++n) {
        dmem[n * 2] = (uint8_t)(a[n] >> 8); dmem[n * 2 + 1] = (uint8_t)a[n];
        dmem[16 + n * 2] = (uint8_t)(b[n] >> 8); dmem[16 + n * 2 + 1] = (uint8_t)b[n];
    }
    const uint32_t prog[] = {
        0xC8012000,          // lqv  $v1, 0x00($0)
        0xC8022001,          // lqv  $v2, 0x10($0)
        0x4A0208D0,          // vadd $v3, $v1, $v2
        0x4A020900,          // vmulf $v4, $v1, $v2
        0xE8032002,          // sqv  $v3, 0x20($0)
        0xE8042003,          // sqv  $v4, 0x30($0)
        0x0000000D,          // break
        0x00000000,
    };
    for (int k = 0; k < 8; ++k) N64System::put32(dmem + 0x1000 + k * 4, prog[k]);
    g.sys->write32(0x04300000 + 0x0C, 0x2);                    // MI: разрешить прерывание SP
    g.sys->write32(0x04080000, 0);                             // SP_PC
    g.sys->write32(0x04040010, 0x1 | 0x100);                   // пуск + прерывание по BREAK
    g.run();
    auto lane = [&](uint32_t off, int n) { return (int16_t)(dmem[off + n * 2] << 8 | dmem[off + n * 2 + 1]); };
    const int16_t add[8] = { 150, -150, 32767, -32768, 32767, 0, 1, 0 };
    const int16_t mul[8] = { 0, 0, 1, 1, 0x2000, -0x2000, 0, 0 };
    for (int n = 0; n < 8; ++n) {
        EXPECT_EQ(lane(0x20, n), add[n]) << "vadd " << n;
        EXPECT_EQ(lane(0x30, n), mul[n]) << "vmulf " << n;
    }
    EXPECT_TRUE(g.sys->spHalted());
    EXPECT_EQ(g.sys->read32(0x04040010) & 0x3, 0x3u);          // halt + broke
    EXPECT_EQ(g.sys->read32(0x04300008) & N64System::MI_SP, N64System::MI_SP);
}

// ─── RDP ──────────────────────────────────────────────────────────────────────
namespace {

struct DisplayList {
    std::vector<uint64_t> w;
    void cmd(uint64_t v) { w.push_back(v); }
    void colorImage(uint32_t addr, uint32_t width) { cmd(0x3Full << 56 | 2ull << 51 | (uint64_t)(width - 1) << 32 | addr); }
    void scissor(uint32_t xl, uint32_t yl) { cmd(0x2Dull << 56 | (uint64_t)(xl << 2) << 12 | (yl << 2)); }
    void otherModes(uint64_t m) { cmd(0x2Full << 56 | m); }
    void rect(uint32_t cmdId, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, int tile = 0)
    {
        cmd((uint64_t)cmdId << 56 | (uint64_t)(x1 << 2) << 44 | (uint64_t)(y1 << 2) << 32 | (uint64_t)tile << 24 |
            (uint64_t)(x0 << 2) << 12 | (y0 << 2));
    }
    // Комбайнер: оба такта одинаковы, результат — вход D (addRgb/addAlpha)
    void combineAdd(uint32_t addRgb, uint32_t addAlpha)
    {
        uint64_t m = 0x3Cull << 56;
        m |= 15ull << 52 | 31ull << 47 | 7ull << 44 | 7ull << 41 | 15ull << 37 | 31ull << 32;
        m |= 15ull << 28 | 15ull << 24 | 7ull << 21 | 7ull << 18 | 7ull << 12 | 7ull << 3;
        m |= (uint64_t)addRgb << 15 | (uint64_t)addAlpha << 9 | (uint64_t)addRgb << 6 | addAlpha;
        cmd(m);
    }
    void syncFull() { cmd(0x29ull << 56); }
};

// Список в RDRAM по 0x200000 и запуск RDP записью DPC_START/DPC_END.
void runList(Rig& g, const DisplayList& dl)
{
    for (size_t k = 0; k < dl.w.size(); ++k) {
        N64System::put32(g.sys->rdram() + 0x200000 + k * 8, (uint32_t)(dl.w[k] >> 32));
        N64System::put32(g.sys->rdram() + 0x200000 + k * 8 + 4, (uint32_t)dl.w[k]);
    }
    g.sys->write32(0x04100000, 0x200000);
    g.sys->write32(0x04100004, 0x200000 + (uint32_t)dl.w.size() * 8);
    g.sys->rdpFlush();                                          // RDP рисует в своём потоке
}

uint16_t pixel16(Rig& g, uint32_t base, uint32_t width, int x, int y)
{
    const uint8_t* p = g.sys->rdram() + base + ((uint32_t)y * width + (uint32_t)x) * 2;
    return (uint16_t)(p[0] << 8 | p[1]);
}

} // namespace

// Режим заливки: прямоугольник включает правый и нижний край.
TEST(N64Rdp, FillRectangleAndSyncFullInterrupt)
{
    Rig g;
    g.a.halt();
    g.boot();
    DisplayList dl;
    dl.colorImage(0x100000, 64);
    dl.scissor(64, 64);
    dl.otherModes(3ull << 52);                                  // заливка
    dl.cmd(0x37ull << 56 | 0xF801F801u);                        // красный
    dl.rect(0x36, 0, 0, 15, 7);
    dl.syncFull();
    runList(g, dl);
    EXPECT_EQ(pixel16(g, 0x100000, 64, 0, 0), 0xF801);
    EXPECT_EQ(pixel16(g, 0x100000, 64, 15, 7), 0xF801);
    EXPECT_EQ(pixel16(g, 0x100000, 64, 16, 0), 0x0000);
    EXPECT_EQ(pixel16(g, 0x100000, 64, 0, 8), 0x0000);
    EXPECT_EQ(g.sys->read32(0x04300008) & N64System::MI_DP, 0u);   // прерывание — чуть позже
    g.run();
    EXPECT_EQ(g.sys->read32(0x04300008) & N64System::MI_DP, N64System::MI_DP);
}

// Плоский треугольник цвета prim: вершины (0,0), (32,0), (0,32); главное ребро слева.
TEST(N64Rdp, FlatTriangleUsesPrimColor)
{
    Rig g;
    g.a.halt();
    g.boot();
    DisplayList dl;
    dl.colorImage(0x100000, 64);
    dl.scissor(64, 64);
    dl.otherModes(0);                                           // 1 такт, без Z и смешивания
    dl.combineAdd(3, 3);                                        // цвет = prim
    dl.cmd(0x3Aull << 56 | 0x00FF00FFu);                        // prim: зелёный
    dl.cmd(0x08ull << 56 | 1ull << 55 | (uint64_t)(32 << 2) << 32);
    dl.cmd((uint64_t)(32u << 16) << 32 | 0xFFFF0000u);          // XL=32, dXL/dy=−1
    dl.cmd(0);                                                  // XH=0, dXH/dy=0
    dl.cmd((uint64_t)(32u << 16) << 32);                        // XM=32
    dl.syncFull();
    runList(g, dl);
    EXPECT_EQ(pixel16(g, 0x100000, 64, 5, 5), 0x07C1);
    EXPECT_EQ(pixel16(g, 0x100000, 64, 30, 0), 0x07C1);
    EXPECT_EQ(pixel16(g, 0x100000, 64, 20, 20), 0x0000);
    EXPECT_EQ(pixel16(g, 0x100000, 64, 31, 1), 0x0000);
}

// Текстура 4×4 RGBA16 через Load Tile и текстурный прямоугольник 1:1.
TEST(N64Rdp, TextureRectangleSamplesLoadedTile)
{
    Rig g;
    g.a.halt();
    g.boot();
    const uint16_t tex[4] = { 0xF801, 0x07C1, 0x003F, 0xFFFF };
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            uint8_t* p = g.sys->rdram() + 0x120000 + (y * 4 + x) * 2;
            p[0] = (uint8_t)(tex[(x + y) & 3] >> 8); p[1] = (uint8_t)tex[(x + y) & 3];
        }
    DisplayList dl;
    dl.colorImage(0x100000, 64);
    dl.scissor(64, 64);
    dl.otherModes(0);
    dl.combineAdd(1, 1);                                        // цвет = texel0
    dl.cmd(0x3Dull << 56 | 2ull << 51 | 3ull << 32 | 0x120000); // текстура RGBA16, ширина 4
    dl.cmd(0x35ull << 56 | 2ull << 51 | 1ull << 41);            // тайл 0: RGBA16, строка 1 слово
    dl.cmd(0x34ull << 56 | (3u << 2) << 12 | (3u << 2));        // Load Tile (0,0)-(3,3): угол (0,0) — в старших полях
    dl.rect(0x24, 0, 0, 4, 4);                                  // Texture Rectangle
    dl.cmd((uint64_t)(1u << 10) << 16 | (1u << 10));            // S=T=0, dS/dx=dT/dy=1
    dl.syncFull();
    runList(g, dl);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            EXPECT_EQ(pixel16(g, 0x100000, 64, x, y), tex[(x + y) & 3]) << x << "," << y;
    EXPECT_EQ(pixel16(g, 0x100000, 64, 4, 0), 0x0000);
}

// ─── Save state ───────────────────────────────────────────────────────────────
namespace {

// Картридж: бесконечный счётчик в RDRAM 0x100000.
std::vector<uint8_t> counterRom(uint32_t salt)
{
    Asm a;
    a.li(S0, OUT);
    a.li(T0, salt);
    a.sw(T0, 0, S0);                         // 0: salt
    a.i(0x09, T0, T0, 1);                    // addiu t0,t0,1
    a.sw(T0, 0, S0);
    a.i(0x04, 0, 0, (uint16_t)-3);           // beq r0,r0,-3
    a.nop();
    std::vector<uint8_t> rom(0x101000, 0);
    N64System::put32(&rom[0], 0x80371240);
    for (size_t k = 0; k < a.w.size(); ++k) N64System::put32(&rom[0x40 + k * 4], a.w[k]);
    return rom;
}

} // namespace

TEST(N64State, SaveLoadContinuesExactly)
{
    auto con = std::make_unique<N64Console>();
    ASSERT_TRUE(con->loadROMData(counterRom(0)));
    for (int f = 0; f < 2; ++f) con->runFrame();
    std::stringstream st;
    ASSERT_TRUE(con->saveState(st));
    for (int f = 0; f < 3; ++f) con->runFrame();
    const uint32_t expected = N64System::get32(con->system().rdram() + 0x100000);
    ASSERT_TRUE(con->loadState(st));
    for (int f = 0; f < 3; ++f) con->runFrame();
    EXPECT_EQ(N64System::get32(con->system().rdram() + 0x100000), expected);
}

TEST(N64State, RejectsStateOfAnotherGame)
{
    auto a = std::make_unique<N64Console>();
    auto b = std::make_unique<N64Console>();
    ASSERT_TRUE(a->loadROMData(counterRom(0)));
    ASSERT_TRUE(b->loadROMData(counterRom(12345)));
    a->runFrame();
    b->runFrame();
    std::stringstream st;
    ASSERT_TRUE(a->saveState(st));
    const uint32_t before = N64System::get32(b->system().rdram() + 0x100000);
    EXPECT_FALSE(b->loadState(st));
    EXPECT_EQ(N64System::get32(b->system().rdram() + 0x100000), before);
}
