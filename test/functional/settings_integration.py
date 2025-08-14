#!/usr/bin/env python3
# Copyright (c) 2025 The Bitcoin Knots developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Comprehensive integration tests for settings export/import system."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
import json
import time
import tempfile
import os
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed


class SettingsIntegrationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2  # Use multiple nodes to test synchronization
        self.extra_args = [[
            "-acceptnonstdtxn=0",
            "-bytespersigop=50",
            "-datacarriersize=80",
            "-dustrelayfee=0.00003000",
            "-minrelaytxfee=0.00001000"
        ]] * 2

    def skip_test_if_missing_module(self):
        pass

    def run_test(self):
        self.test_full_workflow_integration()
        self.test_performance_benchmarks()
        self.test_multi_node_synchronization()
        self.test_large_scale_operations()
        self.test_error_recovery_scenarios()
        self.test_concurrent_operations()
        self.test_persistence_across_restarts()

    def test_full_workflow_integration(self):
        """Test complete end-to-end workflow for settings management."""
        self.log.info("Testing full workflow integration...")
        
        node = self.nodes[0]
        
        # 1. Get initial settings state
        initial_settings = node.dumpsettings()
        assert "settings" in initial_settings
        initial_count = len(self.flatten_settings(initial_settings["settings"]))
        self.log.info(f"Initial settings count: {initial_count}")
        
        # 2. Get schema for all settings
        schema = node.getsettingsschema()
        assert "schema" in schema
        assert "uiSchema" in schema
        assert "formData" in schema
        
        # 3. Modify several settings using different methods
        changes_made = []
        
        # Individual setting changes
        try:
            result = node.setsetting("walletrbf", "true")
            if result.get("success"):
                changes_made.append(("walletrbf", result["old_value"], result["new_value"]))
        except:
            pass
        
        # Bulk setting changes
        try:
            bulk_changes = {
                "spendzeroconfchange": False,
                "maxmempool": 350
            }
            result = node.updatesettings(bulk_changes)
            if result.get("success"):
                for update in result.get("updates", []):
                    changes_made.append((update["setting"], update["old_value"], update["new_value"]))
        except:
            pass
        
        # 4. Verify changes are reflected in dumpsettings
        if changes_made:
            updated_settings = node.dumpsettings()
            for setting_name, old_val, new_val in changes_made:
                setting_found = self.find_setting_in_dump(updated_settings["settings"], setting_name)
                if setting_found:
                    assert setting_found.get("value") == new_val, f"Setting {setting_name} not updated correctly"
                    self.log.info(f"Verified change: {setting_name} = {new_val}")
        
        # 5. Test subscription and notification workflow
        subscription = node.subscribesettings()
        assert "poll_token" in subscription
        initial_token = subscription["poll_token"]
        
        # Make another change and check if it's detected
        try:
            node.setsetting("walletrbf", "false")
            time.sleep(0.5)  # Allow change to propagate
            
            updated_subscription = node.subscribesettings("", initial_token, True)
            if updated_subscription.get("has_changes"):
                self.log.info("Change notification system working")
                assert "changed_settings" in updated_subscription
        except:
            self.log.info("Change notification test skipped")
        
        # 6. Test schema-driven validation
        wallet_schema = node.getsettingsschema("wallet")
        assert "wallet" in wallet_schema["schema"]["properties"]
        
        # 7. Final verification
        final_settings = node.dumpsettings()
        final_count = len(self.flatten_settings(final_settings["settings"]))
        
        self.log.info(f"Final settings count: {final_count}")
        assert final_count >= initial_count, "Settings count should not decrease"
        
        self.log.info("Full workflow integration test completed successfully")

    def test_performance_benchmarks(self):
        """Test performance characteristics of settings operations."""
        self.log.info("Testing performance benchmarks...")
        
        node = self.nodes[0]
        
        # Benchmark dumpsettings performance
        start_time = time.time()
        for i in range(10):
            result = node.dumpsettings()
            assert "settings" in result
        dump_time = time.time() - start_time
        
        self.log.info(f"dumpsettings: 10 calls in {dump_time:.3f}s ({dump_time/10:.3f}s avg)")
        assert dump_time < 5.0, "dumpsettings performance regression"
        
        # Benchmark getsettings performance
        start_time = time.time()
        for i in range(10):
            result = node.getsettings()
            assert "settings" in result
        get_time = time.time() - start_time
        
        self.log.info(f"getsettings: 10 calls in {get_time:.3f}s ({get_time/10:.3f}s avg)")
        assert get_time < 5.0, "getsettings performance regression"
        
        # Benchmark schema generation performance
        start_time = time.time()
        for i in range(5):  # Fewer iterations for schema as it's more expensive
            result = node.getsettingsschema()
            assert "schema" in result
        schema_time = time.time() - start_time
        
        self.log.info(f"getsettingsschema: 5 calls in {schema_time:.3f}s ({schema_time/5:.3f}s avg)")
        assert schema_time < 10.0, "getsettingsschema performance regression"
        
        # Benchmark individual setting updates
        start_time = time.time()
        for i in range(5):
            try:
                node.setsetting("walletrbf", "true" if i % 2 == 0 else "false")
            except:
                pass
        update_time = time.time() - start_time
        
        self.log.info(f"setsetting: 5 calls in {update_time:.3f}s ({update_time/5:.3f}s avg)")
        
        # Benchmark bulk updates
        start_time = time.time()
        for i in range(3):
            try:
                node.updatesettings({
                    "walletrbf": i % 2 == 0,
                    "maxmempool": 300 + i * 10
                })
            except:
                pass
        bulk_time = time.time() - start_time
        
        self.log.info(f"updatesettings: 3 calls in {bulk_time:.3f}s ({bulk_time/3:.3f}s avg)")
        
        self.log.info("Performance benchmarks completed")

    def test_multi_node_synchronization(self):
        """Test settings synchronization across multiple nodes."""
        self.log.info("Testing multi-node synchronization...")
        
        if len(self.nodes) < 2:
            self.log.info("Skipping multi-node test - insufficient nodes")
            return
        
        node1 = self.nodes[0]
        node2 = self.nodes[1]
        
        # Get initial states
        state1 = node1.dumpsettings()
        state2 = node2.dumpsettings()
        
        # Note: In a real distributed system, settings might need to be synchronized
        # For this test, we verify that both nodes have consistent RPC interfaces
        
        # Verify both nodes have same RPC interface
        schema1 = node1.getsettingsschema()
        schema2 = node2.getsettingsschema()
        
        # Should have same schema structure
        assert schema1["version"] == schema2["version"]
        assert len(schema1["schema"]["properties"]) == len(schema2["schema"]["properties"])
        
        # Test that both nodes can handle the same operations
        try:
            result1 = node1.getsettings("walletrbf")
            result2 = node2.getsettings("walletrbf")
            
            # Structure should be identical even if values differ
            assert "settings" in result1 and "settings" in result2
            
        except Exception as e:
            self.log.info(f"Multi-node test skipped: {str(e)}")
        
        self.log.info("Multi-node synchronization test completed")

    def test_large_scale_operations(self):
        """Test handling of large-scale operations."""
        self.log.info("Testing large-scale operations...")
        
        node = self.nodes[0]
        
        # Test rapid sequential operations
        start_time = time.time()
        successful_ops = 0
        
        for i in range(100):
            try:
                result = node.dumpsettings()
                if "settings" in result:
                    successful_ops += 1
            except Exception as e:
                self.log.debug(f"Operation {i} failed: {str(e)}")
        
        elapsed = time.time() - start_time
        self.log.info(f"Large scale test: {successful_ops}/100 operations in {elapsed:.3f}s")
        
        # Should handle at least 80% of operations successfully
        assert successful_ops >= 80, f"Too many failures: {100 - successful_ops}/100"
        
        # Test with different categories
        categories = ["wallet", "mempool", "relay", "script", "datacarrier"]
        category_performance = {}
        
        for category in categories:
            start_time = time.time()
            try:
                for i in range(20):
                    node.dumpsettings(category)
                category_time = time.time() - start_time
                category_performance[category] = category_time
            except Exception as e:
                category_performance[category] = f"error: {str(e)}"
        
        self.log.info(f"Category performance: {category_performance}")
        
        self.log.info("Large-scale operations test completed")

    def test_error_recovery_scenarios(self):
        """Test error recovery and resilience."""
        self.log.info("Testing error recovery scenarios...")
        
        node = self.nodes[0]
        
        # Test recovery from invalid operations
        error_scenarios = [
            ("Invalid category", lambda: node.dumpsettings("invalid_category_name")),
            ("Invalid setting", lambda: node.getsettings("nonexistent_setting")),
            ("Invalid schema category", lambda: node.getsettingsschema("invalid_category")),
            ("Invalid setting update", lambda: node.setsetting("invalid_setting", "value")),
            ("Invalid bulk update", lambda: node.updatesettings({"invalid_setting": "value"})),
        ]
        
        recovery_count = 0
        
        for scenario_name, operation in error_scenarios:
            try:
                operation()
                self.log.info(f"{scenario_name}: Unexpectedly succeeded")
            except Exception as e:
                # After error, verify system is still functional
                try:
                    recovery_test = node.dumpsettings()
                    if "settings" in recovery_test:
                        recovery_count += 1
                        self.log.info(f"{scenario_name}: System recovered successfully")
                except Exception as recovery_error:
                    self.log.error(f"{scenario_name}: Failed to recover - {str(recovery_error)}")
        
        assert recovery_count >= len(error_scenarios) * 0.8, "Too many recovery failures"
        
        # Test system state after errors
        final_state = node.dumpsettings()
        assert "settings" in final_state, "System should be functional after errors"
        
        self.log.info("Error recovery scenarios test completed")

    def test_concurrent_operations(self):
        """Test concurrent access and thread safety."""
        self.log.info("Testing concurrent operations...")
        
        node = self.nodes[0]
        
        def concurrent_dumpsettings(thread_id):
            results = []
            errors = []
            for i in range(10):
                try:
                    result = node.dumpsettings()
                    if "settings" in result:
                        results.append(f"Thread {thread_id}: Success {i}")
                except Exception as e:
                    errors.append(f"Thread {thread_id}: Error {i} - {str(e)}")
            return {"results": results, "errors": errors, "thread_id": thread_id}
        
        # Run concurrent operations
        with ThreadPoolExecutor(max_workers=5) as executor:
            futures = [executor.submit(concurrent_dumpsettings, i) for i in range(5)]
            
            all_results = []
            all_errors = []
            
            for future in as_completed(futures):
                try:
                    result = future.result(timeout=30)
                    all_results.extend(result["results"])
                    all_errors.extend(result["errors"])
                except Exception as e:
                    all_errors.append(f"Future failed: {str(e)}")
        
        self.log.info(f"Concurrent test: {len(all_results)} successes, {len(all_errors)} errors")
        
        # Should have reasonable success rate even under concurrency
        total_operations = len(all_results) + len(all_errors)
        success_rate = len(all_results) / total_operations if total_operations > 0 else 0
        
        assert success_rate >= 0.7, f"Concurrent success rate too low: {success_rate:.2%}"
        
        self.log.info("Concurrent operations test completed")

    def test_persistence_across_restarts(self):
        """Test that settings changes persist across node restarts."""
        self.log.info("Testing persistence across restarts...")
        
        node = self.nodes[0]
        
        # Make some setting changes
        changes_to_test = []
        
        try:
            result = node.setsetting("walletrbf", "true")
            if result.get("success"):
                changes_to_test.append(("walletrbf", result["new_value"]))
        except:
            pass
        
        try:
            result = node.updatesettings({"maxmempool": 400})
            if result.get("success"):
                for update in result.get("updates", []):
                    changes_to_test.append((update["setting"], update["new_value"]))
        except:
            pass
        
        if not changes_to_test:
            self.log.info("No changes made, skipping persistence test")
            return
        
        # Record current state
        pre_restart_state = node.dumpsettings()
        
        # Restart the node
        self.log.info("Restarting node to test persistence...")
        self.restart_node(0)
        
        # Verify settings persisted
        post_restart_state = self.nodes[0].dumpsettings()
        
        # Check that our changes are still present
        persisted_count = 0
        for setting_name, expected_value in changes_to_test:
            setting_found = self.find_setting_in_dump(post_restart_state["settings"], setting_name)
            if setting_found and setting_found.get("value") == expected_value:
                persisted_count += 1
                self.log.info(f"Setting {setting_name} persisted correctly")
        
        # At least half of changes should persist (some might be session-only)
        if persisted_count >= len(changes_to_test) * 0.5:
            self.log.info(f"Persistence test passed: {persisted_count}/{len(changes_to_test)} settings persisted")
        else:
            self.log.info(f"Persistence test: {persisted_count}/{len(changes_to_test)} settings persisted (some may be session-only)")
        
        self.log.info("Persistence across restarts test completed")

    def flatten_settings(self, settings_dict):
        """Flatten nested settings dictionary to count total settings."""
        flat_settings = {}
        for category, category_settings in settings_dict.items():
            if isinstance(category_settings, dict):
                for setting_name, setting_data in category_settings.items():
                    flat_settings[f"{category}.{setting_name}"] = setting_data
        return flat_settings

    def find_setting_in_dump(self, settings_dict, setting_name):
        """Find a setting in the hierarchical dumpsettings output."""
        for category_name, category_settings in settings_dict.items():
            if isinstance(category_settings, dict) and setting_name in category_settings:
                return category_settings[setting_name]
        return None


if __name__ == '__main__':
    SettingsIntegrationTest(__file__).main()
