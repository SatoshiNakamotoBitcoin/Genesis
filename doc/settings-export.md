# Settings Export

Bitcoin Knots provides RPC commands to export and import GUI settings for headless deployments.

## RPC Commands

### dumpsettings

Export settings to JSON format.

**Syntax:** `dumpsettings ( "category" include_sensitive "encrypt_password" )`

**Arguments:**
- `category` (string, optional): Filter by category ("wallet", "mempool", "relay", "script", "transaction", "data_carrier", "dust", "block_creation", "network", "gui")
- `include_sensitive` (boolean, optional, default=false): Include sensitive settings
- `encrypt_password` (string, optional): Password to encrypt output

**Examples:**
```bash
bitcoin-cli dumpsettings
bitcoin-cli dumpsettings "wallet"
bitcoin-cli dumpsettings "" true "password"
```

### getsettings

Retrieve specific settings with metadata.

**Syntax:** `getsettings ( "setting_name_or_pattern" )`

**Examples:**
```bash
bitcoin-cli getsettings
bitcoin-cli getsettings "walletrbf"
bitcoin-cli getsettings '["walletrbf", "mintxfee"]'
```

### getsettingsschema

Generate JSON Forms compatible schema for UI generation.

**Syntax:** `getsettingsschema ( "category" )`

**Example:**
```bash
bitcoin-cli getsettingsschema
bitcoin-cli getsettingsschema "wallet"
```

### setsetting

Update an individual setting.

**Syntax:** `setsetting "setting_name" "new_value"`

**Example:**
```bash
bitcoin-cli setsetting "walletrbf" "true"
bitcoin-cli setsetting "maxmempool" "500"
```

### updatesettings

Perform bulk atomic updates of multiple settings.

**Syntax:** `updatesettings "settings_json"`

**Example:**
```bash
bitcoin-cli updatesettings '{"walletrbf": true, "maxmempool": 400}'
```

### subscribesettings

Subscribe to settings changes for polling-based notifications.

**Syntax:** `subscribesettings ( "category" "token" "wait_for_changes" )`

**Example:**
```bash
bitcoin-cli subscribesettings
bitcoin-cli subscribesettings "wallet"
```

## Security

See [settings-security.md](settings-security.md) for detailed security configuration.