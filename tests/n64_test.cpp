#include <gtest/gtest.h>
#include "n64/n64_system.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
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
