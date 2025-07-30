// Copyright (c) 2025 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <config/bitcoin-config.h>

#include <interfaces/settings_notifications.h>
#include <node/interface_ui.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/time.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

BOOST_FIXTURE_TEST_SUITE(settings_notifications_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(settings_notification_subscription)
{
    // Test basic subscription functionality
    auto notifications = interfaces::MakeSettingsNotifications();
    BOOST_CHECK(notifications != nullptr);
    
    // Test initial state
    BOOST_CHECK_EQUAL(notifications->GetSubscriberCount(), 0);
    BOOST_CHECK(!notifications->HasCategorySubscribers("wallet"));
    
    // Test subscription
    std::atomic<bool> callback_called{false};
    std::string received_setting;
    UniValue received_old_value, received_new_value;
    std::string received_source;
    
    auto subscription = notifications->Subscribe(
        [&](const std::string& setting_name, 
            const UniValue& old_value, 
            const UniValue& new_value,
            const std::string& source) {
            callback_called = true;
            received_setting = setting_name;
            received_old_value = old_value;
            received_new_value = new_value;
            received_source = source;
        },
        "wallet"
    );
    
    BOOST_CHECK(subscription != nullptr);
    BOOST_CHECK_EQUAL(notifications->GetSubscriberCount(), 1);
    BOOST_CHECK(notifications->HasCategorySubscribers("wallet"));
    
    // Test notification
    UniValue old_val(false);
    UniValue new_val(true);
    notifications->NotifySettingChanged("walletrbf", old_val, new_val, "TEST");
    
    // Give some time for async processing
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    BOOST_CHECK(callback_called);
    BOOST_CHECK_EQUAL(received_setting, "walletrbf");
    BOOST_CHECK_EQUAL(received_old_value.get_bool(), false);
    BOOST_CHECK_EQUAL(received_new_value.get_bool(), true);
    BOOST_CHECK_EQUAL(received_source, "TEST");
    
    // Test subscription cleanup
    subscription.reset();
    BOOST_CHECK_EQUAL(notifications->GetSubscriberCount(), 0);
    BOOST_CHECK(!notifications->HasCategorySubscribers("wallet"));
}

BOOST_AUTO_TEST_CASE(settings_notification_category_filtering)
{
    auto notifications = interfaces::MakeSettingsNotifications();
    
    std::atomic<int> wallet_callbacks{0};
    std::atomic<int> mempool_callbacks{0};
    std::atomic<int> all_callbacks{0};
    
    // Subscribe to wallet category only
    auto wallet_sub = notifications->Subscribe(
        [&](const std::string&, const UniValue&, const UniValue&, const std::string&) {
            wallet_callbacks++;
        },
        "wallet"
    );
    
    // Subscribe to mempool category only
    auto mempool_sub = notifications->Subscribe(
        [&](const std::string&, const UniValue&, const UniValue&, const std::string&) {
            mempool_callbacks++;
        },
        "mempool"
    );
    
    // Subscribe to all categories
    auto all_sub = notifications->Subscribe(
        [&](const std::string&, const UniValue&, const UniValue&, const std::string&) {
            all_callbacks++;
        },
        "" // Empty category means all
    );
    
    BOOST_CHECK_EQUAL(notifications->GetSubscriberCount(), 3);
    
    // Send wallet notification
    UniValue old_val(false), new_val(true);
    notifications->NotifySettingChanged("walletrbf", old_val, new_val, "TEST");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Only wallet and all subscribers should be notified
    BOOST_CHECK_EQUAL(wallet_callbacks.load(), 1);
    BOOST_CHECK_EQUAL(mempool_callbacks.load(), 0);
    BOOST_CHECK_EQUAL(all_callbacks.load(), 1);
    
    // Send mempool notification
    UniValue old_mem(300), new_mem(500);
    notifications->NotifySettingChanged("maxmempool", old_mem, new_mem, "TEST");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Only mempool and all subscribers should be notified
    BOOST_CHECK_EQUAL(wallet_callbacks.load(), 1);
    BOOST_CHECK_EQUAL(mempool_callbacks.load(), 1);
    BOOST_CHECK_EQUAL(all_callbacks.load(), 2);
}

BOOST_AUTO_TEST_CASE(settings_notification_ui_interface_integration)
{
    // Test integration with UI interface signals
    std::atomic<bool> notification_received{false};
    std::string received_setting;
    UniValue received_value;
    
    // Connect to UI interface signal
    auto connection = uiInterface.NotifySettingChanged_connect(
        [&](const std::string& setting_name, const UniValue& new_value) {
            notification_received = true;
            received_setting = setting_name;
            received_value = new_value;
        }
    );
    
    // Trigger notification through UI interface
    UniValue test_value(true);
    uiInterface.NotifySettingChanged("walletrbf", test_value);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    BOOST_CHECK(notification_received);
    BOOST_CHECK_EQUAL(received_setting, "walletrbf");
    BOOST_CHECK_EQUAL(received_value.get_bool(), true);
    
    // Cleanup
    connection.disconnect();
}

BOOST_AUTO_TEST_CASE(settings_notification_multiple_subscribers)
{
    auto notifications = interfaces::MakeSettingsNotifications();
    
    std::atomic<int> callback_count{0};
    const int num_subscribers = 5;
    
    std::vector<decltype(notifications->Subscribe(nullptr, ""))> subscriptions;
    
    // Create multiple subscribers
    for (int i = 0; i < num_subscribers; i++) {
        auto sub = notifications->Subscribe(
            [&](const std::string&, const UniValue&, const UniValue&, const std::string&) {
                callback_count++;
            },
            "wallet"
        );
        subscriptions.push_back(std::move(sub));
    }
    
    BOOST_CHECK_EQUAL(notifications->GetSubscriberCount(), num_subscribers);
    
    // Send one notification
    UniValue old_val(false), new_val(true);
    notifications->NotifySettingChanged("walletrbf", old_val, new_val, "TEST");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // All subscribers should be notified
    BOOST_CHECK_EQUAL(callback_count.load(), num_subscribers);
    
    // Test unsubscribing
    subscriptions.clear();
    BOOST_CHECK_EQUAL(notifications->GetSubscriberCount(), 0);
}

BOOST_AUTO_TEST_CASE(settings_notification_value_types)
{
    auto notifications = interfaces::MakeSettingsNotifications();
    
    struct ReceivedValues {
        std::string setting;
        UniValue old_value;
        UniValue new_value;
        std::string source;
    };
    
    std::vector<ReceivedValues> received;
    
    auto subscription = notifications->Subscribe(
        [&](const std::string& setting_name, 
            const UniValue& old_value, 
            const UniValue& new_value,
            const std::string& source) {
            received.push_back({setting_name, old_value, new_value, source});
        },
        "" // All categories
    );
    
    // Test different value types
    
    // Boolean
    UniValue old_bool(false), new_bool(true);
    notifications->NotifySettingChanged("walletrbf", old_bool, new_bool, "BOOL_TEST");
    
    // Integer
    UniValue old_int(300), new_int(500);
    notifications->NotifySettingChanged("maxmempool", old_int, new_int, "INT_TEST");
    
    // Double
    UniValue old_double(0.0001), new_double(0.0005);
    notifications->NotifySettingChanged("minrelaytxfee", old_double, new_double, "DOUBLE_TEST");
    
    // String
    UniValue old_str("false"), new_str("true");
    notifications->NotifySettingChanged("mempoolreplacement", old_str, new_str, "STRING_TEST");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    BOOST_CHECK_EQUAL(received.size(), 4);
    
    // Verify boolean values
    BOOST_CHECK_EQUAL(received[0].setting, "walletrbf");
    BOOST_CHECK_EQUAL(received[0].old_value.get_bool(), false);
    BOOST_CHECK_EQUAL(received[0].new_value.get_bool(), true);
    BOOST_CHECK_EQUAL(received[0].source, "BOOL_TEST");
    
    // Verify integer values
    BOOST_CHECK_EQUAL(received[1].setting, "maxmempool");
    BOOST_CHECK_EQUAL(received[1].old_value.getInt<int>(), 300);
    BOOST_CHECK_EQUAL(received[1].new_value.getInt<int>(), 500);
    BOOST_CHECK_EQUAL(received[1].source, "INT_TEST");
    
    // Verify double values
    BOOST_CHECK_EQUAL(received[2].setting, "minrelaytxfee");
    BOOST_CHECK_CLOSE(received[2].old_value.get_real(), 0.0001, 0.000001);
    BOOST_CHECK_CLOSE(received[2].new_value.get_real(), 0.0005, 0.000001);
    BOOST_CHECK_EQUAL(received[2].source, "DOUBLE_TEST");
    
    // Verify string values
    BOOST_CHECK_EQUAL(received[3].setting, "mempoolreplacement");
    BOOST_CHECK_EQUAL(received[3].old_value.get_str(), "false");
    BOOST_CHECK_EQUAL(received[3].new_value.get_str(), "true");
    BOOST_CHECK_EQUAL(received[3].source, "STRING_TEST");
}

BOOST_AUTO_TEST_CASE(settings_notification_performance)
{
    auto notifications = interfaces::MakeSettingsNotifications();
    
    std::atomic<int> total_callbacks{0};
    const int num_subscribers = 100;
    const int num_notifications = 1000;
    
    std::vector<decltype(notifications->Subscribe(nullptr, ""))> subscriptions;
    
    // Create many subscribers
    for (int i = 0; i < num_subscribers; i++) {
        auto sub = notifications->Subscribe(
            [&](const std::string&, const UniValue&, const UniValue&, const std::string&) {
                total_callbacks++;
            },
            "wallet"
        );
        subscriptions.push_back(std::move(sub));
    }
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Send many notifications
    for (int i = 0; i < num_notifications; i++) {
        UniValue old_val(i), new_val(i + 1);
        notifications->NotifySettingChanged("test_setting", old_val, new_val, "PERF_TEST");
    }
    
    // Wait for all callbacks to complete
    while (total_callbacks.load() < num_subscribers * num_notifications) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    BOOST_CHECK_EQUAL(total_callbacks.load(), num_subscribers * num_notifications);
    
    // Performance should be reasonable (less than 5 seconds for this test)
    BOOST_CHECK_LT(duration.count(), 5000);
    
    std::cout << "Performance test completed in " << duration.count() 
              << "ms for " << num_subscribers << " subscribers and " 
              << num_notifications << " notifications" << std::endl;
}

BOOST_AUTO_TEST_SUITE_END()