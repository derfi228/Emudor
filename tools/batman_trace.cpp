// batman_trace.cpp — безголовый трассировщик для Batman: Return of the Joker
// Загружает ROM, прогоняет CPU через 10 000 инструкций, пишет трасс в stdout
// и batman_trace.txt.  Выход: при обнаружении зависания (PC повторяется
// одно и то же место N раз подряд) сообщает об этом и останавливается.

#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include "cpu/cpu.h"
#include "ppu/ppu.h"
#include "apu/apu.h"
#include "memory/memory_bus.h"

int main(int argc, char* argv[]) {
    const char* romPath = (argc >= 2)
        ? argv[1]
        : "roms/Batman - Return of the Joker (USA).nes";

    MemoryBus bus;
    CPU       cpu;
    PPU       ppu;
    APU       apu;

    bus.connectCPU(&cpu);
    bus.connectPPU(&ppu);
    bus.connectAPU(&apu);
    cpu.connectBus(&bus);
    ppu.connectBus(&bus);

    if (!bus.loadROM(romPath)) {
        std::cerr << "Failed to load ROM: " << romPath << "\n";
        return 1;
    }

    cpu.reset();
    ppu.reset();
    // Прогоняем 7 тактов reset-задержки
    for (int i = 0; i < 7; i++) {
        for (int p = 0; p < 3; p++) {
            ppu.clock();
            if (ppu.nmiPending) { ppu.nmiPending = false; cpu.nmi(); }
        }
        cpu.clock();
        apu.clock();
    }

    std::ofstream out("batman_trace.txt");
    if (!out) { std::cerr << "Cannot open batman_trace.txt for writing\n"; return 1; }

    const int kMaxInstr = 50000;
    // Счётчик повторений одного PC — для детектирования зависания
    std::unordered_map<uint16_t, int> pcHits;
    const int kLoopThreshold = 50;   // > 50 раз подряд с одним PC — зависание

    int      instrCount  = 0;
    uint16_t prevPC      = 0xFFFF;
    int      sameCount   = 0;

    while (instrCount < kMaxInstr) {
        // Трассируем перед каждой новой инструкцией
        if (cpu.remainingCycles == 0) {
            uint16_t curPC = cpu.PC;

            // Детектор зависания: один и тот же PC много раз подряд
            if (curPC == prevPC) {
                sameCount++;
            } else {
                sameCount = 0;
                prevPC = curPC;
            }

            std::string line = cpu.trace(ppu.scanline(), ppu.dot());
            out  << line << "\n";

            // Печатаем первые 30 строк в stderr для быстрого просмотра
            if (instrCount < 30) {
                std::cerr << line << "\n";
            }

            instrCount++;

            if (sameCount >= kLoopThreshold) {
                std::cerr << "\n*** LOOP DETECTED at PC=$" << std::hex << curPC
                          << " after " << std::dec << instrCount
                          << " instructions (same PC " << sameCount
                          << " times) ***\n";
                break;
            }
        }

        for (int p = 0; p < 3; p++) {
            ppu.clock();
            if (ppu.nmiPending) { ppu.nmiPending = false; cpu.nmi(); }
        }
        bus.mapperCpuClock();
        if (bus.mapperIrqPending()) { bus.mapperClearIrq(); cpu.irq(); }
        cpu.clock();
        apu.clock();
    }

    out.flush();
    std::cerr << "\nDone. " << instrCount << " instructions traced.\n";
    std::cerr << "Output: batman_trace.txt\n";
    return 0;
}
