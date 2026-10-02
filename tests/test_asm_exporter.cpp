// ---------------------------------------------------------------------------
// test_asm_exporter.cpp — Stage 6.17
//
// Unit tests for the Z88DK ASM Exporter.
// Tests static helpers (convertOperands, generateLabel, etc.) and
// integration tests with real ROM/RDB fixtures.
// ---------------------------------------------------------------------------

#include "asm_exporter.h"
#include "rdb_controller.h"

#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Test framework (minimal)
// ---------------------------------------------------------------------------

static int testsPassed = 0;
static int testsFailed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d): %s\n", msg, __LINE__, #cond); \
        testsFailed++; \
    } else { \
        testsPassed++; \
    } \
} while(0)

#define TEST_BEGIN(name) \
    static void test_##name() { \
        const char *testName_ = #name; \
        (void)testName_;

#define TEST_END() \
    }

#define RUN_TEST(name) do { \
    test_##name(); \
} while(0)

// ---------------------------------------------------------------------------
// Helper: write bytes to a temp file
// ---------------------------------------------------------------------------

static std::string writeTempFile(const std::string &name,
                                  const uint8_t *data, size_t size)
{
    std::string path = "/tmp/test_asm_export_" + name;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(data), size);
    f.close();
    return path;
}

static void cleanupTempFiles()
{
    system("rm -rf /tmp/test_asm_export_*");
}

// ---------------------------------------------------------------------------
// Test: generateLabel
// ---------------------------------------------------------------------------

