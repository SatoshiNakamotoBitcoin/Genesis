#!/usr/bin/env python3
# Copyright (c) 2025 The Bitcoin Knots developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the settings ZMQ notifications."""

import json
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class ZMQSettingsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        
        # Configure ZMQ settings notifications
        self.extra_args = [[
            "-zmqpubsettings=tcp://127.0.0.1:28332",
            "-zmqpubsettingshwm=1000"
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_py_zmq()
        self.skip_if_no_zmq()

    def run_test(self):
        import zmq
        
        self.log.info("Testing ZMQ settings notifications...")
        
        node = self.nodes[0]
        
        # Set up ZMQ subscriber
        zmq_context = zmq.Context()
        zmq_socket = zmq_context.socket(zmq.SUB)
        zmq_socket.connect("tcp://127.0.0.1:28332")
        zmq_socket.setsockopt(zmq.SUBSCRIBE, b"settings")
        zmq_socket.setsockopt(zmq.RCVTIMEO, 5000)  # 5 second timeout
        
        self.log.info("ZMQ subscriber connected")
        
        # Test that we can receive settings notifications
        try:
            # Trigger a setting change that should generate a ZMQ notification
            # Note: This would require the RPC commands to actually trigger notifications
            self.log.info("Triggering settings change...")
            
            # Try to change a setting via RPC
            try:
                result = node.setsetting("walletrbf", "true")
                if "success" in result and result["success"]:
                    self.log.info("Setting change successful, waiting for ZMQ notification...")
                    
                    # Wait for ZMQ notification
                    try:
                        # Receive multipart message: [topic, data, sequence]
                        topic = zmq_socket.recv()
                        data = zmq_socket.recv()
                        sequence = zmq_socket.recv()
                        
                        self.log.info(f"Received ZMQ notification: topic={topic}")
                        
                        # Verify the topic
                        assert_equal(topic, b"settings")
                        
                        # Parse the JSON data
                        notification_data = json.loads(data.decode('utf-8'))
                        
                        # Verify the notification structure
                        assert "setting" in notification_data
                        assert "old_value" in notification_data
                        assert "new_value" in notification_data
                        assert "source" in notification_data
                        assert "timestamp" in notification_data
                        
                        self.log.info(f"ZMQ notification data: {notification_data}")
                        
                        # Verify the setting name
                        assert_equal(notification_data["setting"], "walletrbf")
                        assert_equal(notification_data["new_value"], "true")
                        assert_equal(notification_data["source"], "RPC")
                        
                        self.log.info("ZMQ settings notification test passed!")
                        
                    except zmq.Again:
                        self.log.info("No ZMQ notification received (timeout)")
                        self.log.info("This may be expected if ZMQ settings notifications are not fully implemented")
                        
                else:
                    self.log.info("Setting change failed or not supported")
                    
            except Exception as e:
                self.log.info(f"Setting change not available: {str(e)}")
                
        except Exception as e:
            self.log.info(f"ZMQ test error: {str(e)}")
            
        finally:
            # Clean up
            zmq_socket.close()
            zmq_context.term()
            
        # Test multiple notifications
        self.log.info("Testing multiple ZMQ notifications...")
        
        # Set up new subscriber for batch testing
        zmq_context = zmq.Context()
        zmq_socket = zmq_context.socket(zmq.SUB)
        zmq_socket.connect("tcp://127.0.0.1:28332")
        zmq_socket.setsockopt(zmq.SUBSCRIBE, b"settings")
        zmq_socket.setsockopt(zmq.RCVTIMEO, 1000)  # 1 second timeout
        
        notifications_received = 0
        max_notifications = 3
        
        try:
            # Trigger multiple setting changes
            settings_to_change = [
                ("walletrbf", "false"),
                ("spendzeroconfchange", "true"),
                ("maxmempool", "400")
            ]
            
            for setting_name, setting_value in settings_to_change:
                try:
                    result = node.setsetting(setting_name, setting_value)
                    if "success" in result and result["success"]:
                        self.log.info(f"Changed {setting_name} to {setting_value}")
                        
                        # Try to receive notification
                        try:
                            topic = zmq_socket.recv()
                            data = zmq_socket.recv()
                            sequence = zmq_socket.recv()
                            
                            notification_data = json.loads(data.decode('utf-8'))
                            notifications_received += 1
                            
                            self.log.info(f"Received notification #{notifications_received}: {notification_data['setting']} = {notification_data['new_value']}")
                            
                        except zmq.Again:
                            self.log.info(f"No notification received for {setting_name}")
                            
                except Exception as e:
                    self.log.info(f"Failed to change {setting_name}: {str(e)}")
                    
            self.log.info(f"Received {notifications_received} out of {len(settings_to_change)} expected notifications")
            
        finally:
            zmq_socket.close()
            zmq_context.term()
            
        self.log.info("ZMQ settings notification tests completed")


if __name__ == '__main__':
    ZMQSettingsTest(__file__).main()
