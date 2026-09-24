#pragma once
#include <cstdint>
#include <cstddef>
#include <istream>
#include <ostream>
#include <type_traits>
#include <vector>

// ─── Сериализация состояния для save states ──────────────────────────────────
// Каждый блок эмулятора описывает свои поля ОДИН раз:
//
//     template<class S> void serialize(S& s) { s.io(a_); s.io(ram_); ... }
//
// а S — либо StateWriter (пишет в поток), либо StateReader (читает). Так
// запись и чтение не могут разойтись. Формат — сырые байты полей подряд:
// состояние читает та же сборка, что его записала (формат версионируется).
class StateWriter {
public:
    static constexpr bool reading = false;
    explicit StateWriter(std::ostream& os) : os_(os) {}

    template<class T> void io(const T& v)
    {
        static_assert(std::is_trivially_copyable<T>::value, "только простые типы");
        os_.write(reinterpret_cast<const char*>(&v), (std::streamsize)sizeof(T));
    }
    // Вектор: пишем длину, затем данные.
    template<class T> void vec(const std::vector<T>& v)
    {
        static_assert(std::is_trivially_copyable<T>::value, "только простые типы");
        uint32_t n = (uint32_t)v.size();
        io(n);
        if (n) os_.write(reinterpret_cast<const char*>(v.data()), (std::streamsize)(n * sizeof(T)));
    }
    // Заголовок/маркер совместимости: при записи просто пишется.
    template<class T> bool expect(const T& v) { io(v); return true; }
    bool ok() const { return os_.good(); }

private:
    std::ostream& os_;
};

class StateReader {
public:
    static constexpr bool reading = true;
    explicit StateReader(std::istream& is) : is_(is) {}

    template<class T> void io(T& v)
    {
        static_assert(std::is_trivially_copyable<T>::value, "только простые типы");
        if (ok()) is_.read(reinterpret_cast<char*>(&v), (std::streamsize)sizeof(T));
    }
    // Вектор должен совпасть по длине с уже выделенным (та же игра): иначе
    // состояние чужое, и чтение проваливается целиком.
    template<class T> void vec(std::vector<T>& v)
    {
        uint32_t n = 0;
        io(n);
        if (!ok()) return;
        if (n != v.size()) { bad_ = true; return; }
        if (n) is_.read(reinterpret_cast<char*>(v.data()), (std::streamsize)(n * sizeof(T)));
    }
    // Проверка заголовка/совместимости: несовпадение портит всё чтение.
    template<class T> bool expect(const T& want)
    {
        T got{};
        io(got);
        if (!ok() || !(got == want)) bad_ = true;
        return !bad_;
    }
    bool ok() const { return !bad_ && is_.good(); }

private:
    std::istream& is_;
    bool bad_ = false;
};
