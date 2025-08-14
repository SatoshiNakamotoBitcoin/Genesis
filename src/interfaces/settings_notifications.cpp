// Copyright (c) 2025 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/settings_notifications.h>

#include <logging.h>
#include <sync.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace interfaces {

namespace {

struct Subscription {
    std::function<void(const std::string&, const UniValue&, const UniValue&, const std::string&)> callback;
    std::string category_filter;
    
    Subscription(decltype(callback) cb, std::string category) 
        : callback(std::move(cb)), category_filter(std::move(category)) {}
};

class SettingsNotificationsImpl : public SettingsNotifications
{
private:
    mutable Mutex m_mutex;
    std::vector<std::shared_ptr<Subscription>> m_subscriptions GUARDED_BY(m_mutex);
    
    static void SubscriptionDeleter(void* ptr) {
        auto* impl = static_cast<SettingsNotificationsImpl*>(ptr);
        // The actual subscription cleanup is handled by the shared_ptr
        // This deleter is just for the interface contract
    }
    
public:
    std::unique_ptr<void, std::function<void(void*)>> Subscribe(
        std::function<void(const std::string&, const UniValue&, const UniValue&, const std::string&)> callback,
        const std::string& category) override
    {
        auto subscription = std::make_shared<Subscription>(std::move(callback), category);
        
        {
            LOCK(m_mutex);
            m_subscriptions.push_back(subscription);
        }
        
        LogPrint(BCLog::RPC, "Settings notification subscription added for category: %s\n", 
                category.empty() ? "all" : category);
        
        // Return a handle that will clean up the subscription when destroyed
        return std::unique_ptr<void, std::function<void(void*)>>(
            this,
            [subscription](void*) {
                // Keep the subscription alive until this deleter is called
                // The subscription will be automatically removed from the vector
                // when its reference count drops to zero
            }
        );
    }
    
    void NotifySettingChanged(const std::string& setting_name,
                            const UniValue& old_value,
                            const UniValue& new_value,
                            const std::string& source) override
    {
        std::vector<std::shared_ptr<Subscription>> subscriptions_copy;
        
        {
            LOCK(m_mutex);
            // Clean up expired subscriptions
            m_subscriptions.erase(
                std::remove_if(m_subscriptions.begin(), m_subscriptions.end(),
                    [](const std::weak_ptr<Subscription>& wp) { return wp.expired(); }),
                m_subscriptions.end()
            );
            
            // Copy active subscriptions for processing outside the lock
            subscriptions_copy = m_subscriptions;
        }
        
        LogPrint(BCLog::RPC, "Settings notification: %s changed from %s to %s (source: %s)\n",
                setting_name, old_value.write(), new_value.write(), source);
        
        // Determine the setting category for filtering
        std::string setting_category = GetSettingCategory(setting_name);
        
        // Notify all relevant subscribers
        for (const auto& subscription : subscriptions_copy) {
            if (!subscription) continue;
            
            // Check if this subscription should receive this notification
            if (subscription->category_filter.empty() || 
                subscription->category_filter == setting_category) {
                
                try {
                    subscription->callback(setting_name, old_value, new_value, source);
                } catch (const std::exception& e) {
                    LogPrint(BCLog::RPC, "Error in settings notification callback: %s\n", e.what());
                }
            }
        }
    }
    
    size_t GetSubscriberCount() const override
    {
        LOCK(m_mutex);
        // Clean up expired subscriptions before counting
        auto& subscriptions = const_cast<std::vector<std::shared_ptr<Subscription>>&>(m_subscriptions);
        subscriptions.erase(
            std::remove_if(subscriptions.begin(), subscriptions.end(),
                [](const std::shared_ptr<Subscription>& sp) { return !sp; }),
            subscriptions.end()
        );
        return m_subscriptions.size();
    }
    
    bool HasCategorySubscribers(const std::string& category) const override
    {
        LOCK(m_mutex);
        return std::any_of(m_subscriptions.begin(), m_subscriptions.end(),
            [&category](const std::shared_ptr<Subscription>& subscription) {
                return subscription && 
                       (subscription->category_filter.empty() || 
                        subscription->category_filter == category);
            });
    }
    
private:
    static std::string GetSettingCategory(const std::string& setting_name)
    {
        // Map setting names to categories
        if (setting_name.find("wallet") != std::string::npos ||
            setting_name == "walletrbf" || setting_name == "spendzeroconfchange") {
            return "wallet";
        }
        if (setting_name.find("mempool") != std::string::npos ||
            setting_name == "maxmempool" || setting_name == "mempoolreplacement" ||
            setting_name == "maxorphantx" || setting_name == "mempoolexpiry") {
            return "mempool";
        }
        if (setting_name.find("relay") != std::string::npos ||
            setting_name == "minrelaytxfee" || setting_name == "incrementalrelayfee") {
            return "relay";
        }
        if (setting_name.find("script") != std::string::npos ||
            setting_name.find("reject") != std::string::npos) {
            return "script";
        }
        if (setting_name.find("block") != std::string::npos) {
            return "block_creation";
        }
        if (setting_name.find("dust") != std::string::npos) {
            return "dust";
        }
        if (setting_name.find("datacarrier") != std::string::npos) {
            return "data_carrier";
        }
        
        return "unknown";
    }
};

} // anonymous namespace

std::unique_ptr<SettingsNotifications> MakeSettingsNotifications()
{
    return std::make_unique<SettingsNotificationsImpl>();
}

} // namespace interfaces