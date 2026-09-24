#pragma once
#include "iconsole.h"
#include "gb/gb_bus.h"
#include "gb/gb_cart.h"
#include "gb/gb_cpu.h"
#include <cstdint>
#include <string>
#include <vector>

// ─── GbConsole ───────────────────────────────────────────────────────────────
// Game Boy и Game Boy Color. Режим выбирается по заголовку картриджа: игра с
// поддержкой цвета ($0143 бит 7) запускается как на GBC, остальные — как на
// классическом Game Boy. Экран 160×144, кадр — 70224 точки (~59.7 Гц).
class GbConsole : public IConsole {
public:
    bool loadROM(const std::string& path) override;
    bool loadROMData(std::vector<uint8_t> rom);      // образ из памяти (тесты)
    void reset()    override;
    void runFrame() override;

    uint32_t*       getFramebuffer()       override { return bus_.ppu.framebuffer(); }
    const uint32_t* getFramebuffer() const override { return bus_.ppu.framebuffer(); }
    int getFrameWidth()  const override { return GbPpu::WIDTH;  }
    int getFrameHeight() const override { return GbPpu::HEIGHT; }

    // Кнопки в формате NES: A|B|Sel|Sta|Up|Dn|L|R (биты 7-0) — у Game Boy те же восемь.
    void setInput(int player, uint16_t buttons) override;

    const std::vector<float>& getAudioSamples() const override { return bus_.apu.samples(); }
    void clearAudioSamples() override { bus_.apu.clearSamples(); }

    std::string getConsoleName() const override { return "GB"; }
    bool isColor() const { return bus_.cgb(); }

    bool hasBattery()                      const override { return cart_.hasBattery(); }
    bool isSramDirty()                     const override { return cart_.isRamDirty(); }
    void clearSramDirty()                        override { cart_.clearRamDirty(); }
    bool saveSram(const std::string& path) const override;
    bool loadSram(const std::string& path)       override;

    bool saveState(std::ostream& os) const override;
    bool loadState(std::istream& is)       override;

    // Для тестов
    GbCpu&  cpu() { return cpu_; }
    GbBus&  bus() { return bus_; }

private:
    GbCart   cart_;
    GbBus    bus_;
    GbCpu    cpu_;
    uint64_t rtcDots_ = 0;              // точки экрана до следующей секунды часов MBC3

    template<class S> void serialize(S& s);
};
