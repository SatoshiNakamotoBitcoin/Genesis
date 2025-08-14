// Copyright (c) 2025 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_SETTINGS_NOTIFICATIONS_H
#define BITCOIN_INTERFACES_SETTINGS_NOTIFICATIONS_H

#include <functional>
#include <memory>
#include <string>
#include <univalue.h>

namespace interfaces {

/**
 * Settings notification interface to keep UI clients synchronized with node settings.
 * Provides real-time notifications when settings change via RPC or other mechanisms.
 */
class SettingsNotifications
{
public:
    virtual ~SettingsNotifications() = default;

    /**
     * Subscribe to setting change notifications
     * @param callback Function called when a setting changes
     * @param category Optional category filter (e.g., "wallet", "mempool")
     * @return Subscription handle for managing the subscription
     */
    virtual std::unique_ptr<void, std::function<void(void*)>> Subscribe(
        std::function<void(const std::string& setting_name, 
                          const UniValue& old_value, 
                          const UniValue& new_value,
                          const std::string& source)> callback,
        const std::string& category = "") = 0;

    /**
     * Notify all subscribers of a setting change
     * @param setting_name Name of the setting that changed
     * @param old_value Previous value of the setting
     * @param new_value New value of the setting  
     * @param source Source of the change (e.g., "RPC", "GUI", "Config")
     */
    virtual void NotifySettingChanged(const std::string& setting_name,
                                    const UniValue& old_value,
                                    const UniValue& new_value,
                                    const std::string& source) = 0;

    /**
     * Get the number of active subscribers
     * @return Number of active subscriptions
     */
    virtual size_t GetSubscriberCount() const = 0;

    /**
     * Check if a setting category has any subscribers
     * @param category Setting category to check
     * @return True if category has subscribers
     */
    virtual bool HasCategorySubscribers(const std::string& category) const = 0;
};

/**
 * Create a settings notifications implementation
 * @return Unique pointer to settings notifications instance
 */
std::unique_ptr<SettingsNotifications> MakeSettingsNotifications();

} // namespace interfaces

#endif // BITCOIN_INTERFACES_SETTINGS_NOTIFICATIONS_H