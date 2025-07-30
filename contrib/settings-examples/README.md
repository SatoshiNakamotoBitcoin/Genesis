# Bitcoin Knots Settings Examples

This directory contains examples for using the Bitcoin Knots settings export system.

## Files

- `sample-export.json` - Example of complete settings export
- `wallet-settings.json` - Example wallet-specific settings export
- `web-ui-integration.html` - JSON Forms web UI example
- `backup-restore.sh` - Shell script for settings backup/restore
- `sync-nodes.py` - Python script for synchronizing settings between nodes

## Usage

### Basic Export/Import
```bash
# Export all settings
bitcoin-cli dumpsettings > my-settings.json

# Import settings (GUI)
# Use Options dialog Import button

# Update via RPC
bitcoin-cli updatesettings "$(cat my-settings.json)"
```

### Web UI Integration
Open `web-ui-integration.html` in a browser and configure your Bitcoin Core RPC endpoint to see the automatic UI generation in action.

### Automated Backup
```bash
# Make backup script executable
chmod +x backup-restore.sh

# Create backup
./backup-restore.sh backup

# Restore from backup
./backup-restore.sh restore bitcoin-settings-20250728.json
```

### Node Synchronization
```bash
# Install dependencies
pip install requests

# Sync settings from node1 to node2
python sync-nodes.py http://user:pass@node1:8332 http://user:pass@node2:8332
```