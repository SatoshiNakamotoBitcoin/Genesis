# Settings Security

Security configuration for the settings RPC system.

## Permission Categories

### Read Permissions
- `settings-read`: Basic read access to all settings (sensitive values masked)
- `settings-schema`: Access to settings schema for UI generation

### Write Permissions (Category-based)
- `settings-write`: General write permission (required for all modifications)
- `settings-write:wallet`: Modify wallet settings
- `settings-write:mempool`: Modify mempool settings  
- `settings-write:network`: Modify network settings (critical)
- `settings-write:rpc`: Modify RPC settings (critical)
- `settings-write:block_creation`: Modify mining/block creation settings

Note: Critical categories (network, rpc) require both general write permission and category-specific permission.

## Configuration

### rpcwhitelist

Restrict settings access for specific users:

```bash
# Read-only access
bitcoind -rpcwhitelist=reader:dumpsettings,getsettingsschema,subscribesettings

# Wallet settings modification only
bitcoind -rpcwhitelist=wallet_admin:dumpsettings,setsettings,updatesettings \
         -rpcwhitelistpermissions=wallet_admin:settings-write:wallet

# Full access (including critical network/rpc settings)
bitcoind -rpcwhitelist=admin:dumpsettings,getsettingsschema,subscribesettings,setsettings,updatesettings \
         -rpcwhitelistpermissions=admin:settings-write:network,settings-write:rpc
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

Sensitive settings are always masked in dumpsettings output for security reasons.

## Critical Settings Categories

These categories contain settings that can affect node connectivity and security:

### Network Category
Requires `settings-write:network` permission:
- `bind`, `port`, `listen`, `proxy`, `onion`
- `whitelist`, `whitebind`  
- `maxconnections`, `maxuploadtarget`

### RPC Category  
Requires `settings-write:rpc` permission:
- `rpcbind`, `rpcport`
- `rpcuser`, `rpcpassword`, `rpcauth`
- `rpcwhitelist`, `rpcwhitelistdefault`

