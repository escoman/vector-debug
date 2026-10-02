// ---------------------------------------------------------------------------
// asm_exporter_main.cpp — Stage 6.17
//
// CLI entry point for v06c-asm-export.
//
// Usage:
//   v06c-asm-export --rom putup.rom --rdb putup.rdb --output build/putup
//   v06c-asm-export --rom clrs.rom --output build/clrs
//
// If --rdb is omitted, automatically searches for <rom>.rdb.
// If no RDB is found, exports with code analysis only.
// ---------------------------------------------------------------------------

#include "asm_exporter.h"

#include <cstdio>
#include <cstring>
#include <string>

static void printUsage(const char *progname)
{
    fprintf(stderr,
        "Usage: %s --rom <file> [--rdb <file>] --output <dir> [--origin <hex>]\n"
        "\n"
        "Options:\n"
        "  --rom <file>     Path to ROM file (required)\n"
        "  --rdb <file>     Path to RDB file (optional, auto-detected)\n"
        "  --output <dir>   Output directory for generated ASM files\n"
        "  --origin <hex>   ROM load origin (default: 0100)\n"
        "  --strict         exit with code 2 when ROM coverage is incomplete\n"
        "\n"
        "If --rdb is omitted, looks for <rom_basename>.rdb next to the ROM.\n",
        progname);
}

int main(int argc, char *argv[])
{
    AsmExportConfig config;
    bool strict = false;   // Stage 6.23 §2.3: CI must fail on lost bytes

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            config.romPath = argv[++i];
        } else if (strcmp(argv[i], "--rdb") == 0 && i + 1 < argc) {
            config.rdbPath = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            config.outputDir = argv[++i];
        } else if (strcmp(argv[i], "--origin") == 0 && i + 1 < argc) {
            unsigned int origin = 0;
            if (sscanf(argv[++i], "%x", &origin) == 1 && origin <= 0xFFFF) {
                config.origin = static_cast<uint16_t>(origin);
            } else {
                fprintf(stderr, "Error: invalid origin: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--strict") == 0) {
            strict = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Error: unknown argument: %s\n", argv[i]);
            printUsage(argv[0]);
            return 1;
        }
    }

    // Validate required arguments
    if (config.romPath.empty()) {
        fprintf(stderr, "Error: --rom is required\n");
        printUsage(argv[0]);
        return 1;
    }
    if (config.outputDir.empty()) {
        fprintf(stderr, "Error: --output is required\n");
        printUsage(argv[0]);
        return 1;
    }

    // Auto-detect RDB if not specified
    if (config.rdbPath.empty()) {
        // Try <rom_path_without_ext>.rdb
        std::string rdbPath = config.romPath;
        auto dotPos = rdbPath.find_last_of('.');
        if (dotPos != std::string::npos) {
            rdbPath = rdbPath.substr(0, dotPos) + ".rdb";
        } else {
            rdbPath = rdbPath + ".rdb";
        }

        // Check if file exists
        FILE *f = fopen(rdbPath.c_str(), "r");
        if (f) {
            fclose(f);
            config.rdbPath = rdbPath;
            fprintf(stderr, "Auto-detected RDB: %s\n", rdbPath.c_str());
        }
    }

    // Run export
    AsmExporter exporter(config);
    AsmExportReport report = exporter.run();

    // Print report
    printf("=== ASM Export Report ===\n");
    printf("ROM:              %s\n", report.romFile.c_str());
    printf("ROM size:         %lu bytes\n", (unsigned long)report.romSize);
    printf("ROM SHA-256:      %s\n", report.romSha256.c_str());

    char originBuf[8], entryBuf[8];
    snprintf(originBuf, sizeof(originBuf), "0x%04X", report.origin);
    snprintf(entryBuf, sizeof(entryBuf), "0x%04X", report.mappingEntryPoint);
    printf("Origin:           %s\n", originBuf);
    printf("Mapping entry:    %s\n", entryBuf);
    printf("RDB objects:      %d\n", report.objectCount);
    printf("RDB links:        %d\n", report.linkCount);
    printf("Code ranges:      %d\n", report.codeRanges);
    printf("Data ranges:      %d\n", report.dataRanges);
    printf("Conflicts:        %d\n", report.conflicts);
    printf("Unresolved refs:  %d\n", report.unresolvedReferences);
    printf("Generated files:  %d\n", (int)report.generatedFiles.size());

    const AsmCoverage &cov = report.coverage;
    printf("\nCoverage (Stage 6.23):\n");
    printf("  code bytes:     %lu / %lu ROM\n", (unsigned long)cov.codeBytes,
           (unsigned long)cov.romBytes);
    printf("  data bytes:     %lu\n", (unsigned long)cov.dataBytes);
    printf("  instructions:   %lu\n", (unsigned long)cov.instructionCount);
    printf("  outside window: %lu dropped (clipped)\n",
           (unsigned long)cov.outsideWindowDropped);
    printf("  uncovered:      %d run(s)\n", (int)cov.uncovered.size());
    for (const auto &g : cov.uncovered) {
        printf("    0x%04X-0x%04X (%u bytes)\n", g.first, g.second,
               (unsigned)(g.second - g.first + 1));
    }
    printf("  complete:       %s\n", cov.complete() ? "true" : "FALSE");

    for (const auto &f : report.generatedFiles) {
        printf("  + %s\n", f.c_str());
    }

    if (!report.warnings.empty()) {
        printf("\nWarnings (%d):\n", (int)report.warnings.size());
        for (const auto &w : report.warnings) {
            if (w.address) {
                printf("  [0x%04X] %s\n", w.address, w.message.c_str());
            } else {
                printf("  %s\n", w.message.c_str());
            }
        }
    }

    if (report.hasErrors()) {
        printf("\nErrors (%d):\n", (int)report.errors.size());
        for (const auto &e : report.errors) {
            printf("  %s\n", e.message.c_str());
        }
        return 1;
    }

    if (!report.coverage.complete()) {
        printf("\nCOVERAGE INCOMPLETE — %d uncovered run(s)."
               " Re-run with --strict to fail on this.\n",
               (int)report.coverage.uncovered.size());
        if (strict) return 2;
    }

    printf("\nExport complete.\n");
    return 0;
}
