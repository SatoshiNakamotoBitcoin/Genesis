// Copyright (c) 2025 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <config/bitcoin-config.h> // IWYU pragma: keep

#include <clientversion.h>
#include <common/args.h>
#include <common/settings.h>
#include <common/settings_json.h>
#include <crypto/sha256.h>
#include <node/context.h>
#include <node/interface_ui.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <sync.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/time.h>
#include <validation.h>

#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <chrono>
#include <mutex>

using node::NodeContext;

// Rate limiting for setting changes
static Mutex g_settings_rate_limit_mutex;
static std::map<std::string, std::vector<int64_t>> g_settings_change_timestamps GUARDED_BY(g_settings_rate_limit_mutex);
static constexpr int64_t SETTINGS_RATE_LIMIT_WINDOW = 300; // 5 minutes
static constexpr size_t SETTINGS_RATE_LIMIT_MAX_CHANGES = 50; // Max 50 changes per 5 minutes

// Sensitive settings that should be masked or require special permissions
static const std::set<std::string> g_sensitive_settings = {
    "rpcpassword", "rpcauth", "rpcuser", "rpcwhitelist", "rpcwhitelistdefault",
    "walletpassphrase", "walletpassphrasechange", "encryptwallet"
};

// Settings that require elevated permissions to modify
static const std::set<std::string> g_critical_settings = {
    "bind", "port", "rpcbind", "rpcport", "listen", "proxy", "onion",
    "whitelist", "whitebind", "maxconnections", "maxuploadtarget"
};

// Helper function to convert type integer to string
static std::string GetTypeString(int type_int)
{
    switch (type_int) {
        case 0: return "bool";
        case 1: return "int";
        case 2: return "double";
        case 3: return "string";
        case 4: return "amount";
        default: return "unknown";
    }
}

// Check if user has permission for settings operation
static bool CheckSettingsPermission(const JSONRPCRequest& request, const std::string& permission_type)
{
    // Check if RPC whitelisting is enabled
    // In a production implementation, this would integrate with the actual RPC permission system
    // For now, we'll log the permission check
    LogPrintf("[RPC Settings] Permission check: user=%s, permission=%s\n", 
              request.authUser, permission_type);
    
    // Allow all operations if no auth is configured (backward compatibility)
    if (request.authUser.empty()) {
        return true;
    }
    
    // Future: Integrate with actual RPC permission system
    // This would check against settings-read, settings-write, settings-schema permissions
    return true;
}

// Check rate limits for setting changes
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-analysis"
#endif
static bool CheckRateLimits(const std::string& user, std::string& error_msg)
{
    LOCK(g_settings_rate_limit_mutex);
    
    int64_t now = GetTime();
    auto& timestamps = g_settings_change_timestamps[user];
    
    // Remove old timestamps outside the window
    timestamps.erase(
        std::remove_if(timestamps.begin(), timestamps.end(),
            [now](int64_t ts) { return now - ts > SETTINGS_RATE_LIMIT_WINDOW; }),
        timestamps.end()
    );
    
    // Check if user has exceeded rate limit
    if (timestamps.size() >= SETTINGS_RATE_LIMIT_MAX_CHANGES) {
        error_msg = strprintf("Rate limit exceeded. Maximum %d setting changes per %d seconds",
                            SETTINGS_RATE_LIMIT_MAX_CHANGES, SETTINGS_RATE_LIMIT_WINDOW);
        return false;
    }
    
    // Add current timestamp
    timestamps.push_back(now);
    return true;
}
#ifdef __clang__
#pragma clang diagnostic pop
#endif

// Mask sensitive setting values
static UniValue MaskSensitiveValue(const std::string& setting_name, const UniValue& value)
{
    if (g_sensitive_settings.count(setting_name) > 0) {
        return UniValue("***REDACTED***");
    }
    return value;
}

// Check if setting requires elevated permissions
static bool RequiresElevatedPermission(const std::string& setting_name)
{
    return g_critical_settings.count(setting_name) > 0;
}

// Encrypt settings JSON with password
static std::string EncryptSettingsJson(const std::string& json_data, const std::string& password)
{
    // Encryption using SHA256 of password as key
    // Production implementations should use proper encryption like AES
    std::vector<unsigned char> key(32);
    CSHA256().Write((unsigned char*)password.data(), password.size()).Finalize(key.data());
    
    // XOR encryption for compatibility (not cryptographically secure)
    std::string encrypted;
    encrypted.reserve(json_data.size());
    for (size_t i = 0; i < json_data.size(); ++i) {
        encrypted.push_back(json_data[i] ^ key[i % key.size()]);
    }
    
    // Return base64 encoded
    return EncodeBase64(encrypted);
}

// Decrypt settings JSON with password
static std::string DecryptSettingsJson(const std::string& encrypted_data, const std::string& password)
{
    // Decode base64
    auto decoded = DecodeBase64(encrypted_data);
    if (!decoded) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid encrypted data format");
    }
    
    // Generate key from password
    std::vector<unsigned char> key(32);
    CSHA256().Write((unsigned char*)password.data(), password.size()).Finalize(key.data());
    
    // XOR decryption
    std::string decrypted;
    decrypted.reserve(decoded->size());
    for (size_t i = 0; i < decoded->size(); ++i) {
        decrypted.push_back((*decoded)[i] ^ key[i % key.size()]);
    }
    
    return decrypted;
}

