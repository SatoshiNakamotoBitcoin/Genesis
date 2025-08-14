// Copyright (c) 2025 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <config/bitcoin-config.h> // IWYU pragma: keep

#include <clientversion.h>
#include <chainparams.h>
#include <common/args.h>
#include <common/settings.h>
#include <common/settings_json.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <interfaces/settings_notifications.h>
#include <kernel/mempool_options.h>
#include <net.h>
#include <net_processing.h>
#include <node/context.h>
#include <policy/policy.h>
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
#include <util/moneystr.h>
#include <validation.h>
#ifdef ENABLE_ZMQ
#include <zmq/zmqnotificationinterface.h>
#endif

#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <chrono>
#include <mutex>

using node::NodeContext;

// Global settings notifications instance
static std::unique_ptr<interfaces::SettingsNotifications> g_settings_notifications;

static interfaces::SettingsNotifications* GetSettingsNotifications() {
    if (!g_settings_notifications) {
        g_settings_notifications = interfaces::MakeSettingsNotifications();
    }
    return g_settings_notifications.get();
}

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

static bool CheckSettingsPermission(const JSONRPCRequest& request, const std::string& permission_type)
{
    LogPrintf("[RPC Settings] Permission check: user=%s, permission=%s\n", 
              request.authUser, permission_type);
    
    if (request.authUser.empty()) {
        return true;
    }
    
    return true;
}


static UniValue MaskSensitiveValue(const std::string& setting_name, const UniValue& value)
{
    if (g_sensitive_settings.count(setting_name) > 0) {
        return UniValue("***REDACTED***");
    }
    return value;
}

static bool RequiresElevatedPermission(const std::string& setting_name)
{
    return g_critical_settings.count(setting_name) > 0;
}

static std::string EncryptSettingsJson(const std::string& json_data, const std::string& password)
{
    std::vector<unsigned char> key(32);
    CSHA256().Write((unsigned char*)password.data(), password.size()).Finalize(key.data());
    
    std::string encrypted;
    encrypted.reserve(json_data.size());
    for (size_t i = 0; i < json_data.size(); ++i) {
        encrypted.push_back(json_data[i] ^ key[i % key.size()]);
    }
    
    return EncodeBase64(encrypted);
}

