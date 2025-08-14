// Copyright (c) 2025 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COMMON_SETTINGS_JSON_H
#define BITCOIN_COMMON_SETTINGS_JSON_H

#include <common/settings.h>
#include <univalue.h>

#include <string>
#include <vector>

namespace common {

/**
 * Serialize Settings to JSON format for RPC export
 * 
 * @param settings The Settings object to serialize
 * @return UniValue JSON object containing all settings
 */
UniValue SettingsToJson(const Settings& settings);

/**
 * Deserialize JSON to Settings object
 * 
 * @param json The JSON object to deserialize
 * @param settings The Settings object to populate
 * @param errors Vector to collect any validation errors
 * @return true if successful, false if errors occurred
 */
bool JsonToSettings(const UniValue& json, Settings& settings, std::vector<std::string>& errors);

/**
 * Validate a single setting value according to Bitcoin Knots constraints
 * 
 * @param setting_name The name of the setting
 * @param value The value to validate
 * @param errors Vector to collect validation error messages
 * @return true if valid, false if invalid
 */
bool ValidateSettingValue(const std::string& setting_name, const SettingsValue& value, std::vector<std::string>& errors);

/**
 * Get setting metadata including type, default value, and constraints
 * 
 * @param setting_name The name of the setting
 * @return UniValue object with metadata (type, default, min, max, description, etc.)
 */
UniValue GetSettingMetadata(const std::string& setting_name);

/**
 * Get all available setting names organized by category
 * 
 * @return UniValue object with categories as keys and arrays of setting names as values
 */
UniValue GetSettingCategories();

/**
 * Convert amount values to satoshi representation for JSON
 * 
 * @param amount_str String representation of amount (possibly with unit suffix)
 * @return int64_t amount in satoshi
 */
int64_t AmountToSatoshi(const std::string& amount_str);

/**
 * Convert satoshi amount to string with appropriate unit
 * 
 * @param satoshi Amount in satoshi
 * @return string representation with unit
 */
std::string SatoshiToAmountString(int64_t satoshi);

/**
 * Validate numeric setting within specified bounds
 * 
 * @param value The numeric value to validate
 * @param min_val Minimum allowed value
 * @param max_val Maximum allowed value
 * @param setting_name Name of setting for error messages
 * @param errors Vector to collect validation errors
 * @return true if valid, false if invalid
 */
bool ValidateNumericRange(int64_t value, int64_t min_val, int64_t max_val, 
                         const std::string& setting_name, std::vector<std::string>& errors);

/**
 * Validate string setting against allowed values
 * 
 * @param value The string value to validate
 * @param allowed_values Vector of allowed string values
 * @param setting_name Name of setting for error messages
 * @param errors Vector to collect validation errors
 * @return true if valid, false if invalid
 */
bool ValidateStringOptions(const std::string& value, const std::vector<std::string>& allowed_values,
                          const std::string& setting_name, std::vector<std::string>& errors);

/**
 * Setting categories for organizing exports
 */
enum class SettingCategory {
    WALLET,
    MEMPOOL, 
    RELAY,
    SCRIPT,
    TRANSACTION,
    DATA_CARRIER,
    DUST,
    BLOCK_CREATION,
    NETWORK,
    GUI,
    NODE
};

/**
 * Setting type information for validation
 */
struct SettingTypeInfo {
    enum Type { BOOL, INT, DOUBLE, STRING, AMOUNT } type;
    int64_t min_value = 0;
    int64_t max_value = 0;
    std::vector<std::string> allowed_values;
    std::string description;
    bool restart_required = false;
    SettingCategory category;
};

/**
 * Get type information for a setting
 * 
 * @param setting_name The name of the setting
 * @return SettingTypeInfo structure with type and validation info
 */
SettingTypeInfo GetSettingTypeInfo(const std::string& setting_name);

} // namespace common

#endif // BITCOIN_COMMON_SETTINGS_JSON_H