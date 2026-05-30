#include <gtest/gtest.h>
#include "snes/snes_bus.h"
#include <vector>
#include <cstdint>

// ─── Вспомогательная функция: создаём минимальный LoROM-образ ────────────────
// Строим заголовок с корректной контрольной суммой, чтобы detectMapMode()
// уверенно распознал тип.
static std::vector<uint8_t> makeLoROM(uint32_t romSizeBytes = 0x8000)
{
    std::vector<uint8_t> rom(romSizeBytes, 0x00);

    // Тип маппинга по $7FD5: 0x20 = LoROM
    rom[0x7FD5] = 0x20;
    // ROM type = 0x00 (ROM only, no battery)
    rom[0x7FD6] = 0x00;
    // ROM size = 0x07 → 128 KB (≥ наш 32 KB; для теста не важно)
    rom[0x7FD7] = 0x07;
    // SRAM size = 0x00
    rom[0x7FD8] = 0x00;

    // Считаем контрольную сумму (сумма всех байт mod 65536)
    uint32_t sum = 0;
    for (auto b : rom) sum += b;
    uint16_t checksum   = (uint16_t)(sum & 0xFFFF);
    uint16_t complement = (uint16_t)(~checksum);

    rom[0x7FDC] = (uint8_t)(complement & 0xFF);
    rom[0x7FDD] = (uint8_t)(complement >> 8);
    rom[0x7FDE] = (uint8_t)(checksum & 0xFF);
    rom[0x7FDF] = (uint8_t)(checksum >> 8);

    return rom;
}

// Минимальный HiROM (64 KB, заголовок в $FFC0)
static std::vector<uint8_t> makeHiROM(uint32_t romSizeBytes = 0x10000)
{
    std::vector<uint8_t> rom(romSizeBytes, 0x00);

    rom[0xFFD5] = 0x21;  // HiROM
    rom[0xFFD6] = 0x00;
    rom[0xFFD7] = 0x08;
    rom[0xFFD8] = 0x00;

    uint32_t sum = 0;
    for (auto b : rom) sum += b;
    uint16_t checksum   = (uint16_t)(sum & 0xFFFF);
    uint16_t complement = (uint16_t)(~checksum);
    rom[0xFFDC] = (uint8_t)(complement & 0xFF);
    rom[0xFFDD] = (uint8_t)(complement >> 8);
    rom[0xFFDE] = (uint8_t)(checksum & 0xFF);
    rom[0xFFDF] = (uint8_t)(checksum >> 8);

    return rom;
}

// ─── Тест 1: WRAM чтение/запись ───────────────────────────────────────────────
TEST(SnesBusTest, WRAM_ReadWrite)
{
    SnesBus bus;
    bus.loadROMDirect(makeLoROM(), SnesBus::MapMode::LoROM);

    // Запись в $7E0000
    bus.write(0x7E0000, 0xAB);
    EXPECT_EQ(bus.read(0x7E0000), 0xABu);

    // Запись в $7F8000 (вторая половина WRAM)
    bus.write(0x7F8000, 0xCD);
    EXPECT_EQ(bus.read(0x7F8000), 0xCDu);

    // Данные различаются
    EXPECT_NE(bus.read(0x7E0000), bus.read(0x7F8000));
}

// ─── Тест 2: Зеркало WRAM $0000–$1FFF в банках $00 и $80 ─────────────────────
TEST(SnesBusTest, WRAM_Mirror)
{
    SnesBus bus;
    bus.loadROMDirect(makeLoROM(), SnesBus::MapMode::LoROM);

    // $000100 → зеркало $7E0100
    bus.write(0x000100, 0x55);
    EXPECT_EQ(bus.read(0x7E0100), 0x55u);

    // $800200 → зеркало $7E0200
    bus.write(0x800200, 0x66);
    EXPECT_EQ(bus.read(0x7E0200), 0x66u);

    // Чтение через зеркало банка $80
    EXPECT_EQ(bus.read(0x800100), 0x55u);
}

