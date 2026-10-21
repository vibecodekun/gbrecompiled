#include "recompiler/codegen/exhaustive.h"
#include "recompiler/rom.h"
#include "gb_sha256.h"
#include <algorithm>
#include <array>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace gbrecomp::codegen {
static std::string c_string(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

// [options] rom_patches: a native body for every instruction position whose
// bytes a patch changes, compiled from the patched bytes, and each patch's
// changed bytes and result digest (runtime/include/gb_rom_patch.h).
template <typename InstructionId>
static void append_patch_slots(GeneratedOutput& output, const uint8_t* stock, size_t size,
                               const GeneratorOptions& options, const Decoder& decoder,
                               InstructionId& instruction_id, const std::string& preamble,
                               unsigned banks) {
    const std::string prefix = options.output_prefix + "_patch";
    std::ostringstream code;
    code << preamble << "#include \"gb_rom_patch.h\"\n";
    code << "const uint8_t " << prefix << "_opcode_lengths[256] = {\n";
    for (unsigned op = 0; op < 256; ++op) {
        const uint8_t bytes[3] = {uint8_t(op), 0, 0};
        code << unsigned(decoder.decode_bytes(0, 0, bytes).length) << (op % 32 == 31 ? ",\n" : ",");
    }
    code << "};\n";
    code << "uint32_t* " << prefix << "_overrides[" << banks + 1 << "];\n";

    struct Entry {
        uint32_t slot, id;
        uint8_t length;
        std::array<uint8_t, 3> bytes;
        bool operator<(const Entry& o) const {
            return std::tie(slot, length, bytes) < std::tie(o.slot, o.length, o.bytes);
        }
        bool operator==(const Entry& o) const {
            return slot == o.slot && length == o.length && bytes == o.bytes;
        }
    };
    std::vector<Entry> entries;
    std::ostringstream known, ranges;
    for (size_t index = 0; index < options.rom_patches.size(); ++index) {
        const auto& patch = options.rom_patches[index];
        const uint8_t* image = patch.image.data();
        size_t changed_bytes = 0, positions = 0;
        ranges << "static const GBPatchRange " << prefix << "_changed_" << index << "[] = {\n";
        size_t range_count = 0;
        for (size_t i = 0; i < size;) {
            if (image[i] == stock[i]) { ++i; continue; }
            size_t j = i;
            while (j < size && image[j] != stock[j]) ++j;
            ranges << '{' << i << ',' << (j - i) << "},";
            if (++range_count % 8 == 0) ranges << '\n';
            changed_bytes += j - i;
            i = j;
        }
        if (!range_count) ranges << "{0,0}";
        ranges << "\n};\n";
        for (unsigned window = 0; window <= banks; ++window) {
            const unsigned bank = window ? window - 1 : 0;
            const unsigned base = window ? 0x4000 : 0;
            const size_t start = size_t(bank) * 0x4000;
            bool any = false;
            for (size_t i = 0; i < 0x4000 && !any; ++i) any = image[start + i] != stock[start + i];
            if (!any) continue;
            for (unsigned bug = 0; bug < 2; ++bug) {
                for (unsigned off = 0; off < 0x4000; ++off) {
                    const size_t physical = start + off;
                    uint8_t bytes[3] = {image[physical], 0, 0};
                    for (unsigned j = 1; j < 3; ++j) {
                        unsigned delta = j - bug;
                        if (off + delta < 0x4000) bytes[j] = image[physical + delta];
                    }
                    auto ins = decoder.decode_bytes(uint16_t(base + off), uint8_t(bank), bytes);
                    // The bytes the body is compiled from (gb_rom_patch.c matches the same).
                    unsigned n = ins.length > bug ? ins.length - bug : 1;
                    if (off + n > 0x4000) n = 0x4000 - off;
                    bool dirty = false;
                    for (unsigned k = 0; k < n && !dirty; ++k) dirty = image[physical + k] != stock[physical + k];
                    if (!dirty) continue;
                    const bool live = off + ins.length - bug > 0x4000 && ins.length > 1;
                    Entry entry{(window << 15) | (bug << 14) | off,
                                instruction_id(ins, bytes, bug, live, true), uint8_t(n), {0, 0, 0}};
                    for (unsigned k = 0; k < n; ++k) entry.bytes[k] = image[physical + k];
                    entries.push_back(entry);
                    ++positions;
                }
            }
        }
        char digest[65];
        gb_sha256_hex(image, size, digest);
        known << "    {" << c_string(patch.name) << ", " << c_string(patch.title.empty() ? patch.name : patch.title)
              << ", \"" << patch.patch_sha256 << "\", \"" << digest
              << "\", " << prefix << "_changed_" << index << ", " << range_count << "},\n";
        std::cout << "ROM patch " << patch.name << ": " << changed_bytes << " bytes changed, "
                  << positions << " instruction positions compiled\n";
    }
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    code << "const GBPatchSlot " << prefix << "_slots[] = {\n";
    for (const Entry& e : entries)
        code << '{' << e.slot << ',' << e.id << ',' << unsigned(e.length) << ",{" << unsigned(e.bytes[0])
             << ',' << unsigned(e.bytes[1]) << ',' << unsigned(e.bytes[2]) << "}},\n";
    if (entries.empty()) code << "{0,0,0,{0,0,0}}\n";
    code << "};\n";
    code << "const size_t " << prefix << "_slot_count = " << entries.size() << ";\n";
    code << ranges.str();
    code << "const GBKnownPatch " << options.output_prefix << "_known_patches[] = {\n" << known.str();
    if (options.rom_patches.empty()) code << "    {0, 0, 0, 0, 0, 0}\n";
    code << "};\n";
    code << "const size_t " << options.output_prefix << "_known_patch_count = "
         << options.rom_patches.size() << ";\n";
    output.extra_files.push_back({options.output_prefix + "_exhaustive_patches.c", code.str(), true});
    if (!options.rom_patches.empty())
        std::cout << "ROM patches: " << entries.size() << " distinct patched instruction positions\n";
}

void append_exhaustive_rom(GeneratedOutput& output, const uint8_t* data, size_t size,
                           const GeneratorOptions& options) {
    if (size < 0x8000 || size > 0x400000 || size % 0x4000 || data[0x147] < 0x19 || data[0x147] > 0x1E)
        throw std::runtime_error("exhaustive_rom currently requires an MBC5 ROM of 2..256 full banks");
    auto rom = ROM::load_from_buffer(std::vector<uint8_t>(data, data + size), options.output_prefix);
    Decoder decoder(*rom);
    const std::string prefix = options.output_prefix + "_exhaustive";
    const std::string preamble = "/* All-ROM native entries, generated ahead of time. */\n#include \"" +
        options.output_prefix + "_internal.h\"\n#include <stdio.h>\n#include <stdlib.h>\n";
    std::vector<std::string> bodies;
    std::unordered_map<std::string, uint32_t> body_ids;
    std::unordered_map<uint64_t, uint32_t> instruction_ids;
    std::ostringstream variants;
    variants << "# id bank address halt_bug live_operands opcode\n";
    auto intern_body = [&](const std::string& body, const Instruction& ins, bool bug, bool live) {
        auto [it, inserted] = body_ids.emplace(body, uint32_t(bodies.size()));
        if (inserted) {
            bodies.push_back(body);
            variants << it->second << ' ' << unsigned(ins.bank) << ' ' << ins.address << ' '
                     << bug << ' ' << live << ' ' << unsigned(ins.opcode) << '\n';
        }
        return it->second;
    };
    // A CB second byte at $3FFF/$7FFF is fetched from a different memory window.
    // Precompile all 256 possibilities; the boundary wrapper only selects them.
    std::array<uint32_t, 256> cb_ids;
    for (unsigned cb = 0; cb < 256; ++cb) {
        uint8_t bytes[3] = {0xCB, uint8_t(cb), 0};
        auto ins = decoder.decode_bytes(0, 0, bytes);
        cb_ids[cb] = intern_body(emit_exhaustive_instruction(ins, options, false, false), ins, false, false);
    }
    const std::string cb_boundary = "    " + prefix + "_cb_boundary(ctx);\n";
    // A patch's changed instruction never takes the hook of the stock
    // instruction it replaced (gb_rom_patch.h).
    GeneratorOptions patched_options = options;
    patched_options.imm_override_sites.clear();
    auto instruction_id = [&](const Instruction& ins, const uint8_t bytes[3], bool bug, bool live,
                              bool patched) {
        uint64_t key = uint64_t(bytes[0]) | (uint64_t(bug) << 24) | (uint64_t(live) << 25);
        if (!live && ins.length > 1) key |= uint64_t(bytes[1]) << 8;
        if (!live && ins.length > 2) key |= uint64_t(bytes[2]) << 16;
        const uint32_t source = (uint32_t(ins.bank) << 16) | ins.address;
        if (!patched && options.imm_override_sites.count(source)) key |= (uint64_t(source) + 1) << 32;
        auto found = instruction_ids.find(key);
        if (found != instruction_ids.end()) return found->second;
        auto body = ins.is_cb_prefixed && live ? cb_boundary
                  : emit_exhaustive_instruction(ins, patched ? patched_options : options, bug, live);
        uint32_t id = intern_body(body, ins, bug, live);
        instruction_ids.emplace(key, id);
        return id;
    };
    size_t illegal = 0;
    const unsigned banks = unsigned(size / 0x4000);
    std::ostringstream directory, report;
    directory << preamble;
    directory << "void " << prefix << "_invoke(GBContext* ctx, uint32_t id);\n";
    // Window 0 is fixed ROM; window bank+1 is the switchable ROM window,
    // including MBC5's legal bank-zero mapping.
    for (unsigned window = 0; window <= banks; ++window) {
        unsigned bank = window ? window - 1 : 0;
        unsigned base = window ? 0x4000 : 0;
        std::ostringstream table, listing;
        table << preamble << "const uint32_t " << prefix << "_map_" << window << "[32768] = {\n";
        listing << "; Conservative disassembly at EVERY byte; entries overlap and include data.\n";
        size_t bank_illegal = 0;
        for (unsigned bug = 0; bug < 2; ++bug) {
            for (unsigned off = 0; off < 0x4000; ++off) {
                size_t physical = bank * 0x4000 + off;
                uint8_t bytes[3] = {data[physical], 0, 0};
                for (unsigned j = 1; j < 3; ++j) {
                    unsigned delta = j - bug;
                    if (off + delta < 0x4000) bytes[j] = data[physical + delta];
                }
                auto ins = decoder.decode_bytes(uint16_t(base + off), uint8_t(bank), bytes);
                bool live = off + ins.length - bug > 0x4000 && ins.length > 1;
                uint32_t id = instruction_id(ins, bytes, bug, live, false);
                table << id << ',';
                if (off % 16 == 15) table << '\n';
                if (!bug) {
                    if (ins.type == InstructionType::UNDEFINED) ++bank_illegal;
                    listing << std::hex << std::setfill('0') << std::setw(2) << bank << ':'
                            << std::setw(4) << (base + off) << "  " << std::setw(2) << unsigned(bytes[0])
                            << "  " << ins.disassemble() << (live ? " ; operands fetched across window" : "") << '\n';
                }
            }
        }
        table << "};\n";
        output.extra_files.push_back({prefix + "_map_" + std::to_string(window) + ".c", table.str(), true});
        output.extra_files.push_back({prefix + "_bank_" + std::to_string(window) + ".asm", listing.str(), false});
        directory << "extern const uint32_t " << prefix << "_map_" << window << "[32768];\n";
        if (window != 1) illegal += bank_illegal; // Count physical bank zero once.
    }
    append_patch_slots(output, data, size, options, decoder, instruction_id, preamble, banks);
    directory << "#include \"gb_rom_patch.h\"\n";
    directory << "extern uint32_t* " << options.output_prefix << "_patch_overrides[];\n";
    directory << "static const uint32_t* const maps[] = {\n";
    for (unsigned w = 0; w <= banks; ++w) directory << "    " << prefix << "_map_" << w << ",\n";
    directory << "};\n";
    directory << "uint32_t " << prefix << "_id(unsigned bank, unsigned pc, unsigned bug) {\n"
              << "    unsigned window = pc < 0x4000 ? 0 : bank + 1;\n"
              << "    if (pc >= 0x8000 || window > " << banks << ") return UINT32_MAX;\n"
              << "    const unsigned slot = (bug ? 16384 : 0) + (pc & 0x3FFF);\n"
              << "    const uint32_t* patched = " << options.output_prefix << "_patch_overrides[window];\n"
              << "    if (patched && patched[slot]) { gb_rom_patch_ran(window, slot); return patched[slot] - 1; }\n"
              << "    return maps[window][slot];\n}\n";
    directory << "void " << prefix << "_step(GBContext* ctx) {\n"
              << "    uint32_t id = " << prefix << "_id(ctx->rom_bank, ctx->pc, ctx->halt_bug);\n"
              << "    if (id == UINT32_MAX) { fprintf(stderr, \"[NATIVE] Unmapped ROM %03X:%04X\\n\", ctx->rom_bank, ctx->pc); abort(); }\n"
              << "    if (id == GB_PATCH_UNCOMPILED_ID) { gb_rom_patch_uncompiled(ctx); return; }\n"
              << "    " << prefix << "_invoke(ctx, id);\n}\n";
    directory << "void " << options.output_prefix << "_uncovered(GBContext* ctx) {\n"
              << "    if (ctx->pc < 0x8000) { " << prefix << "_step(ctx); return; }\n"
              << "    fprintf(stderr, \"[NATIVE] Uncompiled writable-memory execution %03X:%04X; interpreter disabled\\n\", ctx->rom_bank, ctx->pc); abort();\n}\n";
    directory << "void " << prefix << "_cb_boundary(GBContext* ctx) {\n"
              << "    static const uint32_t ids[256] = {\n";
    for (auto id : cb_ids) directory << id << ',';
    directory << "\n    };\n    " << prefix << "_invoke(ctx, ids[gb_read8(ctx, ctx->pc + 1)]);\n}\n";
    output.extra_files.push_back({prefix + "_maps.c", directory.str(), true});
    std::ostringstream invoke;
    invoke << preamble;
    const size_t chunk_size = 512;
    for (size_t begin = 0; begin < bodies.size(); begin += chunk_size) {
        size_t end = std::min(begin + chunk_size, bodies.size());
        std::string group = prefix + "_group_" + std::to_string(begin / chunk_size);
        std::ostringstream code;
        code << preamble << "void " << prefix << "_cb_boundary(GBContext* ctx);\n";
        for (size_t id = begin; id < end; ++id)
            code << "static void native_" << id << "(GBContext* ctx) {\n" << bodies[id] << "}\n";
        code << "void " << group << "(GBContext* ctx, unsigned id) {\n"
             << "    static void (*const entries[])(GBContext*) = {\n";
        for (size_t id = begin; id < end; ++id) code << "native_" << id << ',';
        code << "\n    };\n    entries[id](ctx);\n}\n";
        output.extra_files.push_back({group + ".c", code.str(), true});
        invoke << "void " << group << "(GBContext* ctx, unsigned id);\n";
    }
    invoke << "void " << prefix << "_invoke(GBContext* ctx, uint32_t id) {\n"
           << "    static void (*const groups[])(GBContext*, unsigned) = {\n";
    for (size_t begin = 0; begin < bodies.size(); begin += chunk_size)
        invoke << prefix << "_group_" << begin / chunk_size << ',';
    invoke << "\n    };\n    groups[id / 512](ctx, id % 512);\n}\n";
    output.extra_files.push_back({prefix + "_invoke.c", invoke.str(), true});
    output.extra_files.push_back({prefix + "_variants.tsv", variants.str(), false});
    char digest[65];
    gb_sha256_hex(data, size, digest);
    report << "{\n  \"rom_sha256\": \"" << digest << "\",\n  \"banks\": " << banks << ",\n  \"rom_bytes\": " << size
           << ",\n  \"mapped_pc_slots\": " << (banks + 1) * 16384
           << ",\n  \"halt_bug_variants\": true,\n  \"native_bodies\": " << bodies.size()
           << ",\n  \"illegal_opcode_positions\": " << illegal
           << ",\n  \"missing_rom_entries\": 0,\n  \"unknown_ram_policy\": \"fatal diagnostic, never interpreter\"\n}\n";
    output.extra_files.push_back({prefix + ".json", report.str(), false});
    std::cout << "Exhaustive ROM: " << banks << " banks, " << (banks + 1) * 16384
              << " PC slots plus HALT-bug variants, " << bodies.size() << " native instruction bodies\n";
}
}
