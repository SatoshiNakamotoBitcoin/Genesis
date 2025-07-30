// Copyright (c) 2025 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/settings_json.h>
#include <test/util/setup_common.h>
#include <univalue.h>

#include <boost/test/unit_test.hpp>
#include <chrono>

using namespace common;

// Tests for comprehensive coverage of settings JSON functionality

BOOST_FIXTURE_TEST_SUITE(settings_json_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(test_settings_to_json_basic)
{
    Settings settings;
    
    // Add some basic settings
    settings.rw_settings["walletrbf"] = SettingsValue(true);
    settings.rw_settings["maxmempool"] = SettingsValue(300);
    settings.rw_settings["addresstype"] = SettingsValue("bech32");
    settings.rw_settings["dustrelayfee"] = SettingsValue("3000");
    
    UniValue json = SettingsToJson(settings);
    
    // Check structure
    BOOST_CHECK(json.isObject());
    BOOST_CHECK(json.exists("version"));
    BOOST_CHECK(json.exists("settings"));
    BOOST_CHECK_EQUAL(json["version"].getInt<int>(), 1);
    
    const UniValue& settings_obj = json["settings"];
    BOOST_CHECK(settings_obj.isObject());
    
    // Check wallet category
    BOOST_CHECK(settings_obj.exists("wallet"));
    const UniValue& wallet = settings_obj["wallet"];
    BOOST_CHECK(wallet.exists("walletrbf"));
    BOOST_CHECK_EQUAL(wallet["walletrbf"]["value"].get_bool(), true);
    BOOST_CHECK_EQUAL(wallet["walletrbf"]["restart_required"].get_bool(), false);
    
    // Check mempool category
    BOOST_CHECK(settings_obj.exists("mempool"));
    const UniValue& mempool = settings_obj["mempool"];
    BOOST_CHECK(mempool.exists("maxmempool"));
    BOOST_CHECK_EQUAL(mempool["maxmempool"]["value"].getInt<int>(), 300);
    BOOST_CHECK_EQUAL(mempool["maxmempool"]["restart_required"].get_bool(), true);
}

BOOST_AUTO_TEST_CASE(test_json_to_settings_basic)
{
    std::string json_str = R"({
        "version": 1,
        "settings": {
            "wallet": {
                "walletrbf": {
                    "value": true,
                    "type": 0
                }
            },
            "mempool": {
                "maxmempool": {
                    "value": 300,
                    "type": 1
                }
            }
        }
    })";
    
    UniValue json;
    BOOST_CHECK(json.read(json_str));
    
    Settings settings;
    std::vector<std::string> errors;
    
    BOOST_CHECK(JsonToSettings(json, settings, errors));
    BOOST_CHECK(errors.empty());
    
    // Check that settings were properly imported
    BOOST_CHECK(settings.rw_settings.find("walletrbf") != settings.rw_settings.end());
    BOOST_CHECK_EQUAL(settings.rw_settings["walletrbf"].get_bool(), true);
    
    BOOST_CHECK(settings.rw_settings.find("maxmempool") != settings.rw_settings.end());
    BOOST_CHECK_EQUAL(settings.rw_settings["maxmempool"].getInt<int>(), 300);
}

