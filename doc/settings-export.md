# Settings Export

Bitcoin Knots provides RPC commands to export and import GUI settings for headless deployments.

## RPC Commands

### dumpsettings

Export settings to JSON format.

**Syntax:** `dumpsettings ( "filter" "options" )`

**Arguments:**
- `filter` (string, optional): Category ("wallet"), specific setting ("walletrbf"), array of settings, or pattern ("wallet.*")
- `options` (object, optional):
  - `detailed` (boolean, default=false): Include detailed metadata (type, description, constraints, restart requirements)

**Note:** Sensitive settings are always masked for security.

**Examples:**
```bash
bitcoin-cli dumpsettings
bitcoin-cli dumpsettings "wallet"
bitcoin-cli dumpsettings "walletrbf" '{"detailed":true}'
bitcoin-cli dumpsettings '["walletrbf", "mintxfee"]' '{"detailed":true}'
```

### getsettingsschema

Generate JSON Forms compatible schema for UI generation.

**Syntax:** `getsettingsschema ( "category" )`

**Example:**
```bash
bitcoin-cli getsettingsschema
bitcoin-cli getsettingsschema "wallet"
```

### setsettings

Update one or more Bitcoin Knots settings.

**Syntax:** `setsettings {"setting_name": value, ...}`

**Example:**
```bash
bitcoin-cli setsettings '{"walletrbf": true}'
bitcoin-cli setsettings '{"maxmempool": 500, "walletrbf": true}'
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