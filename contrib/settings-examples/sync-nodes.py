#!/usr/bin/env python3
"""
Bitcoin Knots Node Settings Synchronization Tool

This script synchronizes settings between Bitcoin Knots nodes using the
settings export RPC commands.

Usage:
    python sync-nodes.py SOURCE_URL TARGET_URL [options]

Examples:
    python sync-nodes.py http://user:pass@node1:8332 http://user:pass@node2:8332
    python sync-nodes.py node1:8332 node2:8332 --user=admin --password=secret
    python sync-nodes.py node1:8332 node2:8332 --category=wallet --dry-run
"""

import argparse
import json
import sys
import time
from urllib.parse import urlparse
import requests
from typing import Dict, List, Optional, Any


class BitcoinRPC:
    """Simple Bitcoin Core RPC client"""
    
    def __init__(self, url: str, username: str = None, password: str = None):
        self.url = url if url.startswith('http') else f'http://{url}'
        
        parsed = urlparse(self.url)
        self.username = username or parsed.username or 'rpcuser'
        self.password = password or parsed.password or 'rpcpass'
        
        # Clean URL for requests
        if '@' in self.url:
            self.url = f"{parsed.scheme}://{parsed.netloc.split('@')[1]}{parsed.path}"
    
    def call(self, method: str, params: List[Any] = None) -> Any:
        """Make RPC call and return result"""
        if params is None:
            params = []
            
        payload = {
            'jsonrpc': '2.0',
            'method': method,
            'params': params,
            'id': int(time.time() * 1000)
        }
        
        try:
            response = requests.post(
                self.url,
                json=payload,
                auth=(self.username, self.password),
                timeout=30
            )
            response.raise_for_status()
            
            data = response.json()
            if 'error' in data and data['error']:
                raise Exception(f"RPC Error: {data['error']['message']}")
            
            return data.get('result')
            
        except requests.RequestException as e:
            raise Exception(f"Connection error: {e}")
    
    def test_connection(self) -> bool:
        """Test if RPC connection works"""
        try:
            self.call('getblockchaininfo')
            return True
        except Exception:
            return False