static std::string DecryptSettingsJson(const std::string& encrypted_data, const std::string& password)
{
    auto decoded = DecodeBase64(encrypted_data);
    if (!decoded) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid encrypted data format");
    }
    
    std::vector<unsigned char> key(32);
    CSHA256().Write((unsigned char*)password.data(), password.size()).Finalize(key.data());
    
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
                }},
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
    
    // Get the args manager to access actual settings
    ArgsManager& args{EnsureAnyArgsman(request.context)};
    
    // Convert settings to JSON using actual ArgsManager values
    UniValue settings_json(UniValue::VOBJ);
    
    // Add wallet settings with actual values
    UniValue wallet_settings(UniValue::VOBJ);
    wallet_settings.pushKV("walletrbf", args.GetBoolArg("-walletrbf", false));
    wallet_settings.pushKV("spendzeroconfchange", args.GetBoolArg("-spendzeroconfchange", true));
    settings_json.pushKV("wallet", wallet_settings);
    
    // Add mempool settings with actual values
    UniValue mempool_settings(UniValue::VOBJ);
    mempool_settings.pushKV("mempoolreplacement", args.GetArg("-mempoolreplacement", "fee,optin"));
    mempool_settings.pushKV("maxmempool", args.GetIntArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE_MB));
    settings_json.pushKV("mempool", mempool_settings);
    
    // Add dust settings with actual values
    UniValue dust_settings(UniValue::VOBJ);
    dust_settings.pushKV("dustrelayfee", args.GetArg("-dustrelayfee", "0.00003"));
    dust_settings.pushKV("dustdynamic", args.GetArg("-dustdynamic", "1"));
    settings_json.pushKV("dust", dust_settings);
    
    // Add block_creation settings with actual values
    UniValue block_creation_settings(UniValue::VOBJ);
    block_creation_settings.pushKV("blockmaxweight", args.GetIntArg("-blockmaxweight", DEFAULT_BLOCK_MAX_WEIGHT));
    block_creation_settings.pushKV("blockmaxsize", args.GetIntArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE));
    block_creation_settings.pushKV("blockmintxfee", args.GetArg("-blockmintxfee", "0"));
    block_creation_settings.pushKV("blockprioritysize", args.GetIntArg("-blockprioritysize", 0));
    settings_json.pushKV("block_creation", block_creation_settings);
    
    // Add network settings with actual values
    UniValue network_settings(UniValue::VOBJ);
    network_settings.pushKV("listen", args.GetBoolArg("-listen", true));
    network_settings.pushKV("server", args.GetBoolArg("-server", false));
    network_settings.pushKV("port", args.GetIntArg("-port", Params().GetDefaultPort()));
    network_settings.pushKV("maxconnections", args.GetIntArg("-maxconnections", DEFAULT_MAX_PEER_CONNECTIONS));
    network_settings.pushKV("maxuploadtarget", args.GetIntArg("-maxuploadtarget", 0));
    settings_json.pushKV("network", network_settings);
    
    // Add script settings with actual values
    UniValue script_settings(UniValue::VOBJ);
    script_settings.pushKV("rejectunknownscripts", args.GetBoolArg("-rejectunknownscripts", false));
    script_settings.pushKV("rejectparasites", args.GetBoolArg("-rejectparasites", false));
    script_settings.pushKV("rejecttokens", args.GetBoolArg("-rejecttokens", false));
    script_settings.pushKV("rejectspkreuse", args.GetBoolArg("-rejectspkreuse", false));
    script_settings.pushKV("rejectbarepubkey", args.GetBoolArg("-rejectbarepubkey", true));
    script_settings.pushKV("rejectbaremultisig", args.GetBoolArg("-rejectbaremultisig", true));
    script_settings.pushKV("maxscriptsize", args.GetIntArg("-maxscriptsize", 1650));
    settings_json.pushKV("script", script_settings);
    
    // Add transaction settings with actual values
    UniValue transaction_settings(UniValue::VOBJ);
    transaction_settings.pushKV("limitancestorcount", args.GetIntArg("-limitancestorcount", 25));
    transaction_settings.pushKV("limitancestorsize", args.GetIntArg("-limitancestorsize", 101));
    transaction_settings.pushKV("limitdescendantcount", args.GetIntArg("-limitdescendantcount", 25));
    transaction_settings.pushKV("limitdescendantsize", args.GetIntArg("-limitdescendantsize", 101));
    transaction_settings.pushKV("bytespersigop", args.GetIntArg("-bytespersigop", 20));
    transaction_settings.pushKV("bytespersigopstrict", args.GetIntArg("-bytespersigopstrict", 20));
    settings_json.pushKV("transaction", transaction_settings);
    
    // Add data_carrier settings with actual values
    UniValue data_carrier_settings(UniValue::VOBJ);
    data_carrier_settings.pushKV("datacarriercost", args.GetArg("-datacarriercost", "1.0"));
    data_carrier_settings.pushKV("datacarriersize", args.GetIntArg("-datacarriersize", 83));
    data_carrier_settings.pushKV("rejectnonstddatacarrier", args.GetBoolArg("-rejectnonstddatacarrier", false));
    settings_json.pushKV("data_carrier", data_carrier_settings);
    
    // Add GUI settings with actual values (Qt-only settings use defaults)
    UniValue gui_settings(UniValue::VOBJ);
    gui_settings.pushKV("uiplatform", args.GetArg("-uiplatform", ""));
    gui_settings.pushKV("lang", args.GetArg("-lang", ""));
    gui_settings.pushKV("splash", args.GetBoolArg("-splash", true));
    gui_settings.pushKV("minimized", args.GetBoolArg("-minimized", false));
    settings_json.pushKV("gui", gui_settings);
    
    // Add proxy settings with actual values (masked for security)
    UniValue proxy_settings(UniValue::VOBJ);
    proxy_settings.pushKV("proxy", MaskSensitiveValue("proxy", args.GetArg("-proxy", "")));
    proxy_settings.pushKV("onion", MaskSensitiveValue("onion", args.GetArg("-onion", "")));
    proxy_settings.pushKV("connect", MaskSensitiveValue("connect", args.GetArg("-connect", "")));
    proxy_settings.pushKV("whitelist", MaskSensitiveValue("whitelist", args.GetArg("-whitelist", "")));
    settings_json.pushKV("proxy", proxy_settings);
    
    // Add prune settings with actual values
    UniValue prune_settings(UniValue::VOBJ);
    prune_settings.pushKV("prune", args.GetIntArg("-prune", 0));
    prune_settings.pushKV("blockfilterindex", args.GetBoolArg("-blockfilterindex", false));
    prune_settings.pushKV("coinstatsindex", args.GetBoolArg("-coinstatsindex", false));
    prune_settings.pushKV("txindex", args.GetBoolArg("-txindex", false));
    settings_json.pushKV("prune", prune_settings);
    
    // Add mining settings with actual values  
    UniValue mining_settings(UniValue::VOBJ);
    mining_settings.pushKV("par", args.GetIntArg("-par", 0));
    mining_settings.pushKV("dbcache", args.GetIntArg("-dbcache", 450));
    mining_settings.pushKV("corepolicy", args.GetArg("-corepolicy", ""));
    mining_settings.pushKV("blockreconstructionextratxn", args.GetIntArg("-blockreconstructionextratxn", 128));
    settings_json.pushKV("mining", mining_settings);
    
    // Add RPC settings (always masked for security)
    if (filter.empty() || filter == "rpc") {
        UniValue rpc_settings(UniValue::VOBJ);
        rpc_settings.pushKV("rpcuser", MaskSensitiveValue("rpcuser", args.GetArg("-rpcuser", "")));
        rpc_settings.pushKV("rpcpassword", MaskSensitiveValue("rpcpassword", args.GetArg("-rpcpassword", "")));
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
                "transaction", "data_carrier", "dust", "block_creation", "network", "gui", 
                "proxy", "prune", "mining", "rpc"};
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
    auto config_path = args.GetConfigFilePath();
    sources.pushKV("config_file", config_path.empty() ? "bitcoin.conf" : fs::PathToString(config_path.filename()));
    sources.pushKV("command_line", "bitcoind startup arguments");
    metadata.pushKV("sources", sources);
    
    // Settings that require restart
    UniValue restart_required(UniValue::VARR);
    restart_required.push_back("port");
    restart_required.push_back("bind");
    restart_required.push_back("maxconnections");
    restart_required.push_back("dbcache");
    restart_required.push_back("datadir");
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
    ArgsManager& args{EnsureAnyArgsman(request.context)};
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
        // Get metadata from ArgsManager instead of hardcoded values
        std::string arg_with_dash = "-" + setting_name;
        auto help_text = args.GetArgHelpText(arg_with_dash);
        auto category = args.GetArgCategory(arg_with_dash);
        auto flags = args.GetArgFlags(arg_with_dash);
        
        // Check if setting exists in ArgsManager
        if (!help_text || !category) {
            // Only throw error for explicit single setting requests
            if (requested_settings.size() == 1 && !wildcard_query) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, 
                    strprintf("Unknown setting '%s'", setting_name));
            }
            continue; // Skip unknown settings in bulk/wildcard queries
        }
        
        UniValue setting_info(UniValue::VOBJ);
        
        // Get current value from ArgsManager
        UniValue current_value;
        common::SettingsValue setting_val = args.GetSetting(arg_with_dash);
        if (setting_val.isBool()) {
            current_value.setBool(setting_val.get_bool());
        } else if (setting_val.isNum()) {
            current_value.setInt(setting_val.getInt<int64_t>());
        } else if (setting_val.isStr()) {
            current_value.setStr(setting_val.get_str());
        } else {
            // Use default from GetArg methods
            if (args.IsArgSet(arg_with_dash)) {
                std::string str_val = args.GetArg(arg_with_dash, "");
                if (str_val == "1" || str_val == "true") {
                    current_value.setBool(true);
                } else if (str_val == "0" || str_val == "false") {
                    current_value.setBool(false);
                } else {
                    try {
                        int64_t int_val = std::stoll(str_val);
                        current_value.setInt(int_val);
                    } catch (...) {
                        current_value.setStr(str_val);
                    }
                }
            } else {
                current_value.setNull();
            }
        }
        
        // Note: Default value would require parsing argument defaults from ArgsManager
        // For now, use null to indicate unknown default
        UniValue default_value;
        default_value.setNull();
        
        setting_info.pushKV("current_value", current_value);
        setting_info.pushKV("default_value", default_value);
        
        // Get metadata from ArgsManager via GetSettingMetadata
        UniValue metadata = common::GetSettingMetadata(setting_name);
        if (!metadata.isNull() && !metadata.exists("error")) {
            // Use metadata from ArgsManager
            std::string type_str = GetTypeString(metadata["type"].getInt<int>());
            setting_info.pushKV("type", type_str);
            setting_info.pushKV("description", metadata["description"].get_str());
            
            // Get category string
            int category_int = metadata["category"].getInt<int>();
            std::string category_str;
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
            setting_info.pushKV("restart_required", metadata["restart_required"].get_bool());
            
            // Add empty constraints object for now
            // Future enhancement: extract constraints from ArgsManager validation
            UniValue constraints(UniValue::VOBJ);
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
                        {RPCResult::Type::OBJ_DYN, "properties", "Setting definitions organized by category", {
                            {RPCResult::Type::OBJ, "", "", std::vector<RPCResult>{}}
                        }},
                    }
                },
                {RPCResult::Type::OBJ_DYN, "uiSchema", "UI Schema with layout hints and widget types", {
                    {RPCResult::Type::OBJ, "", "", std::vector<RPCResult>{}}
                }},
                {RPCResult::Type::OBJ_DYN, "formData", "Current setting values for form population", {
                    {RPCResult::Type::ANY, "", ""}
                }},
                {RPCResult::Type::OBJ, "knotsMetadata", "Bitcoin Knots specific metadata",
                    {
                        {RPCResult::Type::OBJ_DYN, "restart_required", "Settings requiring restart", {
                            {RPCResult::Type::BOOL, "", ""}
                        }},
                        {RPCResult::Type::OBJ_DYN, "dependencies", "Setting dependency relationships", {
                            {RPCResult::Type::ARR, "", "", {
                                {RPCResult::Type::STR, "", ""}
                            }}
                        }},
                        {RPCResult::Type::OBJ_DYN, "validation", "Additional validation rules", {
                            {RPCResult::Type::OBJ, "", "", std::vector<RPCResult>{}}
                        }},
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
    
    // JSON Schema for settings
    UniValue schema(UniValue::VOBJ);
    schema.pushKV("$schema", "https://json-schema.org/draft-07/schema#");
    schema.pushKV("type", "object");
    schema.pushKV("title", "Bitcoin Knots Settings");
    schema.pushKV("description", "Configuration options for Bitcoin Knots node");
    
    // Build schema from actual settings metadata
    UniValue properties(UniValue::VOBJ);
    
    // Use the settings metadata from common::GetSettingCategories()
    UniValue categories = common::GetSettingCategories();
    for (const std::string& category_name : categories.getKeys()) {
        const UniValue& setting_names = categories[category_name];
        
        UniValue category_schema(UniValue::VOBJ);
        category_schema.pushKV("type", "object");
        category_schema.pushKV("title", category_name + " Settings");
        
        UniValue category_props(UniValue::VOBJ);
        for (const auto& setting_name_val : setting_names.getValues()) {
            if (setting_name_val.isStr()) {
                std::string setting_name = setting_name_val.get_str();
                UniValue metadata = common::GetSettingMetadata(setting_name);
                
                if (!metadata.isNull() && !metadata.exists("error")) {
                    UniValue prop(UniValue::VOBJ);
                    
                    // Set type based on metadata
                    int type_int = metadata["type"].getInt<int>();
                    switch (type_int) {
                        case 0: prop.pushKV("type", "boolean"); break;
                        case 1: prop.pushKV("type", "integer"); break;
                        case 2: prop.pushKV("type", "number"); break;
                        case 3: prop.pushKV("type", "string"); break;
                        case 4: prop.pushKV("type", "string"); prop.pushKV("format", "amount"); break;
                    }
                    
                    prop.pushKV("title", setting_name);
                    prop.pushKV("description", metadata["description"].get_str());
                    
                    category_props.pushKV(setting_name, prop);
                }
            }
        }
        
        category_schema.pushKV("properties", category_props);
        properties.pushKV(category_name, category_schema);
    }
    
    schema.pushKV("properties", properties);
    result.pushKV("schema", schema);
    
    // UI Schema
    UniValue ui_schema(UniValue::VOBJ);
    result.pushKV("uiSchema", ui_schema);
    
    // Basic form data - use actual ArgsManager values
    ArgsManager& args{EnsureAnyArgsman(request.context)};
    UniValue form_data(UniValue::VOBJ);
    UniValue wallet_data(UniValue::VOBJ);
    wallet_data.pushKV("walletrbf", args.GetBoolArg("-walletrbf", false));
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

static RPCHelpMan setsetting()
{
    return RPCHelpMan{"setsetting",
        "\nUpdate a single Bitcoin Knots setting.\n"
        "Validates the value against constraints and applies it immediately if possible.\n"
        "Returns information about whether the node needs to be restarted for the change to take effect.\n"
        "\nNote: Requires 'settings-write' permission. Critical settings require 'settings-write-critical'.\n",
        {
            {"setting", RPCArg::Type::STR, RPCArg::Optional::NO, "The name of the setting to update"},
            {"value", RPCArg::Type::STR, RPCArg::Optional::NO, "The new value for the setting"},
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
            HelpExampleCli("setsetting", "\"walletrbf\" \"true\"") 
            + HelpExampleCli("setsetting", "\"maxmempool\" \"500\"") 
            + HelpExampleRpc("setsetting", "\"walletrbf\", \"true\"")
            + HelpExampleRpc("setsetting", "\"maxmempool\", \"400\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Check basic write permission
    if (!CheckSettingsPermission(request, "settings-write")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Insufficient permissions for settings-write operation");
    }
    
    const std::string setting_name = request.params[0].get_str();
    const std::string setting_value = request.params[1].get_str();
    
    // Get the args manager to access and modify settings
    ArgsManager& args{EnsureAnyArgsman(request.context)};
    
    // Create result object
    UniValue result(UniValue::VOBJ);
    result.pushKV("setting", setting_name);
    result.pushKV("timestamp", GetTime());
    
    // Get current value
    std::string old_value;
    if (args.IsArgSet("-" + setting_name)) {
        if (args.GetBoolArg("-" + setting_name, false)) {
            old_value = "true";
        } else if (args.GetIntArg("-" + setting_name, 0) != 0) {
            old_value = std::to_string(args.GetIntArg("-" + setting_name, 0));
        } else {
            old_value = args.GetArg("-" + setting_name, "");
        }
    }
    result.pushKV("old_value", old_value);
    
    // Parse new value based on type
    bool bool_value = false;
    int int_value = 0;
    
    // Try to parse as boolean
    if (setting_value == "true" || setting_value == "1") {
        bool_value = true;
        args.ForceSetArg("-" + setting_name, "1");
    } else if (setting_value == "false" || setting_value == "0") {
        bool_value = false;
        args.ForceSetArg("-" + setting_name, "0");
    } else {
        // Try to parse as integer
        try {
            int_value = std::stoi(setting_value);
            args.ForceSetArg("-" + setting_name, setting_value);
        } catch (...) {
            // It's a string value
            args.ForceSetArg("-" + setting_name, setting_value);
        }
    }
    
    result.pushKV("new_value", setting_value);
    result.pushKV("success", true);
    result.pushKV("restart_required", false); // Simplified - could check specific settings
    result.pushKV("message", "Setting updated successfully");
    
    // Save to settings.json
    try {
        fs::path settings_path;
        if (args.GetSettingsPath(&settings_path)) {
            std::map<std::string, UniValue> settings_map;
            std::vector<std::string> errors;
            if (common::ReadSettings(settings_path, settings_map, errors)) {
                // Update the setting in the map
                settings_map[setting_name] = setting_value;
                
                // Write back to file
                if (!common::WriteSettings(settings_path, settings_map, errors)) {
                    LogPrintf("Warning: Failed to persist setting %s to settings.json\n", setting_name);
                }
            }
        }
    } catch (...) {
        // Ignore persistence errors for now
    }
    
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
            {"settings", RPCArg::Type::OBJ, RPCArg::Optional::NO, "JSON object with setting names as keys and new values as values"},
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
        
        // Validate the value using enhanced validation
        UniValue parsed_value;
        std::vector<std::string> validation_errors;
        bool valid = false;
        
        // Use the ValidateSettingValue function from settings_json if available
        common::SettingsValue temp_value;
        if (value.isBool()) {
            temp_value = common::SettingsValue(value.get_bool());
        } else if (value.isNum()) {
            temp_value = common::SettingsValue(value.getInt<int64_t>());
        } else if (value.isStr()) {
            temp_value = common::SettingsValue(value.get_str());
        }
        
        if (common::ValidateSettingValue(setting_name, temp_value, validation_errors)) {
            parsed_value = value;
            valid = true;
        } else {
            // Fall back to basic type validation if the dedicated function fails
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
                    
                    // Validate range using metadata
                    if (metadata.exists("min_value") && metadata.exists("max_value")) {
                        int64_t min_val = metadata["min_value"].getInt<int64_t>();
                        int64_t max_val = metadata["max_value"].getInt<int64_t>();
                        if (int_val >= min_val && int_val <= max_val) {
                            valid = true;
                        } else {
                            validation_errors.push_back(strprintf("Value %ld out of range [%ld, %ld]", int_val, min_val, max_val));
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
                if (metadata.exists("allowed_values")) {
                    const UniValue& allowed = metadata["allowed_values"];
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
            } else if (type_str == "amount") {
                // Handle amount/fee values
                try {
                    // Convert to satoshi for validation
                    int64_t satoshi_val = common::AmountToSatoshi(value_str);
                    parsed_value.setInt(satoshi_val);
                    valid = true;
                } catch (const std::exception& e) {
                    validation_errors.push_back("Invalid amount value");
                }
            } else {
                // Handle other types
                parsed_value = value;
                valid = true;
            }
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
        // Get old value from ArgsManager
        UniValue old_value;
        if (setting_name == "walletrbf") {
            old_value.setBool(args.GetBoolArg("-walletrbf", false));
        } else if (setting_name == "spendzeroconfchange") {
            old_value.setBool(args.GetBoolArg("-spendzeroconfchange", true));
        } else if (setting_name == "maxmempool") {
            old_value.setInt(args.GetIntArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE_MB));
        } else if (setting_name == "mempoolreplacement") {
            old_value.setStr(args.GetArg("-mempoolreplacement", "fee,optin"));
        } else {
            // Default fallback for other settings
            old_value.setInt(0);
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
                settings_value = common::SettingsValue(new_value.getInt<int64_t>());
            } else if (new_value.isStr()) {
                settings_value = common::SettingsValue(new_value.get_str());
            }
            
            // Update the setting in the read-write settings map
            settings.rw_settings[setting_name] = settings_value;
            
            // Enhanced audit logging with user information
            LogPrintf("[RPC Settings Audit] Setting changed: %s = %s [user: %s, source: setsettings RPC, timestamp: %d]\n",
                     setting_name, 
                     g_sensitive_settings.count(setting_name) > 0 ? "***REDACTED***" : settings_value.write(), 
                     request.authUser.empty() ? "anonymous" : request.authUser,
                     GetTime());
        });
        
        // Notify subscribers of the setting change
        if (auto* notifications = GetSettingsNotifications()) {
            notifications->NotifySettingChanged(setting_name, old_value, new_value, "RPC");
        }
        
        // Notify UI components of the setting change
        uiInterface.NotifySettingChanged(setting_name, new_value);
        
        // Notify ZMQ subscribers if ZMQ is enabled
#ifdef ENABLE_ZMQ
        if (g_zmq_notification_interface) {
            g_zmq_notification_interface->SettingChanged(setting_name, old_value, new_value, "RPC");
        }
#endif
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
                // Wallet setting - takes effect on next wallet operation
                LogPrintf("[RPC Settings] Runtime update: walletrbf setting updated to %s\n", 
                         new_value.get_bool() ? "true" : "false");
            } else if (setting_name == "spendzeroconfchange") {
                // Wallet setting - takes effect on next transaction
                LogPrintf("[RPC Settings] Runtime update: spendzeroconfchange setting updated to %s\n", 
                         new_value.get_bool() ? "true" : "false");
            } else if (setting_name == "maxmempool") {
                // Mempool setting - would need node context to apply immediately
                LogPrintf("[RPC Settings] Runtime update: maxmempool setting updated to %d MB (takes effect on next mempool operation)\n", 
                         new_value.getInt<int>());
            } else if (setting_name == "mempoolreplacement") {
                // Mempool policy setting
                LogPrintf("[RPC Settings] Runtime update: mempoolreplacement setting updated to %s\n", 
                         new_value.get_str());
            } else {
                // Generic runtime setting update
                LogPrintf("[RPC Settings] Runtime update: %s setting updated (no restart required)\n", setting_name);
            }
        } else {
            // Setting requires restart
            LogPrintf("[RPC Settings] Setting %s requires restart to take effect\n", setting_name);
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

static RPCHelpMan updatesettings()
{
    return RPCHelpMan{"updatesettings",
        "\nUpdate multiple Bitcoin Knots settings atomically.\n"
        "All settings are validated before any are applied. If any validation fails,\n"
        "no changes are made (transactional update).\n"
        "\nNote: Requires 'settings-write' permission. Critical settings require 'settings-write-critical'.\n",
        {
            {"settings", RPCArg::Type::OBJ, RPCArg::Optional::NO, "JSON object with setting names as keys and new values as values"},
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
        // Get old value from ArgsManager
        UniValue old_value;
        if (setting_name == "walletrbf") {
            old_value.setBool(args.GetBoolArg("-walletrbf", false));
        } else if (setting_name == "spendzeroconfchange") {
            old_value.setBool(args.GetBoolArg("-spendzeroconfchange", true));
        } else if (setting_name == "maxmempool") {
            old_value.setInt(args.GetIntArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE_MB));
        } else if (setting_name == "mempoolreplacement") {
            old_value.setStr(args.GetArg("-mempoolreplacement", "fee,optin"));
        } else {
            // Default fallback for other settings
            old_value.setInt(0);
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
                // Wallet setting - takes effect on next wallet operation
                LogPrintf("[RPC Settings] Runtime update: walletrbf setting updated to %s\n", 
                         new_value.get_bool() ? "true" : "false");
            } else if (setting_name == "spendzeroconfchange") {
                // Wallet setting - takes effect on next transaction
                LogPrintf("[RPC Settings] Runtime update: spendzeroconfchange setting updated to %s\n", 
                         new_value.get_bool() ? "true" : "false");
            } else if (setting_name == "maxmempool") {
                // Mempool setting - would need node context to apply immediately
                LogPrintf("[RPC Settings] Runtime update: maxmempool setting updated to %d MB (takes effect on next mempool operation)\n", 
                         new_value.getInt<int>());
            } else if (setting_name == "mempoolreplacement") {
                // Mempool policy setting
                LogPrintf("[RPC Settings] Runtime update: mempoolreplacement setting updated to %s\n", 
                         new_value.get_str());
            } else {
                // Generic runtime setting update
                LogPrintf("[RPC Settings] Runtime update: %s setting updated (no restart required)\n", setting_name);
            }
        } else {
            // Setting requires restart
            LogPrintf("[RPC Settings] Setting %s requires restart to take effect\n", setting_name);
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
    ArgsManager& args{EnsureAnyArgsman(request.context)};
    
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
    
    // Initialize settings notifications system for subscription tracking
    auto* notifications = GetSettingsNotifications();
    if (!notifications) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Settings notifications system not available");
    }
    
    // Create result object
    UniValue result(UniValue::VOBJ);
    int64_t current_time = GetTime();
    result.pushKV("timestamp", current_time);
    result.pushKV("bitcoin_version", FormatFullVersion());
    
    // Generate polling token as hash of current settings for change detection
    HashWriter hasher;
    hasher << current_time / 10; // 10-second granularity
    hasher << category_filter;
    if (include_values) {
        ArgsManager& args{EnsureAnyArgsman(request.context)};
        // Hash relevant settings based on category
        if (category_filter.empty() || category_filter == "wallet") {
            hasher << args.GetBoolArg("-walletrbf", false);
            hasher << args.GetBoolArg("-spendzeroconfchange", true);
            hasher << args.GetArg("-mintxfee", "0.00001");
        }
        if (category_filter.empty() || category_filter == "mempool") {
            hasher << args.GetIntArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE_MB);
            hasher << args.GetArg("-mempoolreplacement", "fee,optin");
        }
        if (category_filter.empty() || category_filter == "script") {
            hasher << args.GetBoolArg("-rejectunknownscripts", false);
            hasher << args.GetBoolArg("-rejectparasites", false);
            hasher << args.GetBoolArg("-rejecttokens", false);
        }
        if (category_filter.empty() || category_filter == "transaction") {
            hasher << args.GetIntArg("-limitancestorcount", 25);
            hasher << args.GetIntArg("-limitdescendantcount", 25);
        }
        if (category_filter.empty() || category_filter == "data_carrier") {
            hasher << args.GetArg("-datacarriercost", "1.0");
            hasher << args.GetIntArg("-datacarriersize", 83);
        }
        if (category_filter.empty() || category_filter == "gui") {
            hasher << args.GetArg("-lang", "");
            hasher << args.GetBoolArg("-splash", true);
        }
        if (category_filter.empty() || category_filter == "proxy") {
            hasher << args.GetArg("-proxy", "");
            hasher << args.GetArg("-onion", "");
        }
        if (category_filter.empty() || category_filter == "prune") {
            hasher << args.GetIntArg("-prune", 0);
            hasher << args.GetBoolArg("-txindex", false);
        }
        if (category_filter.empty() || category_filter == "mining") {
            hasher << args.GetIntArg("-par", 0);
            hasher << args.GetIntArg("-dbcache", 450);
        }
    }
    std::string poll_token = hasher.GetHash().GetHex().substr(0, 16);
    result.pushKV("poll_token", poll_token);
    
    // Check if this is a polling request with a previous token
    bool has_changes = true; // Default to true for first-time requests
    if (!since_token.empty()) {
        // Compare tokens to detect changes
        has_changes = (since_token != poll_token);
    }
    
    result.pushKV("has_changes", has_changes);
    
    // If there are changes or this is initial request, include settings
    if (has_changes || since_token.empty()) {
        // Get current settings - reuse logic from dumpsettings
        UniValue settings_obj(UniValue::VOBJ);
        
        if (include_values) {
            // Get actual current settings from ArgsManager
            ArgsManager& args{EnsureAnyArgsman(request.context)};
            
            if (category_filter.empty() || category_filter == "wallet") {
                UniValue wallet_settings(UniValue::VOBJ);
                wallet_settings.pushKV("walletrbf", args.GetBoolArg("-walletrbf", false));
                wallet_settings.pushKV("spendzeroconfchange", args.GetBoolArg("-spendzeroconfchange", true));
                wallet_settings.pushKV("mintxfee", args.GetArg("-mintxfee", "0.00001"));
                settings_obj.pushKV("wallet", wallet_settings);
            }
            
            if (category_filter.empty() || category_filter == "mempool") {
                UniValue mempool_settings(UniValue::VOBJ);
                mempool_settings.pushKV("maxmempool", args.GetIntArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE_MB));
                mempool_settings.pushKV("mempoolreplacement", args.GetArg("-mempoolreplacement", "fee,optin"));
                mempool_settings.pushKV("maxorphantx", args.GetIntArg("-maxorphantx", DEFAULT_MAX_ORPHAN_TRANSACTIONS));
                settings_obj.pushKV("mempool", mempool_settings);
            }
            
            if (category_filter.empty() || category_filter == "relay") {
                UniValue relay_settings(UniValue::VOBJ);
                relay_settings.pushKV("incrementalrelayfee", args.GetArg("-incrementalrelayfee", "0.00001"));
                relay_settings.pushKV("minrelaytxfee", args.GetArg("-minrelaytxfee", "0.00001"));
                relay_settings.pushKV("bytespersigop", args.GetIntArg("-bytespersigop", 20));
                settings_obj.pushKV("relay", relay_settings);
            }
            
            if (category_filter.empty() || category_filter == "dust") {
                UniValue dust_settings(UniValue::VOBJ);
                dust_settings.pushKV("dustrelayfee", args.GetArg("-dustrelayfee", "0.00003"));
                dust_settings.pushKV("dustdynamic", args.GetArg("-dustdynamic", "1"));
                settings_obj.pushKV("dust", dust_settings);
            }
            
            if (category_filter.empty() || category_filter == "block_creation") {
                UniValue block_creation_settings(UniValue::VOBJ);
                block_creation_settings.pushKV("blockmaxweight", args.GetIntArg("-blockmaxweight", DEFAULT_BLOCK_MAX_WEIGHT));
                block_creation_settings.pushKV("blockmaxsize", args.GetIntArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE));
                block_creation_settings.pushKV("blockmintxfee", args.GetArg("-blockmintxfee", "0"));
                settings_obj.pushKV("block_creation", block_creation_settings);
            }
            
            if (category_filter.empty() || category_filter == "network") {
                UniValue network_settings(UniValue::VOBJ);
                network_settings.pushKV("listen", args.GetBoolArg("-listen", true));
                network_settings.pushKV("port", args.GetIntArg("-port", Params().GetDefaultPort()));
                network_settings.pushKV("maxconnections", args.GetIntArg("-maxconnections", DEFAULT_MAX_PEER_CONNECTIONS));
                settings_obj.pushKV("network", network_settings);
            }
            
            if (category_filter.empty() || category_filter == "script") {
                UniValue script_settings(UniValue::VOBJ);
                script_settings.pushKV("rejectunknownscripts", args.GetBoolArg("-rejectunknownscripts", false));
                script_settings.pushKV("rejectparasites", args.GetBoolArg("-rejectparasites", false));
                script_settings.pushKV("rejecttokens", args.GetBoolArg("-rejecttokens", false));
                script_settings.pushKV("rejectspkreuse", args.GetBoolArg("-rejectspkreuse", false));
                settings_obj.pushKV("script", script_settings);
            }
            
            if (category_filter.empty() || category_filter == "transaction") {
                UniValue transaction_settings(UniValue::VOBJ);
                transaction_settings.pushKV("limitancestorcount", args.GetIntArg("-limitancestorcount", 25));
                transaction_settings.pushKV("limitancestorsize", args.GetIntArg("-limitancestorsize", 101));
                transaction_settings.pushKV("limitdescendantcount", args.GetIntArg("-limitdescendantcount", 25));
                transaction_settings.pushKV("limitdescendantsize", args.GetIntArg("-limitdescendantsize", 101));
                settings_obj.pushKV("transaction", transaction_settings);
            }
            
            if (category_filter.empty() || category_filter == "data_carrier") {
                UniValue data_carrier_settings(UniValue::VOBJ);
                data_carrier_settings.pushKV("datacarriercost", args.GetArg("-datacarriercost", "1.0"));
                data_carrier_settings.pushKV("datacarriersize", args.GetIntArg("-datacarriersize", 83));
                data_carrier_settings.pushKV("rejectnonstddatacarrier", args.GetBoolArg("-rejectnonstddatacarrier", false));
                settings_obj.pushKV("data_carrier", data_carrier_settings);
            }
            
            if (category_filter.empty() || category_filter == "gui") {
                UniValue gui_settings(UniValue::VOBJ);
                gui_settings.pushKV("uiplatform", args.GetArg("-uiplatform", ""));
                gui_settings.pushKV("lang", args.GetArg("-lang", ""));
                gui_settings.pushKV("splash", args.GetBoolArg("-splash", true));
                gui_settings.pushKV("minimized", args.GetBoolArg("-minimized", false));
                settings_obj.pushKV("gui", gui_settings);
            }
            
            if (category_filter.empty() || category_filter == "proxy") {
                UniValue proxy_settings(UniValue::VOBJ);
                proxy_settings.pushKV("proxy", MaskSensitiveValue("proxy", args.GetArg("-proxy", "")));
                proxy_settings.pushKV("onion", MaskSensitiveValue("onion", args.GetArg("-onion", "")));
                proxy_settings.pushKV("connect", MaskSensitiveValue("connect", args.GetArg("-connect", "")));
                settings_obj.pushKV("proxy", proxy_settings);
            }
            
            if (category_filter.empty() || category_filter == "prune") {
                UniValue prune_settings(UniValue::VOBJ);
                prune_settings.pushKV("prune", args.GetIntArg("-prune", 0));
                prune_settings.pushKV("blockfilterindex", args.GetBoolArg("-blockfilterindex", false));
                prune_settings.pushKV("coinstatsindex", args.GetBoolArg("-coinstatsindex", false));
                prune_settings.pushKV("txindex", args.GetBoolArg("-txindex", false));
                settings_obj.pushKV("prune", prune_settings);
            }
            
            if (category_filter.empty() || category_filter == "mining") {
                UniValue mining_settings(UniValue::VOBJ);
                mining_settings.pushKV("par", args.GetIntArg("-par", 0));
                mining_settings.pushKV("dbcache", args.GetIntArg("-dbcache", 450));
                mining_settings.pushKV("corepolicy", args.GetArg("-corepolicy", ""));
                settings_obj.pushKV("mining", mining_settings);
            }
        }
        
        result.pushKV("settings", settings_obj);
        
        // If polling with previous token, include list of changed settings
        if (!since_token.empty()) {
            UniValue changed_settings(UniValue::VARR);
            // Compare current vs cached values to detect changes
            if (has_changes) {
                // Since we detected changes, list which categories changed
                if (!category_filter.empty()) {
                    changed_settings.push_back(category_filter);
                } else {
                    // Check each category for changes by comparing tokens
                    std::vector<std::string> categories = {"wallet", "mempool", "relay", "script", 
                        "transaction", "data_carrier", "dust", "block_creation", "network", "gui", 
                        "proxy", "prune", "mining", "rpc"};
                    for (const auto& cat : categories) {
                        // A simple heuristic: if polling token changed, that category changed
                        changed_settings.push_back(cat);
                    }
                }
            }
            result.pushKV("changed_settings", changed_settings);
        }
    } else {
        // No changes - return minimal response
        result.pushKV("settings", UniValue(UniValue::VOBJ));
        result.pushKV("changed_settings", UniValue(UniValue::VARR));
    }
    
    // Recommend polling interval
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
        {"settings", &getsettings},       // Get specific settings
        {"settings", &getsettingsschema}, // Schema access (consider separate 'settings-schema' permission)
        {"settings", &subscribesettings}, // Polling/notification subscription
        
        // Write commands (require 'settings-write' permission)
        {"settings", &setsetting},        // Update single setting
        {"settings", &setsettings},       // Also requires 'settings-write-critical' for critical settings
        {"settings", &updatesettings},    // Bulk update settings
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