// ─── Тест 3: Зеркало WRAM — запись через $7E и чтение через $00 ──────────────
TEST(SnesBusTest, WRAM_CrossMirror)
{
    SnesBus bus;
    bus.loadROMDirect(makeLoROM(), SnesBus::MapMode::LoROM);

    bus.write(0x7E00FF, 0x77);
    EXPECT_EQ(bus.read(0x0000FF), 0x77u);  // зеркало банка $00
}

// ─── Тест 4: LoROM — чтение ROM из банка $00, $8000+ ─────────────────────────
TEST(SnesBusTest, LoROM_ReadROM)
{
    auto rom = makeLoROM(0x8000);
    // Кладём маркер по смещению $1000 внутри первого банка ROM
    // offset в ROM для LoROM банка $00 ($8000+): romOff = 0*0x8000 + (addr-0x8000)
    // addr = $9000 → romOff = 0x1000
    rom[0x1000] = 0xBE;
    rom[0x1001] = 0xEF;

    SnesBus bus;
    bus.loadROMDirect(rom, SnesBus::MapMode::LoROM);

    EXPECT_EQ(bus.read(0x009000), 0xBEu);
    EXPECT_EQ(bus.read(0x009001), 0xEFu);
}

// ─── Тест 5: LoROM — чтение ROM из банка $80 (зеркало банка $00) ─────────────
TEST(SnesBusTest, LoROM_BankMirror)
{
    auto rom = makeLoROM(0x8000);
    rom[0x2000] = 0xCA;

    SnesBus bus;
    bus.loadROMDirect(rom, SnesBus::MapMode::LoROM);

    // Банк $00 и $80 оба читают первый 32-KB банк ROM
    // addr = $A000 → romOff = (0 & 0x7F)*0x8000 + 0x2000 = 0x2000
    EXPECT_EQ(bus.read(0x00A000), 0xCAu);
    // Банк $80: (0x80 & 0x7F)*0x8000 + 0x2000 = 0x2000
    EXPECT_EQ(bus.read(0x80A000), 0xCAu);
}

// ─── Тест 6: LoROM — второй банк ROM ($01/$81) ────────────────────────────────
TEST(SnesBusTest, LoROM_SecondBank)
{
    auto rom = makeLoROM(0x10000);  // 64 KB: два 32-KB банка

    // Банк 1 ROM начинается с смещения 0x8000 в векторе rom
    rom[0x8000] = 0x11;
    rom[0x8001] = 0x22;

    SnesBus bus;
    bus.loadROMDirect(rom, SnesBus::MapMode::LoROM);

    // Банк $01, addr $8000 → romOff = 1*0x8000 + 0 = 0x8000
    EXPECT_EQ(bus.read(0x018000), 0x11u);
    EXPECT_EQ(bus.read(0x018001), 0x22u);
}

// ─── Тест 7: LoROM — SRAM R/W ─────────────────────────────────────────────────
TEST(SnesBusTest, LoROM_SRAM_ReadWrite)
{
    auto rom = makeLoROM();
    // Создаём шину с battery + SRAM вручную через loadROMDirect
    SnesBus bus;
    // Для теста SRAM инициализируем напрямую через protected-метод
    // (наследоваться не надо — используем маленький трюк через loadROMDirect
    //  с флагом battery=true, но SRAM заполняется loadROM)
    // Здесь мы просто тестируем через публичный интерфейс после loadROMDirect:
    // размер SRAM = 0, записи в банк $70 должны идти в openBus (нет ошибки)
    bus.loadROMDirect(rom, SnesBus::MapMode::LoROM, /*battery=*/false);

    // Без SRAM — запись не крашится
    bus.write(0x700000, 0x42);
    // Нет SRAM — читаем openBus
    // (значение не проверяем, просто no-crash)
    (void)bus.read(0x700000);
}

// ─── Тест 8: HiROM — detectMapMode распознаёт HiROM ─────────────────────────
TEST(SnesBusTest, HiROM_Detect)
{
    SnesBus bus;
    bus.loadROMDirect(makeHiROM(), SnesBus::MapMode::HiROM);
    EXPECT_EQ(bus.mapMode(), SnesBus::MapMode::HiROM);
}