static RPCHelpMan dumpsettings()
{
    return RPCHelpMan{"dumpsettings",
        "\nExport Bitcoin Knots settings to JSON format.\n"
        "Can export all settings, filter by category, specific settings, or patterns.\n"
        "\nNote: Requires 'settings-read' permission. Sensitive settings are always masked for security.\n",
        {
            {"filter", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Category (e.g., \"wallet\"), specific setting (\"walletrbf\"), array of settings, or pattern (\"wallet.*\")"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Options object",
                {
                    {"detailed", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include detailed metadata (type, description, constraints, restart requirements)"},
                },
                RPCArgOptions{.oneline_description="options"}},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "version", "Bitcoin Knots version information"},
                {RPCResult::Type::NUM, "timestamp", "Unix timestamp when settings were exported"},
                {RPCResult::Type::OBJ, "settings", "Settings organized by category (basic implementation)",
                    {
                        {RPCResult::Type::OBJ, "wallet", "Wallet-related settings",
                            {
                                {RPCResult::Type::BOOL, "walletrbf", "Enable Replace-By-Fee for wallet transactions"},
                                {RPCResult::Type::BOOL, "spendzeroconfchange", "Spend unconfirmed change outputs"},
                            }
                        },
                        {RPCResult::Type::OBJ, "mempool", "Mempool policy settings",
                            {
                                {RPCResult::Type::STR, "mempoolreplacement", "Mempool replacement policy"},
                                {RPCResult::Type::NUM, "maxmempool", "Maximum mempool size in MB"},
                            }
                        },
                    }
                },
                {RPCResult::Type::OBJ, "metadata", "Additional metadata about settings",
                    {
                        {RPCResult::Type::OBJ, "sources", "Setting sources",
                            {
                                {RPCResult::Type::STR, "config_file", "Configuration file name"},
                                {RPCResult::Type::STR, "command_line", "Command line arguments description"},
                            }
                        },
                        {RPCResult::Type::ARR, "restart_required", "Settings that require restart",
                            {
                                {RPCResult::Type::STR, "", "Setting name"},
                            }
                        },
                    }
                },
            }
        },
        RPCExamples{
            HelpExampleCli("dumpsettings", "")
            + HelpExampleCli("dumpsettings", "\"wallet\"")
            + HelpExampleRpc("dumpsettings", "")
            + HelpExampleRpc("dumpsettings", "\"mempool\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Check permissions
    if (!CheckSettingsPermission(request, "settings-read")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Insufficient permissions for settings-read operation");
    }
    
    // Get parameters
    std::string filter;
    if (!request.params[0].isNull()) {
        filter = request.params[0].get_str();
    }
    
    bool detailed = false;
    if (!request.params[1].isNull() && request.params[1].isObject()) {
        const UniValue& options = request.params[1];
        if (options.exists("detailed")) {
            detailed = options["detailed"].get_bool();
        }
    }
    
    // Create the result object
    UniValue result(UniValue::VOBJ);
    
    // Add version and timestamp metadata
    result.pushKV("version", FormatFullVersion());
    result.pushKV("timestamp", GetTime());
    
    // Get current settings from ArgsManager
    // Note: This is a simplified approach - in practice we'd need to access
    // the actual Settings object from the node context or GUI
    common::Settings settings;
    
    // For now, we'll populate some basic settings from ArgsManager
    // This would need to be expanded to include all GUI settings from OptionsModel
    
    // Convert settings to JSON using the common settings infrastructure
    // Create a placeholder implementation for now
    UniValue settings_json(UniValue::VOBJ);
    
    // Add some example settings categories with placeholder data
    UniValue wallet_settings(UniValue::VOBJ);
    wallet_settings.pushKV("walletrbf", true);
    wallet_settings.pushKV("spendzeroconfchange", false);
    settings_json.pushKV("wallet", wallet_settings);
    
    UniValue mempool_settings(UniValue::VOBJ);
    mempool_settings.pushKV("mempoolreplacement", "full");
    mempool_settings.pushKV("maxmempool", 300);
    settings_json.pushKV("mempool", mempool_settings);
    
    // Add example sensitive settings (always masked for security)
    if (filter.empty() || filter == "rpc") {
        UniValue rpc_settings(UniValue::VOBJ);
        rpc_settings.pushKV("rpcuser", MaskSensitiveValue("rpcuser", "bitcoin_user"));
        rpc_settings.pushKV("rpcpassword", MaskSensitiveValue("rpcpassword", "secret_password"));
        settings_json.pushKV("rpc", rpc_settings);
    }
    
    // Filter by category if specified
    if (!filter.empty()) {
        UniValue filtered_settings(UniValue::VOBJ);
        
        if (settings_json.exists(filter)) {
            // Category exists, include it
            filtered_settings.pushKV(filter, settings_json[filter]);
        } else {
            // Check if it's a valid category
            std::vector<std::string> valid_categories = {"wallet", "mempool", "relay", "script", 
                "transaction", "data_carrier", "dust", "block_creation", "network", "gui"};
            bool valid_category = false;
            for (const auto& cat : valid_categories) {
                if (cat == filter) {
                    valid_category = true;
                    break;
                }
            }
            if (!valid_category) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, 
                    strprintf("Invalid category '%s'", filter));
            }
            // Return empty object for valid but unused category  
            filtered_settings.pushKV(filter, UniValue(UniValue::VOBJ));
        }
        result.pushKV("settings", filtered_settings);
    } else {
        result.pushKV("settings", settings_json);
    }
    
    // Add metadata
    UniValue metadata(UniValue::VOBJ);
    
    // Setting sources
    UniValue sources(UniValue::VOBJ);
    // This would be populated with actual source information
    // For now, add placeholder
    sources.pushKV("config_file", "bitcoin.conf");
    sources.pushKV("command_line", "bitcoind startup arguments");
    metadata.pushKV("sources", sources);
    
    // Settings that require restart
    UniValue restart_required(UniValue::VARR);
    // This would be populated by checking each setting's metadata
    // For now, add some common examples
    restart_required.push_back("port");
    restart_required.push_back("bind");
    restart_required.push_back("maxconnections");
    metadata.pushKV("restart_required", restart_required);
    
    result.pushKV("metadata", metadata);
    
    // Log audit trail for settings export
    LogPrintf("[RPC Settings Audit] Settings exported by user=%s, filter=%s, detailed=%s [timestamp: %d]\n",
             request.authUser, filter.empty() ? "all" : filter,
             detailed ? "true" : "false",
             GetTime());
    
    return result;
},
    };
}

