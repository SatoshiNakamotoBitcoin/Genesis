// Copyright (c) 2025 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <config/bitcoin-config.h> // IWYU pragma: keep

#include <common/args.h>
#include <common/settings.h>
#include <common/settings_json.h>
#include <clientversion.h>
#include <node/context.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/time.h>

#include <string>
#include <vector>
#include <map>

using node::NodeContext;

namespace {

// Define schema version for compatibility tracking
const std::string SCHEMA_VERSION = "1.0.0";

// Helper function to create JSON Schema property definition
UniValue CreateSchemaProperty(const std::string& type, const std::string& title, 
                            const std::string& description, const UniValue& constraints = UniValue())
{
    UniValue prop(UniValue::VOBJ);
    prop.pushKV("type", type);
    prop.pushKV("title", title);
    prop.pushKV("description", description);
    
    if (!constraints.isNull()) {
        // Add constraints like minimum, maximum, enum values
        for (const auto& key : constraints.getKeys()) {
            prop.pushKV(key, constraints[key]);
        }
    }
    
    return prop;
}

// Helper function to create UI schema element
UniValue CreateUISchemaElement(const std::string& widget, const std::string& help = "", 
                              const UniValue& options = UniValue())
{
    UniValue ui_element(UniValue::VOBJ);
    
    if (!widget.empty()) {
        ui_element.pushKV("ui:widget", widget);
    }
    
    if (!help.empty()) {
        ui_element.pushKV("ui:help", help);
    }
    
    if (!options.isNull()) {
        ui_element.pushKV("ui:options", options);
    }
    
    return ui_element;
}

// Create JSON Schema for wallet settings
UniValue CreateWalletSchema()
{
    UniValue properties(UniValue::VOBJ);
    UniValue required(UniValue::VARR);
    
    // walletrbf
    properties.pushKV("walletrbf", CreateSchemaProperty("boolean", 
        "Enable Replace-By-Fee", 
        "Allow transactions to be replaced with higher fee versions"));
    
    // spendzeroconfchange
    properties.pushKV("spendzeroconfchange", CreateSchemaProperty("boolean",
        "Spend Unconfirmed Change",
        "Allow spending change from transactions that haven't been confirmed yet"));
    
    // mintxfee
    UniValue mintxfee_constraints(UniValue::VOBJ);
    mintxfee_constraints.pushKV("minimum", 0);
    mintxfee_constraints.pushKV("maximum", 1000000000); // 10 BTC in satoshi
    properties.pushKV("mintxfee", CreateSchemaProperty("integer",
        "Minimum Transaction Fee",
        "Minimum fee rate in satoshi per kilobyte for wallet transactions",
        mintxfee_constraints));
    
    // walletbroadcast
    properties.pushKV("walletbroadcast", CreateSchemaProperty("boolean",
        "Broadcast Transactions",
        "Automatically broadcast new transactions to the network"));
    
    // avoidpartialspends
    properties.pushKV("avoidpartialspends", CreateSchemaProperty("boolean",
        "Avoid Partial Spends",
        "Group inputs from the same address together to avoid partial spends"));
    
    UniValue schema(UniValue::VOBJ);
    schema.pushKV("type", "object");
    schema.pushKV("title", "Wallet Settings");
    schema.pushKV("properties", properties);
    schema.pushKV("required", required);
    
    return schema;
}

// Create JSON Schema for mempool settings
UniValue CreateMempoolSchema()
{
    UniValue properties(UniValue::VOBJ);
    UniValue required(UniValue::VARR);
    
    // mempoolreplacement
    UniValue replacement_enum(UniValue::VARR);
    replacement_enum.push_back("never");
    replacement_enum.push_back("fee");
    replacement_enum.push_back("full");
    
    UniValue replacement_constraints(UniValue::VOBJ);
    replacement_constraints.pushKV("enum", replacement_enum);
    
    properties.pushKV("mempoolreplacement", CreateSchemaProperty("string",
        "Mempool Replacement Policy",
        "Policy for replacing transactions in the mempool",
        replacement_constraints));
    
    // maxmempool
    UniValue maxmempool_constraints(UniValue::VOBJ);
    maxmempool_constraints.pushKV("minimum", 1);
    maxmempool_constraints.pushKV("maximum", 10000);
    properties.pushKV("maxmempool", CreateSchemaProperty("integer",
        "Maximum Mempool Size (MB)",
        "Maximum size of the transaction memory pool in megabytes",
        maxmempool_constraints));
    
    // mempoolexpiry
    UniValue expiry_constraints(UniValue::VOBJ);
    expiry_constraints.pushKV("minimum", 1);
    expiry_constraints.pushKV("maximum", 999999);
    properties.pushKV("mempoolexpiry", CreateSchemaProperty("integer",
        "Mempool Expiry (hours)",
        "Time in hours after which transactions expire from mempool",
        expiry_constraints));
    
    // maxorphantx
    UniValue orphan_constraints(UniValue::VOBJ);
    orphan_constraints.pushKV("minimum", 0);
    orphan_constraints.pushKV("maximum", 1000);
    properties.pushKV("maxorphantx", CreateSchemaProperty("integer",
        "Maximum Orphan Transactions",
        "Maximum number of orphan transactions to keep in memory",
        orphan_constraints));
    
    // mempooltruc
    properties.pushKV("mempooltruc", CreateSchemaProperty("boolean",
        "Enable TRUC Transactions",
        "Accept v3 transactions with topologically restricted until confirmation"));
    
    UniValue schema(UniValue::VOBJ);
    schema.pushKV("type", "object");
    schema.pushKV("title", "Mempool Settings");
    schema.pushKV("properties", properties);
    schema.pushKV("required", required);
    
    return schema;
}

// Create JSON Schema for relay settings
UniValue CreateRelaySchema()
{
    UniValue properties(UniValue::VOBJ);
    UniValue required(UniValue::VARR);
    
    // incrementalrelayfee
    UniValue incremental_constraints(UniValue::VOBJ);
    incremental_constraints.pushKV("minimum", 0);
    incremental_constraints.pushKV("maximum", 100000000); // 1 BTC in satoshi
    properties.pushKV("incrementalrelayfee", CreateSchemaProperty("integer",
        "Incremental Relay Fee",
        "Fee rate increment for mempool limiting and replacement in satoshi/kB",
        incremental_constraints));
    
    // minrelaytxfee
    UniValue minrelay_constraints(UniValue::VOBJ);
    minrelay_constraints.pushKV("minimum", 0);
    minrelay_constraints.pushKV("maximum", 100000000);
    properties.pushKV("minrelaytxfee", CreateSchemaProperty("integer",
        "Minimum Relay Fee",
        "Minimum fee rate in satoshi/kB for transaction relay",
        minrelay_constraints));
    
    // bytespersigop
    UniValue bytespersigop_constraints(UniValue::VOBJ);
    bytespersigop_constraints.pushKV("minimum", 1);
    bytespersigop_constraints.pushKV("maximum", 10000);
    properties.pushKV("bytespersigop", CreateSchemaProperty("integer",
        "Bytes Per SigOp",
        "Equivalent bytes per sigop in transactions for relay and mining",
        bytespersigop_constraints));
    
    // permitbaremultisig
    properties.pushKV("permitbaremultisig", CreateSchemaProperty("boolean",
        "Permit Bare Multisig",
        "Relay non-P2SH multisig transactions"));
    
    UniValue schema(UniValue::VOBJ);
    schema.pushKV("type", "object");
    schema.pushKV("title", "Relay Settings");
    schema.pushKV("properties", properties);
    schema.pushKV("required", required);
    
    return schema;
}

// Create JSON Schema for script/policy settings
UniValue CreateScriptSchema()
{
    UniValue properties(UniValue::VOBJ);
    UniValue required(UniValue::VARR);
    
    // rejectunknownscripts
    properties.pushKV("rejectunknownscripts", CreateSchemaProperty("boolean",
        "Reject Unknown Scripts",
        "Reject transactions with unknown script versions"));
    
    // rejectparasites
    properties.pushKV("rejectparasites", CreateSchemaProperty("boolean",
        "Reject Parasites",
        "Reject transactions that appear to be parasitic spam"));
    
    // rejecttokens
    properties.pushKV("rejecttokens", CreateSchemaProperty("boolean",
        "Reject Tokens",
        "Reject transactions creating or transferring tokens"));
    
    // rejectspkreuse
    properties.pushKV("rejectspkreuse", CreateSchemaProperty("boolean",
        "Reject Script PubKey Reuse",
        "Reject transactions that reuse addresses"));
    
    UniValue schema(UniValue::VOBJ);
    schema.pushKV("type", "object");
    schema.pushKV("title", "Script Policy Settings");
    schema.pushKV("properties", properties);
    schema.pushKV("required", required);
    
    return schema;
}

// Create JSON Schema for data carrier settings
UniValue CreateDataCarrierSchema()
{
    UniValue properties(UniValue::VOBJ);
    UniValue required(UniValue::VARR);
    
    // acceptdatacarrier
    properties.pushKV("acceptdatacarrier", CreateSchemaProperty("boolean",
        "Accept Data Carrier",
        "Relay and mine data carrier transactions"));
    
    // maxscriptsize
    UniValue maxscript_constraints(UniValue::VOBJ);
    maxscript_constraints.pushKV("minimum", 0);
    maxscript_constraints.pushKV("maximum", 520);
    properties.pushKV("maxscriptsize", CreateSchemaProperty("integer",
        "Maximum Script Size",
        "Maximum script size in bytes for data carrier outputs",
        maxscript_constraints));
    
    // datacarriersize
    UniValue datasize_constraints(UniValue::VOBJ);
    datasize_constraints.pushKV("minimum", 0);
    datasize_constraints.pushKV("maximum", 83);
    properties.pushKV("datacarriersize", CreateSchemaProperty("integer",
        "Data Carrier Size",
        "Maximum size of data in data carrier transactions",
        datasize_constraints));
    
    // datacarriercost
    UniValue datacost_constraints(UniValue::VOBJ);
    datacost_constraints.pushKV("minimum", 0);
    datacost_constraints.pushKV("maximum", 1000);
    properties.pushKV("datacarriercost", CreateSchemaProperty("integer",
        "Data Carrier Cost",
        "Equivalent bytes cost per actual data byte in data carrier outputs",
        datacost_constraints));
    
    UniValue schema(UniValue::VOBJ);
    schema.pushKV("type", "object");
    schema.pushKV("title", "Data Carrier Settings");
    schema.pushKV("properties", properties);
    schema.pushKV("required", required);
    
    return schema;
}

// Create UI Schema for wallet settings
UniValue CreateWalletUISchema()
{
    UniValue ui_schema(UniValue::VOBJ);
    
    // Use toggle widgets for boolean settings
    ui_schema.pushKV("walletrbf", CreateUISchemaElement("checkbox",
        "Enable RBF to allow replacing transactions with higher fees"));
    
    ui_schema.pushKV("spendzeroconfchange", CreateUISchemaElement("checkbox",
        "Enable to spend unconfirmed change outputs"));
    
    // Use number input with suffix for fee settings
    UniValue mintxfee_options(UniValue::VOBJ);
    mintxfee_options.pushKV("inputType", "number");
    mintxfee_options.pushKV("suffix", "sat/kB");
    ui_schema.pushKV("mintxfee", CreateUISchemaElement("", 
        "Minimum fee rate for wallet transactions", mintxfee_options));
    
    ui_schema.pushKV("walletbroadcast", CreateUISchemaElement("checkbox"));
    ui_schema.pushKV("avoidpartialspends", CreateUISchemaElement("checkbox"));
    
    // Group ordering
    UniValue ui_order(UniValue::VARR);
    ui_order.push_back("walletrbf");
    ui_order.push_back("spendzeroconfchange");
    ui_order.push_back("mintxfee");
    ui_order.push_back("walletbroadcast");
    ui_order.push_back("avoidpartialspends");
    ui_schema.pushKV("ui:order", ui_order);
    
    return ui_schema;
}

// Create UI Schema for mempool settings
UniValue CreateMempoolUISchema()
{
    UniValue ui_schema(UniValue::VOBJ);
    
    // Radio buttons for replacement policy
    ui_schema.pushKV("mempoolreplacement", CreateUISchemaElement("radio",
        "Select transaction replacement policy"));
    
    // Number input with MB suffix
    UniValue maxmempool_options(UniValue::VOBJ);
    maxmempool_options.pushKV("inputType", "number");
    maxmempool_options.pushKV("suffix", "MB");
    ui_schema.pushKV("maxmempool", CreateUISchemaElement("",
        "Maximum memory pool size", maxmempool_options));
    
    // Number input with hours suffix
    UniValue expiry_options(UniValue::VOBJ);
    expiry_options.pushKV("inputType", "number");
    expiry_options.pushKV("suffix", "hours");
    ui_schema.pushKV("mempoolexpiry", CreateUISchemaElement("",
        "Transaction expiry time", expiry_options));
    
    ui_schema.pushKV("mempooltruc", CreateUISchemaElement("checkbox"));
    
    UniValue ui_order(UniValue::VARR);
    ui_order.push_back("mempoolreplacement");
    ui_order.push_back("maxmempool");
    ui_order.push_back("mempoolexpiry");
    ui_order.push_back("maxorphantx");
    ui_order.push_back("mempooltruc");
    ui_schema.pushKV("ui:order", ui_order);
    
    return ui_schema;
}

// Generate complete JSON Forms schema
UniValue GenerateJSONFormsSchema(const NodeContext& node_context, const ArgsManager& args)
{
    UniValue result(UniValue::VOBJ);
    
    // Schema version and metadata
    result.pushKV("version", SCHEMA_VERSION);
    result.pushKV("generated", GetTime());
    result.pushKV("bitcoin_version", FormatFullVersion());
    
    // JSON Schema (following JSON Schema Draft 7)
    UniValue json_schema(UniValue::VOBJ);
    json_schema.pushKV("$schema", "https://json-schema.org/draft-07/schema#");
    json_schema.pushKV("type", "object");
    json_schema.pushKV("title", "Bitcoin Knots Settings");
    json_schema.pushKV("description", "Complete configuration options for Bitcoin Knots node");
    
    // Properties organized by category
    UniValue properties(UniValue::VOBJ);
    properties.pushKV("wallet", CreateWalletSchema());
    properties.pushKV("mempool", CreateMempoolSchema());
    properties.pushKV("relay", CreateRelaySchema());
    properties.pushKV("script", CreateScriptSchema());
    properties.pushKV("datacarrier", CreateDataCarrierSchema());
    
    json_schema.pushKV("properties", properties);
    
    // Additional schema attributes  
    json_schema.pushKV("additionalProperties", false);
    
    result.pushKV("schema", json_schema);
    
    // UI Schema for layout and widget hints
    UniValue ui_schema(UniValue::VOBJ);
    ui_schema.pushKV("wallet", CreateWalletUISchema());
    ui_schema.pushKV("mempool", CreateMempoolUISchema());
    
    // Tab layout
    UniValue ui_tabs(UniValue::VARR);
    
    UniValue wallet_tab(UniValue::VOBJ);
    wallet_tab.pushKV("title", "Wallet");
    UniValue wallet_fields(UniValue::VARR);
    wallet_fields.push_back("wallet");
    wallet_tab.pushKV("fields", wallet_fields);
    ui_tabs.push_back(wallet_tab);
    
    UniValue mempool_tab(UniValue::VOBJ);
    mempool_tab.pushKV("title", "Mempool");
    UniValue mempool_fields(UniValue::VARR);
    mempool_fields.push_back("mempool");
    mempool_tab.pushKV("fields", mempool_fields);
    ui_tabs.push_back(mempool_tab);
    
    UniValue relay_tab(UniValue::VOBJ);
    relay_tab.pushKV("title", "Relay");
    UniValue relay_fields(UniValue::VARR);
    relay_fields.push_back("relay");
    relay_tab.pushKV("fields", relay_fields);
    ui_tabs.push_back(relay_tab);
    
    UniValue script_tab(UniValue::VOBJ);
    script_tab.pushKV("title", "Script Policy");
    UniValue script_fields(UniValue::VARR);
    script_fields.push_back("script");
    script_tab.pushKV("fields", script_fields);
    ui_tabs.push_back(script_tab);
    
    UniValue data_tab(UniValue::VOBJ);
    data_tab.pushKV("title", "Data Carrier");
    UniValue data_fields(UniValue::VARR);
    data_fields.push_back("datacarrier");
    data_tab.pushKV("fields", data_fields);
    ui_tabs.push_back(data_tab);
    
    ui_schema.pushKV("ui:tabs", ui_tabs);
    result.pushKV("uiSchema", ui_schema);
    
    // Form data with current values
    // In a real implementation, these would be retrieved from Settings/ArgsManager
    UniValue form_data(UniValue::VOBJ);
    
    UniValue wallet_data(UniValue::VOBJ);
    wallet_data.pushKV("walletrbf", true);
    wallet_data.pushKV("spendzeroconfchange", false);
    wallet_data.pushKV("mintxfee", 1000);
    wallet_data.pushKV("walletbroadcast", true);
    wallet_data.pushKV("avoidpartialspends", false);
    form_data.pushKV("wallet", wallet_data);
    
    UniValue mempool_data(UniValue::VOBJ);
    mempool_data.pushKV("mempoolreplacement", "full");
    mempool_data.pushKV("maxmempool", 300);
    mempool_data.pushKV("mempoolexpiry", 336);
    mempool_data.pushKV("maxorphantx", 100);
    mempool_data.pushKV("mempooltruc", true);
    form_data.pushKV("mempool", mempool_data);
    
    result.pushKV("formData", form_data);
    
    // Bitcoin Knots specific attributes
    UniValue knots_meta(UniValue::VOBJ);
    
    // Restart required indicators
    UniValue restart_required(UniValue::VOBJ);
    restart_required.pushKV("network.port", true);
    restart_required.pushKV("network.bind", true);
    restart_required.pushKV("network.maxconnections", true);
    knots_meta.pushKV("restart_required", restart_required);
    
    // Dependencies between settings
    UniValue dependencies(UniValue::VOBJ);
    UniValue dust_deps(UniValue::VOBJ);
    UniValue dust_dep_array(UniValue::VARR);
    dust_dep_array.push_back("dustrelayfee");
    dust_deps.pushKV("dustdynamic", dust_dep_array);
    dependencies.pushKV("conditionals", dust_deps);
    knots_meta.pushKV("dependencies", dependencies);
    
    // Validation rules
    UniValue validation(UniValue::VOBJ);
    UniValue cross_field(UniValue::VARR);
    validation.pushKV("cross_field", cross_field);
    knots_meta.pushKV("validation", validation);
    
    result.pushKV("knotsMetadata", knots_meta);
    
    return result;
}

} // anonymous namespace

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
    // Get the node context to access settings
    const NodeContext& node_context{EnsureAnyNodeContext(request.context)};
    const ArgsManager& args{EnsureAnyArgsman(request.context)};
    
    // Get optional category filter
    std::string category_filter;
    if (!request.params[0].isNull()) {
        category_filter = request.params[0].get_str();
        
        // Validate category
        std::vector<std::string> valid_categories = {
            "wallet", "mempool", "relay", "script", "transaction", 
            "datacarrier", "dust", "block_creation", "network", "gui"
        };
        
        bool valid = false;
        for (const auto& cat : valid_categories) {
            if (cat == category_filter) {
                valid = true;
                break;
            }
        }
        
        if (!valid) {
            std::string valid_str;
            for (size_t i = 0; i < valid_categories.size(); ++i) {
                if (i > 0) valid_str += ", ";
                valid_str += valid_categories[i];
            }
            throw JSONRPCError(RPC_INVALID_PARAMETER, 
                strprintf("Invalid category '%s'. Valid categories: %s", 
                    category_filter, valid_str));
        }
    }
    
    // Generate complete schema
    UniValue full_schema = GenerateJSONFormsSchema(node_context, args);
    
    // Filter by category if requested
    if (!category_filter.empty()) {
        UniValue filtered(UniValue::VOBJ);
        filtered.pushKV("version", full_schema["version"]);
        filtered.pushKV("generated", full_schema["generated"]);
        filtered.pushKV("bitcoin_version", full_schema["bitcoin_version"]);
        
        // Filter schema
        UniValue filtered_schema(UniValue::VOBJ);
        filtered_schema.pushKV("$schema", full_schema["schema"]["$schema"]);
        filtered_schema.pushKV("type", "object");
        filtered_schema.pushKV("title", strprintf("%s Settings", category_filter));
        
        UniValue filtered_props(UniValue::VOBJ);
        if (full_schema["schema"]["properties"].exists(category_filter)) {
            filtered_props.pushKV(category_filter, 
                full_schema["schema"]["properties"][category_filter]);
        }
        filtered_schema.pushKV("properties", filtered_props);
        filtered.pushKV("schema", filtered_schema);
        
        // Filter UI schema
        UniValue filtered_ui(UniValue::VOBJ);
        if (full_schema["uiSchema"].exists(category_filter)) {
            filtered_ui.pushKV(category_filter, full_schema["uiSchema"][category_filter]);
        }
        filtered.pushKV("uiSchema", filtered_ui);
        
        // Filter form data
        UniValue filtered_data(UniValue::VOBJ);
        if (full_schema["formData"].exists(category_filter)) {
            filtered_data.pushKV(category_filter, full_schema["formData"][category_filter]);
        }
        filtered.pushKV("formData", filtered_data);
        
        // Include metadata
        filtered.pushKV("knotsMetadata", full_schema["knotsMetadata"]);
        
        return filtered;
    }
    
    return full_schema;
},
    };
}

// Add this function to the existing RegisterSettingsRPCCommands
// This will be called from src/rpc/settings.cpp
void RegisterSettingsSchemaRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"settings", &getsettingsschema},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}