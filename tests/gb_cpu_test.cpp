#include <gtest/gtest.h>
#include "console/gb_console.h"
#include <cstdint>
#include <vector>

// ─── Стенд: программа по $0100, стек и (HL) — в рабочей памяти ───────────────
namespace {

std::vector<uint8_t> makeRom(const std::vector<uint8_t>& code)
{
    std::vector<uint8_t> rom(0x8000, 0x00);
    for (size_t i = 0; i < code.size(); ++i) rom[0x100 + i] = code[i];
    return rom;
}

struct Gb {
    GbConsole gb;
    explicit Gb(const std::vector<uint8_t>& code)
    {
        gb.loadROMData(makeRom(code));
        gb.cpu().sp = 0xDFF0;
        gb.cpu().h = 0xC0; gb.cpu().l = 0x00;          // HL → WRAM
    }
    GbCpu& cpu() { return gb.cpu(); }
    // Выполнить инструкцию и вернуть её длительность в M-циклах.
    int step()
    {
        uint64_t before = gb.bus().dots();
        gb.cpu().step();
        return (int)((gb.bus().dots() - before) / 4);
    }
};

// Эталонная таблица M-циклов (переход не взят); 0 — недопустимый опкод или
// особый случай (STOP, HALT, префикс CB), он проверяется отдельно.
constexpr uint8_t kCycles[256] = {
    1,3,2,2,1,1,2,1,5,2,2,2,1,1,2,1,
    0,3,2,2,1,1,2,1,3,2,2,2,1,1,2,1,
    2,3,2,2,1,1,2,1,2,2,2,2,1,1,2,1,
    2,3,2,2,3,3,3,1,2,2,2,2,1,1,2,1,
    1,1,1,1,1,1,2,1,1,1,1,1,1,1,2,1,
    1,1,1,1,1,1,2,1,1,1,1,1,1,1,2,1,
    1,1,1,1,1,1,2,1,1,1,1,1,1,1,2,1,
    2,2,2,2,2,2,0,2,1,1,1,1,1,1,2,1,
    1,1,1,1,1,1,2,1,1,1,1,1,1,1,2,1,
    1,1,1,1,1,1,2,1,1,1,1,1,1,1,2,1,
    1,1,1,1,1,1,2,1,1,1,1,1,1,1,2,1,
    1,1,1,1,1,1,2,1,1,1,1,1,1,1,2,1,
    2,3,3,4,3,4,2,4,2,4,3,0,3,6,2,4,
    2,3,3,0,3,4,2,4,2,4,3,0,3,0,2,4,
    3,3,2,0,0,4,2,4,4,1,4,0,0,0,2,4,
    3,3,2,1,0,4,2,4,3,2,4,1,0,0,2,4,
};

// Флаги, при которых условие опкода ЛОЖНО (для таблицы «не взят»).
uint8_t flagsNotTaken(uint8_t op)
{
    const int cc = (op >> 3) & 3;
    switch (cc) {
    case 0: return GbCpu::FZ;   // NZ ложно при Z=1
    case 1: return 0;           // Z  ложно при Z=0
    case 2: return GbCpu::FC;   // NC ложно при C=1
    default: return 0;          // C  ложно при C=0
    }
}

bool isConditional(uint8_t op)
{
    return op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38 ||   // JR cc
           (op >= 0xC0 && (op & 0xE7) == 0xC0) ||                     // RET cc
           (op >= 0xC0 && (op & 0xE7) == 0xC2) ||                     // JP cc
           (op >= 0xC0 && (op & 0xE7) == 0xC4);                       // CALL cc
}

} // namespace

// ─── Тайминг: все основные опкоды ────────────────────────────────────────────
TEST(GbCpu, Timing_AllBaseOpcodes)
{
    for (int op = 0; op < 256; ++op) {
        if (kCycles[op] == 0 || op == 0xCB) continue;
        Gb t({ (uint8_t)op, 0x00, 0xC0 });
        if (isConditional((uint8_t)op)) t.cpu().f = flagsNotTaken((uint8_t)op);
        EXPECT_EQ(t.step(), kCycles[op]) << "опкод " << std::hex << op;
    }
}

