#include <gtest/gtest.h>
#include "console/snes_console.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ─── Крошечный LoROM: в цикле крутит цвет фона, картинка меняется ────────────
namespace {

std::string writeTestRom(const char* name, uint8_t salt)
{
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t code[] = {
        0x78,             // SEI
        0x18, 0xFB,       // CLC; XCE — нативный режим
        0xA9, 0x0F,       // LDA #$0F
        0x8D, 0x00, 0x21, // STA $2100 — экран включён
        0xE6, 0x00,       // loop: INC $00
        0xA5, 0x00,       // LDA $00
        0x9C, 0x21, 0x21, // STZ $2121
        0x8D, 0x22, 0x21, // STA $2122 — цвет фона
        0x9C, 0x22, 0x21, // STZ $2122
        0x80, 0xF1,       // BRA loop
    };
    for (size_t i = 0; i < sizeof code; ++i) rom[i] = code[i];
    rom[0x7000] = salt;                         // отличает «разные игры»
    rom[0x7FD5] = 0x20;                         // LoROM
    rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;     // RESET → $8000
    uint32_t sum = 0;
    for (uint8_t b : rom) sum += b;
    uint16_t chk = (uint16_t)sum;
    rom[0x7FDC] = (uint8_t)~chk; rom[0x7FDD] = (uint8_t)(~chk >> 8);
    rom[0x7FDE] = (uint8_t)chk;  rom[0x7FDF] = (uint8_t)(chk >> 8);

    std::string path = std::string(name) + ".sfc";
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(rom.data()), (std::streamsize)rom.size());
    return path;
}

uint32_t runAndHash(SnesConsole& c, int frames)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < frames; ++i) {
        c.runFrame();
        const uint32_t* fb = c.getFramebuffer();
        size_t n = (size_t)c.getFrameWidth() * (size_t)c.getFrameHeight();
        for (size_t k = 0; k < n; ++k) { h ^= fb[k]; h *= 16777619u; }
        for (float s : c.getAudioSamples()) {
            uint32_t bits; std::memcpy(&bits, &s, sizeof bits);
            h ^= bits; h *= 16777619u;
        }
        c.clearAudioSamples();
    }
    return h;
}

} // namespace

// После загрузки эмуляция идёт байт-в-байт так же, как без сохранения.
TEST(SnesState, LoadContinuesIdentically)
{
    std::string rom = writeTestRom("state_test_a", 1);
    SnesConsole c;
    ASSERT_TRUE(c.loadROM(rom));
    runAndHash(c, 20);

    std::stringstream st;
    ASSERT_TRUE(c.saveState(st));
    uint32_t straight = runAndHash(c, 30);

    ASSERT_TRUE(c.loadState(st));
    EXPECT_EQ(runAndHash(c, 30), straight);
    std::remove(rom.c_str());
}

// Обрезанный файл не загружается, и эмуляция не портится (откат).
TEST(SnesState, TruncatedStateRejectedAndRolledBack)
{
    std::string rom = writeTestRom("state_test_b", 1);
    SnesConsole c, ref;
    ASSERT_TRUE(c.loadROM(rom));
    ASSERT_TRUE(ref.loadROM(rom));
    runAndHash(c, 10);
    runAndHash(ref, 10);

    std::stringstream st;
    ASSERT_TRUE(c.saveState(st));
    std::string half = st.str().substr(0, st.str().size() / 2);
    runAndHash(c, 5);
    runAndHash(ref, 5);

    std::stringstream bad(half);
    EXPECT_FALSE(c.loadState(bad));
    EXPECT_EQ(runAndHash(c, 10), runAndHash(ref, 10));
    std::remove(rom.c_str());
}

// Состояние от другой игры не подходит.
TEST(SnesState, StateFromOtherGameRejected)
{
    std::string romA = writeTestRom("state_test_c", 1);
    std::string romB = writeTestRom("state_test_d", 2);
    SnesConsole a, b;
    ASSERT_TRUE(a.loadROM(romA));
    ASSERT_TRUE(b.loadROM(romB));
    runAndHash(a, 5);

    std::stringstream st;
    ASSERT_TRUE(a.saveState(st));
    EXPECT_FALSE(b.loadState(st));
    std::remove(romA.c_str());
    std::remove(romB.c_str());
}
