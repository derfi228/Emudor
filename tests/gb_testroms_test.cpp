#include <gtest/gtest.h>
#include "console/gb_console.h"
#include <cstdint>
#include <fstream>
#include <string>

// ─── Открытые тестовые ROM Game Boy (Blargg, Matt Currie) ────────────────────
// Сами ROM в репозиторий не входят: лежат в roms/gb/tests (см. CLAUDE.md).
// Если файла нет — тест пропускается.
namespace {

std::string findRom(const std::string& name)
{
    for (const char* dir : { "roms/gb/tests/", "../roms/gb/tests/", "../../roms/gb/tests/",
                             "../../../roms/gb/tests/" }) {
        std::string p = std::string(dir) + name;
        if (std::ifstream(p).good()) return p;
    }
    return {};
}

// Тесты Blargg печатают итог в последовательный порт: «Passed» или «Failed».
void runBlargg(const char* name, int maxFrames)
{
    const std::string path = findRom(name);
    if (path.empty()) GTEST_SKIP() << "нет " << name << " в roms/gb/tests";
    GbConsole gb;
    ASSERT_TRUE(gb.loadROM(path));
    const std::string& log = gb.bus().serialLog();
    for (int f = 0; f < maxFrames; ++f) {
        gb.runFrame();
        gb.clearAudioSamples();
        if (log.find("Passed") != std::string::npos || log.find("Failed") != std::string::npos) break;
    }
    EXPECT_NE(log.find("Passed"), std::string::npos) << log;
}

// Отпечаток кадра: итог halt_bug, dmg_sound и acid2 виден только на экране. Эталоны
// сверены глазами с описанием тестов (надпись «Passed», целое лицо acid2).
uint32_t screenHashAfter(const std::string& path, int frames)
{
    GbConsole gb;
    if (!gb.loadROM(path)) return 0;
    for (int f = 0; f < frames; ++f) { gb.runFrame(); gb.clearAudioSamples(); }
    uint32_t h = 2166136261u;
    const uint32_t* fb = gb.getFramebuffer();
    for (int i = 0; i < 160 * 144; ++i) { h ^= fb[i]; h *= 16777619u; }
    return h;
}

} // namespace

TEST(GbTestRoms, BlarggCpuInstrs)   { runBlargg("cpu_instrs.gb", 4000); }
TEST(GbTestRoms, BlarggInstrTiming) { runBlargg("instr_timing.gb", 600); }
TEST(GbTestRoms, BlarggMemTiming)   { runBlargg("mem_timing.gb", 600); }

TEST(GbTestRoms, ScreenResults)
{
    struct Case { const char* name; int frames; uint32_t hash; };
    const Case cases[] = {
        { "halt_bug.gb",   300,  131165434u },    // «Passed»
        { "dmg_sound.gb",  3600, 3045645237u },           // «Passed», 12/12 (звук)
        { "dmg-acid2.gb",  60,   2465587413u },  // лицо целиком
        { "cgb-acid2.gbc", 60,   595768028u },   // лицо целиком (цвет)
    };
    int ran = 0;
    for (const Case& c : cases) {
        const std::string path = findRom(c.name);
        if (path.empty()) continue;
        ++ran;
        EXPECT_EQ(screenHashAfter(path, c.frames), c.hash) << c.name;
    }
    if (!ran) GTEST_SKIP() << "нет ROM в roms/gb/tests";
}
