// mame_dasm.cpp -- prints MAME's SSP1601 disassembly (ssp1601d.cpp, compiled unchanged) of a cartridge image range,
// one line an instruction: "<addr> <size> <text>"; with a fourth argument STEP the disassembly starts at every
// STEP-th word instead of following instruction sizes.  Used by tests/gate_o.sh and tests/gate_v.sh.
#include "emu.h"
#include "ssp1601d.h"
#include <sstream>
#include <vector>
#include <cstdlib>
int main(int argc, char **argv)
{
    if (argc < 4) return 1;
    FILE *f = std::fopen(argv[1], "rb"); if (!f) return 1;
    std::vector<unsigned char> raw(0x200000); size_t n = std::fread(raw.data(), 1, raw.size(), f); std::fclose(f);
    std::vector<u16> w(0x100002, 0); for (size_t i = 0; i < n / 2; i++) w[i] = (raw[2*i] << 8) | raw[2*i+1];
    util::disasm_interface::data_buffer b; b.w = w.data();
    ssp1601_disassembler d; long a = std::strtol(argv[2], 0, 0), e = std::strtol(argv[3], 0, 0);
    long step = argc > 4 ? std::strtol(argv[4], 0, 0) : 0;
    while (a < e) { std::ostringstream s; offs_t r = d.disassemble(s, a, b, b); int sz = r & 3;
        std::printf("%04lX %d %s\n", a, sz, s.str().c_str()); a += step ? step : sz; }
}
