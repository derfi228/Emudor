#include <gtest/gtest.h>
#include "snes/superfx.h"
#include <vector>
#include <cstdint>

// ─── Стенд: ПЗУ с программой по $00:8000 (для GSU это смещение 0) ─────────────
namespace {

struct Gsu {
    std::vector<uint8_t> rom = std::vector<uint8_t>(0x20000, 0x01);  // NOP-заливка
    SuperFX fx;

    explicit Gsu(const std::vector<uint8_t>& code, uint32_t at = 0)
    {
        for (size_t i = 0; i < code.size(); ++i) rom[at + i] = code[i];
        fx.connect(&rom, 32 * 1024);
    }
    // Запуск как у CPU: адрес в R15, запись старшего байта ставит GO.
    void start(uint16_t pc = 0x8000)
    {
        fx.writeIO(0x301E, (uint8_t)pc);
        fx.writeIO(0x301F, (uint8_t)(pc >> 8));
        fx.run(1000000);
    }
};

} // namespace

// Команда сразу после перехода (delay slot) исполняется до прыжка.
TEST(SuperFX, Branch_DelaySlotExecutes)
{
    Gsu g({
        0xA1, 0x05,   // 8000 IBT R1,#5
        0x05, 0x02,   // 8002 BRA $8006
        0xD1,         // 8004 INC R1   ← delay slot, исполняется
        0xD1,         // 8005 INC R1   ← перепрыгнули
        0xD1,         // 8006 INC R1
        0x00, 0x01,   // 8007 STOP
    });
    g.start();
    EXPECT_FALSE(g.fx.running());
    EXPECT_EQ(g.fx.dbgR(1), 7);
}

// BLT берёт переход при S != OV (1 - 2 < 0), BGE — нет.
TEST(SuperFX, Branch_LessThanAfterCompare)
{
    Gsu g({
        0xA1, 0x01,   // 8000 IBT R1,#1
        0xA2, 0x02,   // 8002 IBT R2,#2
        0xB1,         // 8004 FROM R1
        0x3F, 0x62,   // 8005 CMP R2 (ALT3)
        0x06, 0x02,   // 8007 BGE $800B  — не берётся
        0x01,         // 8009 NOP
        0x07, 0x03,   // 800A BLT $800F  — берётся
        0x01,         // 800C NOP (delay)
        0xA3, 0x11,   // 800D IBT R3,#$11 — пропущено
        0xA4, 0x22,   // 800F IBT R4,#$22
        0x00, 0x01,   // 8011 STOP
    });
    g.start();
    EXPECT_EQ(g.fx.dbgR(3), 0);
    EXPECT_EQ(g.fx.dbgR(4), 0x22);
    EXPECT_EQ(g.fx.dbgR(1), 1);   // CMP не пишет результат
}

// LOOP: R12 — счётчик, R13 — начало тела (MOVE R13,R15 даёт адрес следующей).
TEST(SuperFX, Loop_CountsDownR12)
{
    Gsu g({
        0xAC, 0x03,   // 8000 IBT R12,#3
        0x2F, 0x1D,   // 8002 MOVE R13,R15  → R13 = $8004
        0xD1,         // 8004 INC R1
        0x3C,         // 8005 LOOP
        0x01,         // 8006 NOP (delay)
        0x00, 0x01,   // 8007 STOP
    });
    g.start();
    EXPECT_EQ(g.fx.dbgR(1), 3);
    EXPECT_EQ(g.fx.dbgR(12), 0);
    EXPECT_EQ(g.fx.dbgR(13), 0x8004);
}

// Запись в R14 запускает чтение ПЗУ; ROMB (ALT3 $DF) меняет банк данных.
TEST(SuperFX, RomBuffer_GetbAndRomb)
{
    Gsu g({
        0xFE, 0x20, 0x80,   // 8000 IWT R14,#$8020
        0x15, 0xEF,         // 8003 TO R5; GETB
        0xA0, 0x01,         // 8005 IBT R0,#1
        0x3F, 0xDF,         // 8007 ROMB  (банк 1)
        0xFE, 0x00, 0x80,   // 8009 IWT R14,#$8000
        0x16, 0x3D, 0xEF,   // 800C TO R6; GETBH → R6 = (байт << 8) | R0.lo
        0x00, 0x01,         // 800F STOP
    });
    g.rom[0x0020] = 0x5A;
    g.rom[0x8000] = 0xC3;   // банк 1 по LoROM: смещение $8000
    g.start();
    EXPECT_EQ(g.fx.dbgR(5), 0x5A);
    EXPECT_EQ(g.fx.dbgR(6), 0xC301);
}

