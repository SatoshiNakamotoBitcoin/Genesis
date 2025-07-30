#!/bin/bash

# Bitcoin Knots Settings Backup and Restore Script
# Usage: ./backup-restore.sh [backup|restore] [filename]

set -e

BITCOIN_CLI="${BITCOIN_CLI:-bitcoin-cli}"
BACKUP_DIR="${BACKUP_DIR:-./backups}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

show_help() {
    cat << EOF
Bitcoin Knots Settings Backup and Restore Tool

Usage:
    $0 backup [filename]              Create settings backup
    $0 restore <filename>             Restore settings from backup
    $0 list                          List available backups
    $0 compare <file1> <file2>       Compare two settings files

Environment Variables:
    BITCOIN_CLI     Path to bitcoin-cli (default: bitcoin-cli)
    BACKUP_DIR      Backup directory (default: ./backups)

Examples:
    $0 backup                        # Create timestamped backup
    $0 backup my-config              # Create named backup
    $0 restore backups/config.json   # Restore from specific file
    $0 list                          # Show available backups
EOF
}

ensure_backup_dir() {
    if [ ! -d "$BACKUP_DIR" ]; then
        mkdir -p "$BACKUP_DIR"
        echo "Created backup directory: $BACKUP_DIR"
    fi
}

test_bitcoin_cli() {
    if ! command -v "$BITCOIN_CLI" &> /dev/null; then
        echo "Error: bitcoin-cli not found. Set BITCOIN_CLI environment variable."
        exit 1
    fi

    if ! "$BITCOIN_CLI" getblockchaininfo &> /dev/null; then
        echo "Error: Cannot connect to Bitcoin Core. Check if bitcoind is running."
        exit 1
    fi
}

backup_settings() {
    local filename="$1"
    
    if [ -z "$filename" ]; then
        filename="bitcoin-settings-$TIMESTAMP.json"
    elif [[ "$filename" != *.json ]]; then
        filename="${filename}.json"
    fi
    
    local filepath="$BACKUP_DIR/$filename"
    
    echo "Creating settings backup..."
    ensure_backup_dir
    
    if ! "$BITCOIN_CLI" dumpsettings > "$filepath"; then
        echo "Error: Failed to export settings"
        exit 1
    fi
    
    echo "Settings backed up to: $filepath"
    
    # Create metadata file
    cat > "${filepath}.meta" << EOF
{
    "backup_time": "$(date -Iseconds)",
    "bitcoin_version": "$("$BITCOIN_CLI" getnetworkinfo | jq -r '.version // "unknown"')",
    "hostname": "$(hostname)",
    "file_size": $(stat -f%z "$filepath" 2>/dev/null || stat -c%s "$filepath" 2>/dev/null || echo "unknown")
}
EOF
    
    echo "Metadata saved to: ${filepath}.meta"
    echo "Backup completed successfully!"
}

restore_settings() {
    local filepath="$1"
    
    if [ -z "$filepath" ]; then
        echo "Error: Please specify a backup file to restore"
        show_help
        exit 1
    fi
    
    if [ ! -f "$filepath" ]; then
        echo "Error: Backup file not found: $filepath"
        exit 1
    fi
    
    echo "Restoring settings from: $filepath"
    
    # Validate JSON first
    if ! jq . "$filepath" > /dev/null 2>&1; then
        echo "Error: Invalid JSON in backup file"
        exit 1
    fi
    
    # Show preview of changes
    echo "Settings to be restored:"
    jq -r '.settings | keys[]' "$filepath" | head -10
    local total_settings=$(jq -r '.settings | keys | length' "$filepath")
    echo "Total settings: $total_settings"
    
    if [ "$total_settings" -gt 10 ]; then
        echo "... and $(($total_settings - 10)) more"
    fi
    
    read -p "Continue with restore? (y/N): " -n 1 -r
    echo
    
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Restore cancelled"
        exit 0
    fi
    
    # Extract just the settings object for updatesettings command
    local settings_json=$(jq -c '.settings' "$filepath")
    
    echo "Applying settings..."
    if ! "$BITCOIN_CLI" updatesettings "$settings_json"; then
        echo "Error: Failed to restore settings"
        echo "Your Bitcoin Core configuration was not modified"
        exit 1
    fi
    
    echo "Settings restored successfully!"
    
    # Check if restart is required
    local restart_required=$(jq -r '.metadata.restart_required // [] | length' "$filepath")
    if [ "$restart_required" -gt 0 ]; then
        echo ""
        echo "WARNING: Some settings require a Bitcoin Core restart to take effect:"
        jq -r '.metadata.restart_required[]?' "$filepath" | sed 's/^/  - /'
        echo ""
        echo "Please restart bitcoind when convenient."
    fi
}

list_backups() {
    ensure_backup_dir
    
    echo "Available backups in $BACKUP_DIR:"
    echo ""
    
    if [ ! "$(ls -A "$BACKUP_DIR"/*.json 2>/dev/null)" ]; then
        echo "No backups found."
        return
    fi
    
    for backup in "$BACKUP_DIR"/*.json; do
        if [ -f "$backup" ]; then
            local basename=$(basename "$backup")
            local size=$(stat -f%z "$backup" 2>/dev/null || stat -c%s "$backup" 2>/dev/null || echo "?")
            local date=""
            
            # Try to get date from metadata file
            if [ -f "${backup}.meta" ]; then
                date=$(jq -r '.backup_time // ""' "${backup}.meta" 2>/dev/null || echo "")
            fi
            
            # Fallback to file modification time
            if [ -z "$date" ]; then
                date=$(stat -f%Sm -t"%Y-%m-%d %H:%M:%S" "$backup" 2>/dev/null || stat -c%y "$backup" 2>/dev/null | cut -d. -f1 || echo "unknown")
            fi
            
            printf "  %-40s %8s bytes  %s\n" "$basename" "$size" "$date"
        fi
    done
}

compare_settings() {
    local file1="$1"
    local file2="$2"
    
    if [ -z "$file1" ] || [ -z "$file2" ]; then
        echo "Error: Please specify two files to compare"
        show_help
        exit 1
    fi
    
    if [ ! -f "$file1" ] || [ ! -f "$file2" ]; then
        echo "Error: One or both files not found"
        exit 1
    fi
    
    echo "Comparing settings files:"
    echo "  File 1: $file1"
    echo "  File 2: $file2"
    echo ""
    
    # Extract and compare settings
    local temp1=$(mktemp)
    local temp2=$(mktemp)
    
    trap 'rm -f "$temp1" "$temp2"' EXIT
    
    jq -S '.settings' "$file1" > "$temp1"
    jq -S '.settings' "$file2" > "$temp2"
    
    if diff -u "$temp1" "$temp2"; then
        echo "Files are identical."
    else
        echo ""
        echo "Use 'diff -u <(jq .settings \"$file1\") <(jq .settings \"$file2\")' for detailed comparison."
    fi
}

# Main script logic
case "${1:-}" in
    backup)
        test_bitcoin_cli
        backup_settings "$2"
        ;;
    restore)
        test_bitcoin_cli
        restore_settings "$2"
        ;;
    list)
        list_backups
        ;;
    compare)
        compare_settings "$2" "$3"
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        echo "Error: Invalid command"
        echo ""
        show_help
        exit 1
        ;;
esac