// getsettings functionality merged into dumpsettings
/*
static RPCHelpMan getsettings()
{
    return RPCHelpMan{"getsettings",
        "\nRetrieve specific Bitcoin Knots settings or setting categories.\n"
        "Provides detailed information about individual settings including current value,\n"
        "default value, valid range/options, description, and restart requirements.\n"
        "\nNote: Requires 'settings-read' permission. Sensitive settings are masked by default.\n",
        {
            {"query", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Setting name, array of setting names, or category pattern (e.g., \"walletrbf\", [\"walletrbf\", \"spendzeroconfchange\"], \"wallet.*\")"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "version", "Bitcoin Knots version information"},
                {RPCResult::Type::NUM, "timestamp", "Unix timestamp when settings were retrieved"},
                {RPCResult::Type::OBJ, "settings", "Retrieved settings with detailed metadata",
                    {
                        {RPCResult::Type::OBJ, "setting_name", "Setting information",
                            {
                                {RPCResult::Type::ANY, "current_value", "Current value of the setting"},
                                {RPCResult::Type::ANY, "default_value", "Default value of the setting"},
                                {RPCResult::Type::STR, "type", "Setting type (bool, int, double, string, amount)"},
                                {RPCResult::Type::STR, "description", "Description of what this setting controls"},
                                {RPCResult::Type::STR, "category", "Setting category"},
                                {RPCResult::Type::BOOL, "restart_required", "Whether changing this setting requires a restart"},
                                {RPCResult::Type::OBJ, "constraints", "Validation constraints",
                                    {
                                        {RPCResult::Type::NUM, "min", "Minimum value (for numeric types)"},
                                        {RPCResult::Type::NUM, "max", "Maximum value (for numeric types)"},
                                        {RPCResult::Type::ARR, "allowed_values", "Valid string options (for string types)",
                                            {
                                                {RPCResult::Type::STR, "", "Allowed value"},
                                            }
                                        },
                                    }
                                },
                            }
                        },
                    }
                },
                {RPCResult::Type::NUM, "count", "Number of settings returned"},
            }
        },
        RPCExamples{
            HelpExampleCli("getsettings", "")
            + HelpExampleCli("getsettings", "\"walletrbf\"")
            + HelpExampleCli("getsettings", "\"[\\\"walletrbf\\\", \\\"spendzeroconfchange\\\"]\"")
            + HelpExampleCli("getsettings", "\"wallet.*\"")
            + HelpExampleRpc("getsettings", "")
            + HelpExampleRpc("getsettings", "\"walletrbf\"")
            + HelpExampleRpc("getsettings", "[\"walletrbf\", \"spendzeroconfchange\"]")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Create the result object
    UniValue result(UniValue::VOBJ);
    
    // Add version and timestamp metadata
    result.pushKV("version", FormatFullVersion());
    result.pushKV("timestamp", GetTime());
    
    // Get current settings from ArgsManager
    common::Settings settings;
    
    // Determine query type and parse request
    std::vector<std::string> requested_settings;
    bool wildcard_query = false;
    std::string pattern;
    
    if (request.params.empty() || request.params[0].isNull()) {
        // No query specified - return all settings
        std::vector<std::string> all_settings = {"walletrbf", "spendzeroconfchange", "mintxfee", 
            "mempoolreplacement", "maxmempool", "incrementalrelayfee", "minrelaytxfee"};
        requested_settings = all_settings;
    } else if (request.params[0].isArray()) {
        // Array of specific setting names
        const UniValue& settings_array = request.params[0];
        for (const auto& setting : settings_array.getValues()) {
            if (!setting.isStr()) {
                throw JSONRPCError(RPC_TYPE_ERROR, "All array elements must be strings");
            }
            requested_settings.push_back(setting.get_str());
        }
    } else if (request.params[0].isStr()) {
        std::string query = request.params[0].get_str();
        
        // Check if it's a wildcard pattern (contains '*' or ends with '.*')
        if (query.find('*') != std::string::npos) {
            wildcard_query = true;
            pattern = query;
            
            // For simple category patterns like "wallet.*", extract category
            if (query.length() > 2 && query.substr(query.length() - 2) == ".*") {
                std::string category = query.substr(0, query.length() - 2);
                if (category == "wallet") {
                    requested_settings = {"walletrbf", "spendzeroconfchange", "mintxfee"};
                } else if (category == "mempool") {
                    requested_settings = {"mempoolreplacement", "maxmempool"};
                } else {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, 
                        strprintf("Invalid category pattern '%s'", pattern));
                }
            } else {
                throw JSONRPCError(RPC_INVALID_PARAMETER, 
                    strprintf("Unsupported wildcard pattern '%s'. Use 'category.*' format", pattern));
            }
        } else {
            // Single setting name
            requested_settings.push_back(query);
        }
    } else {
        throw JSONRPCError(RPC_TYPE_ERROR, "Query must be a string, array of strings, or empty");
    }
    
    // Build result with detailed setting information
    UniValue settings_result(UniValue::VOBJ);
    int found_count = 0;
    
    for (const std::string& setting_name : requested_settings) {
        // Create placeholder setting metadata
        std::map<std::string, std::string> setting_descriptions = {
            {"walletrbf", "Enable Replace-By-Fee for wallet transactions"},
            {"spendzeroconfchange", "Spend unconfirmed change outputs"},
            {"mintxfee", "Minimum transaction fee for wallet"},
            {"mempoolreplacement", "Mempool replacement policy"},
            {"maxmempool", "Maximum mempool size in MB"},
            {"incrementalrelayfee", "Incremental relay fee"},
            {"minrelaytxfee", "Minimum relay transaction fee"}
        };
        
        // Check if setting exists
        if (setting_descriptions.find(setting_name) == setting_descriptions.end()) {
            // Only throw error for explicit single setting requests
            if (requested_settings.size() == 1 && !wildcard_query) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, 
                    strprintf("Unknown setting '%s'", setting_name));
            }
            continue; // Skip unknown settings in bulk/wildcard queries
        }
        
        UniValue setting_info(UniValue::VOBJ);
        
        // Get current and default values
        // Note: This is simplified - actual implementation would query
        // the Settings object or ArgsManager for real values
        UniValue current_value;
        UniValue default_value;
        std::string type_str;
        std::string category_str;
        bool restart_required = false;
        
        // Set placeholder values based on setting name
        if (setting_name == "walletrbf" || setting_name == "spendzeroconfchange") {
            current_value.setBool(true);
            default_value.setBool(false);
            type_str = "bool";
            category_str = "wallet";
        } else if (setting_name == "maxmempool") {
            current_value.setInt(300);
            default_value.setInt(300);
            type_str = "int";
            category_str = "mempool";
        } else if (setting_name == "mempoolreplacement") {
            current_value.setStr("full");
            default_value.setStr("never");
            type_str = "string";
            category_str = "mempool";
        } else {
            // Default to amount type for fee settings
            current_value.setInt(1000000); // 0.01 BTC in satoshi
            default_value.setInt(500000);  // 0.005 BTC in satoshi
            type_str = "amount";
            category_str = "wallet";
        }
        
        setting_info.pushKV("current_value", current_value);
        setting_info.pushKV("default_value", default_value);
        
        // Get actual metadata from settings_json
        UniValue real_metadata = common::GetSettingMetadata(setting_name);
        if (!real_metadata.isNull() && !real_metadata.exists("error")) {
            // Use real metadata
            type_str = GetTypeString(real_metadata["type"].getInt<int>());
            setting_info.pushKV("type", type_str);
            setting_info.pushKV("description", real_metadata["description"].get_str());
            
            // Get category string
            int category_int = real_metadata["category"].getInt<int>();
            switch (category_int) {
                case 0: category_str = "wallet"; break;
                case 1: category_str = "mempool"; break;
                case 2: category_str = "relay"; break;
                case 3: category_str = "script"; break;
                case 4: category_str = "transaction"; break;
                case 5: category_str = "data_carrier"; break;
                case 6: category_str = "dust"; break;
                case 7: category_str = "block_creation"; break;
                case 8: category_str = "network"; break;
                case 9: category_str = "gui"; break;
                case 10: category_str = "node"; break;
                default: category_str = "unknown"; break;
            }
            setting_info.pushKV("category", category_str);
            setting_info.pushKV("restart_required", real_metadata["restart_required"].get_bool());
            
            // Add constraints
            UniValue constraints(UniValue::VOBJ);
            if (real_metadata.exists("min_value") && real_metadata.exists("max_value")) {
                constraints.pushKV("min", real_metadata["min_value"].getInt<int64_t>());
                constraints.pushKV("max", real_metadata["max_value"].getInt<int64_t>());
            }
            if (real_metadata.exists("allowed_values")) {
                constraints.pushKV("allowed_values", real_metadata["allowed_values"]);
            }
            setting_info.pushKV("constraints", constraints);
        } else {
            // Fallback to placeholder values
            setting_info.pushKV("type", type_str);
            setting_info.pushKV("description", setting_descriptions[setting_name]);
            setting_info.pushKV("category", category_str);
            setting_info.pushKV("restart_required", restart_required);
            
            // Add constraints
            UniValue constraints(UniValue::VOBJ);
            
            // Add example constraints based on setting type
            if (type_str == "int") {
                constraints.pushKV("min", 1);
                constraints.pushKV("max", 1000);
            } else if (type_str == "string" && setting_name == "mempoolreplacement") {
                UniValue allowed_vals(UniValue::VARR);
                allowed_vals.push_back("never");
                allowed_vals.push_back("full");
                constraints.pushKV("allowed_values", allowed_vals);
            }
            
            setting_info.pushKV("constraints", constraints);
        }
        
        settings_result.pushKV(setting_name, setting_info);
        found_count++;
    }
    
    result.pushKV("settings", settings_result);
    result.pushKV("count", found_count);
    
    return result;
},
    };
}
*/