// Взятые условные переходы стоят дороже: JR 3, JP 4, CALL 6, RET 5.
TEST(GbCpu, Timing_TakenBranches)
{
    for (int op = 0; op < 256; ++op) {
        if (!isConditional((uint8_t)op)) continue;
        Gb t({ (uint8_t)op, 0x00, 0xC0 });
        t.cpu().f = (uint8_t)(flagsNotTaken((uint8_t)op) ^ (((op >> 3) & 3) < 2 ? GbCpu::FZ : GbCpu::FC));
        int want = (op < 0x40) ? 3 : ((op & 7) == 0 ? 5 : (op & 7) == 2 ? 4 : 6);
        EXPECT_EQ(t.step(), want) << "опкод " << std::hex << op;
    }
}

// ─── Тайминг: все опкоды с префиксом CB ──────────────────────────────────────
TEST(GbCpu, Timing_AllCbOpcodes)
{
    for (int op = 0; op < 256; ++op) {
        Gb t({ 0xCB, (uint8_t)op });
        int want = 2;
        if ((op & 7) == 6) want = ((op >> 6) == 1) ? 3 : 4;    // (HL): BIT 3, остальные 4
        EXPECT_EQ(t.step(), want) << "CB " << std::hex << op;
    }
}

// ─── Арифметика и флаги ──────────────────────────────────────────────────────
TEST(GbCpu, Alu_AddAdcFlags)
{
    Gb t({ 0x3E, 0x3A, 0xC6, 0xC6,     // LD A,$3A; ADD A,$C6 → 0, Z H C
           0x3E, 0x0F, 0xCE, 0x00 });  // LD A,$0F; ADC A,$00 (C=1) → $10, H
    t.step(); t.step();
    EXPECT_EQ(t.cpu().a, 0x00);
    EXPECT_EQ(t.cpu().f, GbCpu::FZ | GbCpu::FH | GbCpu::FC);
    t.step(); t.step();
    EXPECT_EQ(t.cpu().a, 0x10);
    EXPECT_EQ(t.cpu().f, GbCpu::FH);
}

TEST(GbCpu, Alu_SubSbcCpFlags)
{
    Gb t({ 0x3E, 0x3E, 0xD6, 0x3E,     // LD A,$3E; SUB $3E → 0: Z N
           0x3E, 0x10, 0xDE, 0x01,     // LD A,$10; SBC A,$01 (C=0) → $0F: N H
           0xFE, 0x20 });              // CP $20 → A не меняется: N C
    t.step(); t.step();
    EXPECT_EQ(t.cpu().a, 0x00);
    EXPECT_EQ(t.cpu().f, GbCpu::FZ | GbCpu::FN);
    t.step(); t.step();
    EXPECT_EQ(t.cpu().a, 0x0F);
    EXPECT_EQ(t.cpu().f, GbCpu::FN | GbCpu::FH);
    t.step();
    EXPECT_EQ(t.cpu().a, 0x0F);
    EXPECT_EQ(t.cpu().f, GbCpu::FN | GbCpu::FC);
}

TEST(GbCpu, Alu_LogicFlags)
{
    Gb t({ 0x3E, 0xF0, 0xE6, 0x0F,     // AND → 0: Z H
           0x3E, 0x55, 0xEE, 0x55,     // XOR → 0: Z
           0x3E, 0x00, 0xF6, 0x80 });  // OR  → $80
    t.step(); t.step(); EXPECT_EQ(t.cpu().f, GbCpu::FZ | GbCpu::FH);
    t.step(); t.step(); EXPECT_EQ(t.cpu().f, GbCpu::FZ);
    t.step(); t.step(); EXPECT_EQ(t.cpu().a, 0x80); EXPECT_EQ(t.cpu().f, 0);
}

