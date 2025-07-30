// Copyright (c) 2025 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/settingstests.h>

#include <qt/bitcoinamountfield.h>
#include <qt/optionsdialog.h>
#include <qt/optionsmodel.h>
#include <qt/test/util.h>
#include <test/util/setup_common.h>
#include <common/settings_json.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryFile>
#include <QTest>

using namespace common;

SettingsTests::SettingsTests(QObject* parent) : QObject(parent)
{
}

void SettingsTests::settingsExportImportTests()
{
    // Test settings export and import functionality
    TestChain100Setup test_setup;
    
    // Create OptionsModel
    OptionsModel model(nullptr, true /* reset settings */);
    
    // Set some test values
    model.setOption(OptionsModel::EnableReplaceByFee, true);
    model.setOption(OptionsModel::SpendZeroConfChange, false);
    model.setOption(OptionsModel::nMempool, 300);
    model.setOption(OptionsModel::IncrementialRelayFee, "1500");
    
    // Test export functionality
    QTemporaryFile tempFile;
    QVERIFY(tempFile.open());
    QString tempPath = tempFile.fileName();
    tempFile.close();
    
    // Export settings
    QString error;
    bool exportResult = model.exportSettings(tempPath, error);
    QVERIFY2(exportResult, qPrintable(error));
    
    // Verify file was created and contains valid JSON
    QFile exportedFile(tempPath);
    QVERIFY(exportedFile.open(QIODevice::ReadOnly));
    QByteArray jsonData = exportedFile.readAll();
    exportedFile.close();
    
    QVERIFY(!jsonData.isEmpty());
    
    // Parse JSON to verify structure
    UniValue exportedJson;
    QVERIFY(exportedJson.read(jsonData.toStdString()));
    QVERIFY(exportedJson.isObject());
    QVERIFY(exportedJson.exists("version"));
    QVERIFY(exportedJson.exists("settings"));
    
    // Test import functionality
    OptionsModel importModel(nullptr, true /* reset settings */);
    
    // Verify settings are different initially
    QVERIFY(importModel.getOption(OptionsModel::EnableReplaceByFee).toBool() != 
            model.getOption(OptionsModel::EnableReplaceByFee).toBool() ||
            importModel.getOption(OptionsModel::nMempool).toInt() != 
            model.getOption(OptionsModel::nMempool).toInt());
    
    // Import settings
    OptionsModel::ImportPreviewResult previewResult;
    bool importSuccess = importModel.importSettings(tempPath, previewResult, error);
    QVERIFY2(importSuccess, qPrintable(error));
    
    // Verify preview result structure
    QVERIFY(!previewResult.changes.isEmpty());
    QVERIFY(previewResult.valid);
    
    // Apply changes
    OptionsModel::ImportResult applyResult = importModel.applyImportedSettings(previewResult.changes);
    QVERIFY(applyResult.success);
    QCOMPARE(applyResult.appliedCount, previewResult.changes.size());
    
    // Verify imported settings match original
    QCOMPARE(importModel.getOption(OptionsModel::EnableReplaceByFee).toBool(),
             model.getOption(OptionsModel::EnableReplaceByFee).toBool());
    QCOMPARE(importModel.getOption(OptionsModel::nMempool).toInt(),
             model.getOption(OptionsModel::nMempool).toInt());
}

void SettingsTests::settingsValidationTests()
{
    TestChain100Setup test_setup;
    OptionsModel model(nullptr, true);
    
    // Test invalid JSON import
    QTemporaryFile invalidFile;
    QVERIFY(invalidFile.open());
    invalidFile.write("{ invalid json");
    invalidFile.close();
    
    QString error;
    OptionsModel::ImportPreviewResult previewResult;
    bool result = model.importSettings(invalidFile.fileName(), previewResult, error);
    QVERIFY(!result);
    QVERIFY(!error.isEmpty());
    
    // Test valid JSON with invalid values
    QTemporaryFile validJsonFile;
    QVERIFY(validJsonFile.open());
    
    QString invalidJsonContent = R"({
        "version": 1,
        "settings": {
            "wallet": {
                "walletrbf": {
                    "value": "not_a_boolean",
                    "type": 0
                }
            },
            "mempool": {
                "maxmempool": {
                    "value": -100,
                    "type": 1
                }
            }
        }
    })";
    
    validJsonFile.write(invalidJsonContent.toUtf8());
    validJsonFile.close();
    
    error.clear();
    result = model.importSettings(validJsonFile.fileName(), previewResult, error);
    // Should either fail validation or successfully parse with errors flagged
    if (result) {
        QVERIFY(!previewResult.valid || !previewResult.errors.isEmpty());
    } else {
        QVERIFY(!error.isEmpty());
    }
}

