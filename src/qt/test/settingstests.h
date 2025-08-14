// Copyright (c) 2025 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_SETTINGSTESTS_H
#define BITCOIN_QT_TEST_SETTINGSTESTS_H

#include <QObject>
#include <QTest>

/**
 * Test settings export/import functionality and GUI integration.
 */
class SettingsTests : public QObject
{
    Q_OBJECT

public:
    explicit SettingsTests(QObject* parent = nullptr);

private Q_SLOTS:
    void settingsExportImportTests();
    void settingsValidationTests();
    void settingsDialogTests();
    void settingsSynchronizationTests();
    void settingsErrorHandlingTests();
    void settingsPerformanceTests();
    void settingsDataIntegrityTests();
};

#endif // BITCOIN_QT_TEST_SETTINGSTESTS_H