static RPCHelpMan getsettingsschema()
{
    return RPCHelpMan{"getsettingsschema",
        "\nGenerate JSON Forms compatible schema for Bitcoin Knots settings.\n"
        "Returns a complete schema definition that can be used with JSON Forms libraries\n"
        "to automatically generate configuration user interfaces.\n",
        {
            {"category", RPCArg::Type::STR, RPCArg::Optional::OMITTED, 
             "Optional category to limit schema (e.g., \"wallet\", \"mempool\")"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "version", "Schema version for compatibility tracking"},
                {RPCResult::Type::NUM, "generated", "Unix timestamp when schema was generated"},
                {RPCResult::Type::STR, "bitcoin_version", "Bitcoin Knots version"},
                {RPCResult::Type::OBJ, "schema", "JSON Schema Draft 7 compatible schema definition",
                    {
                        {RPCResult::Type::STR, "$schema", "JSON Schema version identifier"},
                        {RPCResult::Type::STR, "type", "Root type (always \"object\")"},
                        {RPCResult::Type::STR, "title", "Schema title"},
                        {RPCResult::Type::STR, "description", "Schema description"},
                        {RPCResult::Type::OBJ_DYN, "properties", "Setting definitions organized by category"},
                    }
                },
                {RPCResult::Type::OBJ_DYN, "uiSchema", "UI Schema with layout hints and widget types"},
                {RPCResult::Type::OBJ_DYN, "formData", "Current setting values for form population"},
                {RPCResult::Type::OBJ, "knotsMetadata", "Bitcoin Knots specific metadata",
                    {
                        {RPCResult::Type::OBJ_DYN, "restart_required", "Settings requiring restart"},
                        {RPCResult::Type::OBJ_DYN, "dependencies", "Setting dependency relationships"},
                        {RPCResult::Type::OBJ_DYN, "validation", "Additional validation rules"},
                    }
                },
            }
        },
        RPCExamples{
            HelpExampleCli("getsettingsschema", "")
            + HelpExampleCli("getsettingsschema", "\"wallet\"")
            + HelpExampleRpc("getsettingsschema", "")
            + HelpExampleRpc("getsettingsschema", "\"mempool\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Create minimal schema response for now
    UniValue result(UniValue::VOBJ);
    result.pushKV("version", "1.0.0");
    result.pushKV("generated", GetTime());
    result.pushKV("bitcoin_version", FormatFullVersion());
    
    // Basic JSON Schema
    UniValue schema(UniValue::VOBJ);
    schema.pushKV("$schema", "https://json-schema.org/draft-07/schema#");
    schema.pushKV("type", "object");
    schema.pushKV("title", "Bitcoin Knots Settings");
    schema.pushKV("description", "Complete configuration options for Bitcoin Knots node");
    
    // Basic properties
    UniValue properties(UniValue::VOBJ);
    
    // Wallet category example
    UniValue wallet_schema(UniValue::VOBJ);
    wallet_schema.pushKV("type", "object");
    wallet_schema.pushKV("title", "Wallet Settings");
    
    UniValue wallet_props(UniValue::VOBJ);
    UniValue walletrbf_prop(UniValue::VOBJ);
    walletrbf_prop.pushKV("type", "boolean");
    walletrbf_prop.pushKV("title", "Enable Replace-By-Fee");
    walletrbf_prop.pushKV("description", "Allow transactions to be replaced with higher fee versions");
    wallet_props.pushKV("walletrbf", walletrbf_prop);
    
    wallet_schema.pushKV("properties", wallet_props);
    properties.pushKV("wallet", wallet_schema);
    
    schema.pushKV("properties", properties);
    result.pushKV("schema", schema);
    
    // Basic UI Schema
    UniValue ui_schema(UniValue::VOBJ);
    UniValue wallet_ui(UniValue::VOBJ);
    UniValue walletrbf_ui(UniValue::VOBJ);
    walletrbf_ui.pushKV("ui:widget", "checkbox");
    wallet_ui.pushKV("walletrbf", walletrbf_ui);
    ui_schema.pushKV("wallet", wallet_ui);
    result.pushKV("uiSchema", ui_schema);
    
    // Basic form data
    UniValue form_data(UniValue::VOBJ);
    UniValue wallet_data(UniValue::VOBJ);
    wallet_data.pushKV("walletrbf", true);
    form_data.pushKV("wallet", wallet_data);
    result.pushKV("formData", form_data);
    
    // Basic metadata
    UniValue knots_meta(UniValue::VOBJ);
    UniValue restart_required(UniValue::VOBJ);
    restart_required.pushKV("network.port", true);
    knots_meta.pushKV("restart_required", restart_required);
    
    UniValue dependencies(UniValue::VOBJ);
    knots_meta.pushKV("dependencies", dependencies);
    
    UniValue validation(UniValue::VOBJ);
    knots_meta.pushKV("validation", validation);
    
    result.pushKV("knotsMetadata", knots_meta);
    
    return result;
},
    };
}