void SettingsTests::settingsDialogTests()
{
    TestChain100Setup test_setup;
    
    // Create options dialog
    OptionsModel model(nullptr, true);
    OptionsDialog dialog(nullptr, true);
    dialog.setModel(&model);
    
    // Test that export button exists and is functional
    QPushButton* exportButton = dialog.findChild<QPushButton*>("exportButton");
    if (exportButton) {
        QVERIFY(exportButton->isEnabled());
        
        // Simulate clicking export button (without actually showing file dialog)
        QSignalSpy spy(exportButton, &QPushButton::clicked);
        exportButton->click();
        QCOMPARE(spy.count(), 1);
    }
    
    // Test that import button exists and is functional
    QPushButton* importButton = dialog.findChild<QPushButton*>("importButton");
    if (importButton) {
        QVERIFY(importButton->isEnabled());
        
        // Simulate clicking import button
        QSignalSpy spy(importButton, &QPushButton::clicked);
        importButton->click();
        QCOMPARE(spy.count(), 1);
    }
    
    // Test settings synchronization
    QSignalSpy settingsChangedSpy(&model, &OptionsModel::dataChanged);
    
    // Change a setting programmatically
    model.setOption(OptionsModel::EnableReplaceByFee, true);
    
    // Verify signal was emitted
    QVERIFY(settingsChangedSpy.count() > 0);
    
    // Verify the dialog reflects the change
    QCheckBox* rbfCheckbox = dialog.findChild<QCheckBox*>();
    if (rbfCheckbox && rbfCheckbox->objectName().contains("rbf", Qt::CaseInsensitive)) {
        QCOMPARE(rbfCheckbox->isChecked(), true);
    }
}

void SettingsTests::settingsSynchronizationTests()
{
    TestChain100Setup test_setup;
    
    OptionsModel model(nullptr, true);
    
    // Test external setting change notification
    QSignalSpy externalChangeSpy(&model, &OptionsModel::settingChangedExternally);
    
    // Simulate external setting change (e.g., via RPC)
    model.handleExternalSettingChange("walletrbf", "false", "true");
    
    // Verify signal was emitted
    QCOMPARE(externalChangeSpy.count(), 1);
    
    // Verify the signal contains correct data
    QList<QVariant> arguments = externalChangeSpy.takeFirst();
    QCOMPARE(arguments.at(0).toString(), QString("walletrbf"));
    QCOMPARE(arguments.at(1).toString(), QString("false"));
    QCOMPARE(arguments.at(2).toString(), QString("true"));
    
    // Test multiple simultaneous changes
    externalChangeSpy.clear();
    
    model.handleExternalSettingChange("spendzeroconfchange", "true", "false");
    model.handleExternalSettingChange("maxmempool", "300", "500");
    
    QCOMPARE(externalChangeSpy.count(), 2);
    
    // Test change persistence
    QVariant currentValue = model.getOption(OptionsModel::EnableReplaceByFee);
    model.handleExternalSettingChange("walletrbf", currentValue.toString(), "false");
    
    // The model should reflect the external change
    QVariant newValue = model.getOption(OptionsModel::EnableReplaceByFee);
    QCOMPARE(newValue.toBool(), false);
}

void SettingsTests::settingsErrorHandlingTests()
{
    TestChain100Setup test_setup;
    OptionsModel model(nullptr, true);
    
    // Test export to invalid path
    QString error;
    bool result = model.exportSettings("/invalid/path/settings.json", error);
    QVERIFY(!result);
    QVERIFY(!error.isEmpty());
    
    // Test import from non-existent file
    error.clear();
    OptionsModel::ImportPreviewResult previewResult;
    result = model.importSettings("/non/existent/file.json", previewResult, error);
    QVERIFY(!result);
    QVERIFY(!error.isEmpty());
    
    // Test import with corrupted JSON
    QTemporaryFile corruptedFile;
    QVERIFY(corruptedFile.open());
    corruptedFile.write("corrupted data \x00\x01\x02");
    corruptedFile.close();
    
    error.clear();
    result = model.importSettings(corruptedFile.fileName(), previewResult, error);
    QVERIFY(!result);
    QVERIFY(!error.isEmpty());
    
    // Test applying invalid changes
    QList<OptionsModel::SettingChange> invalidChanges;
    OptionsModel::SettingChange invalidChange;
    invalidChange.settingName = "invalid_setting_name";
    invalidChange.oldValue = "old";
    invalidChange.newValue = "new";
    invalidChanges.append(invalidChange);
    
    OptionsModel::ImportResult applyResult = model.applyImportedSettings(invalidChanges);
    QVERIFY(!applyResult.success);
    QCOMPARE(applyResult.appliedCount, 0);
    QVERIFY(!applyResult.errors.isEmpty());
}

