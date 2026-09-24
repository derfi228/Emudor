#include <gtest/gtest.h>
#include "console/gb_console.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <vector>

namespace {

std::vector<uint8_t> makeRom(const std::vector<uint8_t>& code, uint8_t type = 0x00,
                             uint8_t romSize = 0x00, uint8_t ramSize = 0x00)
{
    std::vector<uint8_t> rom((size_t)0x8000 << romSize, 0x00);
    for (size_t i = 0; i < code.size(); ++i) rom[0x100 + i] = code[i];
    rom[0x147] = type;
    rom[0x148] = romSize;
    rom[0x149] = ramSize;
    return rom;
}

// Бесконечный цикл: JR -2.
const std::vector<uint8_t> kLoop = { 0x18, 0xFE };

void runDots(GbConsole& gb, uint64_t dots)
{
    uint64_t end = gb.bus().dots() + dots;
    while (gb.bus().dots() < end) gb.cpu().step();
}

} // namespace

// ─── Таймер ───────────────────────────────────────────────────────────────────
TEST(GbTimer, TimaCountsAtSelectedRate)
{
    GbConsole gb;
    gb.loadROMData(makeRom(kLoop));
    GbBus& bus = gb.bus();
    bus.write(0xFF04, 0);                      // сброс делителя
    bus.write(0xFF05, 0);
    bus.write(0xFF07, 0x05);                   // включён, 262144 Гц: шаг каждые 16 тактов
    runDots(gb, 16 * 100);
    int tima = bus.read(0xFF05);
    EXPECT_GE(tima, 99);
    EXPECT_LE(tima, 101);
}

TEST(GbTimer, OverflowReloadsTmaAndRaisesInterrupt)
{
    GbConsole gb;
    gb.loadROMData(makeRom(kLoop));
    GbBus& bus = gb.bus();
    bus.write(0xFF0F, 0);
    bus.write(0xFF06, 0xF0);                   // TMA
    bus.write(0xFF05, 0xFF);
    bus.write(0xFF04, 0);
    bus.write(0xFF07, 0x05);
    runDots(gb, 64);
    EXPECT_GE(bus.read(0xFF05), 0xF0);
    EXPECT_EQ(bus.read(0xFF0F) & 0x04, 0x04);
}

TEST(GbTimer, DivIncrementsAt16384Hz)
{
    GbConsole gb;
    gb.loadROMData(makeRom(kLoop));
    gb.bus().write(0xFF04, 0);
    runDots(gb, 256 * 10);
    EXPECT_EQ(gb.bus().read(0xFF04), 10);
}

// ─── Видео ────────────────────────────────────────────────────────────────────
TEST(GbPpu, FrameIs70224DotsAndRaisesVBlank)
{
    GbConsole gb;
    gb.loadROMData(makeRom(kLoop));
    gb.runFrame();                             // до первого VBlank
    gb.bus().write(0xFF0F, 0);
    uint64_t start = gb.bus().dots();
    gb.runFrame();
    EXPECT_EQ(gb.bus().dots() - start, 70224u);
    EXPECT_EQ(gb.bus().read(0xFF44), 144);
    EXPECT_EQ(gb.bus().read(0xFF0F) & 0x01, 0x01);
    EXPECT_EQ(gb.bus().read(0xFF41) & 0x03, 1);   // режим 1 — VBlank
}

TEST(GbPpu, LycInterruptOnMatchingLine)
{
    GbConsole gb;
    gb.loadROMData(makeRom(kLoop));
    GbBus& bus = gb.bus();
    gb.runFrame();
    bus.write(0xFF45, 42);                     // LYC
    bus.write(0xFF41, 0x40);                   // прерывание по совпадению
    bus.write(0xFF0F, 0);
    while (bus.read(0xFF44) != 41) gb.cpu().step();
    EXPECT_EQ(bus.read(0xFF0F) & 0x02, 0);
    while (bus.read(0xFF44) != 42) gb.cpu().step();
    EXPECT_EQ(bus.read(0xFF0F) & 0x02, 0x02);
    EXPECT_EQ(bus.read(0xFF41) & 0x04, 0x04);
}