TEST(GbCpu, IncDec_KeepCarry)
{
    Gb t({ 0x37, 0x06, 0x0F, 0x04,     // SCF; LD B,$0F; INC B → $10: H, C сохранён
           0x0E, 0x01, 0x0D });        // LD C,1; DEC C → 0: Z N, C сохранён
    t.step(); t.step(); t.step();
    EXPECT_EQ(t.cpu().b, 0x10);
    EXPECT_EQ(t.cpu().f, GbCpu::FH | GbCpu::FC);
    t.step(); t.step();
    EXPECT_EQ(t.cpu().c, 0x00);
    EXPECT_EQ(t.cpu().f, GbCpu::FZ | GbCpu::FN | GbCpu::FC);
}

TEST(GbCpu, Daa_AfterAddAndSub)
{
    Gb t({ 0x3E, 0x09, 0xC6, 0x08, 0x27,     // 09 + 08 = 11 → DAA → 17
           0x3E, 0x10, 0xD6, 0x01, 0x27,     // 10 - 01 = 0F → DAA → 09
           0x3E, 0x99, 0xC6, 0x01, 0x27 });  // 99 + 01 = 9A → DAA → 00, C
    t.step(); t.step(); t.step();
    EXPECT_EQ(t.cpu().a, 0x17);
    t.step(); t.step(); t.step();
    EXPECT_EQ(t.cpu().a, 0x09);
    t.step(); t.step(); t.step();
    EXPECT_EQ(t.cpu().a, 0x00);
    EXPECT_EQ(t.cpu().f & (GbCpu::FZ | GbCpu::FC), GbCpu::FZ | GbCpu::FC);
}

TEST(GbCpu, Add16_HalfCarryFromBit11)
{
    Gb t({ 0x21, 0xFF, 0x0F, 0x01, 0x01, 0x00, 0x09 });   // HL=$0FFF, BC=1, ADD HL,BC
    t.step(); t.step(); t.step();
    EXPECT_EQ(t.cpu().hl(), 0x1000);
    EXPECT_EQ(t.cpu().f & (GbCpu::FH | GbCpu::FC | GbCpu::FN), GbCpu::FH);
}

TEST(GbCpu, SpPlusSigned_FlagsFromLowByte)
{
    Gb t({ 0x31, 0xF8, 0xFF, 0xF8, 0x08,   // SP=$FFF8; LD HL,SP+8 → 0000: H C
           0xE8, 0xFF });                  // ADD SP,-1 → $FFF7
    t.step(); t.step();
    EXPECT_EQ(t.cpu().hl(), 0x0000);
    EXPECT_EQ(t.cpu().f, GbCpu::FH | GbCpu::FC);
    t.step();
    EXPECT_EQ(t.cpu().sp, 0xFFF7);
}

TEST(GbCpu, RotateA_ClearsZero)
{
    Gb t({ 0x3E, 0x80, 0x07 });            // RLCA: $80 → $01, C; Z всегда 0
    t.step(); t.step();
    EXPECT_EQ(t.cpu().a, 0x01);
    EXPECT_EQ(t.cpu().f, GbCpu::FC);
}