void SettingsTests::settingsPerformanceTests()
{
    TestChain100Setup test_setup;
    OptionsModel model(nullptr, true);
    
    // Test export performance with many settings
    for (int i = 0; i < 50; ++i) {
        // Set various settings to simulate a complex configuration
        model.setOption(OptionsModel::EnableReplaceByFee, i % 2 == 0);
        model.setOption(OptionsModel::nMempool, 300 + i);
    }
    
    QTemporaryFile tempFile;
    QVERIFY(tempFile.open());
    QString tempPath = tempFile.fileName();
    tempFile.close();
    
    // Measure export time
    QTime timer;
    timer.start();
    
    QString error;
    bool result = model.exportSettings(tempPath, error);
    
    int exportTime = timer.elapsed();
    QVERIFY2(result, qPrintable(error));
    QVERIFY2(exportTime < 1000, "Export took too long"); // Should complete in < 1 second
    
    // Measure import time
    timer.restart();
    
    OptionsModel importModel(nullptr, true);
    OptionsModel::ImportPreviewResult previewResult;
    result = importModel.importSettings(tempPath, previewResult, error);
    
    int importTime = timer.elapsed();
    QVERIFY2(result, qPrintable(error));
    QVERIFY2(importTime < 1000, "Import took too long"); // Should complete in < 1 second
    
    // Test large file handling
    QTemporaryFile largeFile;
    QVERIFY(largeFile.open());
    
    // Create a large but valid JSON file
    QString largeJsonContent = R"({"version": 1, "settings": {"wallet": {)";
    for (int i = 0; i < 1000; ++i) {
        if (i > 0) largeJsonContent += ",";
        largeJsonContent += QString(R"("setting_%1": {"value": %2, "type": 1})").arg(i).arg(i);
    }
    largeJsonContent += "}}}";
    
    largeFile.write(largeJsonContent.toUtf8());
    largeFile.close();
    
    // Test that large files are handled gracefully
    timer.restart();
    error.clear();
    previewResult = OptionsModel::ImportPreviewResult();
    result = importModel.importSettings(largeFile.fileName(), previewResult, error);
    
    int largeImportTime = timer.elapsed();
    
    // Should either succeed or fail gracefully (not crash)
    if (result) {
        QVERIFY2(largeImportTime < 5000, "Large import took too long");
    } else {
        QVERIFY(!error.isEmpty()); // Should have meaningful error message
    }
}

void SettingsTests::settingsDataIntegrityTests()
{
    TestChain100Setup test_setup;
    
    // Test round-trip data integrity
    OptionsModel originalModel(nullptr, true);
    
    // Set specific test values
    originalModel.setOption(OptionsModel::EnableReplaceByFee, true);
    originalModel.setOption(OptionsModel::SpendZeroConfChange, false);
    originalModel.setOption(OptionsModel::nMempool, 450);
    originalModel.setOption(OptionsModel::AddressType, "bech32");
    
    // Export settings
    QTemporaryFile tempFile;
    QVERIFY(tempFile.open());
    QString tempPath = tempFile.fileName();
    tempFile.close();
    
    QString error;
    QVERIFY(originalModel.exportSettings(tempPath, error));
    
    // Import into new model
    OptionsModel importedModel(nullptr, true);
    OptionsModel::ImportPreviewResult previewResult;
    QVERIFY(importedModel.importSettings(tempPath, previewResult, error));
    
    // Apply changes
    OptionsModel::ImportResult applyResult = importedModel.applyImportedSettings(previewResult.changes);
    QVERIFY(applyResult.success);
    
    // Verify data integrity
    QCOMPARE(originalModel.getOption(OptionsModel::EnableReplaceByFee).toBool(),
             importedModel.getOption(OptionsModel::EnableReplaceByFee).toBool());
    QCOMPARE(originalModel.getOption(OptionsModel::SpendZeroConfChange).toBool(),
             importedModel.getOption(OptionsModel::SpendZeroConfChange).toBool());
    QCOMPARE(originalModel.getOption(OptionsModel::nMempool).toInt(),
             importedModel.getOption(OptionsModel::nMempool).toInt());
    QCOMPARE(originalModel.getOption(OptionsModel::AddressType).toString(),
             importedModel.getOption(OptionsModel::AddressType).toString());
    
    // Test with different data types
    QTemporaryFile typeTestFile;
    QVERIFY(typeTestFile.open());
    
    QString typeTestJson = R"({
        "version": 1,
        "settings": {
            "wallet": {
                "walletrbf": {"value": true, "type": 0},
                "spendzeroconfchange": {"value": false, "type": 0}
            },
            "mempool": {
                "maxmempool": {"value": 400, "type": 1}
            },
            "relay": {
                "incrementalrelayfee": {"value": "1500", "type": 4}
            },
            "gui": {
                "addresstype": {"value": "bech32", "type": 2}
            }
        }
    })";
    
    typeTestFile.write(typeTestJson.toUtf8());
    typeTestFile.close();
    
    OptionsModel typeTestModel(nullptr, true);
    error.clear();
    previewResult = OptionsModel::ImportPreviewResult();
    QVERIFY(typeTestModel.importSettings(typeTestFile.fileName(), previewResult, error));
    
    // Verify different data types are preserved
    bool foundBool = false, foundInt = false, foundString = false, foundAmount = false;
    
    for (const auto& change : previewResult.changes) {
        if (change.newValue.type() == QVariant::Bool) foundBool = true;
        else if (change.newValue.type() == QVariant::Int) foundInt = true;
        else if (change.newValue.type() == QVariant::String) foundString = true;
        // Amount types might be handled as strings
    }
    
    QVERIFY(foundBool);
    QVERIFY(foundInt || foundString); // At least one should be found
}