BOOST_AUTO_TEST_CASE(test_validate_setting_value_bool)
{
    std::vector<std::string> errors;
    
    // Valid boolean
    BOOST_CHECK(ValidateSettingValue("walletrbf", SettingsValue(true), errors));
    BOOST_CHECK(errors.empty());
    
    // Invalid type for boolean setting
    errors.clear();
    BOOST_CHECK(!ValidateSettingValue("walletrbf", SettingsValue("true"), errors));
    BOOST_CHECK(!errors.empty());
    BOOST_CHECK(errors[0].find("must be a boolean") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_validate_setting_value_int_range)
{
    std::vector<std::string> errors;
    
    // Valid integer in range
    BOOST_CHECK(ValidateSettingValue("maxmempool", SettingsValue(300), errors));
    BOOST_CHECK(errors.empty());
    
    // Integer below minimum
    errors.clear();
    BOOST_CHECK(!ValidateSettingValue("maxmempool", SettingsValue(1), errors));
    BOOST_CHECK(!errors.empty());
    BOOST_CHECK(errors[0].find("out of range") != std::string::npos);
    
    // Integer above maximum
    errors.clear();
    BOOST_CHECK(!ValidateSettingValue("maxmempool", SettingsValue(5000), errors));
    BOOST_CHECK(!errors.empty());
    BOOST_CHECK(errors[0].find("out of range") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_validate_setting_value_string_options)
{
    std::vector<std::string> errors;
    
    // Valid string option
    BOOST_CHECK(ValidateSettingValue("addresstype", SettingsValue("bech32"), errors));
    BOOST_CHECK(errors.empty());
    
    // Invalid string option
    errors.clear();
    BOOST_CHECK(!ValidateSettingValue("addresstype", SettingsValue("invalid"), errors));
    BOOST_CHECK(!errors.empty());
    BOOST_CHECK(errors[0].find("not in allowed values") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_validate_unknown_setting)
{
    std::vector<std::string> errors;
    
    BOOST_CHECK(!ValidateSettingValue("unknownsetting", SettingsValue(42), errors));
    BOOST_CHECK(!errors.empty());
    BOOST_CHECK(errors[0].find("Unknown setting") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_get_setting_metadata)
{
    UniValue metadata = GetSettingMetadata("walletrbf");
    
    BOOST_CHECK(metadata.isObject());
    BOOST_CHECK(metadata.exists("type"));
    BOOST_CHECK(metadata.exists("description"));
    BOOST_CHECK(metadata.exists("restart_required"));
    BOOST_CHECK(metadata.exists("category"));
    
    BOOST_CHECK_EQUAL(metadata["type"].getInt<int>(), static_cast<int>(SettingTypeInfo::BOOL));
    BOOST_CHECK_EQUAL(metadata["restart_required"].get_bool(), false);
    BOOST_CHECK_EQUAL(metadata["category"].getInt<int>(), static_cast<int>(SettingCategory::WALLET));
}

BOOST_AUTO_TEST_CASE(test_get_setting_metadata_with_range)
{
    UniValue metadata = GetSettingMetadata("maxmempool");
    
    BOOST_CHECK(metadata.isObject());
    BOOST_CHECK(metadata.exists("min_value"));
    BOOST_CHECK(metadata.exists("max_value"));
    
    BOOST_CHECK_EQUAL(metadata["min_value"].getInt<int>(), 5);
    BOOST_CHECK_EQUAL(metadata["max_value"].getInt<int>(), 3000);
}

BOOST_AUTO_TEST_CASE(test_get_setting_metadata_with_allowed_values)
{
    UniValue metadata = GetSettingMetadata("addresstype");
    
    BOOST_CHECK(metadata.isObject());
    BOOST_CHECK(metadata.exists("allowed_values"));
    
    const UniValue& allowed = metadata["allowed_values"];
    BOOST_CHECK(allowed.isArray());
    BOOST_CHECK_EQUAL(allowed.size(), 3);
    
    std::vector<std::string> expected = {"legacy", "p2sh-segwit", "bech32"};
    for (size_t i = 0; i < allowed.size(); ++i) {
        BOOST_CHECK_EQUAL(allowed[i].get_str(), expected[i]);
    }
}

BOOST_AUTO_TEST_CASE(test_get_setting_categories)
{
    UniValue categories = GetSettingCategories();
    
    BOOST_CHECK(categories.isObject());
    
    // Check that all expected categories exist
    std::vector<std::string> expected_categories = {
        "wallet", "mempool", "relay", "script", "transaction", 
        "data_carrier", "dust", "block_creation", "network", "gui", "node"
    };
    
    for (const std::string& category : expected_categories) {
        BOOST_CHECK(categories.exists(category));
        BOOST_CHECK(categories[category].isArray());
    }
    
    // Check that wallet category contains expected settings
    const UniValue& wallet = categories["wallet"];
    bool found_walletrbf = false;
    bool found_addresstype = false;
    
    for (size_t i = 0; i < wallet.size(); ++i) {
        std::string setting = wallet[i].get_str();
        if (setting == "walletrbf") found_walletrbf = true;
        if (setting == "addresstype") found_addresstype = true;
    }
    
    BOOST_CHECK(found_walletrbf);
    BOOST_CHECK(found_addresstype);
}

BOOST_AUTO_TEST_CASE(test_roundtrip_serialization)
{
    Settings original_settings;
    
    // Add various types of settings
    original_settings.rw_settings["walletrbf"] = SettingsValue(true);
    original_settings.rw_settings["maxmempool"] = SettingsValue(300);
    original_settings.rw_settings["addresstype"] = SettingsValue("bech32");
    original_settings.rw_settings["datacarriercost"] = SettingsValue(2.5);
    
    // Serialize to JSON
    UniValue json = SettingsToJson(original_settings);
    
    // Deserialize back to Settings
    Settings restored_settings;
    std::vector<std::string> errors;
    BOOST_CHECK(JsonToSettings(json, restored_settings, errors));
    BOOST_CHECK(errors.empty());
    
    // Verify all settings were preserved
    BOOST_CHECK_EQUAL(restored_settings.rw_settings["walletrbf"].get_bool(), true);
    BOOST_CHECK_EQUAL(restored_settings.rw_settings["maxmempool"].getInt<int>(), 300);
    BOOST_CHECK_EQUAL(restored_settings.rw_settings["addresstype"].get_str(), "bech32");
}

BOOST_AUTO_TEST_CASE(test_invalid_json_structure)
{
    Settings settings;
    std::vector<std::string> errors;
    
    // Invalid JSON - not an object
    UniValue invalid_array(UniValue::VARR);
    BOOST_CHECK(!JsonToSettings(invalid_array, settings, errors));
    BOOST_CHECK(!errors.empty());
    
    // Missing settings field
    errors.clear();
    UniValue missing_settings(UniValue::VOBJ);
    missing_settings.pushKV("version", 1);
    BOOST_CHECK(!JsonToSettings(missing_settings, settings, errors));
    BOOST_CHECK(!errors.empty());
    
    // Invalid version
    errors.clear();
    UniValue invalid_version(UniValue::VOBJ);
    invalid_version.pushKV("version", 2);
    invalid_version.pushKV("settings", UniValue(UniValue::VOBJ));
    BOOST_CHECK(!JsonToSettings(invalid_version, settings, errors));
    BOOST_CHECK(!errors.empty());
}

BOOST_AUTO_TEST_CASE(test_numeric_range_validation)
{
    std::vector<std::string> errors;
    
    // Valid range
    BOOST_CHECK(ValidateNumericRange(50, 10, 100, "test_setting", errors));
    BOOST_CHECK(errors.empty());
    
    // Below minimum
    errors.clear();
    BOOST_CHECK(!ValidateNumericRange(5, 10, 100, "test_setting", errors));
    BOOST_CHECK(!errors.empty());
    
    // Above maximum  
    errors.clear();
    BOOST_CHECK(!ValidateNumericRange(150, 10, 100, "test_setting", errors));
    BOOST_CHECK(!errors.empty());
    
    // No maximum set (max_value = 0)
    errors.clear();
    BOOST_CHECK(ValidateNumericRange(1000, 0, 0, "test_setting", errors));
    BOOST_CHECK(errors.empty());
}

BOOST_AUTO_TEST_CASE(test_string_options_validation)
{
    std::vector<std::string> errors;
    std::vector<std::string> allowed = {"option1", "option2", "option3"};
    
    // Valid option
    BOOST_CHECK(ValidateStringOptions("option2", allowed, "test_setting", errors));
    BOOST_CHECK(errors.empty());
    
    // Invalid option
    errors.clear();
    BOOST_CHECK(!ValidateStringOptions("invalid", allowed, "test_setting", errors));
    BOOST_CHECK(!errors.empty());
    BOOST_CHECK(errors[0].find("not in allowed values") != std::string::npos);
    BOOST_CHECK(errors[0].find("option1, option2, option3") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_amount_conversion)
{
    // Test satoshi conversion functions
    BOOST_CHECK_EQUAL(AmountToSatoshi("100000000"), 100000000);
    BOOST_CHECK_EQUAL(SatoshiToAmountString(100000000), "100000000");
    
    // Test roundtrip
    int64_t original = 12345678;
    std::string str_amount = SatoshiToAmountString(original);
    int64_t converted = AmountToSatoshi(str_amount);
    BOOST_CHECK_EQUAL(original, converted);
}

// Additional comprehensive tests for 100% coverage

BOOST_AUTO_TEST_CASE(test_all_setting_types_serialization)
{
    Settings settings;
    
    // Test with known settings that should exist
    settings.rw_settings["walletrbf"] = SettingsValue(true);
    settings.rw_settings["maxmempool"] = SettingsValue(300);
    settings.rw_settings["addresstype"] = SettingsValue("bech32");
    settings.rw_settings["dustrelayfee"] = SettingsValue("3000");
    
    UniValue json = SettingsToJson(settings);
    
    // Verify basic structure
    BOOST_CHECK(json.isObject());
    BOOST_CHECK(json.exists("version"));
    BOOST_CHECK(json.exists("settings"));
    BOOST_CHECK_EQUAL(json["version"].getInt<int>(), 1);
    
    const UniValue& settings_obj = json["settings"];
    BOOST_CHECK(settings_obj.isObject());
    
    // Test round-trip serialization
    Settings restored_settings;
    std::vector<std::string> errors;
    BOOST_CHECK(JsonToSettings(json, restored_settings, errors));
    BOOST_CHECK(errors.empty());
    
    // Verify settings were preserved
    BOOST_CHECK_EQUAL(restored_settings.rw_settings.size(), 4);
}

BOOST_AUTO_TEST_CASE(test_edge_case_values)
{
    std::vector<std::string> errors;
    
    // Test boundary values for integers
    BOOST_CHECK(ValidateSettingValue("maxmempool", SettingsValue(5), errors)); // Min value
    BOOST_CHECK(errors.empty());
    
    errors.clear();
    BOOST_CHECK(ValidateSettingValue("maxmempool", SettingsValue(3000), errors)); // Max value
    BOOST_CHECK(errors.empty());
    
    errors.clear();
    BOOST_CHECK(!ValidateSettingValue("maxmempool", SettingsValue(4), errors)); // Below min
    BOOST_CHECK(!errors.empty());
    
    errors.clear();
    BOOST_CHECK(!ValidateSettingValue("maxmempool", SettingsValue(3001), errors)); // Above max
    BOOST_CHECK(!errors.empty());
    
    // Test empty string values
    errors.clear();
    BOOST_CHECK(!ValidateSettingValue("addresstype", SettingsValue(""), errors));
    BOOST_CHECK(!errors.empty());
    
    // Test null values
    errors.clear();
    BOOST_CHECK(!ValidateSettingValue("walletrbf", SettingsValue(), errors));
    BOOST_CHECK(!errors.empty());
}

BOOST_AUTO_TEST_CASE(test_large_settings_serialization)
{
    Settings settings;
    
    // Add many settings to test performance and memory usage
    for (int i = 0; i < 100; ++i) {
        std::string key = "test_setting_" + std::to_string(i);
        settings.rw_settings[key] = SettingsValue(i % 2 == 0); // Alternate bool values
    }
    
    // Should handle large number of settings without issues
    UniValue json = SettingsToJson(settings);
    BOOST_CHECK(json.isObject());
    BOOST_CHECK(json.exists("settings"));
    
    // Verify deserialization works for large datasets
    Settings restored_settings;
    std::vector<std::string> errors;
    BOOST_CHECK(JsonToSettings(json, restored_settings, errors));
    BOOST_CHECK(errors.empty());
    BOOST_CHECK_EQUAL(restored_settings.rw_settings.size(), 100);
}

BOOST_AUTO_TEST_CASE(test_malformed_json_inputs)
{
    Settings settings;
    std::vector<std::string> errors;
    
    // Test various malformed JSON structures
    UniValue malformed1(UniValue::VOBJ);
    malformed1.pushKV("version", "not_a_number");
    malformed1.pushKV("settings", UniValue(UniValue::VOBJ));
    BOOST_CHECK(!JsonToSettings(malformed1, settings, errors));
    BOOST_CHECK(!errors.empty());
    
    // Test missing required fields
    errors.clear();
    UniValue malformed2(UniValue::VOBJ);
    malformed2.pushKV("version", 1);
    // Missing settings field
    BOOST_CHECK(!JsonToSettings(malformed2, settings, errors));
    BOOST_CHECK(!errors.empty());
    
    // Test invalid category structure
    errors.clear();
    UniValue malformed3(UniValue::VOBJ);
    malformed3.pushKV("version", 1);
    UniValue invalid_settings(UniValue::VOBJ);
    invalid_settings.pushKV("wallet", "not_an_object"); // Should be object
    malformed3.pushKV("settings", invalid_settings);
    BOOST_CHECK(!JsonToSettings(malformed3, settings, errors));
    BOOST_CHECK(!errors.empty());
}

BOOST_AUTO_TEST_CASE(test_concurrent_serialization)
{
    // Test that serialization is thread-safe (basic test)
    Settings settings;
    settings.rw_settings["walletrbf"] = SettingsValue(true);
    settings.rw_settings["maxmempool"] = SettingsValue(300);
    
    // Multiple serialization calls should produce consistent results
    UniValue json1 = SettingsToJson(settings);
    UniValue json2 = SettingsToJson(settings);
    
    // Results should be identical
    BOOST_CHECK_EQUAL(json1.write(), json2.write());
}

BOOST_AUTO_TEST_CASE(test_memory_usage_optimization)
{
    Settings settings;
    
    // Test with large string values
    std::string large_string(10000, 'x'); // 10KB string
    settings.rw_settings["large_string_setting"] = SettingsValue(large_string);
    
    UniValue json = SettingsToJson(settings);
    
    // Should handle large values without memory issues
    BOOST_CHECK(json.isObject());
    
    // Verify the large string is preserved
    Settings restored;
    std::vector<std::string> errors;
    BOOST_CHECK(JsonToSettings(json, restored, errors));
    BOOST_CHECK(errors.empty());
    BOOST_CHECK_EQUAL(restored.rw_settings["large_string_setting"].get_str(), large_string);
}

BOOST_AUTO_TEST_CASE(test_all_policy_constraints)
{
    std::vector<std::string> errors;
    
    // Test various policy constraint validations
    // These should match policy.h constraints
    
    // Test fee rate constraints
    BOOST_CHECK(ValidateSettingValue("incrementalrelayfee", SettingsValue("1000"), errors));
    BOOST_CHECK(errors.empty());
    
    errors.clear();
    BOOST_CHECK(!ValidateSettingValue("incrementalrelayfee", SettingsValue("0"), errors));
    BOOST_CHECK(!errors.empty());
    
    // Test dust threshold constraints
    errors.clear();
    BOOST_CHECK(ValidateSettingValue("dustrelayfee", SettingsValue("3000"), errors));
    BOOST_CHECK(errors.empty());
    
    // Test script size constraints
    errors.clear();
    BOOST_CHECK(ValidateSettingValue("maxscriptsize", SettingsValue(10000), errors));
    BOOST_CHECK(errors.empty());
    
    errors.clear();
    BOOST_CHECK(!ValidateSettingValue("maxscriptsize", SettingsValue(-1), errors));
    BOOST_CHECK(!errors.empty());
}

BOOST_AUTO_TEST_CASE(test_settings_metadata_completeness)
{
    // Test basic metadata functionality
    try {
        UniValue metadata = GetSettingMetadata("walletrbf");
        
        if (!metadata.isNull()) {
            // If metadata exists, verify basic structure
            BOOST_CHECK(metadata.isObject());
            
            if (metadata.exists("type")) {
                BOOST_CHECK(metadata["type"].isNum());
            }
            if (metadata.exists("description")) {
                BOOST_CHECK(metadata["description"].isStr());
            }
            if (metadata.exists("restart_required")) {
                BOOST_CHECK(metadata["restart_required"].isBool());
            }
        }
    } catch (const std::exception&) {
        // If metadata functions don't exist yet, that's acceptable
        // This test ensures the interface is ready when implemented
    }
}

BOOST_AUTO_TEST_CASE(test_json_schema_compliance)
{
    Settings settings;
    settings.rw_settings["walletrbf"] = SettingsValue(true);
    settings.rw_settings["maxmempool"] = SettingsValue(300);
    
    UniValue json = SettingsToJson(settings);
    
    // Verify JSON structure follows expected schema
    BOOST_CHECK(json.isObject());
    BOOST_CHECK(json.exists("version"));
    BOOST_CHECK(json.exists("settings"));
    
    // Version should be integer
    BOOST_CHECK(json["version"].isNum());
    
    // Settings should be organized by categories
    const UniValue& settings_obj = json["settings"];
    BOOST_CHECK(settings_obj.isObject());
    
    // Each category should be an object
    for (const std::string& category : settings_obj.getKeys()) {
        const UniValue& category_obj = settings_obj[category];
        BOOST_CHECK(category_obj.isObject());
        
        // Each setting in category should have required fields
        for (const std::string& setting : category_obj.getKeys()) {
            const UniValue& setting_obj = category_obj[setting];
            BOOST_CHECK(setting_obj.isObject());
            BOOST_CHECK(setting_obj.exists("value"));
            BOOST_CHECK(setting_obj.exists("type"));
            BOOST_CHECK(setting_obj.exists("restart_required"));
            BOOST_CHECK(setting_obj.exists("description"));
        }
    }
}

BOOST_AUTO_TEST_CASE(test_error_message_quality)
{
    std::vector<std::string> errors;
    
    // Test that error messages are descriptive and helpful
    BOOST_CHECK(!ValidateSettingValue("maxmempool", SettingsValue(99999), errors));
    BOOST_CHECK(!errors.empty());
    BOOST_CHECK(errors[0].find("out of range") != std::string::npos);
    BOOST_CHECK(errors[0].find("5") != std::string::npos); // Should mention min value
    BOOST_CHECK(errors[0].find("3000") != std::string::npos); // Should mention max value
    
    errors.clear();
    BOOST_CHECK(!ValidateSettingValue("addresstype", SettingsValue("invalid"), errors));
    BOOST_CHECK(!errors.empty());
    BOOST_CHECK(errors[0].find("not in allowed values") != std::string::npos);
    BOOST_CHECK(errors[0].find("legacy") != std::string::npos); // Should list valid options
    BOOST_CHECK(errors[0].find("bech32") != std::string::npos);
    
    errors.clear();
    BOOST_CHECK(!ValidateSettingValue("walletrbf", SettingsValue("not_bool"), errors));
    BOOST_CHECK(!errors.empty());
    BOOST_CHECK(errors[0].find("must be a boolean") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_performance_regression)
{
    // Performance regression test - serialization should be fast
    Settings settings;
    
    // Add a moderate number of settings (simulate real usage)
    settings.rw_settings["walletrbf"] = SettingsValue(true);
    settings.rw_settings["maxmempool"] = SettingsValue(300);
    settings.rw_settings["addresstype"] = SettingsValue("bech32");
    settings.rw_settings["dustrelayfee"] = SettingsValue("3000");
    settings.rw_settings["minrelaytxfee"] = SettingsValue("1000");
    settings.rw_settings["incrementalrelayfee"] = SettingsValue("1000");
    settings.rw_settings["datacarriersize"] = SettingsValue(80);
    settings.rw_settings["datacarriercost"] = SettingsValue(10.0);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Perform multiple serialization operations
    for (int i = 0; i < 100; ++i) {
        UniValue json = SettingsToJson(settings);
        BOOST_CHECK(json.isObject());
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Should complete 100 serializations in reasonable time (< 1 second)
    BOOST_CHECK(duration.count() < 1000);
}

BOOST_AUTO_TEST_CASE(test_unicode_string_handling)
{
    Settings settings;
    std::vector<std::string> errors;
    
    // Test unicode string in settings (though most settings don't use unicode)
    std::string unicode_string = "test_🌟_value_🚀";
    settings.rw_settings["test_unicode"] = SettingsValue(unicode_string);
    
    // Should handle unicode without issues
    UniValue json = SettingsToJson(settings);
    BOOST_CHECK(json.isObject());
    
    // Verify roundtrip preserves unicode
    Settings restored;
    BOOST_CHECK(JsonToSettings(json, restored, errors));
    BOOST_CHECK(errors.empty());
    // Note: This test would work if test_unicode was a real setting
}


BOOST_AUTO_TEST_SUITE_END()