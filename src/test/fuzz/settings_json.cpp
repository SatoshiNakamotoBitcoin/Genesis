// Copyright (c) 2025 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/settings_json.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <univalue.h>

#include <cassert>
#include <vector>
#include <string>

using namespace common;

namespace {

// Fuzz the settings JSON serialization and deserialization
void FuzzSettingsJsonSerialization(FuzzedDataProvider& fuzzed_data_provider)
{
    Settings settings;
    
    // Generate random settings to fuzz serialization
    const size_t num_settings = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 50);
    
    for (size_t i = 0; i < num_settings; ++i) {
        std::string key = fuzzed_data_provider.ConsumeRandomLengthString(100);
        if (key.empty()) continue;
        
        // Generate random SettingsValue
        uint8_t value_type = fuzzed_data_provider.ConsumeIntegralInRange<uint8_t>(0, 4);
        
        switch (value_type) {
            case 0: // Boolean
                settings.rw_settings[key] = SettingsValue(fuzzed_data_provider.ConsumeBool());
                break;
            case 1: // Integer
                settings.rw_settings[key] = SettingsValue(fuzzed_data_provider.ConsumeIntegral<int64_t>());
                break;
            case 2: // Double
                settings.rw_settings[key] = SettingsValue(fuzzed_data_provider.ConsumeFloatingPoint<double>());
                break;
            case 3: // String
                settings.rw_settings[key] = SettingsValue(fuzzed_data_provider.ConsumeRandomLengthString(200));
                break;
            case 4: // Array (less common, test simple array)
                {
                    UniValue arr(UniValue::VARR);
                    size_t arr_size = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 10);
                    for (size_t j = 0; j < arr_size; ++j) {
                        arr.push_back(fuzzed_data_provider.ConsumeRandomLengthString(50));
                    }
                    settings.rw_settings[key] = SettingsValue(arr);
                }
                break;
        }
    }
    
    // Try to serialize - should not crash
    try {
        UniValue json = SettingsToJson(settings);
        
        // If serialization succeeded, try to deserialize
        Settings restored_settings;
        std::vector<std::string> errors;
        
        // This might fail with validation errors, but should not crash
        JsonToSettings(json, restored_settings, errors);
        
    } catch (const std::exception&) {
        // Exceptions are acceptable, crashes are not
    }
}

// Fuzz JSON parsing with malformed input
void FuzzJsonParsing(FuzzedDataProvider& fuzzed_data_provider)
{
    // Generate potentially malformed JSON
    std::string json_input = fuzzed_data_provider.ConsumeRemainingBytesAsString();
    
    // Try to parse as UniValue
    UniValue json;
    if (!json.read(json_input)) {
        // Failed to parse as valid JSON - this is expected for malformed input
        return;
    }
    
    // If it parsed as valid JSON, try to process it as settings
    Settings settings;
    std::vector<std::string> errors;
    
    try {
        // This should handle malformed settings gracefully
        JsonToSettings(json, settings, errors);
    } catch (const std::exception&) {
        // Exceptions are acceptable for malformed input
    }
}

// Fuzz settings validation
void FuzzSettingsValidation(FuzzedDataProvider& fuzzed_data_provider)
{
    // Generate random setting name and value for validation testing
    std::string setting_name = fuzzed_data_provider.ConsumeRandomLengthString(100);
    
    if (setting_name.empty()) return;
    
    // Generate various types of values to test validation
    uint8_t value_type = fuzzed_data_provider.ConsumeIntegralInRange<uint8_t>(0, 6);
    SettingsValue value;
    
    switch (value_type) {
        case 0: // Boolean
            value = SettingsValue(fuzzed_data_provider.ConsumeBool());
            break;
        case 1: // Small integer
            value = SettingsValue(fuzzed_data_provider.ConsumeIntegralInRange<int>(-1000, 1000));
            break;
        case 2: // Large integer
            value = SettingsValue(fuzzed_data_provider.ConsumeIntegral<int64_t>());
            break;
        case 3: // String
            value = SettingsValue(fuzzed_data_provider.ConsumeRandomLengthString(500));
            break;
        case 4: // Empty value
            value = SettingsValue();
            break;
        case 5: // Special float values
            {
                double d = fuzzed_data_provider.ConsumeFloatingPoint<double>();
                // Include special values that might cause issues
                if (fuzzed_data_provider.ConsumeBool()) {
                    d = std::numeric_limits<double>::infinity();
                } else if (fuzzed_data_provider.ConsumeBool()) {
                    d = std::numeric_limits<double>::quiet_NaN();
                }
                value = SettingsValue(d);
            }
            break;
        case 6: // Very long string
            value = SettingsValue(std::string(fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 10000), 'x'));
            break;
    }
    
    // Test validation - should not crash regardless of input
    std::vector<std::string> errors;
    try {
        ValidateSettingValue(setting_name, value, errors);
    } catch (const std::exception&) {
        // Validation errors are acceptable
    }
}