TEST_BEGIN(generate_label)
{
    CHECK(AsmExporter::generateLabel(0x0000) == "label_0000", "label at 0000");
    CHECK(AsmExporter::generateLabel(0x1234) == "label_1234", "label at 1234");
    CHECK(AsmExporter::generateLabel(0xFFFF) == "label_FFFF", "label at FFFF");
    CHECK(AsmExporter::generateLabel(0x00FF) == "label_00FF", "label at 00FF");

    // Stability: same address → same label
    CHECK(AsmExporter::generateLabel(0x2A40) == AsmExporter::generateLabel(0x2A40),
          "labels are stable");
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: sanitizeLabel
// ---------------------------------------------------------------------------

TEST_BEGIN(sanitize_label)
{
    CHECK(AsmExporter::sanitizeLabel("draw_sprite") == "draw_sprite", "underscore OK");
    CHECK(AsmExporter::sanitizeLabel("my-func") == "my_func", "dash → underscore");
    CHECK(AsmExporter::sanitizeLabel("123abc") == "_123abc", "digit prefix → _");
    CHECK(AsmExporter::sanitizeLabel("hello world") == "hello_world", "space → underscore");
    CHECK(AsmExporter::sanitizeLabel("ABC") == "ABC", "uppercase preserved");
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: formatByte
// ---------------------------------------------------------------------------

TEST_BEGIN(format_byte)
{
    // A z80asm literal must start with a digit: bytes >= 0xA0 get a '0' guard.
    CHECK(AsmExporter::formatByte(0x00) == "00h", "00h");
    CHECK(AsmExporter::formatByte(0x0A) == "0Ah", "0Ah");
    CHECK(AsmExporter::formatByte(0xFF) == "0FFh", "0FFh (leading-0 guard)");
    CHECK(AsmExporter::formatByte(0xD3) == "0D3h", "0D3h (leading-0 guard)");
    CHECK(AsmExporter::formatByte(0xA1) == "0A1h", "0A1h (leading-0 guard)");
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: convertOperands — register operands
// ---------------------------------------------------------------------------

TEST_BEGIN(convert_operands_registers)
{
    // Single register.  M/PSW stay 8080 names (m/psw): the Z80 "(hl)"/"af"
    // forms are rejected by `z80asm -m=8080_strict`.
    CHECK(AsmExporter::convertOperands("MOV", "A") == "a", "MOV A");
    CHECK(AsmExporter::convertOperands("INR", "M") == "m", "INR M");
    CHECK(AsmExporter::convertOperands("PUSH", "PSW") == "psw", "PUSH PSW");
    CHECK(AsmExporter::convertOperands("INR", "B") == "b", "INR B");

    // Two registers (MOV A,B etc.)
    CHECK(AsmExporter::convertOperands("MOV", "A,B") == "a,b", "MOV A,B");
    CHECK(AsmExporter::convertOperands("MOV", "M,A") == "m,a", "MOV M,A");
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: convertOperands — register + hex
// ---------------------------------------------------------------------------

TEST_BEGIN(convert_operands_hex)
{
    // LXI H,0100 -> h,0100h  (starts with a digit: no guard needed)
    CHECK(AsmExporter::convertOperands("LXI", "H,0100") == "h,0100h", "LXI H,0100");
    // MVI A,D3 -> a,0d3h  (leading hex letter -> '0' guard so it is not a symbol)
    CHECK(AsmExporter::convertOperands("MVI", "A,D3") == "a,0d3h", "MVI A,D3");
    // ADI D3 -> 0d3h
    CHECK(AsmExporter::convertOperands("ADI", "D3") == "0d3h", "ADI D3");
    // CPI 00 -> 00h
    CHECK(AsmExporter::convertOperands("CPI", "00") == "00h", "CPI 00");
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: convertOperands — direct addresses
// ---------------------------------------------------------------------------

TEST_BEGIN(convert_operands_address)
{
    // STA 0038 → 0038h
    CHECK(AsmExporter::convertOperands("STA", "0038") == "0038h", "STA 0038");
    // LDA 8000 → 8000h
    CHECK(AsmExporter::convertOperands("LDA", "8000") == "8000h", "LDA 8000");
    // OUT 02 → 02h
    CHECK(AsmExporter::convertOperands("OUT", "02") == "02h", "OUT 02");
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: convertOperands — REAL disassembler format (space after comma)
// The producer emits "B, 082A" / "E, M"; fixtures must match it exactly, not a
// spaceless form (that mismatch is why the old tests stayed green on broken code).
// ---------------------------------------------------------------------------

TEST_BEGIN(convert_operands_realistic)
{
    // LXI emits "B, 082A" -> trims, keeps hex, adds h (starts with digit: no guard)
    CHECK(AsmExporter::convertOperands("LXI", "B, 082A") == "b,082ah", "LXI B, 082A");
    // MOV emits "E, M" -> trims; M stays 8080 "m" (not "(hl)")
    CHECK(AsmExporter::convertOperands("MOV", "E, M") == "e,m", "MOV E, M");
    CHECK(AsmExporter::convertOperands("MOV", "A, M") == "a,m", "MOV A, M");
    CHECK(AsmExporter::convertOperands("MOV", "M, A") == "m,a", "MOV M, A");
    // POP emits "PSW" -> "psw" (not "af")
    CHECK(AsmExporter::convertOperands("POP", "PSW") == "psw", "POP PSW");
    // The silent-corruption case: MVI "C, 10" must become hex 10h (=0x10),
    // never a bare "10" which z80asm reads as DECIMAL ten.
    CHECK(AsmExporter::convertOperands("MVI", "C, 10") == "c,10h", "MVI C, 10 -> 10h");
    // Immediate whose value starts with a hex letter gets the '0' guard
    CHECK(AsmExporter::convertOperands("MVI", "A, FF") == "a,0ffh", "MVI A, FF");
    // 16-bit address starting with a letter: "A1FF" -> "0a1ffh"
    CHECK(AsmExporter::convertOperands("STA", "A1FF") == "0a1ffh", "STA A1FF");
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: hex-literal invariant — no emitted literal may start with a hex letter
// (item 8.3).  Checks convertOperands and formatByte outputs directly.
// ---------------------------------------------------------------------------

static bool isHexLetter(char c) {
    return c=='a'||c=='b'||c=='c'||c=='d'||c=='e'||c=='f'
        || c=='A'||c=='B'||c=='C'||c=='D'||c=='E'||c=='F';
}

// Returns true if token is a hex literal that wrongly starts with a letter,
// i.e. matches /^[0-9A-Fa-f]*h$/ but first char is a-f (would be a symbol).
static bool isBadHexLiteral(const std::string &tok) {
    if (tok.size() < 2) return false;
    if (tok.back() != 'h' && tok.back() != 'H') return false;
    if (!isHexLetter(tok[0])) return false;          // must start with a-f to be bad
    for (size_t i = 0; i + 1 < tok.size(); ++i)      // body must be all hex digits
        if (!std::isxdigit(static_cast<unsigned char>(tok[i]))) return false;
    return true;
}

TEST_BEGIN(hex_literal_invariant)
{
    // formatByte: every byte value yields a literal starting with a digit.
    for (int b = 0; b <= 0xFF; ++b) {
        std::string s = AsmExporter::formatByte(static_cast<uint8_t>(b));
        CHECK(s.back() == 'h', "formatByte ends with h");
        if (!std::isdigit(static_cast<unsigned char>(s[0]))) { testsFailed++; \
            fprintf(stderr, "  FAIL: formatByte(0x%02X)='%s' not digit-led\n", b, s.c_str()); }
        CHECK(!isBadHexLiteral(s), "formatByte no letter-led literal");
    }
    // convertOperands over representative high-address/high-immediate operands.
    const char* addrs[] = {"A1FF","FFFF","B0C0","D3","FF","82A"};
    for (auto a : addrs) {
        std::string s = AsmExporter::convertOperands("STA", a);
        CHECK(!isBadHexLiteral(s), "convertOperands addr no letter-led literal");
    }
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: full export emits only buildable literals (space-format + high bytes)
// Wires a synthetic ROM with the tricky constructs through the real pipeline
// and scans code.asm for any letter-led hex literal (item 8.3 / 8.4 end-to-end).
// ---------------------------------------------------------------------------

TEST_BEGIN(export_hex_literals_buildable)
{
    std::vector<uint8_t> rom(64, 0x00);
    rom[0] = 0x3E; rom[1] = 0xFF;               // MVI A,FF   -> mvi a,0ffh
    rom[2] = 0x21; rom[3] = 0xFF; rom[4] = 0xFF; // LXI H,FFFF -> lxi h,0ffffh
    rom[5] = 0x06; rom[6] = 0x10;               // MVI B,10   -> mvi b,10h (not dec 16)
    rom[7] = 0x7E;                              // MOV A,M    -> mov a,m  (not (hl))
    rom[8] = 0xBE;                              // CMP M      -> cmp m
    rom[9] = 0x87;                              // ADD A      -> add a
    rom[10] = 0xAF;                             // XRA A      -> xra a
    rom[11] = 0xC3; rom[12] = 0x20; rom[13] = 0x00; // JMP 0020 (in ROM)
    rom[14] = 0xC9;                             // RET

    std::string romPath = writeTempFile("buildable.rom", rom.data(), rom.size());
    AsmExportConfig config;
    config.romPath = romPath;
    config.outputDir = "/tmp/test_asm_export_buildable";
    config.origin = 0x0000;

    AsmExporter exporter(config);
    AsmExportReport report = exporter.run();
    CHECK(!report.hasErrors(), "buildable export succeeds");

    std::ifstream cf("/tmp/test_asm_export_buildable/code/code.asm");
    CHECK(cf.is_open(), "code.asm readable");
    std::string content((std::istreambuf_iterator<char>(cf)),
                         std::istreambuf_iterator<char>());

    // Tokenize on separators; flag any letter-led hex literal (the bug class).
    std::string tok;
    bool foundBad = false;
    auto feed = [&](char sep) {
        if (!tok.empty()) { if (isBadHexLiteral(tok)) foundBad = true; tok.clear(); }
    };
    for (char c : content) {
        if (c==' '||c=='\t'||c==','||c==';'||c==':'||c=='\n'||c=='\r') feed(c);
        else tok += c;
    }
    feed('\0');
    CHECK(!foundBad, "no letter-led hex literal anywhere in code.asm");

    // Positive: the corrected 8080 forms appear (not the Z80/decimal bugs).
    // Mnemonic and operand are separated by a TAB in the emitted text.
    CHECK(content.find("mov\ta,m") != std::string::npos, "has 'mov a,m' (not (hl))");
    CHECK(content.find("cmp\tm")   != std::string::npos, "has 'cmp m'");
    CHECK(content.find("add\ta")   != std::string::npos, "has 'add a'");
    CHECK(content.find("xra\ta")   != std::string::npos, "has 'xra a'");
    CHECK(content.find("mvi\ta,0ffh") != std::string::npos, "has 'mvi a,0ffh'");
    CHECK(content.find("mvi\tb,10h")  != std::string::npos, "has 'mvi b,10h' (hex, not decimal)");
    CHECK(content.find("(hl)") == std::string::npos, "no Z80 '(hl)' emitted");
    CHECK(content.find("\taf") == std::string::npos &&
          content.find(" af") == std::string::npos, "no Z80 'af' emitted");

    cleanupTempFiles();
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: Full export with synthetic ROM
// ---------------------------------------------------------------------------

TEST_BEGIN(full_export_synthetic)
{
    // Create a small ROM fixture:
    //   0000: NOP
    //   0001: NOP
    //   0002: JMP 0010
    //   ...
    //   0010: CALL 0020
    //   0013: RET
    //   ...
    //   0020: NOP
    //   0021: RET
    std::vector<uint8_t> rom(256, 0x00); // NOP fill

    // 0002: JMP 0010
    rom[0x02] = 0xC3;
    rom[0x03] = 0x10;
    rom[0x04] = 0x00;

    // 0010: CALL 0020
    rom[0x10] = 0xCD;
    rom[0x11] = 0x20;
    rom[0x12] = 0x00;

    // 0013: RET
    rom[0x13] = 0xC9;

    // 0020: NOP (already 0x00)
    // 0021: RET
    rom[0x21] = 0xC9;

    std::string romPath = writeTempFile("synth.rom", rom.data(), rom.size());

    AsmExportConfig config;
    config.romPath = romPath;
    config.outputDir = "/tmp/test_asm_export_output";
    config.origin = 0x0000;

    AsmExporter exporter(config);
    AsmExportReport report = exporter.run();

    CHECK(!report.hasErrors(), "export succeeds");
    CHECK(report.romSize == 256, "ROM size = 256");
    CHECK(!report.romSha256.empty(), "SHA-256 computed");
    CHECK(report.codeRanges >= 1, "at least 1 code range");
    CHECK(report.generatedFiles.size() >= 3, "at least 3 files generated");

    // Verify main.asm was created
    bool hasMain = false;
    for (const auto &f : report.generatedFiles) {
        if (f == "main.asm") hasMain = true;
    }
    CHECK(hasMain, "main.asm generated");

    // Verify export.json was created
    bool hasJson = false;
    for (const auto &f : report.generatedFiles) {
        if (f == "export.json") hasJson = true;
    }
    CHECK(hasJson, "export.json generated");

    // Verify export.json content
    {
        std::ifstream jf("/tmp/test_asm_export_output/export.json");
        CHECK(jf.is_open(), "export.json readable");
        std::string content((std::istreambuf_iterator<char>(jf)),
                             std::istreambuf_iterator<char>());
        CHECK(content.find("\"rom_size\": 256") != std::string::npos,
              "export.json has rom_size");
        CHECK(content.find("\"origin\": \"0x0000\"") != std::string::npos,
              "export.json has origin");
    }

    // Verify main.asm content
    {
        std::ifstream af("/tmp/test_asm_export_output/main.asm");
        CHECK(af.is_open(), "main.asm readable");
        std::string content((std::istreambuf_iterator<char>(af)),
                             std::istreambuf_iterator<char>());
        CHECK(content.find("org\t0000h") != std::string::npos,
              "main.asm has org directive");
        CHECK(content.find("include") != std::string::npos,
              "main.asm has includes");
    }

    // Verify code.asm has instructions
    {
        std::ifstream cf("/tmp/test_asm_export_output/code/code.asm");
        CHECK(cf.is_open(), "code.asm readable");
        std::string content((std::istreambuf_iterator<char>(cf)),
                             std::istreambuf_iterator<char>());
        CHECK(content.find("nop") != std::string::npos,
              "code.asm has NOP instructions");
        CHECK(content.find("jmp") != std::string::npos,
              "code.asm has JMP instruction");
        CHECK(content.find("call") != std::string::npos,
              "code.asm has CALL instruction");
        CHECK(content.find("ret") != std::string::npos,
              "code.asm has RET instruction");
    }

    cleanupTempFiles();
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: Export with RDB
// ---------------------------------------------------------------------------

TEST_BEGIN(export_with_rdb)
{
    // Create ROM
    std::vector<uint8_t> rom(256, 0x00);
    rom[0x10] = 0xCD; // CALL 0030
    rom[0x11] = 0x30;
    rom[0x12] = 0x00;
    rom[0x13] = 0xC9; // RET
    rom[0x30] = 0x00; // NOP
    rom[0x31] = 0xC9; // RET

    // Data at 0x40
    rom[0x40] = 0x01;
    rom[0x41] = 0x02;
    rom[0x42] = 0x03;
    rom[0x43] = 0x04;

    std::string romPath = writeTempFile("rdb_test.rom", rom.data(), rom.size());

    // Create RDB
    RdbController rdb;
    rdb.initialize("vector06c", {});

    RdbObject func;
    func.address = 0x0030;
    func.type = RdbObjectType::Function;
    func.name = "my_func";
    func.comment = "Test function";
    rdb.addObject(func);

    RdbObject data;
    data.address = 0x0040;
    data.type = RdbObjectType::Data;
    data.name = "my_data";
    data.size = 4;
    data.hasSize = true;
    data.comment = "Test data table";
    rdb.addObject(data);

    std::string rdbContent;
    {
        // Serialize RDB to JSON manually — use saveAs
        std::string rdbPath = "/tmp/test_asm_export_rdb_test.rdb";
        rdb.saveAs(rdbPath);

        AsmExportConfig config;
        config.romPath = romPath;
        config.rdbPath = rdbPath;
        config.outputDir = "/tmp/test_asm_export_rdb_output";
        config.origin = 0x0000;

        AsmExporter exporter(config);
        AsmExportReport report = exporter.run();

        CHECK(!report.hasErrors(), "export with RDB succeeds");
        CHECK(report.objectCount == 2, "2 RDB objects");
        CHECK(report.dataRanges >= 1, "at least 1 data range");

        // Verify code.asm has the function label
        {
            std::ifstream cf("/tmp/test_asm_export_rdb_output/code/code.asm");
            std::string content((std::istreambuf_iterator<char>(cf)),
                                 std::istreambuf_iterator<char>());
            CHECK(content.find("my_func:") != std::string::npos,
                  "code.asm has my_func label");
            CHECK(content.find("; Test function") != std::string::npos,
                  "code.asm has RDB comment");
        }

        // Verify data.asm has the data block
        {
            std::ifstream df("/tmp/test_asm_export_rdb_output/data/data.asm");
            std::string content((std::istreambuf_iterator<char>(df)),
                                 std::istreambuf_iterator<char>());
            CHECK(content.find("my_data:") != std::string::npos,
                  "data.asm has my_data label");
            CHECK(content.find("defb") != std::string::npos,
                  "data.asm has defb");
            CHECK(content.find("01h") != std::string::npos,
                  "data.asm has correct byte 01h");
            CHECK(content.find("04h") != std::string::npos,
                  "data.asm has correct byte 04h");
            CHECK(content.find("; Test data table") != std::string::npos,
                  "data.asm has RDB comment");
        }
    }

    cleanupTempFiles();
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: Deterministic export
// ---------------------------------------------------------------------------

TEST_BEGIN(deterministic_export)
{
    std::vector<uint8_t> rom(128, 0x00);
    rom[0x05] = 0xC3; // JMP 0020
    rom[0x06] = 0x20;
    rom[0x07] = 0x00;
    rom[0x20] = 0xC9; // RET

    std::string romPath = writeTempFile("det.rom", rom.data(), rom.size());

    // Export twice
    AsmExportConfig config;
    config.romPath = romPath;
    config.outputDir = "/tmp/test_asm_export_det1";
    config.origin = 0x0000;

    AsmExporter(config).run();

    config.outputDir = "/tmp/test_asm_export_det2";
    AsmExporter(config).run();

    // Compare generated files
    auto readFile = [](const std::string &path) -> std::string {
        std::ifstream f(path);
        return std::string((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    };

    std::string code1 = readFile("/tmp/test_asm_export_det1/code/code.asm");
    std::string code2 = readFile("/tmp/test_asm_export_det2/code/code.asm");
    CHECK(code1 == code2, "code.asm is deterministic");

    std::string main1 = readFile("/tmp/test_asm_export_det1/main.asm");
    std::string main2 = readFile("/tmp/test_asm_export_det2/main.asm");
    CHECK(main1 == main2, "main.asm is deterministic");

    cleanupTempFiles();
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: Export without RDB
// ---------------------------------------------------------------------------

TEST_BEGIN(export_without_rdb)
{
    std::vector<uint8_t> rom(64, 0x00);
    rom[0x00] = 0xC9; // RET

    std::string romPath = writeTempFile("nordb.rom", rom.data(), rom.size());

    AsmExportConfig config;
    config.romPath = romPath;
    config.outputDir = "/tmp/test_asm_export_nordb";
    config.origin = 0x0000;
    // No RDB path

    AsmExporter exporter(config);
    AsmExportReport report = exporter.run();

    CHECK(!report.hasErrors(), "export without RDB succeeds");
    CHECK(report.objectCount == 0, "0 RDB objects");

    // data.asm should mention "No RDB available"
    {
        std::ifstream df("/tmp/test_asm_export_nordb/data/data.asm");
        std::string content((std::istreambuf_iterator<char>(df)),
                             std::istreambuf_iterator<char>());
        CHECK(content.find("No RDB") != std::string::npos,
              "data.asm mentions no RDB");
    }

    cleanupTempFiles();
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: ROM not found
// ---------------------------------------------------------------------------

TEST_BEGIN(rom_not_found)
{
    AsmExportConfig config;
    config.romPath = "/tmp/nonexistent_rom_file.rom";
    config.outputDir = "/tmp/test_asm_export_err";

    AsmExporter exporter(config);
    AsmExportReport report = exporter.run();

    CHECK(report.hasErrors(), "error when ROM not found");
    CHECK(!report.errors.empty(), "at least 1 error");
TEST_END()
}

// ---------------------------------------------------------------------------
// Stage 6.23 additions to test_asm_exporter.cpp
//
// Address-ordered layout (2.2), ROM-window clipping (2.1), coverage and gaps
// (2.3), Variable attribution and safe name substitution (2.4).
//
// Inserted before main() by .rt/fix6_exporter_tests.py.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Test: layout.asm is one address-ordered stream; gaps are explicit (2.1-2.3)
// Fixture ROM (origin 0), 9 bytes:
//   0000: JMP 0008        code
//   0003: 00              reachable by nothing -> must become gap_0003
//   0004: 'A','B','C','D' RDB Data "table_04"
//   0008: RET             code
// ---------------------------------------------------------------------------

TEST_BEGIN(layout_stream_and_coverage)
{
    std::vector<uint8_t> rom(9, 0x00);
    rom[0x00] = 0xC3; rom[0x01] = 0x08; rom[0x02] = 0x00;   // JMP 0008
    rom[0x03] = 0x00;                                           // unreachable byte
    rom[0x04] = 'A'; rom[0x05] = 'B'; rom[0x06] = 'C'; rom[0x07] = 'D';
    rom[0x08] = 0xC9;                                           // RET

    std::string romPath = writeTempFile("layout.rom", rom.data(), rom.size());

    RdbController rdb;
    rdb.initialize("vector06c", {});
    RdbObject data;
    data.address = 0x0004;
    data.type = RdbObjectType::Data;
    data.name = "table_04";
    data.size = 4;
    data.hasSize = true;
    data.comment = "Interleaved data block";
    rdb.addObject(data);
    std::string rdbPath = "/tmp/test_asm_export_layout.rdb";
    rdb.saveAs(rdbPath);

    AsmExportConfig config;
    config.romPath = romPath;
    config.rdbPath = rdbPath;
    config.outputDir = "/tmp/test_asm_export_layout";
    config.origin = 0x0000;

    AsmExporter exporter(config);
    AsmExportReport report = exporter.run();
    CHECK(!report.hasErrors(), "layout export succeeds");

    // --- coverage numbers (2.3) -------------------------------------------
    CHECK(report.coverage.romBytes == 9, "coverage: rom bytes = 9");
    CHECK(report.coverage.codeBytes == 4, "coverage: code bytes = 4");
    CHECK(report.coverage.dataBytes == 4, "coverage: data bytes = 4");
    CHECK(report.coverage.uncovered.size() == 1, "coverage: one uncovered run");
    if (!report.coverage.uncovered.empty()) {
        CHECK(report.coverage.uncovered[0].first == 0x0003 &&
              report.coverage.uncovered[0].second == 0x0003,
              "coverage: uncovered run is 0x0003-0x0003");
    }
    CHECK(!report.coverage.complete(), "coverage: not complete");

    bool gapWarned = false;
    for (const auto &w : report.warnings) {
        if (w.message.find("gap_0003") != std::string::npos) gapWarned = true;
    }
    CHECK(gapWarned, "uncovered byte raised a warning (2.3)");

    // --- layout.asm content and ordering ----------------------------------
    std::ifstream lf("/tmp/test_asm_export_layout/layout.asm");
    CHECK(lf.is_open(), "layout.asm generated");
    std::string layout((std::istreambuf_iterator<char>(lf)),
                        std::istreambuf_iterator<char>());

    size_t pJmp   = layout.find("jmp");
    size_t pGap   = layout.find("gap_0003:");
    size_t pData  = layout.find("table_04:");
    size_t pRet   = layout.find("ret");
    CHECK(pGap != std::string::npos, "layout: gap_0003 label present");
    CHECK(pData != std::string::npos, "layout: data label present");
    CHECK(pJmp != std::string::npos && pRet != std::string::npos,
          "layout: both instructions present");
    CHECK(pJmp < pGap && pGap < pData && pData < pRet,
          "layout: units are in address order");
    CHECK(layout.find("41h,42h,43h,44h") != std::string::npos,
          "layout: data bytes emitted verbatim");
    CHECK(layout.find("; Interleaved data block") != std::string::npos,
          "layout: RDB comment carried over");

    // --- main.asm builds the layout, not the two views --------------------
    std::ifstream mf("/tmp/test_asm_export_layout/main.asm");
    std::string mainAsm((std::istreambuf_iterator<char>(mf)),
                         std::istreambuf_iterator<char>());
    CHECK(mainAsm.find("include\t\"layout.asm\"") != std::string::npos,
          "main.asm includes layout.asm");
    CHECK(mainAsm.find("\n\tinclude\t\"code/code.asm\"") == std::string::npos &&
          mainAsm.find("\n\tinclude\t\"data/data.asm\"") == std::string::npos,
          "main.asm does not build the views (they stay commented)");

    // --- export.json carries coverage -------------------------------------
    std::ifstream jf("/tmp/test_asm_export_layout/export.json");
    std::string json((std::istreambuf_iterator<char>(jf)),
                     std::istreambuf_iterator<char>());
    CHECK(json.find("\"coverage\"") != std::string::npos, "json: coverage block");
    CHECK(json.find("\"code_bytes\": 4") != std::string::npos, "json: code_bytes");
    CHECK(json.find("\"data_bytes\": 4") != std::string::npos, "json: data_bytes");
    CHECK(json.find("\"outside_window_dropped\"") != std::string::npos,
          "json: outside_window_dropped");
    CHECK(json.find("\"complete\": false") != std::string::npos, "json: complete=false");

    cleanupTempFiles();
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: instructions outside the ROM window are not emitted (2.1)
//
// The analyzer seeds 0x0000 on purpose (real hardware falls through the reset
// NOP sled), but with origin 0x0100 those 256 bytes are not in the image.
// Emitting them shifted everything after them — that was the 91 %-differing
// build this item removes.
// ---------------------------------------------------------------------------

TEST_BEGIN(clipping_outside_window)
{
    std::vector<uint8_t> rom(0x100, 0x00);
    rom[0x00] = 0xC9;                                  // RET at 0x0100

    std::string romPath = writeTempFile("clip.rom", rom.data(), rom.size());

    AsmExportConfig config;
    config.romPath = romPath;
    config.outputDir = "/tmp/test_asm_export_clip";
    config.origin = 0x0100;

    AsmExporter exporter(config);
    AsmExportReport report = exporter.run();
    CHECK(!report.hasErrors(), "clipped export succeeds");

    CHECK(report.coverage.instructionCount == 1,
          "clip: only the in-window instruction is kept");
    CHECK(report.coverage.outsideWindowDropped == 0x100,
          "clip: 256 page-0 NOPs dropped");
    CHECK(report.coverage.codeBytes == 1, "clip: code bytes = 1");

    std::ifstream lf("/tmp/test_asm_export_clip/layout.asm");
    std::string layout((std::istreambuf_iterator<char>(lf)),
                        std::istreambuf_iterator<char>());
    CHECK(layout.find("nop") == std::string::npos,
          "clip: no phantom NOP emitted");
    CHECK(layout.find("ret") != std::string::npos, "clip: real instruction emitted");

    cleanupTempFiles();
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: Variable objects own bytes and name their operands (2.4)
// ---------------------------------------------------------------------------

TEST_BEGIN(variable_attribution_and_operand_name)
{
    std::vector<uint8_t> rom(0x14, 0x00);
    rom[0x00] = 0x3A; rom[0x01] = 0x10; rom[0x02] = 0x00;   // LDA 0010
    rom[0x03] = 0xC9;                                        // RET
    rom[0x10] = 0xFF;                                        // the variable itself
    rom[0x11] = 0x22;

    std::string romPath = writeTempFile("var.rom", rom.data(), rom.size());

    RdbController rdb;
    rdb.initialize("vector06c", {});
    RdbObject var;
    var.address = 0x0010;
    var.type = RdbObjectType::Variable;
    var.name = "var_flag";
    var.size = 2;
    var.hasSize = true;
    var.comment = "Named variable, not data";
    rdb.addObject(var);
    std::string rdbPath = "/tmp/test_asm_export_var.rdb";
    rdb.saveAs(rdbPath);

    AsmExportConfig config;
    config.romPath = romPath;
    config.rdbPath = rdbPath;
    config.outputDir = "/tmp/test_asm_export_var";
    config.origin = 0x0000;

    AsmExporter exporter(config);
    AsmExportReport report = exporter.run();
    CHECK(!report.hasErrors(), "variable export succeeds");

    // Before 6.23 Variable was filtered out of attribution: its bytes showed
    // up as unknown_ gaps and the name appeared only inside comments.
    CHECK(report.coverage.dataBytes == 2, "variable owns its 2 bytes");
    // Bytes 0x04..0x0F are a deliberate hole (neither code nor an RDB object);
    // what must hold is that the variable's own bytes are covered.
    bool varHole = false;
    for (const auto &g : report.coverage.uncovered) {
        if (g.first <= 0x0010 && 0x0010 <= g.second) varHole = true;
    }
    CHECK(!varHole, "the variable's own bytes are not an uncovered hole");

    std::ifstream lf("/tmp/test_asm_export_var/layout.asm");
    std::string layout((std::istreambuf_iterator<char>(lf)),
                        std::istreambuf_iterator<char>());
    CHECK(layout.find("var_flag:") != std::string::npos,
          "layout: variable emitted as a label");
    CHECK(layout.find("lda\tvar_flag") != std::string::npos,
          "layout: LDA operand replaced by the variable name");
    CHECK(layout.find("unknown_0010") == std::string::npos,
          "layout: variable is not an unknown_ gap");
    CHECK(layout.find("; Named variable, not data") != std::string::npos,
          "layout: variable comment carried over");

    cleanupTempFiles();
TEST_END()
}

// ---------------------------------------------------------------------------
// Test: an RDB overlap must not move a symbol (2.4, found by TESTAY acceptance)
//
// data_c_channel_state (0x0486+10) and var_ay_ch_a_period (0x048F+2) share one
// byte in TESTAY.  The loser used to keep its name on the shifted run, so the
// operand `shld 048Fh` assembled to 0490h — one silently corrupted byte.
// Fixture reproduces that shape at 0x0004/0x0007.
// ---------------------------------------------------------------------------

TEST_BEGIN(overlapping_name_keeps_true_address)
{
    std::vector<uint8_t> rom(0x10, 0x00);
    rom[0x00] = 0x3A; rom[0x01] = 0x07; rom[0x02] = 0x00;   // LDA 0007
    rom[0x03] = 0xC9;                                        // RET
    for (int i = 0x04; i <= 0x09; ++i) rom[i] = static_cast<uint8_t>(i);

    std::string romPath = writeTempFile("overlap.rom", rom.data(), rom.size());

    RdbController rdb;
    rdb.initialize("vector06c", {});
    RdbObject block;
    block.address = 0x0004;
    block.type = RdbObjectType::Data;
    block.name = "block_a";
    block.size = 4;      // 0x0004..0x0007
    block.hasSize = true;
    rdb.addObject(block);

    RdbObject var;
    var.address = 0x0007;   // overlaps the last byte of block_a
    var.type = RdbObjectType::Variable;
    var.name = "var_ptr";
    var.size = 2;           // 0x0007..0x0008
    var.hasSize = true;
    rdb.addObject(var);

    std::string rdbPath = "/tmp/test_asm_export_overlap.rdb";
    rdb.saveAs(rdbPath);

    AsmExportConfig config;
    config.romPath = romPath;
    config.rdbPath = rdbPath;
    config.outputDir = "/tmp/test_asm_export_overlap";
    config.origin = 0x0000;

    AsmExporter exporter(config);
    AsmExportReport report = exporter.run();
    CHECK(!report.hasErrors(), "overlap export succeeds");

    bool collisionWarned = false;
    for (const auto &w : report.warnings) {
        if (w.message.find("var_ptr") != std::string::npos &&
            w.message.find("overlaps") != std::string::npos) collisionWarned = true;
    }
    CHECK(collisionWarned, "overlap surfaced as a warning, not silently");

    std::ifstream lf("/tmp/test_asm_export_overlap/layout.asm");
    std::string layout((std::istreambuf_iterator<char>(lf)),
                        std::istreambuf_iterator<char>());

    CHECK(layout.find("block_a:") != std::string::npos,
          "winner of the collision keeps its label");
    CHECK(layout.find("var_ptr:") == std::string::npos,
          "loser is NOT emitted as a label at a shifted address");
    CHECK(layout.find("var_ptr\tequ\t0007h") != std::string::npos,
          "loser released as equ at its true address");
    CHECK(layout.find("lda\tvar_ptr") != std::string::npos,
          "operand still uses the name");

    cleanupTempFiles();
TEST_END()
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main()
{
    printf("ASM Exporter Tests (Stage 6.17 + 6.23):\n\n");

    RUN_TEST(generate_label);
    RUN_TEST(sanitize_label);
    RUN_TEST(format_byte);
    RUN_TEST(convert_operands_registers);
    RUN_TEST(convert_operands_hex);
    RUN_TEST(convert_operands_address);
    RUN_TEST(convert_operands_realistic);
    RUN_TEST(hex_literal_invariant);
    RUN_TEST(export_hex_literals_buildable);
    RUN_TEST(full_export_synthetic);
    RUN_TEST(export_with_rdb);
    RUN_TEST(deterministic_export);
    RUN_TEST(export_without_rdb);
    RUN_TEST(rom_not_found);
    RUN_TEST(layout_stream_and_coverage);
    RUN_TEST(clipping_outside_window);
    RUN_TEST(variable_attribution_and_operand_name);
    RUN_TEST(overlapping_name_keeps_true_address);

    printf("\nResults: %d passed, %d failed\n", testsPassed, testsFailed);
    return testsFailed > 0 ? 1 : 0;
}