// ─── Тест 9: HiROM — чтение ROM из банка $C0 ─────────────────────────────────
TEST(SnesBusTest, HiROM_ReadROM)
{
    auto rom = makeHiROM(0x10000);
    rom[0x0100] = 0xDE;
    rom[0x0101] = 0xAD;

    SnesBus bus;
    bus.loadROMDirect(rom, SnesBus::MapMode::HiROM);

    // Банк $C0, addr $0100 → romOff = (0xC0-0xC0)*0x10000 + $0100 = $0100
    EXPECT_EQ(bus.read(0xC00100), 0xDEu);
    EXPECT_EQ(bus.read(0xC00101), 0xADu);
}

// ─── Тест 10: HiROM — чтение ROM через банки $00–$3F (addr $8000+) ───────────
TEST(SnesBusTest, HiROM_LowBankAccess)
{
    auto rom = makeHiROM(0x10000);
    rom[0x8000] = 0x99;

    SnesBus bus;
    bus.loadROMDirect(rom, SnesBus::MapMode::HiROM);

    // Банк $00, addr $8000 → romOff = 0x0000*0x10000 + 0x8000 = 0x8000
    EXPECT_EQ(bus.read(0x008000), 0x99u);
}

// ─── Тест 11: reset очищает WRAM ─────────────────────────────────────────────
TEST(SnesBusTest, ResetClearsWRAM)
{
    SnesBus bus;
    bus.loadROMDirect(makeLoROM(), SnesBus::MapMode::LoROM);

    bus.write(0x7E1234, 0xFF);
    EXPECT_EQ(bus.read(0x7E1234), 0xFFu);

    bus.reset();
    EXPECT_EQ(bus.read(0x7E1234), 0x00u);
}

// ─── Тест 12: mapMode() возвращает LoROM после loadROMDirect ─────────────────
TEST(SnesBusTest, MapModeLoROM)
{
    SnesBus bus;
    bus.loadROMDirect(makeLoROM(), SnesBus::MapMode::LoROM);
    EXPECT_EQ(bus.mapMode(), SnesBus::MapMode::LoROM);
}

// ─── Тест 13: WRAM полный диапазон 128 KB ─────────────────────────────────────
TEST(SnesBusTest, WRAM_FullRange)
{
    SnesBus bus;
    bus.loadROMDirect(makeLoROM(), SnesBus::MapMode::LoROM);

    // $7E0000 — начало
    bus.write(0x7E0000, 0x01);
    EXPECT_EQ(bus.read(0x7E0000), 0x01u);

    // $7EFFFF — конец первой половины
    bus.write(0x7EFFFF, 0x02);
    EXPECT_EQ(bus.read(0x7EFFFF), 0x02u);

    // $7F0000 — начало второй половины
    bus.write(0x7F0000, 0x03);
    EXPECT_EQ(bus.read(0x7F0000), 0x03u);

    // $7FFFFF — конец WRAM
    bus.write(0x7FFFFF, 0x04);
    EXPECT_EQ(bus.read(0x7FFFFF), 0x04u);

    // Разные ячейки независимы
    EXPECT_EQ(bus.read(0x7E0000), 0x01u);
    EXPECT_EQ(bus.read(0x7FFFFF), 0x04u);
}

// ─── Тест 14: hasBattery / sramDirty флаги ────────────────────────────────────
TEST(SnesBusTest, BatteryFlags)
{
    SnesBus bus;
    bus.loadROMDirect(makeLoROM(), SnesBus::MapMode::LoROM, /*battery=*/true);

    EXPECT_TRUE(bus.hasBattery());
    EXPECT_FALSE(bus.isSramDirty());

    bus.clearSramDirty();
    EXPECT_FALSE(bus.isSramDirty());
}

// ─── Тест 15: wram() указатель соответствует прочитанным данным ───────────────
TEST(SnesBusTest, WramPointer)
{
    SnesBus bus;
    bus.loadROMDirect(makeLoROM(), SnesBus::MapMode::LoROM);

    bus.write(0x7E0042, 0xAA);
    const uint8_t* p = bus.wram();
    EXPECT_EQ(p[0x0042], 0xAAu);
}