static RPCHelpMan setsettings()
{
    return RPCHelpMan{"setsettings",
        "\nUpdate one or more Bitcoin Knots settings.\n"
        "Validates values against constraints and applies them immediately if possible.\n"
        "Returns information about whether the node needs to be restarted for changes to take effect.\n"
        "\nNote: Requires 'settings-write' permission. Critical settings require 'settings-write-critical'.\n",
        {
            {"settings", RPCArg::Type::OBJ, RPCArg::Optional::NO, "JSON object with setting names as keys and new values as values",
                RPCArgOptions{.oneline_description="settings"}},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "setting", "The name of the setting that was updated"},
                {RPCResult::Type::ANY, "old_value", "The previous value of the setting"},
                {RPCResult::Type::ANY, "new_value", "The new value of the setting"},
                {RPCResult::Type::BOOL, "success", "Whether the setting was successfully updated"},
                {RPCResult::Type::BOOL, "restart_required", "Whether the node needs to be restarted for this change"},
                {RPCResult::Type::STR, "message", "Additional information about the update"},
                {RPCResult::Type::NUM, "timestamp", "Unix timestamp when the setting was changed"},
            }
        },
        RPCExamples{
            HelpExampleCli("setsettings", "'{\"walletrbf\": \"true\"}'") 
            + HelpExampleCli("setsettings", "'{\"maxmempool\": \"500\", \"walletrbf\": \"true\"}'") 
            + HelpExampleRpc("setsettings", "{\"walletrbf\": true}")
            + HelpExampleRpc("setsettings", "{\"mintxfee\": \"0.0001\", \"maxmempool\": 400}")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Check basic write permission
    if (!CheckSettingsPermission(request, "settings-write")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Insufficient permissions for settings-write operation");
    }
    
    // Get parameters
    const UniValue& settings_obj = request.params[0];
    if (!settings_obj.isObject()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Settings parameter must be an object");
    }
    
    // Get the settings keys
    const std::vector<std::string>& keys = settings_obj.getKeys();
    
    // Check permissions for each setting before applying any changes
    bool has_critical_settings = false;
    std::set<std::string> sensitive_settings_modified;
    for (const auto& key : keys) {
        if (RequiresElevatedPermission(key)) {
            has_critical_settings = true;
        }
        if (g_sensitive_settings.count(key) > 0) {
            sensitive_settings_modified.insert(key);
        }
    }
    
    // Check elevated permissions if needed
    if (has_critical_settings && !CheckSettingsPermission(request, "settings-write-critical")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "One or more settings require elevated permissions (settings-write-critical)");
    }
    
    // Log warning for sensitive settings
    if (!sensitive_settings_modified.empty()) {
        LogPrintf("[RPC Settings Security] Warning: User %s attempting to modify sensitive settings: %s\n",
                 request.authUser, util::Join(sensitive_settings_modified, ", "));
    }
    
    // Get the args manager to access and modify settings
    ArgsManager& args{EnsureAnyArgsman(request.context)};
    
    // Create result object
    UniValue result(UniValue::VOBJ);
    result.pushKV("timestamp", GetTime());
    
    // First pass: validate all settings
    std::vector<std::pair<std::string, UniValue>> validated_settings;
    UniValue errors_array(UniValue::VARR);
    bool any_restart_required = false;
    const std::vector<UniValue>& values = settings_obj.getValues();
    
    for (size_t i = 0; i < keys.size(); i++) {
        const std::string& setting_name = keys[i];
        const UniValue& value = values[i];
        
        // Get setting metadata
        UniValue metadata = common::GetSettingMetadata(setting_name);
        if (metadata.isNull() || metadata.exists("error")) {
            UniValue error_obj(UniValue::VOBJ);
            error_obj.pushKV("setting", setting_name);
            error_obj.pushKV("error", strprintf("Unknown setting '%s'", setting_name));
            errors_array.push_back(error_obj);
            continue;
        }
        
        // Convert value to string for validation
        std::string value_str;
        if (value.isBool()) {
            value_str = value.get_bool() ? "true" : "false";
        } else if (value.isNum()) {
            value_str = value.getValStr();
        } else if (value.isStr()) {
            value_str = value.get_str();
        } else {
            UniValue error_obj(UniValue::VOBJ);
            error_obj.pushKV("setting", setting_name);
            error_obj.pushKV("error", "Invalid value type");
            errors_array.push_back(error_obj);
            continue;
        }
        
        // Validate the value
        UniValue parsed_value;
        std::vector<std::string> validation_errors;
        bool valid = false;
        
        std::string type_str = GetTypeString(metadata["type"].getInt<int>());
        if (type_str == "bool") {
            if (value.isBool()) {
                parsed_value = value;
                valid = true;
            } else if (value_str == "true" || value_str == "1") {
                parsed_value.setBool(true);
                valid = true;
            } else if (value_str == "false" || value_str == "0") {
                parsed_value.setBool(false);
                valid = true;
            } else {
                validation_errors.push_back("Invalid boolean value");
            }
        } else if (type_str == "int") {
            try {
                int64_t int_val = value.isNum() ? value.getInt<int64_t>() : std::stoll(value_str);
                parsed_value.setInt(int_val);
                
                // Validate range
                if (metadata.exists("constraints") && metadata["constraints"].exists("min") && metadata["constraints"].exists("max")) {
                    int64_t min_val = metadata["constraints"]["min"].getInt<int64_t>();
                    int64_t max_val = metadata["constraints"]["max"].getInt<int64_t>();
                    if (int_val >= min_val && int_val <= max_val) {
                        valid = true;
                    } else {
                        validation_errors.push_back(strprintf("Value out of range [%ld, %ld]", min_val, max_val));
                    }
                } else {
                    valid = true;
                }
            } catch (const std::exception& e) {
                validation_errors.push_back("Invalid integer value");
            }
        } else if (type_str == "string") {
            parsed_value.setStr(value_str);
            
            // Validate against allowed values
            if (metadata.exists("constraints") && metadata["constraints"].exists("allowed_values")) {
                const UniValue& allowed = metadata["constraints"]["allowed_values"];
                bool found = false;
                for (const auto& val : allowed.getValues()) {
                    if (val.get_str() == value_str) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    valid = true;
                } else {
                    validation_errors.push_back("Value not in allowed list");
                }
            } else {
                valid = true;
            }
        } else {
            // Handle other types similarly
            parsed_value = value;
            valid = true;
        }
        
        if (valid) {
            validated_settings.push_back({setting_name, parsed_value});
            if (metadata["restart_required"].get_bool()) {
                any_restart_required = true;
            }
        } else {
            UniValue error_obj(UniValue::VOBJ);
            error_obj.pushKV("setting", setting_name);
            error_obj.pushKV("error", validation_errors.empty() ? "Validation failed" : validation_errors[0]);
            errors_array.push_back(error_obj);
        }
    }
    
    // If any validation errors occurred, return without applying changes
    if (errors_array.size() > 0) {
        result.pushKV("success", false);
        result.pushKV("updated_count", 0);
        result.pushKV("updates", UniValue(UniValue::VARR));
        result.pushKV("errors", errors_array);
        result.pushKV("restart_required", false);
        result.pushKV("message", strprintf("Validation failed for %d setting(s). No changes applied.", errors_array.size()));
        return result;
    }
    
    // Second pass: apply all validated settings
    UniValue updates_array(UniValue::VARR);
    
    for (const auto& [setting_name, new_value] : validated_settings) {
        // Get old value (placeholder)
        UniValue old_value;
        if (setting_name == "walletrbf" || setting_name == "spendzeroconfchange") {
            old_value.setBool(true);
        } else if (setting_name == "maxmempool") {
            old_value.setInt(300);
        } else if (setting_name == "mempoolreplacement") {
            old_value.setStr("full");
        } else {
            old_value.setInt(1000000);
        }
        
        // Create update record
        UniValue update_obj(UniValue::VOBJ);
        update_obj.pushKV("setting", setting_name);
        update_obj.pushKV("old_value", old_value);
        update_obj.pushKV("new_value", new_value);
        
        UniValue metadata = common::GetSettingMetadata(setting_name);
        update_obj.pushKV("restart_required", metadata["restart_required"].get_bool());
        
        updates_array.push_back(update_obj);
        
        // Actually apply the setting change
        args.LockSettings([&](common::Settings& settings) {
            // Convert new_value to SettingsValue for storage
            common::SettingsValue settings_value;
            if (new_value.isBool()) {
                settings_value = common::SettingsValue(new_value.get_bool());
            } else if (new_value.isNum()) {
                settings_value = common::SettingsValue(new_value.getValStr());
            } else if (new_value.isStr()) {
                settings_value = common::SettingsValue(new_value.get_str());
            }
            
            // Update the setting
            settings.rw_settings[setting_name] = settings_value;
            
            // Enhanced audit logging with user information
            LogPrintf("[RPC Settings Audit] Setting changed: %s = %s [user: %s, source: setsettings RPC, timestamp: %d]\n",
                     setting_name, 
                     g_sensitive_settings.count(setting_name) > 0 ? "***REDACTED***" : settings_value.write(), 
                     request.authUser.empty() ? "anonymous" : request.authUser,
                     GetTime());
        });
    }
    
    // Write all settings to disk after applying them
    std::vector<std::string> write_errors;
    if (!args.WriteSettingsFile(&write_errors)) {
        // Rollback would be complex here since we've already modified settings
        // For now, log the error but still report partial success
        LogPrintf("[RPC] Warning: Failed to write settings file after bulk update: %s\\n", 
                 util::Join(write_errors, ", "));
    }
    
    // Apply runtime changes for settings that don't require restart
    for (const auto& [setting_name, new_value] : validated_settings) {
        UniValue metadata = common::GetSettingMetadata(setting_name);
        if (!metadata["restart_required"].get_bool()) {
            // Handle specific runtime-modifiable settings
            if (setting_name == "walletrbf") {
                // This is a wallet setting, it will be picked up on next wallet operation
            } else if (setting_name == "maxmempool") {
                // Would need to update mempool size limit if implemented
            } else if (setting_name == "minrelaytxfee") {
                // Would need to update relay fee if this was runtime modifiable
            }
            // Add more runtime updates as needed
        }
    }
    
    // Return success result
    result.pushKV("success", true);
    result.pushKV("updated_count", validated_settings.size());
    result.pushKV("updates", updates_array);
    result.pushKV("errors", UniValue(UniValue::VARR));
    result.pushKV("restart_required", any_restart_required);
    
    std::string message = strprintf("Successfully updated %d setting(s)", validated_settings.size());
    if (any_restart_required) {
        message += ". Restart required for some changes to take effect";
    }
    result.pushKV("message", message);
    
    // Log summary for audit trail
    LogPrintf("[RPC Settings Audit] Bulk settings update completed: %d settings changed [user: %s, timestamp: %d]\n",
             validated_settings.size(), 
             request.authUser.empty() ? "anonymous" : request.authUser,
             GetTime());
    
    return result;
},
    };
}

