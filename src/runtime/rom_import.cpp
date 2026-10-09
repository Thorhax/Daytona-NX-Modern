#include "runtime/rom_import.h"

#include "runtime/archive.h"
#include "runtime/zip.h"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <span>

namespace rt {

namespace {

enum Region { Program, MainData, CoproData, Polygons, Textures, CoproTables, SoundProgram, Pcm1, Pcm2, Samples1, Samples2 };

struct Load {
    const char *file;
    uint32_t crc;
    Region region;
    uint32_t offset, size; // ROM_LOAD32_WORD: 16-bit words into bytes 0-1 (offset 0) or 2-3 (offset 2) of each dword;
                           // the sound regions load whole files (see import_rom_set)
};

// MAME model2.cpp ROM_START(daytona93), ROM_START(daytona) and
// MODEL2_CPU_BOARD at dddd7368. The parts both sets share come first.
#define SHARED_LOADS                                                         \
    {"mpr-16528.10", 0x9CE591F6, MainData, 0x000000, 0x200000},              \
    {"mpr-16529.11", 0xF7095EAF, MainData, 0x000002, 0x200000},              \
    {"mpr-16537.ic28", 0x36B7C35A, CoproData, 0x000000, 0x200000},           \
    {"mpr-16536.ic29", 0x6D6AFED9, CoproData, 0x000002, 0x200000},           \
    {"mpr-16523.ic16", 0x2F484D42, Polygons, 0x000000, 0x200000},            \
    {"mpr-16518.ic20", 0xDF683BF7, Polygons, 0x000002, 0x200000},            \
    {"mpr-16524.ic17", 0x34658BD7, Polygons, 0x400000, 0x200000},            \
    {"mpr-16519.ic21", 0xFACD1C81, Polygons, 0x400002, 0x200000},            \
    {"mpr-16525.ic18", 0xFB517521, Polygons, 0x800000, 0x200000},            \
    {"mpr-16520.ic22", 0xD66BD9BD, Polygons, 0x800002, 0x200000},            \
    {"mpr-16522.25", 0x55D39A57, Textures, 0x000000, 0x200000},              \
    {"mpr-16521.24", 0xAF1934FB, Textures, 0x000002, 0x200000},              \
    {"opr-14742a.45", 0x90C6B117, CoproTables, 0x000000, 0x020000},          \
    {"opr-14743a.46", 0xAE7F446B, CoproTables, 0x000002, 0x020000},          \
    /* Model 1 sound board (segam1audio): the MultiPCMs' sample ROMs */      \
    {"mpr-16491.32", 0x89920903, Pcm1, 0x000000, 0x200000},                  \
    {"mpr-16492.33", 0x459E701B, Pcm1, 0x200000, 0x200000},                  \
    {"mpr-16493.4", 0x9990DB15, Pcm2, 0x000000, 0x200000},                   \
    {"mpr-16494.5", 0x600E1D6C, Pcm2, 0x200000, 0x200000}

// Daytona USA, Deluxe '93 (daytona93).
const Load kDaytona93[] = {
    {"epr-16530a.12", 0x39E962B5, Program, 0x000000, 0x020000},
    {"epr-16531a.13", 0x693126EB, Program, 0x000002, 0x020000},
    {"mpr-16526.8", 0x5273B8B5, MainData, 0x400000, 0x200000},
    {"mpr-16527.9", 0xFC4CB0EF, MainData, 0x400002, 0x200000},
    {"epr-16534a.6", 0x1BB0D72D, MainData, 0x800000, 0x100000},
    {"epr-16535a.7", 0x459A8BFB, MainData, 0x800002, 0x100000},
    {"epr-16646.ic19", 0x7BA9FD6B, Polygons, 0xC00000, 0x080000},
    {"epr-16645.ic23", 0x78FE0B8A, Polygons, 0xC00002, 0x080000},
    {"mpr-16517.27", 0x4705D3DD, Textures, 0x800000, 0x200000},
    {"mpr-16516.26", 0xA260D45D, Textures, 0x800002, 0x200000},
    // the sound board's 68000 program (ROM_LOAD16_WORD_SWAP)
    {"epr-16489.7", 0xC20E543E, SoundProgram, 0x000000, 0x020000},
    {"epr-16490.8", 0xC24EDAAB, SoundProgram, 0x020000, 0x020000},
    SHARED_LOADS,
};

// Daytona USA, Revision A, 1994 (daytona).
const Load kDaytona[] = {
    {"epr-16722a.12", 0x48B94318, Program, 0x000000, 0x020000},
    {"epr-16723a.13", 0x8AF8B32D, Program, 0x000002, 0x020000},
    {"mpr-16808.8", 0x44F1F5A0, MainData, 0x400000, 0x200000},
    {"mpr-16809.9", 0x37A2DD12, MainData, 0x400002, 0x200000},
    {"epr-16724a.6", 0x469F10FD, MainData, 0x800000, 0x080000},
    {"epr-16725a.7", 0xBA0DF8DB, MainData, 0x800002, 0x080000},
    {"mpr-16772.ic19", 0x770ED912, Polygons, 0xC00000, 0x200000},
    {"mpr-16771.ic23", 0xA2205124, Polygons, 0xC00002, 0x200000},
    {"mpr-16770.27", 0xF9FA7BFB, Textures, 0x800000, 0x200000},
    {"mpr-16769.26", 0xE57429E9, Textures, 0x800002, 0x200000},
    {"epr-16720.7", 0x8E73CFFD, SoundProgram, 0x000000, 0x020000},
    {"epr-16721.8", 0x1BB3B7B7, SoundProgram, 0x020000, 0x020000},
    SHARED_LOADS,
};
#undef SHARED_LOADS

// Virtua Fighter 2 (Version 2.1), Model 2A (vf2).
const Load kVF2[] = {
    {"epr-18385.12", 0x78ED2D41, Program, 0x000000, 0x020000},
    {"epr-18386.13", 0x3418F428, Program, 0x000002, 0x020000},
    {"epr-18387.14", 0x124A8453, Program, 0x040000, 0x020000},
    {"epr-18388.15", 0x8D347980, Program, 0x040002, 0x020000},
    {"mpr-17560.10", 0xD1389864, MainData, 0x000000, 0x200000},
    {"mpr-17561.11", 0xB98D0101, MainData, 0x000002, 0x200000},
    {"mpr-17558.8",  0x4B15F5A6, MainData, 0x400000, 0x200000},
    {"mpr-17559.9",  0xD3264DE6, MainData, 0x400002, 0x200000},
    {"mpr-17566.6",  0xFB41EF98, MainData, 0x800000, 0x200000},
    {"mpr-17567.7",  0xC3396922, MainData, 0x800002, 0x200000},
    {"mpr-17564.4",  0xD8062489, MainData, 0xC00000, 0x200000},
    {"mpr-17565.5",  0x0517C6E9, MainData, 0xC00002, 0x200000},
    {"mpr-17554.16", 0x27896D82, Polygons, 0x000000, 0x200000},
    {"mpr-17548.20", 0xC95FACC2, Polygons, 0x000002, 0x200000},
    {"mpr-17555.17", 0x4DF2810B, Polygons, 0x400000, 0x200000},
    {"mpr-17549.21", 0xE0BCE0E6, Polygons, 0x400002, 0x200000},
    {"mpr-17556.18", 0x41A47616, Polygons, 0x800000, 0x200000},
    {"mpr-17550.22", 0xC36FF3F5, Polygons, 0x800002, 0x200000},
    {"mpr-17553.25", 0x5DA1C5D3, Textures, 0x000000, 0x200000},
    {"mpr-17552.24", 0xE91E7427, Textures, 0x000002, 0x200000},
    {"mpr-17547.27", 0xBE940431, Textures, 0x800000, 0x200000},
    {"mpr-17546.26", 0x042A194B, Textures, 0x800002, 0x200000},
    {"opr-14742a.45", 0x90C6B117, CoproTables, 0x000000, 0x020000},
    {"opr-14743a.46", 0xAE7F446B, CoproTables, 0x000002, 0x020000},
    {"epr-17574.30", 0x4D4C3A55, SoundProgram, 0x000000, 0x080000},
    {"mpr-17573.31", 0xE43557FE, Samples1, 0x000000, 0x200000},
    {"mpr-17572.32", 0x4FEBECC8, Samples1, 0x200000, 0x200000},
    {"mpr-17571.36", 0x51CAA584, Samples2, 0x000000, 0x200000},
    {"mpr-17570.37", 0xBCCD324B, Samples2, 0x200000, 0x200000},
};

// Per set: its files; main_data's ROM_COPY source (copied to every 1 MB
// from there up to 0xF00000); where the TGP program the i960 uploads at
// boot sits in main_data or program.
struct Set {
    const char *name;
    const Load *loads;
    size_t count;
    uint32_t mirror_from, tgp_offset;
    uint32_t tgp_words, tgp_crc;
    bool tgp_in_program;
};
const Set kSets[] = {
    {"daytona93", kDaytona93, std::size(kDaytona93), 0x900000, 0x860020, 2024, 0xD6D611DDu, false},
    {"daytona", kDaytona, std::size(kDaytona), 0x800000, 0x800020, 2024, 0xD6D611DDu, false},
    {"vf2", kVF2, std::size(kVF2), 0x1000000, 0x3e90, 3802, 0xfd8d0383u, true},
};

const Set &this_set() {
    for (const Set &s : kSets)
        if (!std::strcmp(s.name, M2_ROMSET)) return s;
    throw ZipError(std::string("unknown ROM set ") + M2_ROMSET);
}

std::string hex8(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

} // namespace

std::vector<RomCheck> check_rom_set(const std::string &zip_path) {
    std::vector<RomCheck> out;
    const auto z = open_archive(zip_path);
    const Set &set = this_set();
    for (const Load &l : std::span(set.loads, set.count)) {
        RomCheck c;
        c.file = l.file;
        const auto it = z->entries().find(l.file);
        if (it == z->entries().end()) c.problem = "missing";
        else if (it->second.size != l.size) c.problem = "wrong size";
        else if (it->second.has_crc && it->second.crc != l.crc)
            c.problem = "wrong CRC " + hex8(it->second.crc) + " (expected " + hex8(l.crc) + ")";
        else c.ok = true;
        out.push_back(c);
    }
    return out;
}

M2Board::Images import_rom_set(const std::string &zip_path) {
    const auto z = open_archive(zip_path);
    M2Board::Images img;
    img.program.assign(0x200000, 0);
    img.main_data.assign(0x2000000, 0);
    img.copro_data.assign(0x800000, 0);
    img.polygons.assign(0x2000000, 0);
    img.textures.assign(0x1000000, 0);
    img.copro_tables.assign(0x40000, 0);
    img.sound_program.assign(0x80000, 0);
    img.pcm1.assign(0x400000, 0);
    img.pcm2.assign(0x400000, 0);
    const Set &set = this_set();
    for (const Load &l : std::span(set.loads, set.count)) {
        const std::vector<uint8_t> data = z->read(l.file); // CRC against the archive's own
        if (data.size() != l.size || crc32(data.data(), data.size()) != l.crc)
            throw ZipError(std::string(l.file) + ": not the " + set.name + " ROM this build was recompiled from");
        std::vector<uint8_t> *r = nullptr;
        switch (l.region) {
        case Program: r = &img.program; break;
        case MainData: r = &img.main_data; break;
        case CoproData: r = &img.copro_data; break;
        case Polygons: r = &img.polygons; break;
        case Textures: r = &img.textures; break;
        case CoproTables: r = &img.copro_tables; break;
        case SoundProgram: // 68000, big-endian: the files hold byte-swapped words
            for (uint32_t i = 0; i < l.size; i += 2) {
                img.sound_program[l.offset + i] = data[i + 1];
                img.sound_program[l.offset + i + 1] = data[i];
            }
            continue;
        case Pcm1: std::copy(data.begin(), data.end(), img.pcm1.begin() + l.offset); continue;
        case Pcm2: std::copy(data.begin(), data.end(), img.pcm2.begin() + l.offset); continue;
        case Samples1:
            for (uint32_t i = 0; i < l.size; i += 2) {
                img.pcm1[l.offset + i] = data[i + 1];
                img.pcm1[l.offset + i + 1] = data[i];
            }
            continue;
        case Samples2:
            for (uint32_t i = 0; i < l.size; i += 2) {
                img.pcm2[l.offset + i] = data[i + 1];
                img.pcm2[l.offset + i + 1] = data[i];
            }
            continue;
        }
        for (uint32_t w = 0; w < l.size / 2; w++) {
            (*r)[l.offset + w * 4] = data[w * 2];
            (*r)[l.offset + w * 4 + 1] = data[w * 2 + 1];
        }
    }
    for (uint32_t dst = set.mirror_from + 0x100000; dst <= 0xF00000; dst += 0x100000) // ROM_COPY mirrors
        std::copy_n(img.main_data.begin() + set.mirror_from, 0x100000, img.main_data.begin() + dst);
    return img;
}

const char *rom_set_name() { return M2_ROMSET; }

std::vector<uint8_t> tgp_program(const M2Board::Images &img) {
    const Set &set = this_set();
    const uint8_t *base = set.tgp_in_program ? img.program.data() : img.main_data.data();
    const uint8_t *p = base + set.tgp_offset;
    if (crc32(p, set.tgp_words * 4) != set.tgp_crc) throw ZipError("TGP program not where expected");
    return {p, p + set.tgp_words * 4};
}

} // namespace rt