// Тайл фона 0 — сплошной цвет 3; спрайт с цветом 1 поверх в левом верхнем углу.
TEST(GbPpu, RendersBackgroundTileAndSprite)
{
    GbConsole gb;
    gb.loadROMData(makeRom(kLoop));
    GbBus& bus = gb.bus();
    bus.write(0xFF40, 0x00);                   // экран выкл — видеопамять доступна
    for (int i = 0; i < 16; ++i) bus.write((uint16_t)(0x8000 + i), 0xFF);          // тайл 0: цвет 3
    for (int i = 0; i < 16; i += 2) bus.write((uint16_t)(0x8010 + i), 0xFF);       // тайл 1: цвет 1
    for (int i = 0; i < 0x400; ++i) bus.write((uint16_t)(0x9800 + i), 0x00);
    bus.write(0xFF47, 0xE4);                   // BGP: 3 2 1 0
    bus.write(0xFF48, 0xE4);                   // OBP0
    bus.write(0xFE00, 16);                     // спрайт: Y
    bus.write(0xFE01, 8);                      //         X
    bus.write(0xFE02, 1);                      //         тайл 1
    bus.write(0xFE03, 0);
    bus.write(0xFF40, 0x93);                   // экран, фон ($8000), спрайты
    gb.runFrame();
    gb.runFrame();
    const uint32_t* fb = gb.getFramebuffer();
    const uint32_t dark = 0xFF081820u, light = 0xFF88C070u;
    EXPECT_EQ(fb[0], light);                   // спрайт, цвет 1
    EXPECT_EQ(fb[8], dark);                    // фон, цвет 3
    EXPECT_EQ(fb[143 * 160 + 159], dark);
}

// ─── Картридж ─────────────────────────────────────────────────────────────────
TEST(GbCart, Mbc1SwitchesBanks)
{
    std::vector<uint8_t> rom = makeRom(kLoop, 0x01, 0x05);   // MBC1, 1 МБ (64 банка)
    for (int bank = 0; bank < 64; ++bank) rom[(size_t)bank * 0x4000 + 0x1000] = (uint8_t)bank;
    GbConsole gb;
    ASSERT_TRUE(gb.loadROMData(rom));
    GbBus& bus = gb.bus();
    EXPECT_EQ(bus.read(0x5000), 1);            // по умолчанию банк 1
    bus.write(0x2000, 0x07);
    EXPECT_EQ(bus.read(0x5000), 7);
    bus.write(0x2000, 0x00);                   // 0 превращается в 1
    EXPECT_EQ(bus.read(0x5000), 1);
    bus.write(0x2000, 0x01);
    bus.write(0x4000, 0x01);                   // старшие биты → банк $21
    EXPECT_EQ(bus.read(0x5000), 0x21);
}

TEST(GbCart, Mbc5NineBitBankAndRam)
{
    std::vector<uint8_t> rom = makeRom(kLoop, 0x1B, 0x08, 0x03);   // MBC5+RAM+BATT, 8 МБ
    rom[(size_t)0x101 * 0x4000 + 5] = 0xAB;
    GbConsole gb;
    ASSERT_TRUE(gb.loadROMData(rom));
    GbBus& bus = gb.bus();
    bus.write(0x2000, 0x01);
    bus.write(0x3000, 0x01);                   // банк $101
    EXPECT_EQ(bus.read(0x4005), 0xAB);
    bus.write(0x0000, 0x0A);                   // ОЗУ включено
    bus.write(0x4000, 0x02);
    bus.write(0xA010, 0x5A);
    bus.write(0x4000, 0x00);
    EXPECT_NE(bus.read(0xA010), 0x5A);
    bus.write(0x4000, 0x02);
    EXPECT_EQ(bus.read(0xA010), 0x5A);
    EXPECT_TRUE(gb.isSramDirty());
}

TEST(GbCart, Mbc3RtcLatchAndSaveRoundTrip)
{
    std::vector<uint8_t> rom = makeRom(kLoop, 0x10, 0x01, 0x03);   // MBC3+TIMER+RAM+BATT
    GbConsole gb;
    ASSERT_TRUE(gb.loadROMData(rom));
    GbBus& bus = gb.bus();
    bus.write(0x0000, 0x0A);
    bus.write(0x4000, 0x09);                   // минуты
    bus.write(0xA000, 30);
    for (int i = 0; i < 125; ++i) gb.runFrame();   // ~2 секунды эмулированного времени
    bus.write(0x6000, 0x00);
    bus.write(0x6000, 0x01);                   // защёлкнуть
    bus.write(0x4000, 0x08);                   // секунды
    EXPECT_GE(bus.read(0xA000), 1);
    bus.write(0x4000, 0x09);
    EXPECT_EQ(bus.read(0xA000), 30);
}

TEST(GbCart, BatterySaveRoundTrip)
{
    std::vector<uint8_t> rom = makeRom(kLoop, 0x03, 0x01, 0x02);   // MBC1+RAM+BATT
    GbConsole a;
    ASSERT_TRUE(a.loadROMData(rom));
    a.bus().write(0x0000, 0x0A);
    a.bus().write(0xA123, 0x77);
    ASSERT_TRUE(a.saveSram("gb_test_save.bin"));
    GbConsole b;
    ASSERT_TRUE(b.loadROMData(rom));
    ASSERT_TRUE(b.loadSram("gb_test_save.bin"));
    b.bus().write(0x0000, 0x0A);
    EXPECT_EQ(b.bus().read(0xA123), 0x77);
    std::remove("gb_test_save.bin");
}