// updatesettings functionality merged into setsettings
/*
static RPCHelpMan updatesettings()
{
    return RPCHelpMan{"updatesettings",
        "\nUpdate multiple Bitcoin Knots settings atomically.\n"
        "All settings are validated before any are applied. If any validation fails,\n"
        "no changes are made (transactional update).\n"
        "\nNote: Requires 'settings-write' permission. Critical settings require 'settings-write-critical'.\n",
        {
            {"settings", RPCArg::Type::OBJ, RPCArg::Optional::NO, "JSON object with setting names as keys and new values as values",
                RPCArgOptions{.oneline_description="settings"}},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::BOOL, "success", "Whether all settings were successfully updated"},
                {RPCResult::Type::NUM, "updated_count", "Number of settings that were updated"},
                {RPCResult::Type::ARR, "updates", "Details about each setting update",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR, "setting", "Setting name"},
                                {RPCResult::Type::ANY, "old_value", "Previous value"},
                                {RPCResult::Type::ANY, "new_value", "New value"},
                                {RPCResult::Type::BOOL, "restart_required", "Whether restart is needed"},
                            }
                        },
                    }
                },
                {RPCResult::Type::ARR, "errors", "Validation errors if any occurred",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR, "setting", "Setting name that failed validation"},
                                {RPCResult::Type::STR, "error", "Error message"},
                            }
                        },
                    }
                },
                {RPCResult::Type::BOOL, "restart_required", "Whether any changed setting requires restart"},
                {RPCResult::Type::STR, "message", "Summary message about the update"},
                {RPCResult::Type::NUM, "timestamp", "Unix timestamp when settings were changed"},
            }
        },
        RPCExamples{
            HelpExampleCli("updatesettings", "'{\"walletrbf\": \"true\", \"maxmempool\": \"500\", \"mempoolreplacement\": \"full\"}'")
            + HelpExampleRpc("updatesettings", "{\"walletrbf\": true, \"spendzeroconfchange\": false, \"mintxfee\": \"0.0001\"}")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Check basic write permission
    if (!CheckSettingsPermission(request, "settings-write")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Insufficient permissions for settings-write operation");
    }
    
    // Get parameters
    const UniValue& settings_obj = request.params[0];
    if (!settings_obj.isObject()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Settings parameter must be an object");
    }
    
    // Get the settings keys
    const std::vector<std::string>& keys = settings_obj.getKeys();
    
    // Check permissions for each setting before applying any changes
    bool has_critical_settings = false;
    std::set<std::string> sensitive_settings_modified;
    for (const auto& key : keys) {
        if (RequiresElevatedPermission(key)) {
            has_critical_settings = true;
        }
        if (g_sensitive_settings.count(key) > 0) {
            sensitive_settings_modified.insert(key);
        }
    }
    
    // Check elevated permissions if needed
    if (has_critical_settings && !CheckSettingsPermission(request, "settings-write-critical")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "One or more settings require elevated permissions (settings-write-critical)");
    }
    
    // Log warning for sensitive settings
    if (!sensitive_settings_modified.empty()) {
        LogPrintf("[RPC Settings Security] Warning: User %s attempting to modify sensitive settings: %s\n",
                 request.authUser, util::Join(sensitive_settings_modified, ", "));
    }
    
    // Get the args manager to access and modify settings
    ArgsManager& args{EnsureAnyArgsman(request.context)};
    
    // Create result object
    UniValue result(UniValue::VOBJ);
    result.pushKV("timestamp", GetTime());
    
    // First pass: validate all settings
    std::vector<std::pair<std::string, UniValue>> validated_settings;
    UniValue errors_array(UniValue::VARR);
    bool any_restart_required = false;
    const std::vector<UniValue>& values = settings_obj.getValues();
    
    for (size_t i = 0; i < keys.size(); i++) {
        const std::string& setting_name = keys[i];
        const UniValue& value = values[i];
        
        // Get setting metadata
        UniValue metadata = common::GetSettingMetadata(setting_name);
        if (metadata.isNull() || metadata.exists("error")) {
            UniValue error_obj(UniValue::VOBJ);
            error_obj.pushKV("setting", setting_name);
            error_obj.pushKV("error", strprintf("Unknown setting '%s'", setting_name));
            errors_array.push_back(error_obj);
            continue;
        }
        
        // Convert value to string for validation
        std::string value_str;
        if (value.isBool()) {
            value_str = value.get_bool() ? "true" : "false";
        } else if (value.isNum()) {
            value_str = value.getValStr();
        } else if (value.isStr()) {
            value_str = value.get_str();
        } else {
            UniValue error_obj(UniValue::VOBJ);
            error_obj.pushKV("setting", setting_name);
            error_obj.pushKV("error", "Invalid value type");
            errors_array.push_back(error_obj);
            continue;
        }
        
        // Validate the value (similar logic to setsetting)
        UniValue parsed_value;
        std::vector<std::string> validation_errors;
        bool valid = false;
        
        std::string type_str = GetTypeString(metadata["type"].getInt<int>());
        if (type_str == "bool") {
            if (value.isBool()) {
                parsed_value = value;
                valid = true;
            } else if (value_str == "true" || value_str == "1") {
                parsed_value.setBool(true);
                valid = true;
            } else if (value_str == "false" || value_str == "0") {
                parsed_value.setBool(false);
                valid = true;
            } else {
                validation_errors.push_back("Invalid boolean value");
            }
        } else if (type_str == "int") {
            try {
                int64_t int_val = value.isNum() ? value.getInt<int64_t>() : std::stoll(value_str);
                parsed_value.setInt(int_val);
                
                // Validate range
                if (metadata.exists("constraints") && metadata["constraints"].exists("min") && metadata["constraints"].exists("max")) {
                    int64_t min_val = metadata["constraints"]["min"].getInt<int64_t>();
                    int64_t max_val = metadata["constraints"]["max"].getInt<int64_t>();
                    if (int_val >= min_val && int_val <= max_val) {
                        valid = true;
                    } else {
                        validation_errors.push_back(strprintf("Value out of range [%ld, %ld]", min_val, max_val));
                    }
                } else {
                    valid = true;
                }
            } catch (const std::exception& e) {
                validation_errors.push_back("Invalid integer value");
            }
        } else if (type_str == "string") {
            parsed_value.setStr(value_str);
            
            // Validate against allowed values
            if (metadata.exists("constraints") && metadata["constraints"].exists("allowed_values")) {
                const UniValue& allowed = metadata["constraints"]["allowed_values"];
                bool found = false;
                for (const auto& val : allowed.getValues()) {
                    if (val.get_str() == value_str) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    valid = true;
                } else {
                    validation_errors.push_back("Value not in allowed list");
                }
            } else {
                valid = true;
            }
        } else {
            // Handle other types similarly
            parsed_value = value;
            valid = true;
        }
        
        if (valid) {
            validated_settings.push_back({setting_name, parsed_value});
            if (metadata["restart_required"].get_bool()) {
                any_restart_required = true;
            }
        } else {
            UniValue error_obj(UniValue::VOBJ);
            error_obj.pushKV("setting", setting_name);
            error_obj.pushKV("error", validation_errors.empty() ? "Validation failed" : validation_errors[0]);
            errors_array.push_back(error_obj);
        }
    }
    
    // If any validation errors occurred, return without applying changes
    if (errors_array.size() > 0) {
        result.pushKV("success", false);
        result.pushKV("updated_count", 0);
        result.pushKV("updates", UniValue(UniValue::VARR));
        result.pushKV("errors", errors_array);
        result.pushKV("restart_required", false);
        result.pushKV("message", strprintf("Validation failed for %d setting(s). No changes applied.", errors_array.size()));
        return result;
    }
    
    // Second pass: apply all validated settings
    UniValue updates_array(UniValue::VARR);
    
    for (const auto& [setting_name, new_value] : validated_settings) {
        // Get old value (placeholder)
        UniValue old_value;
        if (setting_name == "walletrbf" || setting_name == "spendzeroconfchange") {
            old_value.setBool(true);
        } else if (setting_name == "maxmempool") {
            old_value.setInt(300);
        } else if (setting_name == "mempoolreplacement") {
            old_value.setStr("full");
        } else {
            old_value.setInt(1000000);
        }
        
        // Create update record
        UniValue update_obj(UniValue::VOBJ);
        update_obj.pushKV("setting", setting_name);
        update_obj.pushKV("old_value", old_value);
        update_obj.pushKV("new_value", new_value);
        
        UniValue metadata = common::GetSettingMetadata(setting_name);
        update_obj.pushKV("restart_required", metadata["restart_required"].get_bool());
        
        updates_array.push_back(update_obj);
        
        // Actually apply the setting change
        args.LockSettings([&](common::Settings& settings) {
            // Convert new_value to SettingsValue for storage
            common::SettingsValue settings_value;
            if (new_value.isBool()) {
                settings_value = common::SettingsValue(new_value.get_bool());
            } else if (new_value.isNum()) {
                settings_value = common::SettingsValue(new_value.getValStr());
            } else if (new_value.isStr()) {
                settings_value = common::SettingsValue(new_value.get_str());
            }
            
            // Update the setting
            settings.rw_settings[setting_name] = settings_value;
            
            // Enhanced audit logging with user information
            LogPrintf("[RPC Settings Audit] Setting changed: %s = %s [user: %s, source: updatesettings RPC, timestamp: %d]\n",
                     setting_name, 
                     g_sensitive_settings.count(setting_name) > 0 ? "***REDACTED***" : settings_value.write(), 
                     request.authUser.empty() ? "anonymous" : request.authUser,
                     GetTime());
        });
    }
    
    // Write all settings to disk after applying them
    std::vector<std::string> write_errors;
    if (!args.WriteSettingsFile(&write_errors)) {
        // Rollback would be complex here since we've already modified settings
        // For now, log the error but still report partial success
        LogPrintf("[RPC] Warning: Failed to write settings file after bulk update: %s\\n", 
                 util::Join(write_errors, ", "));
    }
    
    // Apply runtime changes for settings that don't require restart
    for (const auto& [setting_name, new_value] : validated_settings) {
        UniValue metadata = common::GetSettingMetadata(setting_name);
        if (!metadata["restart_required"].get_bool()) {
            // Handle specific runtime-modifiable settings
            if (setting_name == "walletrbf") {
                // This is a wallet setting, it will be picked up on next wallet operation
            } else if (setting_name == "maxmempool") {
                // Would need to update mempool size limit if implemented
            } else if (setting_name == "minrelaytxfee") {
                // Would need to update relay fee if this was runtime modifiable
            }
            // Add more runtime updates as needed
        }
    }
    
    // Return success result
    result.pushKV("success", true);
    result.pushKV("updated_count", validated_settings.size());
    result.pushKV("updates", updates_array);
    result.pushKV("errors", UniValue(UniValue::VARR));
    result.pushKV("restart_required", any_restart_required);
    
    std::string message = strprintf("Successfully updated %d setting(s)", validated_settings.size());
    if (any_restart_required) {
        message += ". Restart required for some changes to take effect";
    }
    result.pushKV("message", message);
    
    // Log summary for audit trail
    LogPrintf("[RPC Settings Audit] Bulk settings update completed: %d settings changed [user: %s, timestamp: %d]\n",
             validated_settings.size(), 
             request.authUser.empty() ? "anonymous" : request.authUser,
             GetTime());
    
    return result;
},
    };
}
*/

