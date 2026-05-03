#include <iostream>
#include <vector>
#include <iomanip>
#include <cstring>
#include <cstdint>

#include "apex/apex.h"
#include "apex/engine.hpp"
#include "apex/core/registry.hpp"
#include "apex/core/types.hpp"

// IoT Sensor struct: 24 bytes (natural alignment)
struct Sensor {
    uint32_t id;           // offset 0, 32 bits
    uint32_t padding1;     // offset 4, 32 bits (for alignment)
    uint64_t temperature;  // offset 8, 64 bits (in 0.1°C units, so 350 = 35°C)
    uint64_t humidity;     // offset 16, 64 bits (in 0.1% units)
};

static_assert(sizeof(Sensor) == 24, "Sensor struct must be exactly 24 bytes");

int main() {
    std::cout << "=== APEX SDK Test: IoT Sensor Processing ===\n\n";

    // Initialize the ApexEngine
    apex::ApexEngine engine;
    std::cout << "✓ ApexEngine initialized\n\n";

    // Define schema fields for the Sensor struct
    std::vector<apex::core::FieldDescriptor> sensor_fields{
        {"id", 0, 32, apex::core::DataType::UINT32},
        {"temperature", 8, 64, apex::core::DataType::UINT64},
        {"humidity", 16, 64, apex::core::DataType::UINT64},
    };

    // Register the sensor schema with 24-byte stride
    engine.register_schema("SENSOR", sensor_fields, sizeof(Sensor));
    std::cout << "✓ Registered SENSOR schema\n";
    std::cout << "  Struct size: " << sizeof(Sensor) << " bytes\n";
    std::cout << "  Field layout:\n";
    std::cout << "    - id: offset 0 (32-bit)\n";
    std::cout << "    - temperature: offset 8 (64-bit)\n";
    std::cout << "    - humidity: offset 16 (64-bit)\n\n";

    // Set logic: find sensors with temperature > 35°C (3500 in 0.1°C units)
    uint64_t temp_threshold = 3500;  // 35.0°C
    engine.set_logic("SENSOR", "temperature", temp_threshold);
    std::cout << "✓ Set logic: temperature > " << (temp_threshold / 100.0) << "°C\n\n";

    // Create test data: 192 sensors (3 chunks of 64)
    constexpr size_t kNumSensors = 192;
    std::vector<Sensor> sensor_data(kNumSensors);

    std::cout << "=== Generating Test Data ===\n";
    std::cout << "Creating " << kNumSensors << " sensor records...\n";

    // Fill with deterministic pattern
    for (size_t i = 0; i < kNumSensors; i++) {
        sensor_data[i].id = static_cast<uint32_t>(i);
        // Temperature: range from 20°C to 40°C (2000 to 4000 in 0.1°C units)
        uint64_t base_temp = 2000 + (i * 100) % 2000;  // Cycles 20-40°C
        sensor_data[i].temperature = base_temp;
        // Humidity: fixed at 55% for simplicity (5500 in 0.1% units)
        sensor_data[i].humidity = 5500;
    }

    std::cout << "✓ Generated " << kNumSensors << " sensors\n";
    std::cout << "  Sample sensors:\n";
    for (size_t i = 0; i < 5; i++) {
        std::cout << "    [" << i << "] temp=" << (sensor_data[i].temperature / 100.0)
                  << "°C, hum=" << (sensor_data[i].humidity / 100.0) << "%\n";
    }
    std::cout << "\n";

    // Execute the SDK
    std::cout << "=== Executing SDK ===\n";
    uint64_t matches = engine.execute(sensor_data.data(), kNumSensors);

    std::cout << "✓ Executed SDK\n";
    std::cout << "  Total matches (temp > 35°C): " << matches << "\n\n";

    // Verify results against expected
    std::cout << "=== Verification ===\n";
    uint64_t expected_matches = 0;
    for (size_t i = 0; i < kNumSensors; i++) {
        if (sensor_data[i].temperature > temp_threshold) {
            expected_matches++;
        }
    }

    std::cout << "Expected matches: " << expected_matches << "\n";
    std::cout << "Actual matches:   " << matches << "\n";

    if (matches == expected_matches) {
        std::cout << "\n✅ TEST PASSED: Results match!\n";
        return 0;
    } else {
        std::cout << "\n❌ TEST FAILED: Mismatch detected\n";
        std::cout << "   Expected: " << expected_matches << ", Got: " << matches << "\n";

        // Detailed breakdown
        std::cout << "\nDetailed breakdown:\n";
        for (size_t chunk = 0; chunk < (kNumSensors + 63) / 64; chunk++) {
            size_t chunk_start = chunk * 64;
            size_t chunk_end = std::min(chunk_start + 64, kNumSensors);
            uint64_t chunk_expected = 0;
            for (size_t i = chunk_start; i < chunk_end; i++) {
                if (sensor_data[i].temperature > temp_threshold) {
                    chunk_expected++;
                }
            }
            std::cout << "  Chunk " << chunk << " (" << chunk_start << "-" << (chunk_end - 1)
                      << "): expected " << chunk_expected << " matches\n";
        }

        return 1;
    }
}
