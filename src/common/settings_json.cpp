// Copyright (c) 2025 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/settings_json.h>

#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <policy/policy.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <tinyformat.h>

#include <algorithm>
#include <map>
#include <unordered_map>

namespace common {

// Define type information for all settings from OptionsModel
static const std::unordered_map<std::string, SettingTypeInfo> SETTING_TYPE_MAP = {
    // GUI settings
    {"startatstartup", {SettingTypeInfo::BOOL, 0, 0, {}, "Start Bitcoin on system startup", false, SettingCategory::GUI}},
    {"showtrayicon", {SettingTypeInfo::BOOL, 0, 0, {}, "Show tray icon", false, SettingCategory::GUI}},
    {"minimizetotray", {SettingTypeInfo::BOOL, 0, 0, {}, "Minimize to tray instead of taskbar", false, SettingCategory::GUI}},
    {"minimizeonclose", {SettingTypeInfo::BOOL, 0, 0, {}, "Minimize on close instead of exiting", false, SettingCategory::GUI}},
    {"displayunit", {SettingTypeInfo::STRING, 0, 0, {"BTC", "mBTC", "µBTC", "sat"}, "Unit to show amounts in", false, SettingCategory::GUI}},
    {"displayaddresses", {SettingTypeInfo::BOOL, 0, 0, {}, "Display addresses in transaction list", false, SettingCategory::GUI}},
    {"thirdpartytxurls", {SettingTypeInfo::STRING, 0, 0, {}, "Third-party transaction URLs", false, SettingCategory::GUI}},
    {"language", {SettingTypeInfo::STRING, 0, 0, {}, "User interface language", false, SettingCategory::GUI}},
    {"fontformoney", {SettingTypeInfo::STRING, 0, 0, {}, "Font for money amounts", false, SettingCategory::GUI}},
    {"fontforqrcodes", {SettingTypeInfo::STRING, 0, 0, {}, "Font for QR codes", false, SettingCategory::GUI}},
    {"peerstabalternatingrowcolors", {SettingTypeInfo::BOOL, 0, 0, {}, "Use alternating row colors in peers tab", false, SettingCategory::GUI}},
    {"coincontrolfeatures", {SettingTypeInfo::BOOL, 0, 0, {}, "Enable coin control features", false, SettingCategory::GUI}},
    {"subfeefromamount", {SettingTypeInfo::BOOL, 0, 0, {}, "Subtract fee from send amount by default", false, SettingCategory::GUI}},
    {"enablepsbtcontrols", {SettingTypeInfo::BOOL, 0, 0, {}, "Enable PSBT controls", false, SettingCategory::GUI}},
    {"maskvalues", {SettingTypeInfo::BOOL, 0, 0, {}, "Mask sensitive values in GUI", false, SettingCategory::GUI}},
    
    // Wallet settings
    {"walletrbf", {SettingTypeInfo::BOOL, 0, 0, {}, "Enable Replace-By-Fee by default", false, SettingCategory::WALLET}},
    {"spendzeroconfchange", {SettingTypeInfo::BOOL, 0, 0, {}, "Spend unconfirmed change when sending transactions", false, SettingCategory::WALLET}},
    {"addresstype", {SettingTypeInfo::STRING, 0, 0, {"legacy", "p2sh-segwit", "bech32"}, "Default address type for receiving", false, SettingCategory::WALLET}},
    
    // Mempool policies
    {"mempoolreplacement", {SettingTypeInfo::STRING, 0, 0, {"never", "fee", "fee,optin", "fee,optin,notsignalled"}, "Transaction replacement policy", true, SettingCategory::MEMPOOL}},
    {"mempooltruc", {SettingTypeInfo::BOOL, 0, 0, {}, "Enable TRUC (Topologically Restricted Until Confirmation) transactions", true, SettingCategory::MEMPOOL}},
    {"maxorphantx", {SettingTypeInfo::INT, 0, 10000, {}, "Maximum number of orphan transactions", true, SettingCategory::MEMPOOL}},
    {"maxmempool", {SettingTypeInfo::INT, 5, 3000, {}, "Maximum memory pool size in MB", true, SettingCategory::MEMPOOL}},
    {"mempoolexpiry", {SettingTypeInfo::INT, 1, 999999, {}, "Hours for transactions to expire from mempool", true, SettingCategory::MEMPOOL}},
    
    // Relay policies  
    {"incrementalrelayfee", {SettingTypeInfo::AMOUNT, 0, MAX_MONEY, {}, "Fee rate increase required for mempool limiting/replacement", true, SettingCategory::RELAY}},
    {"minrelaytxfee", {SettingTypeInfo::AMOUNT, 0, MAX_MONEY, {}, "Minimum relay fee rate", true, SettingCategory::RELAY}},
    {"bytespersigop", {SettingTypeInfo::INT, 1, 10000, {}, "Bytes per sigop for relay policy", true, SettingCategory::RELAY}},
    {"bytespersigopstrict", {SettingTypeInfo::INT, 1, 10000, {}, "Strict bytes per sigop for relay policy", true, SettingCategory::RELAY}},
    {"limitancestorcount", {SettingTypeInfo::INT, 1, 1000, {}, "Maximum ancestor count for mempool transactions", true, SettingCategory::RELAY}},
    {"limitancestorsize", {SettingTypeInfo::INT, 1, 10000, {}, "Maximum ancestor size in kvB for mempool transactions", true, SettingCategory::RELAY}},
    {"limitdescendantcount", {SettingTypeInfo::INT, 1, 1000, {}, "Maximum descendant count for mempool transactions", true, SettingCategory::RELAY}},
    {"limitdescendantsize", {SettingTypeInfo::INT, 1, 10000, {}, "Maximum descendant size in kvB for mempool transactions", true, SettingCategory::RELAY}},
    
    // Script policies
    {"rejectunknownscripts", {SettingTypeInfo::BOOL, 0, 0, {}, "Reject transactions with unknown script types", true, SettingCategory::SCRIPT}},
    {"rejectparasites", {SettingTypeInfo::BOOL, 0, 0, {}, "Reject parasite transactions", true, SettingCategory::SCRIPT}},
    {"rejecttokens", {SettingTypeInfo::BOOL, 0, 0, {}, "Reject token transactions", true, SettingCategory::SCRIPT}},
    {"rejectspkreuse", {SettingTypeInfo::STRING, 0, 0, {"allow", "conflict", "strict"}, "Script public key reuse policy", true, SettingCategory::SCRIPT}},
    
    // Transaction policies
    {"rejectbarepubkey", {SettingTypeInfo::BOOL, 0, 0, {}, "Reject bare public key transactions", true, SettingCategory::TRANSACTION}},
    {"rejectbaremultisig", {SettingTypeInfo::BOOL, 0, 0, {}, "Reject bare multisig transactions", true, SettingCategory::TRANSACTION}},
    {"maxscriptsize", {SettingTypeInfo::INT, 1, 100000, {}, "Maximum script size", true, SettingCategory::TRANSACTION}},
    
    // Data carrier settings
    {"datacarriercost", {SettingTypeInfo::DOUBLE, 0, 100, {}, "Weight multiplier for data carrier outputs", true, SettingCategory::DATA_CARRIER}},
    {"datacarriersize", {SettingTypeInfo::INT, 0, 1000, {}, "Maximum size of data carrier outputs", true, SettingCategory::DATA_CARRIER}},
    {"rejectnonstddatacarrier", {SettingTypeInfo::BOOL, 0, 0, {}, "Reject non-standard data carrier transactions", true, SettingCategory::DATA_CARRIER}},
    
    // Dust settings
    {"dustrelayfee", {SettingTypeInfo::AMOUNT, 0, MAX_MONEY, {}, "Fee rate threshold for dust", true, SettingCategory::DUST}},
    {"dustdynamic", {SettingTypeInfo::STRING, 0, 0, {"off", "min1", "min2", "min3", "min4", "min5"}, "Dynamic dust threshold policy", true, SettingCategory::DUST}},
    
    // Block creation settings
    {"blockmintxfee", {SettingTypeInfo::AMOUNT, 0, MAX_MONEY, {}, "Minimum fee rate for transactions in created blocks", true, SettingCategory::BLOCK_CREATION}},
    {"blockmaxsize", {SettingTypeInfo::INT, 1000, MAX_BLOCK_SERIALIZED_SIZE, {}, "Maximum block size for mining", true, SettingCategory::BLOCK_CREATION}},
    {"blockprioritysize", {SettingTypeInfo::INT, 0, 1000000, {}, "Size reserved for priority transactions in blocks", true, SettingCategory::BLOCK_CREATION}},
    {"blockmaxweight", {SettingTypeInfo::INT, 4000, MAX_BLOCK_WEIGHT, {}, "Maximum block weight for mining", true, SettingCategory::BLOCK_CREATION}},
    {"blockreconstructionextratxn", {SettingTypeInfo::INT, 0, 1000, {}, "Extra transactions for block reconstruction", true, SettingCategory::BLOCK_CREATION}},
    
    // Network settings
    {"listen", {SettingTypeInfo::BOOL, 0, 0, {}, "Accept connections from outside", true, SettingCategory::NETWORK}},
    {"server", {SettingTypeInfo::BOOL, 0, 0, {}, "Accept JSON-RPC commands", true, SettingCategory::NETWORK}},
    {"networkport", {SettingTypeInfo::INT, 1, 65535, {}, "Network port to listen on", true, SettingCategory::NETWORK}},
    {"mapportupnp", {SettingTypeInfo::BOOL, 0, 0, {}, "Use UPnP to map the listening port", true, SettingCategory::NETWORK}},
    {"mapportnatpmp", {SettingTypeInfo::BOOL, 0, 0, {}, "Use NAT-PMP to map the listening port", true, SettingCategory::NETWORK}},
    {"maxuploadtarget", {SettingTypeInfo::INT, 0, 1000000, {}, "Maximum upload target in MB", true, SettingCategory::NETWORK}},
    {"peerbloomfilters", {SettingTypeInfo::BOOL, 0, 0, {}, "Support filtering of blocks and transaction with bloom filters", true, SettingCategory::NETWORK}},
    {"peerblockfilters", {SettingTypeInfo::BOOL, 0, 0, {}, "Serve compact block filters to peers", true, SettingCategory::NETWORK}},
    
    // Node settings  
    {"threadsscriptverif", {SettingTypeInfo::INT, 0, 32, {}, "Number of script verification threads", true, SettingCategory::NODE}},
    {"prunetristate", {SettingTypeInfo::INT, 0, 2, {}, "Prune tristate (0=no, 1=yes, 2=auto)", true, SettingCategory::NODE}},
    {"prunesize", {SettingTypeInfo::INT, 550, 1000000, {}, "Target size in MB for pruned blockchain", true, SettingCategory::NODE}},
    {"databasecache", {SettingTypeInfo::INT, 4, 16384, {}, "Database cache size in MB", true, SettingCategory::NODE}},
    {"externalsignerpath", {SettingTypeInfo::STRING, 0, 0, {}, "Path to external signer", false, SettingCategory::NODE}},
    {"corepolicy", {SettingTypeInfo::STRING, 0, 0, {"mainnet", "testnet", "signet", "regtest"}, "Core network policy", true, SettingCategory::NODE}},
    
    // Proxy settings
    {"proxyuse", {SettingTypeInfo::BOOL, 0, 0, {}, "Use proxy for connections", true, SettingCategory::NETWORK}},
    {"proxyip", {SettingTypeInfo::STRING, 0, 0, {}, "Proxy IP address", true, SettingCategory::NETWORK}},
    {"proxyport", {SettingTypeInfo::INT, 1, 65535, {}, "Proxy port", true, SettingCategory::NETWORK}},
    {"proxyusetor", {SettingTypeInfo::BOOL, 0, 0, {}, "Use separate proxy for Tor", true, SettingCategory::NETWORK}},
    {"proxyiptor", {SettingTypeInfo::STRING, 0, 0, {}, "Tor proxy IP address", true, SettingCategory::NETWORK}},
    {"proxyporttor", {SettingTypeInfo::INT, 1, 65535, {}, "Tor proxy port", true, SettingCategory::NETWORK}},
};

UniValue SettingsToJson(const Settings& settings)
{
    UniValue result(UniValue::VOBJ);
    
    // Add version information
    result.pushKV("version", 1);
    result.pushKV("timestamp", GetTime());
    
    // Initialize category objects
    std::map<std::string, UniValue> category_objects;
    category_objects["wallet"] = UniValue(UniValue::VOBJ);
    category_objects["mempool"] = UniValue(UniValue::VOBJ);
    category_objects["relay"] = UniValue(UniValue::VOBJ);
    category_objects["script"] = UniValue(UniValue::VOBJ);
    category_objects["transaction"] = UniValue(UniValue::VOBJ);
    category_objects["data_carrier"] = UniValue(UniValue::VOBJ);
    category_objects["dust"] = UniValue(UniValue::VOBJ);
    category_objects["block_creation"] = UniValue(UniValue::VOBJ);
    category_objects["network"] = UniValue(UniValue::VOBJ);
    category_objects["gui"] = UniValue(UniValue::VOBJ);
    category_objects["node"] = UniValue(UniValue::VOBJ);
    
    // Serialize all settings by category
    for (const auto& [setting_name, type_info] : SETTING_TYPE_MAP) {
        SettingsValue value = GetSetting(settings, "", setting_name, false, false, false);
        
        if (!value.isNull()) {
            UniValue setting_obj(UniValue::VOBJ);
            
            // Convert value based on type
            switch (type_info.type) {
                case SettingTypeInfo::BOOL:
                    setting_obj.pushKV("value", value.get_bool());
                    break;
                case SettingTypeInfo::INT:
                    setting_obj.pushKV("value", value.getInt<int64_t>());
                    break;
                case SettingTypeInfo::DOUBLE:
                    setting_obj.pushKV("value", value.get_real());
                    break;
                case SettingTypeInfo::AMOUNT:
                    // Store amounts as satoshi for precision
                    setting_obj.pushKV("value", AmountToSatoshi(value.get_str()));
                    setting_obj.pushKV("unit", "satoshi");
                    break;
                case SettingTypeInfo::STRING:
                    setting_obj.pushKV("value", value.get_str());
                    break;
            }
            
            // Add metadata
            setting_obj.pushKV("type", static_cast<int>(type_info.type));
            setting_obj.pushKV("description", type_info.description);
            setting_obj.pushKV("restart_required", type_info.restart_required);
            
            // Add to appropriate category
            std::string category_key;
            switch (type_info.category) {
                case SettingCategory::WALLET: category_key = "wallet"; break;
                case SettingCategory::MEMPOOL: category_key = "mempool"; break;
                case SettingCategory::RELAY: category_key = "relay"; break;
                case SettingCategory::SCRIPT: category_key = "script"; break;
                case SettingCategory::TRANSACTION: category_key = "transaction"; break;
                case SettingCategory::DATA_CARRIER: category_key = "data_carrier"; break;
                case SettingCategory::DUST: category_key = "dust"; break;
                case SettingCategory::BLOCK_CREATION: category_key = "block_creation"; break;
                case SettingCategory::NETWORK: category_key = "network"; break;
                case SettingCategory::GUI: category_key = "gui"; break;
                case SettingCategory::NODE: category_key = "node"; break;
            }
            
            category_objects[category_key].pushKV(setting_name, setting_obj);
        }
    }
    
    // Build final categories object
    UniValue categories(UniValue::VOBJ);
    for (const auto& [key, obj] : category_objects) {
        categories.pushKV(key, obj);
    }
    
    result.pushKV("settings", categories);
    return result;
}

bool JsonToSettings(const UniValue& json, Settings& settings, std::vector<std::string>& errors)
{
    if (!json.isObject()) {
        errors.push_back("Input must be a JSON object");
        return false;
    }
    
    // Validate version if present
    if (json.exists("version")) {
        const UniValue& version = json["version"];
        if (!version.isNum() || version.getInt<int>() != 1) {
            errors.push_back("Unsupported settings version");
            return false;
        }
    }
    
    if (!json.exists("settings")) {
        errors.push_back("Missing 'settings' field");
        return false;
    }
    
    const UniValue& settings_obj = json["settings"];
    if (!settings_obj.isObject()) {
        errors.push_back("'settings' must be an object");
        return false;
    }
    
    bool success = true;
    
    // Process each category
    for (const std::string& category : settings_obj.getKeys()) {
        const UniValue& category_obj = settings_obj[category];
        if (!category_obj.isObject()) {
            errors.push_back(strprintf("Category '%s' must be an object", category));
            success = false;
            continue;
        }
        
        // Process each setting in category
        for (const std::string& setting_name : category_obj.getKeys()) {
            const UniValue& setting_obj = category_obj[setting_name];
            
            if (!setting_obj.isObject() || !setting_obj.exists("value")) {
                errors.push_back(strprintf("Setting '%s' must have a 'value' field", setting_name));
                success = false;
                continue;
            }
            
            const UniValue& value = setting_obj["value"];
            
            // Validate the setting value
            if (!ValidateSettingValue(setting_name, value, errors)) {
                success = false;
                continue;
            }
            
            // Convert and store the setting
            SettingsValue converted_value;
            auto type_info_it = SETTING_TYPE_MAP.find(setting_name);
            if (type_info_it != SETTING_TYPE_MAP.end()) {
                const SettingTypeInfo& type_info = type_info_it->second;
                
                switch (type_info.type) {
                    case SettingTypeInfo::BOOL:
                        converted_value.setBool(value.get_bool());
                        break;
                    case SettingTypeInfo::INT:
                        converted_value.setInt(value.getInt<int64_t>());
                        break;
                    case SettingTypeInfo::DOUBLE:
                        converted_value.setFloat(value.get_real());
                        break;
                    case SettingTypeInfo::AMOUNT:
                        converted_value.setStr(SatoshiToAmountString(value.getInt<int64_t>()));
                        break;
                    case SettingTypeInfo::STRING:
                        converted_value.setStr(value.get_str());
                        break;
                }
                
                settings.rw_settings[setting_name] = converted_value;
            }
        }
    }
    
    return success;
}

bool ValidateSettingValue(const std::string& setting_name, const SettingsValue& value, std::vector<std::string>& errors)
{
    auto it = SETTING_TYPE_MAP.find(setting_name);
    if (it == SETTING_TYPE_MAP.end()) {
        errors.push_back(strprintf("Unknown setting: %s", setting_name));
        return false;
    }
    
    const SettingTypeInfo& type_info = it->second;
    
    switch (type_info.type) {
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
            return ValidateNumericRange(value.getInt<int64_t>(), type_info.min_value, type_info.max_value, setting_name, errors);
            
        case SettingTypeInfo::DOUBLE:
            if (!value.isNum()) {
                errors.push_back(strprintf("Setting '%s' must be a number", setting_name));
                return false;
            }
            // Additional validation for double ranges could be added here
            break;
            
        case SettingTypeInfo::STRING:
            if (!value.isStr()) {
                errors.push_back(strprintf("Setting '%s' must be a string", setting_name));
                return false;
            }
            if (!type_info.allowed_values.empty()) {
                return ValidateStringOptions(value.get_str(), type_info.allowed_values, setting_name, errors);
            }
            break;
    }
    
    return true;
}

bool ValidateNumericRange(int64_t value, int64_t min_val, int64_t max_val, 
                         const std::string& setting_name, std::vector<std::string>& errors)
{
    if (max_val > 0 && (value < min_val || value > max_val)) {
        errors.push_back(strprintf("Setting '%s' value %d is out of range [%d, %d]", 
                                  setting_name, value, min_val, max_val));
        return false;
    }
    return true;
}

bool ValidateStringOptions(const std::string& value, const std::vector<std::string>& allowed_values,
                          const std::string& setting_name, std::vector<std::string>& errors)
{
    if (std::find(allowed_values.begin(), allowed_values.end(), value) == allowed_values.end()) {
        std::string allowed_str;
        for (size_t i = 0; i < allowed_values.size(); ++i) {
            if (i > 0) allowed_str += ", ";
            allowed_str += allowed_values[i];
        }
        errors.push_back(strprintf("Setting '%s' value '%s' not in allowed values: %s", 
                                  setting_name, value, allowed_str));
        return false;
    }
    return true;
}

UniValue GetSettingMetadata(const std::string& setting_name)
{
    UniValue metadata(UniValue::VOBJ);
    
    auto it = SETTING_TYPE_MAP.find(setting_name);
    if (it == SETTING_TYPE_MAP.end()) {
        metadata.pushKV("error", "Unknown setting");
        return metadata;
    }
    
    const SettingTypeInfo& type_info = it->second;
    
    metadata.pushKV("type", static_cast<int>(type_info.type));
    metadata.pushKV("description", type_info.description);
    metadata.pushKV("restart_required", type_info.restart_required);
    metadata.pushKV("category", static_cast<int>(type_info.category));
    
    if (type_info.max_value > 0) {
        metadata.pushKV("min_value", type_info.min_value);
        metadata.pushKV("max_value", type_info.max_value);
    }
    
    if (!type_info.allowed_values.empty()) {
        UniValue allowed(UniValue::VARR);
        for (const std::string& val : type_info.allowed_values) {
            allowed.push_back(val);
        }
        metadata.pushKV("allowed_values", allowed);
    }
    
    return metadata;
}

UniValue GetSettingCategories()
{
    UniValue categories(UniValue::VOBJ);
    
    // Initialize category arrays
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
    
    // Populate categories
    for (const auto& [setting_name, type_info] : SETTING_TYPE_MAP) {
        std::string category_key;
        switch (type_info.category) {
            case SettingCategory::WALLET: category_key = "wallet"; break;
            case SettingCategory::MEMPOOL: category_key = "mempool"; break;
            case SettingCategory::RELAY: category_key = "relay"; break;
            case SettingCategory::SCRIPT: category_key = "script"; break;
            case SettingCategory::TRANSACTION: category_key = "transaction"; break;
            case SettingCategory::DATA_CARRIER: category_key = "data_carrier"; break;
            case SettingCategory::DUST: category_key = "dust"; break;
            case SettingCategory::BLOCK_CREATION: category_key = "block_creation"; break;
            case SettingCategory::NETWORK: category_key = "network"; break;
            case SettingCategory::GUI: category_key = "gui"; break;
            case SettingCategory::NODE: category_key = "node"; break;
        }
        category_arrays[category_key].push_back(setting_name);
    }
    
    // Add to result
    for (auto& [key, arr] : category_arrays) {
        categories.pushKV(key, arr);
    }
    
    return categories;
}

SettingTypeInfo GetSettingTypeInfo(const std::string& setting_name)
{
    auto it = SETTING_TYPE_MAP.find(setting_name);
    if (it != SETTING_TYPE_MAP.end()) {
        return it->second;
    }
    
    // Return default for unknown settings
    return {SettingTypeInfo::STRING, 0, 0, {}, "Unknown setting", false, SettingCategory::NODE};
}

int64_t AmountToSatoshi(const std::string& amount_str)
{
    // Parse amount string and convert to satoshi
    // This is a simplified implementation - would need proper parsing
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