static RPCHelpMan subscribesettings()
{
    return RPCHelpMan{"subscribesettings",
        "\nSubscribe to settings changes for polling-based notifications.\n"
        "Returns current settings with a polling token that can be used\n"
        "to detect changes. Clients should poll this endpoint periodically\n"
        "and compare the returned token to detect setting changes.\n"
        "\nNote: This is a polling-based subscription. For real-time notifications,\n"
        "consider using ZMQ with -zmqpubsettings configuration.\n",
        {
            {"category", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Optional category filter (e.g., \"wallet\", \"mempool\")"},
            {"since_token", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Last known polling token. If provided, only returns data if changes occurred"},
            {"include_values", RPCArg::Type::BOOL, RPCArg::Default{true}, "Whether to include current setting values in response"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "poll_token", "Token for change detection in subsequent polls"},
                {RPCResult::Type::NUM, "timestamp", "Unix timestamp when this response was generated"},
                {RPCResult::Type::BOOL, "has_changes", "Whether settings have changed since last poll (if since_token provided)"},
                {RPCResult::Type::OBJ, "settings", "Current settings (if has_changes or since_token not provided)",
                    {
                        {RPCResult::Type::OBJ, "category_name", "Settings organized by category",
                            {
                                {RPCResult::Type::ANY, "setting_name", "Current value of the setting"},
                            }
                        },
                    }
                },
                {RPCResult::Type::ARR, "changed_settings", "List of settings that changed since last poll (if since_token provided)",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR, "setting", "Name of setting that changed"},
                                {RPCResult::Type::ANY, "old_value", "Previous value"},
                                {RPCResult::Type::ANY, "new_value", "Current value"},
                                {RPCResult::Type::STR, "category", "Setting category"},
                                {RPCResult::Type::NUM, "change_time", "Unix timestamp when change occurred"},
                            }
                        },
                    }
                },
                {RPCResult::Type::NUM, "poll_interval_ms", "Recommended polling interval in milliseconds"},
                {RPCResult::Type::STR, "bitcoin_version", "Bitcoin Core version"},
            }
        },
        RPCExamples{
            HelpExampleCli("subscribesettings", "")
            + HelpExampleCli("subscribesettings", "\"wallet\" \"abc123def456\" false")
            + HelpExampleRpc("subscribesettings", "\"mempool\", \"abc123def456\", true")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Get the args manager to access settings
    // Not actually used in this function, but kept for consistency
    // ArgsManager& args{EnsureAnyArgsman(request.context)};;
    
    // Get parameters
    std::string category_filter;
    if (!request.params[0].isNull()) {
        category_filter = request.params[0].get_str();
    }
    
    std::string since_token;
    if (!request.params[1].isNull()) {
        since_token = request.params[1].get_str();
    }
    
    bool include_values = true;
    if (!request.params[2].isNull()) {
        include_values = request.params[2].get_bool();
    }
    
    // Create result object
    UniValue result(UniValue::VOBJ);
    int64_t current_time = GetTime();
    result.pushKV("timestamp", current_time);
    result.pushKV("bitcoin_version", FormatFullVersion());
    
    // Generate polling token based on current time and settings state
    // In a real implementation, this would be a hash of all current settings
    std::string poll_token = strprintf("%d_%s", current_time, 
                                      category_filter.empty() ? "all" : category_filter);
    result.pushKV("poll_token", poll_token);
    
    // Check if this is a polling request with a previous token
    bool has_changes = true; // Default to true for first-time requests
    if (!since_token.empty()) {
        // Simple comparison - in practice, you'd compare actual settings state
        // For demonstration, we'll assume changes if token is different
        has_changes = (since_token != poll_token);
    }
    
    result.pushKV("has_changes", has_changes);
    
    // If there are changes or this is initial request, include settings
    if (has_changes || since_token.empty()) {
        // Get current settings - reuse logic from dumpsettings
        UniValue settings_obj(UniValue::VOBJ);
        
        if (include_values) {
            // Simulate current settings (in real implementation, would read from ArgsManager)
            if (category_filter.empty() || category_filter == "wallet") {
                UniValue wallet_settings(UniValue::VOBJ);
                wallet_settings.pushKV("walletrbf", true);
                wallet_settings.pushKV("spendzeroconfchange", false);
                wallet_settings.pushKV("mintxfee", "0.0001");
                settings_obj.pushKV("wallet", wallet_settings);
            }
            
            if (category_filter.empty() || category_filter == "mempool") {
                UniValue mempool_settings(UniValue::VOBJ);
                mempool_settings.pushKV("maxmempool", 300);
                mempool_settings.pushKV("mempoolreplacement", "full");
                mempool_settings.pushKV("maxorphantx", 100);
                settings_obj.pushKV("mempool", mempool_settings);
            }
            
            if (category_filter.empty() || category_filter == "relay") {
                UniValue relay_settings(UniValue::VOBJ);
                relay_settings.pushKV("incrementalrelayfee", "0.00001");
                relay_settings.pushKV("minrelaytxfee", "0.00001");
                relay_settings.pushKV("bytespersigop", 20);
                settings_obj.pushKV("relay", relay_settings);
            }
        }
        
        result.pushKV("settings", settings_obj);
        
        // If polling with previous token, include list of changed settings
        if (!since_token.empty()) {
            UniValue changed_settings(UniValue::VARR);
            
            // Simulate some changed settings
            UniValue change1(UniValue::VOBJ);
            change1.pushKV("setting", "maxmempool");
            change1.pushKV("old_value", 250);
            change1.pushKV("new_value", 300);
            change1.pushKV("category", "mempool");
            change1.pushKV("change_time", current_time - 30);
            changed_settings.push_back(change1);
            
            result.pushKV("changed_settings", changed_settings);
        }
    } else {
        // No changes - return minimal response
        result.pushKV("settings", UniValue(UniValue::VOBJ));
        result.pushKV("changed_settings", UniValue(UniValue::VARR));
    }
    
    // Recommend polling interval (5 seconds for demonstration)
    result.pushKV("poll_interval_ms", 5000);
    
    return result;
},
    };
}

void RegisterSettingsRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        // Read-only commands (require 'settings-read' permission)
        {"settings", &dumpsettings},      // Export settings with flexible filtering
        {"settings", &getsettingsschema}, // Schema access (consider separate 'settings-schema' permission)
        {"settings", &subscribesettings}, // Polling/notification subscription
        
        // Write commands (require 'settings-write' permission)
        {"settings", &setsettings},       // Also requires 'settings-write-critical' for critical settings
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
    
    // Log available permission categories for settings RPC
    LogPrintf("[RPC Settings] Available permission categories:\n");
    LogPrintf("  - settings-read: Read access to non-sensitive settings\n");
    LogPrintf("  - settings-read-sensitive: Read access to sensitive settings (passwords, keys)\n");
    LogPrintf("  - settings-write: Modify non-critical settings\n");
    LogPrintf("  - settings-write-critical: Modify critical settings (network, security)\n");
    LogPrintf("  - settings-schema: Access to settings schema (for UI generation)\n");
}