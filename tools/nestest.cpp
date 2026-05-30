#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstring>
#include "cpu/cpu.h"
#include "ppu/ppu.h"
#include "apu/apu.h"
#include "memory/memory_bus.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: nestest <nestest.nes> [nestest.log]\n";
        return 1;
    }

    MemoryBus bus;
    CPU       cpu;
    PPU       ppu;
    APU       apu;

    bus.connectCPU(&cpu);
    bus.connectPPU(&ppu);
    bus.connectAPU(&apu);
    cpu.connectBus(&bus);
    ppu.connectBus(&bus);

    if (!bus.loadROM(argv[1])) {
        std::cerr << "Failed to load ROM: " << argv[1] << "\n";
        return 1;
    }

    cpu.reset();
    ppu.reset();

    // nestest начинается с $C000
    cpu.PC = 0xC000;
    cpu.totalCycles_ = 7;
    cpu.remainingCycles = 0;

    // Загружаем эталонный лог если передан
    std::vector<std::string> refLines;
    if (argc >= 3) {
        std::ifstream logFile(argv[2]);
        std::string line;
        while (std::getline(logFile, line))
            refLines.push_back(line);
    }

    std::ofstream outFile("nestest_out.log");

    uint16_t prevPC    = 0xFFFF;
    int      lineCount = 0;
    int      diffCount = 0;
    // Nestest.log заканчивается на 8991 строке (CYC:26554).
    // Останавливаемся: (1) самозацикленный JMP, (2) конец лога, (3) лимит 30000
    const int refTotal   = refLines.empty() ? 30000 : (int)refLines.size();
    const int hardLimit  = 30000;

    while (true) {
        // Трассируем только когда CPU готов к новой инструкции
        if (cpu.remainingCycles == 0) {
            if (cpu.PC == prevPC) break;          // самозацикленный JMP
            if (lineCount >= refTotal)  break;    // дошли до конца лога
            if (lineCount >= hardLimit) break;    // аварийный лимит
            prevPC = cpu.PC;

            std::string traceLine = cpu.trace(ppu.scanline(), ppu.dot());
            outFile << traceLine << "\n";

            // Сравниваем с эталоном (до " PPU:")
            if (!refLines.empty() && lineCount < (int)refLines.size()) {
                const std::string& ref = refLines[lineCount];
                // Обрезаем оба до " PPU:" для сравнения
                auto cut = [](const std::string& s) {
                    auto pos = s.find(" PPU:");
                    return pos != std::string::npos ? s.substr(0, pos) : s;
                };
                std::string t = cut(traceLine);
                std::string r = cut(ref);
                if (t != r) {
                    diffCount++;
                    if (diffCount <= 5) {
                        std::cerr << "DIFF at line " << lineCount + 1 << ":\n"
                                  << "  GOT: " << t << "\n"
                                  << "  REF: " << r << "\n";
                    }
                }
            }
            lineCount++;
        }

        // 3× PPU, 1× CPU, 1× APU
        for (int i = 0; i < 3; i++) {
            ppu.clock();
            if (ppu.nmiPending) {
                ppu.nmiPending = false;
                cpu.nmi();
            }
        }
        cpu.clock();
        apu.clock();
    }

    // Результат: $0002 и $0003 (0x00 = PASS)
    uint8_t r2 = bus.read(0x0002);
    uint8_t r3 = bus.read(0x0003);

    std::cout << "Lines executed : " << lineCount  << "\n";
    std::cout << "Differences    : " << diffCount  << "\n";
    std::cout << "$0002          : 0x" << std::hex << (unsigned)r2
              << (r2 == 0 ? " (PASS)" : " (FAIL)") << "\n";
    std::cout << "$0003          : 0x" << (unsigned)r3
              << (r3 == 0 ? " (PASS)" : " (FAIL)") << "\n";
    std::cout << "Output         : nestest_out.log\n";

    return (r2 == 0 && r3 == 0) ? 0 : 1;
}
