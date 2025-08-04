Settings Export System
-----------------------

Bitcoin Knots now includes a comprehensive settings export system that enables headless deployments to access and replicate the full GUI interface without reimplementation.

### New RPC Commands

- `dumpsettings`: Export settings to JSON format with flexible filtering options (category, specific settings, patterns)
- `getsettingsschema`: Generate JSON Forms compatible schema for automatic UI generation
- `setsettings`: Update one or more settings with validation
- `subscribesettings`: Subscribe to settings changes for polling-based notifications

### Features

- Complete coverage of all policy options available in the GUI
- JSON serialization with proper type handling and validation
- Real-time change notifications via ZMQ (`-zmqpubsettings`)
- GUI export/import functionality in Options dialog
- Comprehensive documentation and examples in `contrib/settings-examples/`

### Integration Benefits

- Headless systems (Start9, Umbrel, etc.) can build responsive configuration interfaces
- Standardized JSON schema enables automatic UI generation
- Settings synchronization between GUI and RPC remains consistent
- Complete audit trail for all configuration changes

See `doc/settings-export.md` for detailed integration guide and `doc/JSON-RPC-interface.md` for RPC command documentation.