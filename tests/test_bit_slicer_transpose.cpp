#include <iostream>
#include <cstdint>
#include <cassert>
#include "apex/compute/bit_slicer.hpp"
#include "apex/compute/column_buffer.hpp"

int main() {
    std::cout << "=== Bit-Slicer Transpose Audit ===\n\n";

    apex::compute::BitSlicer slicer;
    apex::compute::ColumnBuffer input, output;

    // Test 1: Single bit in specific rows
    std::cout << "Test 1: Verify row 63, bit 0 (MSB of uint64)\n";
    for (int i = 0; i < 64; i++) {
        input.data[i] = 0;
    }
    input.data[63] = (1ULL << 63);  // Set MSB of row 63
    slicer.slice(input, output);

    uint64_t bit63_in_plane0 = output.data[63];  // bit_planes[63] = MSB plane
    bool found = (bit63_in_plane0 >> 63) & 1;     // Check bit 63 of plane 63 (row 63)
    std::cout << "  Row 63 has bit 63 set. After transpose:\n";
    std::cout << "  bit_planes[63] bit 63 = " << found << " (expected 1)\n";
    assert(found && "FAIL: Row 63 bit 63 not in correct plane position");
    std::cout << "  ✓ PASS\n\n";

    // Test 2: Verify LSB transpose
    std::cout << "Test 2: Verify row 63, bit 0 (LSB)\n";
    for (int i = 0; i < 64; i++) {
        input.data[i] = 0;
    }
    input.data[63] = 1;  // Set LSB of row 63
    slicer.slice(input, output);

    uint64_t bit0_in_plane0 = output.data[0];  // bit_planes[0] = LSB plane
    found = (bit0_in_plane0 >> 63) & 1;        // Check bit 63 of plane 0 (row 63)
    std::cout << "  Row 63 has bit 0 set. After transpose:\n";
    std::cout << "  bit_planes[0] bit 63 = " << found << " (expected 1)\n";
    assert(found && "FAIL: Row 63 bit 0 not in correct plane position");
    std::cout << "  ✓ PASS\n\n";

    // Test 3: Comprehensive row/bit mapping (every 8th position)
    std::cout << "Test 3: Spot-check multiple row/bit combinations (stride-8)\n";
    int errors = 0;
    for (int row = 0; row < 64; row += 8) {
        for (int bit = 0; bit < 64; bit += 8) {
            for (int i = 0; i < 64; i++) {
                input.data[i] = 0;
            }
            input.data[row] = (1ULL << bit);
            slicer.slice(input, output);

            uint64_t expected_bit = (output.data[bit] >> row) & 1;
            if (expected_bit != 1) {
                std::cout << "  ERROR: row " << row << " bit " << bit
                          << " -> plane[" << bit << "] bit " << row
                          << " = " << expected_bit << " (expected 1)\n";
                errors++;
            }
        }
    }
    if (errors == 0) {
        std::cout << "  ✓ All stride-8 spot checks PASS\n";
    } else {
        std::cout << "  ✗ " << errors << " mismatches in stride-8 checks\n";
    }

    // Test 4: CRITICAL - Full coverage of rows 53-63 (the failing range)
    std::cout << "\nTest 4: Full coverage audit of rows 53-63 (all bits)\n";
    int errors_high = 0;
    for (int row = 53; row < 64; row++) {
        for (int bit = 0; bit < 64; bit++) {
            for (int i = 0; i < 64; i++) {
                input.data[i] = 0;
            }
            input.data[row] = (1ULL << bit);
            slicer.slice(input, output);

            uint64_t expected_bit = (output.data[bit] >> row) & 1;
            if (expected_bit != 1) {
                std::cout << "  ERROR: row " << row << " bit " << bit
                          << " -> plane[" << bit << "] bit " << row
                          << " = " << expected_bit << " (expected 1)\n";
                errors_high++;
            }
        }
    }
    if (errors_high == 0) {
        std::cout << "  ✓ All rows 53-63 PASS for all 64 bits\n\n";
    } else {
        std::cout << "  ✗ " << errors_high << " mismatches in rows 53-63\n\n";
    }

    if (errors > 0 || errors_high > 0) {
        std::cout << "=== Transpose Audit FAILED ===\n";
        return 1;
    }

    std::cout << "=== Transpose Audit Complete ===\n";
    return errors == 0 ? 0 : 1;
}