// Все 256 опкодов CB по результату и флагам (по регистру B).
TEST(GbCpu, CbOps_AllResultsAndFlags)
{
    const uint8_t v = 0xA5;
    for (int op = 0; op < 256; ++op) {
        if ((op & 7) != 0) continue;           // регистр B
        for (int carry = 0; carry < 2; ++carry) {
            Gb t({ 0xCB, (uint8_t)op });
            t.cpu().b = v;
            t.cpu().f = carry ? GbCpu::FC : 0;
            t.step();
            const int x = op >> 6, y = (op >> 3) & 7;
            uint8_t r = v, f = 0;
            bool c = false;
            if (x == 0) {
                switch (y) {
                case 0: c = v & 0x80; r = (uint8_t)((v << 1) | (v >> 7)); break;
                case 1: c = v & 1;    r = (uint8_t)((v >> 1) | (v << 7)); break;
                case 2: c = v & 0x80; r = (uint8_t)((v << 1) | carry); break;
                case 3: c = v & 1;    r = (uint8_t)((v >> 1) | (carry << 7)); break;
                case 4: c = v & 0x80; r = (uint8_t)(v << 1); break;
                case 5: c = v & 1;    r = (uint8_t)((v >> 1) | (v & 0x80)); break;
                case 6: c = false;    r = (uint8_t)((v >> 4) | (v << 4)); break;
                default: c = v & 1;   r = (uint8_t)(v >> 1); break;
                }
                f = (uint8_t)((r ? 0 : GbCpu::FZ) | (c ? GbCpu::FC : 0));
            } else if (x == 1) {
                f = (uint8_t)(GbCpu::FH | ((v >> y) & 1 ? 0 : GbCpu::FZ) | (carry ? GbCpu::FC : 0));
            } else if (x == 2) {
                r = (uint8_t)(v & ~(1 << y)); f = carry ? GbCpu::FC : 0;
            } else {
                r = (uint8_t)(v | (1 << y));  f = carry ? GbCpu::FC : 0;
            }
            EXPECT_EQ(t.cpu().b, r) << "CB " << std::hex << op << " C=" << carry;
            EXPECT_EQ(t.cpu().f, f) << "CB " << std::hex << op << " C=" << carry;
        }
    }
}

// LD r,r' — все 49 пар регистров (без (HL)).
TEST(GbCpu, LoadRegisterToRegister_All)
{
    for (int op = 0x40; op < 0x80; ++op) {
        const int dst = (op >> 3) & 7, src = op & 7;
        if (dst == 6 || src == 6) continue;
        Gb t({ (uint8_t)op });
        uint8_t* regs[8] = { &t.cpu().b, &t.cpu().c, &t.cpu().d, &t.cpu().e,
                             &t.cpu().h, &t.cpu().l, nullptr, &t.cpu().a };
        for (int i = 0; i < 8; ++i) if (regs[i]) *regs[i] = (uint8_t)(0x10 + i);
        t.step();
        EXPECT_EQ(*regs[dst], 0x10 + src) << "опкод " << std::hex << op;
    }
}

// ─── Стек, переходы, вызовы ──────────────────────────────────────────────────
TEST(GbCpu, PushPopAf_LowNibbleAlwaysZero)
{
    Gb t({ 0x01, 0xFF, 0x12, 0xC5, 0xF1 });   // BC=$12FF; PUSH BC; POP AF
    t.step(); t.step(); t.step();
    EXPECT_EQ(t.cpu().a, 0x12);
    EXPECT_EQ(t.cpu().f, 0xF0);
}

TEST(GbCpu, CallAndReturn)
{
    // $0100: CALL $0108; $0103: LD A,$55; ... $0108: LD A,$11; RET
    Gb t({ 0xCD, 0x08, 0x01, 0x3E, 0x55, 0x00, 0x00, 0x00, 0x3E, 0x11, 0xC9 });
    t.step();
    EXPECT_EQ(t.cpu().pc, 0x0108);
    EXPECT_EQ(t.cpu().sp, 0xDFEE);
    t.step(); t.step();
    EXPECT_EQ(t.cpu().pc, 0x0103);
    EXPECT_EQ(t.cpu().a, 0x11);
    t.step();
    EXPECT_EQ(t.cpu().a, 0x55);
}

TEST(GbCpu, RelativeJumpBackwards)
{
    Gb t({ 0x00, 0x18, 0xFD });                // NOP; JR -3 → $0100
    t.step(); t.step();
    EXPECT_EQ(t.cpu().pc, 0x0100);
}

TEST(GbCpu, LoadIncrementDecrementHL)
{
    Gb t({ 0x3E, 0x42, 0x22, 0x32, 0x2A });    // LD A,$42; LD (HL+),A; LD (HL-),A; LD A,(HL+)
    t.step(); t.step();
    EXPECT_EQ(t.cpu().hl(), 0xC001);
    t.step();
    EXPECT_EQ(t.cpu().hl(), 0xC000);
    t.step();
    EXPECT_EQ(t.cpu().a, 0x42);
    EXPECT_EQ(t.cpu().hl(), 0xC001);
}

