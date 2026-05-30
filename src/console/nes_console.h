#pragma once
#include "iconsole.h"
#include "cpu/cpu.h"
#include "ppu/ppu.h"
#include "apu/apu.h"
#include "memory/memory_bus.h"

// ─── NES-консоль ─────────────────────────────────────────────────────────────
// Оборачивает MemoryBus + CPU + PPU + APU и реализует интерфейс IConsole.
class NesConsole : public IConsole {
public:
    NesConsole();

    // ─── IConsole ──────────────────────────────────────────────────────────
    bool loadROM(const std::string& path) override;
    void reset()    override;
    void runFrame() override;

    uint32_t*       getFramebuffer()       override { return ppu_.screenBuffer.data(); }
    const uint32_t* getFramebuffer() const override { return ppu_.screenBuffer.data(); }
    int getFrameWidth()  const override { return 256; }
    int getFrameHeight() const override { return 240; }

    void setInput(int player, uint16_t buttons) override;

    const std::vector<float>& getAudioSamples() const override { return apu_.getSamples(); }
    void clearAudioSamples() override { apu_.clearSamples(); }

    std::string getConsoleName() const override { return "NES"; }

    // ─── Battery SRAM ──────────────────────────────────────────────────────
    bool hasBattery()                          const override { return bus_.hasBattery(); }
    bool isSramDirty()                         const override { return bus_.isSramDirty(); }
    void clearSramDirty()                            override { bus_.clearSramDirty(); }
    bool saveSram(const std::string& path)     const override { return bus_.saveSram(path); }
    bool loadSram(const std::string& path)           override { return bus_.loadSram(path); }

    // ─── Save state v3 ─────────────────────────────────────────────────────
    bool saveState(std::ostream& os) const override;
    bool loadState(std::istream& is)       override;

private:
    MemoryBus bus_;
    CPU       cpu_;
    PPU       ppu_;
    APU       apu_;
};
