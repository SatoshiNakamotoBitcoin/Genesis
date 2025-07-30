# Settings Security

Security configuration for the settings RPC system.

## Permission Categories

### Read Permissions
- `settings-read`: Basic read access to non-sensitive settings
- `settings-read-sensitive`: Read access to sensitive settings (passwords, keys)
- `settings-schema`: Access to settings schema for UI generation

### Write Permissions
- `settings-write`: Modify non-critical settings
- `settings-write-critical`: Modify critical settings (network, security)

## Configuration

### rpcwhitelist

Restrict settings access for specific users:

```bash
# Read-only access
bitcoind -rpcwhitelist=reader:dumpsettings,getsettings,getsettingsschema,subscribesettings

# Full access
bitcoind -rpcwhitelist=admin:dumpsettings,getsettings,getsettingsschema,subscribesettings,setsetting,updatesettings
```

### rpcauth

Use generated credentials for production:

```bash
python3 share/rpcauth/rpcauth.py settings_reader
# Add result to bitcoin.conf: rpcauth=settings_reader:<hash>
```

## Sensitive Settings

These settings are masked in output unless explicitly requested:
- `rpcpassword`, `rpcauth`, `rpcuser`
- `rpcwhitelist`, `rpcwhitelistdefault`
- `walletpassphrase`, `walletpassphrasechange`, `encryptwallet`

To include sensitive settings:
```bash
bitcoin-cli dumpsettings "" true  # Requires settings-read-sensitive permission
```

## Critical Settings

These settings require `settings-write-critical` permission:
- Network: `bind`, `port`, `listen`, `proxy`, `onion`
- RPC: `rpcbind`, `rpcport`
- Access Control: `whitelist`, `whitebind`
- Resource Limits: `maxconnections`, `maxuploadtarget`

## Rate Limiting

- 50 setting changes per 5 minutes per user
- Applies to `setsetting` and `updatesettings`

## Encrypted Exports

```bash
bitcoin-cli dumpsettings "" false "password"
```

Returns encrypted JSON with base64 encoding.