// ─── Прерывания, EI, HALT ────────────────────────────────────────────────────
TEST(GbCpu, EiEnablesAfterNextInstruction)
{
    // EI; NOP; NOP — прерывание таймера уже висит и разрешено в IE.
    Gb t({ 0xFB, 0x00, 0x00 });
    t.gb.bus().write(0xFFFF, 0x04);
    t.gb.bus().write(0xFF0F, 0x04);
    t.step();                                  // EI
    EXPECT_FALSE(t.cpu().ime);
    t.step();                                  // NOP — после неё IME = 1
    EXPECT_TRUE(t.cpu().ime);
    EXPECT_EQ(t.step(), 5);                    // вызов обработчика — 5 M-циклов
    EXPECT_EQ(t.cpu().pc, 0x0050);
    EXPECT_FALSE(t.cpu().ime);
    EXPECT_EQ(t.gb.bus().read(0xFF0F) & 0x04, 0);
}

TEST(GbCpu, HaltWakesWithoutImeAndSkipsHandler)
{
    Gb t({ 0x76, 0x3E, 0x77 });                // HALT; LD A,$77
    t.gb.bus().write(0xFFFF, 0x04);
    t.step();
    EXPECT_TRUE(t.cpu().halted);
    t.gb.bus().write(0xFF0F, 0x04);            // пришло прерывание при IME = 0
    t.step();                                  // просыпаемся и сразу исполняем LD
    EXPECT_FALSE(t.cpu().halted);
    EXPECT_EQ(t.cpu().a, 0x77);
}

TEST(GbCpu, HaltBugReadsNextByteTwice)
{
    // IME=0 и прерывание уже висит: HALT не останавливает, а следующий байт
    // ($3C = INC A) исполняется дважды.
    Gb t({ 0x76, 0x3C, 0x00 });
    t.gb.bus().write(0xFFFF, 0x04);
    t.gb.bus().write(0xFF0F, 0x04);
    t.cpu().a = 0;
    t.step();
    EXPECT_FALSE(t.cpu().halted);
    t.step(); t.step();
    EXPECT_EQ(t.cpu().a, 2);
    EXPECT_EQ(t.cpu().pc, 0x0102);
}

// EI; HALT при висящем прерывании: обработчик вызывается нормально, а
// возврат — на сам HALT (первый байт обработчика не читается дважды).
TEST(GbCpu, EiHaltWithPendingInterruptReturnsToHalt)
{
    Gb t({ 0xFB, 0x76, 0x3C });                // EI; HALT; INC A
    t.gb.bus().write(0xFFFF, 0x04);
    t.gb.bus().write(0xFF0F, 0x04);
    t.step();                                  // EI
    t.step();                                  // HALT: IME ещё 0, прерывание висит
    t.step();                                  // вызов обработчика
    EXPECT_EQ(t.cpu().pc, 0x0050);
    EXPECT_EQ(t.gb.bus().read(0xDFEE), 0x01);  // адрес возврата $0101 — сам HALT
    EXPECT_EQ(t.gb.bus().read(0xDFEF), 0x01);
    t.step();                                  // первый байт обработчика — один раз
    EXPECT_EQ(t.cpu().pc, 0x0051);
}

TEST(GbCpu, IllegalOpcodeLocksCpu)
{
    Gb t({ 0xD3, 0x3C });
    t.step();
    EXPECT_TRUE(t.cpu().locked);
    uint8_t a = t.cpu().a;
    t.step();
    EXPECT_EQ(t.cpu().a, a);
}

TEST(GbCpu, CgbBootRegistersSignalColorHardware)
{
    std::vector<uint8_t> rom = makeRom({ 0x00 });
    rom[0x143] = 0x80;                         // игра поддерживает цвет
    GbConsole gb;
    ASSERT_TRUE(gb.loadROMData(rom));
    EXPECT_TRUE(gb.isColor());
    EXPECT_EQ(gb.cpu().a, 0x11);               // так игры узнают GBC
}