// Fuzz metadata operations
void FuzzMetadataOperations(FuzzedDataProvider& fuzzed_data_provider)
{
    std::string setting_name = fuzzed_data_provider.ConsumeRandomLengthString(200);
    
    if (setting_name.empty()) return;
    
    try {
        // These functions should handle invalid setting names gracefully
        UniValue metadata = GetSettingMetadata(setting_name);
        
        // If metadata was returned, it should be valid JSON
        if (!metadata.isNull()) {
            assert(metadata.isObject());
        }
    } catch (const std::exception&) {
        // Exceptions are acceptable for invalid setting names
    }
    
    // Test category operations
    try {
        UniValue categories = GetSettingCategories();
        assert(categories.isObject());
    } catch (const std::exception&) {
        // Should not throw for category retrieval
        assert(false);
    }
}

// Fuzz amount conversion functions
void FuzzAmountConversion(FuzzedDataProvider& fuzzed_data_provider)
{
    // Test string to amount conversion
    std::string amount_str = fuzzed_data_provider.ConsumeRandomLengthString(100);
    
    try {
        int64_t amount = AmountToSatoshi(amount_str);
        
        // If conversion succeeded, test reverse conversion
        std::string converted_back = SatoshiToAmountString(amount);
        
        // Round-trip conversion should be stable
        int64_t round_trip = AmountToSatoshi(converted_back);
        assert(round_trip == amount);
        
    } catch (const std::exception&) {
        // Conversion errors are acceptable for invalid input
    }
    
    // Test with extreme values
    if (fuzzed_data_provider.ConsumeBool()) {
        try {
            int64_t extreme_amount = fuzzed_data_provider.ConsumeIntegral<int64_t>();
            std::string extreme_str = SatoshiToAmountString(extreme_amount);
            
            // Should be able to convert back
            int64_t converted = AmountToSatoshi(extreme_str);
            assert(converted == extreme_amount);
            
        } catch (const std::exception&) {
            // Some extreme values might not be convertible
        }
    }
}

// Fuzz numeric range validation
void FuzzNumericValidation(FuzzedDataProvider& fuzzed_data_provider)
{
    std::string setting_name = fuzzed_data_provider.ConsumeRandomLengthString(50);
    
    if (setting_name.empty()) return;
    
    // Generate random numeric range parameters
    int64_t value = fuzzed_data_provider.ConsumeIntegral<int64_t>();
    int64_t min_val = fuzzed_data_provider.ConsumeIntegral<int64_t>();
    int64_t max_val = fuzzed_data_provider.ConsumeIntegral<int64_t>();
    
    // Ensure min <= max for valid test cases sometimes
    if (fuzzed_data_provider.ConsumeBool() && min_val > max_val) {
        std::swap(min_val, max_val);
    }
    
    std::vector<std::string> errors;
    
    try {
        bool result = ValidateNumericRange(value, min_val, max_val, setting_name, errors);
        
        // If min <= max and min <= value <= max, should return true
        if (min_val <= max_val && value >= min_val && value <= max_val) {
            assert(result == true);
            assert(errors.empty());
        }
        
    } catch (const std::exception&) {
        // Should not throw exceptions, but handle gracefully
        assert(false);
    }
}

// Fuzz string options validation
void FuzzStringValidation(FuzzedDataProvider& fuzzed_data_provider)
{
    std::string setting_name = fuzzed_data_provider.ConsumeRandomLengthString(50);
    std::string test_value = fuzzed_data_provider.ConsumeRandomLengthString(100);
    
    if (setting_name.empty()) return;
    
    // Generate random allowed values
    std::vector<std::string> allowed_values;
    size_t num_allowed = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 20);
    
    for (size_t i = 0; i < num_allowed; ++i) {
        std::string allowed = fuzzed_data_provider.ConsumeRandomLengthString(50);
        allowed_values.push_back(allowed);
    }
    
    std::vector<std::string> errors;
    
    try {
        bool result = ValidateStringOptions(test_value, allowed_values, setting_name, errors);
        
        // If test_value is in allowed_values, should return true
        bool found = std::find(allowed_values.begin(), allowed_values.end(), test_value) != allowed_values.end();
        if (found) {
            assert(result == true);
            assert(errors.empty());
        }
        
    } catch (const std::exception&) {
        // Should not throw exceptions
        assert(false);
    }
}

} // anonymous namespace

FUZZ_TARGET(settings_json_serialization)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    FuzzSettingsJsonSerialization(fuzzed_data_provider);
}

FUZZ_TARGET(settings_json_parsing)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    FuzzJsonParsing(fuzzed_data_provider);
}

FUZZ_TARGET(settings_validation)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    FuzzSettingsValidation(fuzzed_data_provider);
}

FUZZ_TARGET(settings_metadata)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    FuzzMetadataOperations(fuzzed_data_provider);
}

FUZZ_TARGET(settings_amount_conversion)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    FuzzAmountConversion(fuzzed_data_provider);
}

FUZZ_TARGET(settings_numeric_validation)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    FuzzNumericValidation(fuzzed_data_provider);
}

FUZZ_TARGET(settings_string_validation)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    FuzzStringValidation(fuzzed_data_provider);
}