/* emu.h -- the minimal subset of MAME's emu.h and util::disasm_interface that src/devices/cpu/ssp1601/ssp1601d.cpp
 * (MAME, BSD-3-Clause) needs to compile outside MAME.  Used only by test/mame_dasm.cpp. */
#pragma once
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <string>
typedef uint16_t u16; typedef uint32_t u32; typedef uint32_t offs_t;
namespace util {
template <typename... A> std::string string_format(const char *f, A... a)
{ char b[256]; std::snprintf(b, sizeof b, f, a...); return b; }
template <typename... A> void stream_format(std::ostream &s, const char *f, A... a) { s << string_format(f, a...); }
inline void stream_format(std::ostream &s, const char *f, const std::string &x) { s << string_format(f, x.c_str()); }
template <typename... A> void stream_format(std::ostream &s, const char *f, const std::string &x, A... a)
{ s << string_format(f, x.c_str(), a...); }
class disasm_interface {
public:
    class data_buffer { public: const u16 *w; u16 r16(offs_t a) const { return w[a]; } };
    enum { SUPPORTED = 0x80000000, STEP_OUT = 0x20000000, STEP_OVER = 0x10000000, STEP_COND = 0x40000000 };
    virtual ~disasm_interface() = default;
    virtual u32 opcode_alignment() const = 0;
    virtual offs_t disassemble(std::ostream &, offs_t, const data_buffer &, const data_buffer &) = 0;
};
}
