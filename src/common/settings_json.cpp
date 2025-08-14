// Copyright (c) 2025 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/settings_json.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <tinyformat.h>
#include <util/strencodings.h>

#include <algorithm>
#include <map>
#include <unordered_map>

extern ArgsManager gArgs;

namespace common {

static SettingTypeInfo::Type DetectSettingType(const std::string& setting_name) {
    if (setting_name.find("rbf") != std::string::npos ||
        setting_name.find("listen") != std::string::npos ||
        setting_name.find("server") != std::string::npos ||
        setting_name.find("spendzeroconfchange") != std::string::npos ||
        setting_name.find("reject") != std::string::npos ||
        setting_name.find("splash") != std::string::npos ||
        setting_name.find("minimized") != std::string::npos ||
        setting_name.find("index") != std::string::npos) {
        return SettingTypeInfo::BOOL;
    }
    
    if (setting_name.find("fee") != std::string::npos ||
        setting_name.find("dust") != std::string::npos) {
        return SettingTypeInfo::AMOUNT;
    }
    
    if (setting_name.find("max") != std::string::npos ||
        setting_name.find("limit") != std::string::npos ||
        setting_name.find("size") != std::string::npos ||
        setting_name.find("count") != std::string::npos ||
        setting_name.find("port") != std::string::npos ||
        setting_name.find("threads") != std::string::npos) {
        return SettingTypeInfo::INT;
    }
    
    return SettingTypeInfo::STRING;
}

static SettingCategory MapOptionsCategory(OptionsCategory opt_cat) {
    switch (opt_cat) {
        case OptionsCategory::WALLET: return SettingCategory::WALLET;
        case OptionsCategory::BLOCK_CREATION: return SettingCategory::BLOCK_CREATION;
        case OptionsCategory::CONNECTION: return SettingCategory::NETWORK;
        case OptionsCategory::GUI: return SettingCategory::GUI;
        case OptionsCategory::NODE_RELAY: return SettingCategory::RELAY;
        default: return SettingCategory::NODE;
    }
}

UniValue GetSettingMetadata(const std::string& setting_name)
{
    UniValue metadata(UniValue::VOBJ);
    
    std::string arg_with_dash = "-" + setting_name;
    auto help_text = gArgs.GetArgHelpText(arg_with_dash);
    auto category = gArgs.GetArgCategory(arg_with_dash);
    auto flags = gArgs.GetArgFlags(arg_with_dash);
    
    if (!help_text || !category) {
        metadata.pushKV("error", "Unknown setting");
        return metadata;
    }
    
    metadata.pushKV("description", *help_text);
    
    SettingCategory setting_cat = MapOptionsCategory(*category);
    metadata.pushKV("category", static_cast<int>(setting_cat));
    
    SettingTypeInfo::Type detected_type = DetectSettingType(setting_name);
    metadata.pushKV("type", static_cast<int>(detected_type));
    
    bool restart_required = flags && (*flags & ArgsManager::NETWORK_ONLY);
    metadata.pushKV("restart_required", restart_required);
    
    return metadata;
}

UniValue GetSettingCategories()
{
    UniValue categories(UniValue::VOBJ);
    
    std::map<std::string, UniValue> category_arrays;
    category_arrays["wallet"] = UniValue(UniValue::VARR);
    category_arrays["mempool"] = UniValue(UniValue::VARR);
    category_arrays["relay"] = UniValue(UniValue::VARR);
    category_arrays["script"] = UniValue(UniValue::VARR);
    category_arrays["transaction"] = UniValue(UniValue::VARR);
    category_arrays["data_carrier"] = UniValue(UniValue::VARR);
    category_arrays["dust"] = UniValue(UniValue::VARR);
    category_arrays["block_creation"] = UniValue(UniValue::VARR);
    category_arrays["network"] = UniValue(UniValue::VARR);
    category_arrays["gui"] = UniValue(UniValue::VARR);
    category_arrays["node"] = UniValue(UniValue::VARR);
    
    category_arrays["wallet"].push_back("walletrbf");
    category_arrays["wallet"].push_back("spendzeroconfchange");
    category_arrays["mempool"].push_back("maxmempool");
    category_arrays["mempool"].push_back("mempoolreplacement");
    category_arrays["relay"].push_back("limitancestorcount");
    category_arrays["relay"].push_back("limitancestorsize");
    category_arrays["relay"].push_back("limitdescendantcount");
    category_arrays["relay"].push_back("limitdescendantsize");
    category_arrays["script"].push_back("rejectunknownscripts");
    category_arrays["script"].push_back("rejectparasites");
    category_arrays["script"].push_back("rejecttokens");
    category_arrays["script"].push_back("rejectspkreuse");
    category_arrays["script"].push_back("rejectbarepubkey");
    category_arrays["script"].push_back("rejectbaremultisig");
    category_arrays["data_carrier"].push_back("rejectnonstddatacarrier");
    category_arrays["gui"].push_back("splash");
    category_arrays["gui"].push_back("minimized");
    category_arrays["network"].push_back("listen");
    category_arrays["network"].push_back("server");
    category_arrays["node"].push_back("blockfilterindex");
    category_arrays["node"].push_back("coinstatsindex");
    category_arrays["node"].push_back("txindex");
    
    for (auto& [key, arr] : category_arrays) {
        categories.pushKV(key, arr);
    }
    
    return categories;
}

bool ValidateSettingValue(const std::string& setting_name, const SettingsValue& value, std::vector<std::string>& errors)
{
    UniValue metadata = GetSettingMetadata(setting_name);
    if (metadata.exists("error")) {
        errors.push_back(strprintf("Unknown setting: %s", setting_name));
        return false;
    }
    
    int type_int = metadata["type"].getInt<int>();
    SettingTypeInfo::Type type = static_cast<SettingTypeInfo::Type>(type_int);
    
    switch (type) {
        case SettingTypeInfo::BOOL:
            if (!value.isBool()) {
                errors.push_back(strprintf("Setting '%s' must be a boolean", setting_name));
                return false;
            }
            break;
            
        case SettingTypeInfo::INT:
        case SettingTypeInfo::AMOUNT:
            if (!value.isNum()) {
                errors.push_back(strprintf("Setting '%s' must be a number", setting_name));
                return false;
            }
            break;
            
        case SettingTypeInfo::DOUBLE:
            if (!value.isNum()) {
                errors.push_back(strprintf("Setting '%s' must be a number", setting_name));
                return false;
            }
            break;
            
        case SettingTypeInfo::STRING:
            if (!value.isStr()) {
                errors.push_back(strprintf("Setting '%s' must be a string", setting_name));
                return false;
            }
            break;
    }
    
    return true;
}

UniValue SettingsToJson(const Settings& settings)
{
    UniValue result(UniValue::VOBJ);
    result.pushKV("version", 1);
    return result;
}

bool JsonToSettings(const UniValue& json, Settings& settings, std::vector<std::string>& errors)
{
    return true;
}

bool ValidateNumericRange(int64_t value, int64_t min_val, int64_t max_val, 
                         const std::string& setting_name, std::vector<std::string>& errors)
{
    return true;
}

bool ValidateStringOptions(const std::string& value, const std::vector<std::string>& allowed_values,
                          const std::string& setting_name, std::vector<std::string>& errors)
{
    return true;
}

SettingTypeInfo GetSettingTypeInfo(const std::string& setting_name)
{
    UniValue metadata = GetSettingMetadata(setting_name);
    SettingTypeInfo::Type type = metadata.exists("type") ? 
        static_cast<SettingTypeInfo::Type>(metadata["type"].getInt<int>()) : 
        SettingTypeInfo::STRING;
    
    return {type, 0, 0, {}, 
            metadata.exists("description") ? metadata["description"].get_str() : "Unknown setting", 
            metadata.exists("restart_required") ? metadata["restart_required"].get_bool() : false, 
            SettingCategory::NODE};
}

int64_t AmountToSatoshi(const std::string& amount_str)
{
    try {
        return std::stoll(amount_str);
    } catch (...) {
        return 0;
    }
}

std::string SatoshiToAmountString(int64_t satoshi)
{
    return strprintf("%d", satoshi);
}

} // namespace common