// ─── Джойпад ─────────────────────────────────────────────────────────────────
TEST(GbJoypad, SelectedGroupAndInterrupt)
{
    GbConsole gb;
    gb.loadROMData(makeRom(kLoop));
    GbBus& bus = gb.bus();
    bus.write(0xFF0F, 0);
    bus.write(0xFF00, 0x10);                   // выбраны кнопки (бит 5 = 0)
    gb.setInput(0, 0x10);                      // START (формат NES)
    EXPECT_EQ(bus.read(0xFF00) & 0x0F, 0x07);  // бит 3 (Start) = 0
    EXPECT_EQ(bus.read(0xFF0F) & 0x10, 0x10);
    bus.write(0xFF00, 0x20);                   // выбрана крестовина
    EXPECT_EQ(bus.read(0xFF00) & 0x0F, 0x0F);
}

// ─── OAM DMA ─────────────────────────────────────────────────────────────────
TEST(GbDma, CopiesOamIn160Cycles)
{
    GbConsole gb;
    gb.loadROMData(makeRom(kLoop));
    GbBus& bus = gb.bus();
    bus.write(0xFF40, 0x00);                   // экран выкл: OAM не занят видео
    for (int i = 0; i < 160; ++i) bus.write((uint16_t)(0xC100 + i), (uint8_t)(i ^ 0x5A));
    bus.write(0xFF46, 0xC1);
    EXPECT_EQ(bus.read(0xFE00), 0xFF);         // во время DMA OAM недоступна
    runDots(gb, 4 * 162);
    for (int i = 0; i < 160; ++i) EXPECT_EQ(bus.read((uint16_t)(0xFE00 + i)), (uint8_t)(i ^ 0x5A));
}

// ─── Звук ─────────────────────────────────────────────────────────────────────
TEST(GbApu, SquareChannelPlaysAndLengthStopsIt)
{
    GbConsole gb;
    gb.loadROMData(makeRom(kLoop));
    GbBus& bus = gb.bus();
    bus.write(0xFF24, 0x77);
    bus.write(0xFF25, 0x22);                   // канал 2 в обе стороны
    bus.write(0xFF16, 0x80 | 0x3E);            // скважность 50%, длина 2
    bus.write(0xFF17, 0xF0);                   // громкость 15
    bus.write(0xFF18, 0x00);
    bus.write(0xFF19, 0xC7);                   // старт, длина включена, частота ~ 440 Гц
    EXPECT_EQ(bus.read(0xFF26) & 0x02, 0x02);
    gb.clearAudioSamples();
    gb.runFrame();
    float peak = 0.0f;
    for (float s : gb.getAudioSamples()) peak = std::max(peak, std::fabs(s));
    EXPECT_GT(peak, 0.05f);
    // Длина 2 при 256 Гц — ~8 мс: через кадр канал уже молчит.
    gb.runFrame();
    EXPECT_EQ(bus.read(0xFF26) & 0x02, 0);
}

// ─── Save state ───────────────────────────────────────────────────────────────
TEST(GbState, LoadContinuesIdentically)
{
    // Счётчик в WRAM и прокрутка фона каждый кадр — картинка меняется.
    const std::vector<uint8_t> code = {
        0x3E, 0x91, 0xE0, 0x40,          // LD A,$91; LDH (LCDC),A
        0x21, 0x00, 0xC0,                // LD HL,$C000
        0x34,                            // loop: INC (HL)
        0x7E, 0xE0, 0x43,                //       LD A,(HL); LDH (SCX),A
        0x18, 0xFA,                      //       JR loop
    };
    std::vector<uint8_t> rom = makeRom(code);
    for (int i = 0; i < 16; ++i) rom[0x4000 + i] = (uint8_t)(0x0F << (i & 1));
    GbConsole gb;
    ASSERT_TRUE(gb.loadROMData(rom));
    for (int i = 0; i < 16; ++i) gb.bus().write((uint16_t)(0x8000 + i), (uint8_t)(i * 17));
    for (int i = 0; i < 10; ++i) gb.runFrame();

    std::stringstream st;
    ASSERT_TRUE(gb.saveState(st));
    auto hashFrames = [&](int n) {
        uint32_t h = 2166136261u;
        for (int i = 0; i < n; ++i) {
            gb.runFrame();
            const uint32_t* fb = gb.getFramebuffer();
            for (int k = 0; k < 160 * 144; ++k) { h ^= fb[k]; h *= 16777619u; }
        }
        return h;
    };
    uint32_t straight = hashFrames(20);
    ASSERT_TRUE(gb.loadState(st));
    EXPECT_EQ(hashFrames(20), straight);
}