class SettingsSync:
    """Settings synchronization manager"""
    
    def __init__(self, source_rpc: BitcoinRPC, target_rpc: BitcoinRPC):
        self.source = source_rpc
        self.target = target_rpc
    
    def get_settings(self, rpc: BitcoinRPC, category: str = None) -> Dict:
        """Get settings from a node"""
        params = [category] if category else []
        return rpc.call('dumpsettings', params)
    
    def update_settings(self, rpc: BitcoinRPC, settings: Dict) -> Dict:
        """Update settings on a node"""
        return rpc.call('updatesettings', [settings])
    
    def compare_settings(self, source_settings: Dict, target_settings: Dict) -> Dict:
        """Compare settings between nodes and return differences"""
        differences = {
            'added': {},
            'modified': {},
            'removed': {}
        }
        
        source_flat = self._flatten_settings(source_settings.get('settings', {}))
        target_flat = self._flatten_settings(target_settings.get('settings', {}))
        
        # Find added and modified settings
        for key, value in source_flat.items():
            if key not in target_flat:
                differences['added'][key] = value
            elif target_flat[key] != value:
                differences['modified'][key] = {
                    'old': target_flat[key],
                    'new': value
                }
        
        # Find removed settings
        for key, value in target_flat.items():
            if key not in source_flat:
                differences['removed'][key] = value
        
        return differences
    
    def _flatten_settings(self, settings: Dict, prefix: str = '') -> Dict:
        """Flatten nested settings dictionary"""
        flat = {}
        for key, value in settings.items():
            full_key = f"{prefix}.{key}" if prefix else key
            if isinstance(value, dict) and 'value' in value:
                flat[full_key] = value['value']
            elif isinstance(value, dict):
                flat.update(self._flatten_settings(value, full_key))
            else:
                flat[full_key] = value
        return flat
    
    def sync(self, category: str = None, dry_run: bool = False, force: bool = False) -> Dict:
        """Synchronize settings from source to target"""
        print(f"Fetching settings from source node...")
        source_settings = self.get_settings(self.source, category)
        
        print(f"Fetching settings from target node...")
        target_settings = self.get_settings(self.target, category)
        
        # Compare settings
        differences = self.compare_settings(source_settings, target_settings)
        
        total_changes = (len(differences['added']) + 
                        len(differences['modified']) + 
                        len(differences['removed']))
        
        if total_changes == 0:
            print("Nodes are already synchronized")
            return {'status': 'no_changes', 'differences': differences}
        
        # Show differences
        print(f"\nFound {total_changes} differences:")
        
        if differences['added']:
            print(f"\n  Added settings ({len(differences['added'])}):")
            for key, value in differences['added'].items():
                print(f"    + {key}: {value}")
        
        if differences['modified']:
            print(f"\n  Modified settings ({len(differences['modified'])}):")
            for key, change in differences['modified'].items():
                print(f"    ~ {key}: {change['old']} → {change['new']}")
        
        if differences['removed']:
            print(f"\n  Removed settings ({len(differences['removed'])}):")
            for key, value in differences['removed'].items():
                print(f"    - {key}: {value}")
        
        if dry_run:
            print("\nDry run mode - no changes applied")
            return {'status': 'dry_run', 'differences': differences}
        
        # Confirm changes
        if not force:
            print(f"\nApply these changes to target node? (y/N): ", end='')
            if input().lower() not in ['y', 'yes']:
                print("Sync cancelled")
                return {'status': 'cancelled', 'differences': differences}
        
        # Apply changes
        print("\nApplying settings to target node...")
        try:
            result = self.update_settings(self.target, source_settings['settings'])
            
            # Check for restart requirements
            restart_required = source_settings.get('metadata', {}).get('restart_required', [])
            if restart_required:
                print(f"\nWARNING: The following settings require a restart:")
                for setting in restart_required:
                    print(f"    - {setting}")
                print("   Please restart the target node when convenient.")
            
            print("Settings synchronized successfully")
            return {'status': 'success', 'differences': differences, 'result': result}
            
        except Exception as e:
            print(f"Failed to apply settings: {e}")
            return {'status': 'error', 'error': str(e), 'differences': differences}


def main():
    parser = argparse.ArgumentParser(
        description='Synchronize Bitcoin Knots settings between nodes',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    
    parser.add_argument('source', help='Source node URL')
    parser.add_argument('target', help='Target node URL')
    parser.add_argument('--user', help='RPC username')
    parser.add_argument('--password', help='RPC password')
    parser.add_argument('--category', help='Sync only specific category')
    parser.add_argument('--dry-run', action='store_true', help='Show changes without applying')
    parser.add_argument('--force', action='store_true', help='Apply changes without confirmation')
    parser.add_argument('--verbose', action='store_true', help='Verbose output')
    
    args = parser.parse_args()
    
    try:
        # Create RPC clients
        source_rpc = BitcoinRPC(args.source, args.user, args.password)
        target_rpc = BitcoinRPC(args.target, args.user, args.password)
        
        # Test connections
        print("Testing connections...")
        if not source_rpc.test_connection():
            print(f"Cannot connect to source node: {args.source}")
            sys.exit(1)
        
        if not target_rpc.test_connection():
            print(f"Cannot connect to target node: {args.target}")
            sys.exit(1)
        
        print("Connected to both nodes")
        
        # Perform sync
        sync = SettingsSync(source_rpc, target_rpc)
        result = sync.sync(
            category=args.category,
            dry_run=args.dry_run,
            force=args.force
        )
        
        if args.verbose:
            print(f"\nSync result: {json.dumps(result, indent=2)}")
        
        # Exit codes
        if result['status'] == 'success':
            sys.exit(0)
        elif result['status'] in ['no_changes', 'dry_run']:
            sys.exit(0)
        elif result['status'] == 'cancelled':
            sys.exit(1)
        else:  # error
            sys.exit(2)
            
    except KeyboardInterrupt:
        print("\n\nSync interrupted by user")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(2)


if __name__ == '__main__':
    main()