// PLOT кладёт пиксель в тайл 4bpp, RPIX читает его обратно.
TEST(SuperFX, Plot_WritesBitplanesAndRpixReadsBack)
{
    Gsu g({
        0xA0, 0x05,   // 8000 IBT R0,#5
        0x4E,         // 8002 COLOR
        0xA1, 0x03,   // 8003 IBT R1,#3
        0xA2, 0x09,   // 8005 IBT R2,#9
        0x4C,         // 8007 PLOT        (R1 → 4)
        0xA1, 0x03,   // 8008 IBT R1,#3
        0x17, 0x3D, 0x4C, // 800A TO R7; RPIX
        0x00, 0x01,   // 800D STOP
    });
    g.fx.writeIO(0x303A, 0x01);   // SCMR: 4 бита на пиксель, высота 128
    g.start();
    EXPECT_EQ(g.fx.dbgR(7), 5);
    // Тайл №1 (x=3 → столбец 0, y=9 → строка тайлов 1), строка 1 внутри тайла:
    // адрес 32 + 2 = 34; пиксель x=3 — бит 4. Цвет 5 = плоскости 0 и 2.
    EXPECT_EQ(g.fx.readRam(34), 0x10);
    EXPECT_EQ(g.fx.readRam(35), 0x00);
    EXPECT_EQ(g.fx.readRam(34 + 16), 0x10);
    EXPECT_EQ(g.fx.readRam(35 + 16), 0x00);
}

// Цвет 0 при включённой прозрачности не рисуется, но R1 всё равно растёт.
TEST(SuperFX, Plot_TransparentColorSkipsPixel)
{
    Gsu g({
        0xA1, 0x00,   // IBT R1,#0
        0xA2, 0x00,   // IBT R2,#0
        0x4C,         // PLOT (цвет 0)
        0x00, 0x01,   // STOP
    });
    g.fx.writeIO(0x303A, 0x01);
    g.fx.writeRam(0, 0xFF);
    g.start();
    EXPECT_EQ(g.fx.readRam(0), 0xFF);
    EXPECT_EQ(g.fx.dbgR(1), 1);
}

// CPU может сам залить программу в кэш через $3100-$32FF.
TEST(SuperFX, Cache_CpuUploadedCodeRuns)
{
    Gsu g({});
    const uint8_t code[16] = { 0xA1, 0x07, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01,
                               0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
    for (int i = 0; i < 16; ++i) g.fx.writeIO((uint16_t)(0x3100 + i), code[i]);
    g.start(0x0000);   // CBR = 0: адреса $0000-$01FF идут через кэш
    EXPECT_EQ(g.fx.dbgR(1), 7);
}

// STOP поднимает IRQ (если CFGR не маскирует), чтение $3031 его снимает.
TEST(SuperFX, Stop_RaisesIrqUntilSfrHighRead)
{
    Gsu g({ 0x00, 0x01 });
    g.start();
    EXPECT_TRUE(g.fx.irqLine());
    EXPECT_EQ(g.fx.readIO(0x3031) & 0x80, 0x80);
    EXPECT_FALSE(g.fx.irqLine());

    Gsu masked({ 0x00, 0x01 });
    masked.fx.writeIO(0x3037, 0x80);   // CFGR: IRQ замаскирован
    masked.start();
    EXPECT_FALSE(masked.fx.irqLine());
}

// Пока GSU работает с RON=1, CPU не видит ПЗУ.
TEST(SuperFX, BusLock_RomHiddenWhileRunning)
{
    Gsu g({ 0x05, 0xFE, 0x01 });   // BRA на себя — вечный цикл
    g.fx.writeIO(0x303A, SuperFX::SCMR_RON);
    g.start();
    EXPECT_TRUE(g.fx.running());
    EXPECT_TRUE(g.fx.romLocked());
    EXPECT_FALSE(g.fx.ramLocked());
    EXPECT_EQ(SuperFX::lockedRomByte(0xFFEA), 0x08);   // NMI → $0108
    EXPECT_EQ(SuperFX::lockedRomByte(0xFFEB), 0x01);
    g.fx.writeIO(0x3030, 0x00);                        // CPU снимает GO
    EXPECT_FALSE(g.fx.running());
    EXPECT_FALSE(g.fx.romLocked());
}

// ADD/SUB с флагами переноса и переполнения, MERGE, FMULT.
TEST(SuperFX, Alu_FlagsAndMultiply)
{
    Gsu g({
        0xF1, 0xFF, 0x7F,   // IWT R1,#$7FFF
        0x21, 0x51,         // WITH R1; ADD R1 → $FFFE, OV=1, CY=0
        0xF2, 0x00, 0x40,   // IWT R2,#$4000
        0xF6, 0x00, 0x40,   // IWT R6,#$4000 (0.5 в 1.15)
        0x23, 0xB2,         // WITH R3 ... (MOVES R3,R2 → R3 = $4000)
        0x13, 0xB2,         // TO R3; FROM R2
        0x9F,               // FMULT: R3 = ($4000 * $4000) >> 16 = $1000
        0x00, 0x01,         // STOP
    });
    g.start();
    EXPECT_EQ(g.fx.dbgR(1), 0xFFFE);
    EXPECT_EQ(g.fx.dbgR(3), 0x1000);
}
