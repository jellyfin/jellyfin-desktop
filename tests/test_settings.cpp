#include <QtTest/QtTest>
#include "../src/settings/SettingsValue.h"
#include "../src/settings/SettingsSection.h"

class TestSettings : public QObject
{
  Q_OBJECT

private slots:
  // SettingsValue
  void testDefaultValueReturnedWhenNoExplicitValue();
  void testExplicitValueOverridesDefault();
  void testInvalidValueRestoresDefault();
  void testPossibleValues();
  void testDescriptionsOutput();
  void testDescriptionsWithInputType();
  void testIsHiddenPlatformAny();
  void testIsHiddenMismatchedPlatform();

  // SettingsSection
  void testRegisterSettingMakesValueRetrievable();
  void testDuplicateRegisterSettingRejected();
  void testValueForUnknownKeyReturnsInvalid();
  void testDefaultValueReturnsSettingDefault();
  void testAllValuesReturnsAllRegistered();
  void testResetValueDescribedSetting();
  void testResetValueDynamicSetting();
  void testResetValuesResetsAll();
  void testSectionIsHiddenPlatform();
  void testSectionOrder();
  void testValuesUpdatedSignalOnReset();
  void testValuesUpdatedSignalNotFiredWhenNoChange();
  void testResetValueNonExistentKeyIsNoop();
  void testSectionDescriptionsFiltersHidden();
  void testSectionDescriptionsSortsByIndexOrder();
  void testSetValueHidden();
  void testStorageFlag();
};


/*!
  SettingsValue
 */

void TestSettings::testDefaultValueReturnedWhenNoExplicitValue()
{
  SettingsValue sv("testKey", 7);
  QCOMPARE(sv.value(), QVariant(7));
}

void TestSettings::testExplicitValueOverridesDefault()
{
  SettingsValue sv("testKey", 100);
  sv.setValue(123);
  QCOMPARE(sv.value(), QVariant(123));
}

void TestSettings::testInvalidValueRestoresDefault()
{
  SettingsValue sv("testKey", 101);
  sv.setValue(456);
  QCOMPARE(sv.value(), QVariant(456));

  sv.setValue(QVariant()); // Not valid
  QCOMPARE(sv.value(), QVariant(101));
}

void TestSettings::testPossibleValues()
{
  SettingsValue sv("testKey", QString("a"));
  sv.addPossibleValue("Alpha", QString("a"));
  sv.addPossibleValue("Beta", QString("b"));

  QVariantList possible = sv.possibleValues();
  QCOMPARE(possible.size(), 2);

  QVariantMap first = possible[0].toMap();
  QCOMPARE(first["title"].toString(), QString("Alpha"));
  QCOMPARE(first["value"].toString(), QString("a"));

  QVariantMap second = possible[1].toMap();
  QCOMPARE(second["title"].toString(), QString("Beta"));
  QCOMPARE(second["value"].toString(), QString("b"));

  // Replace the list
  QVariantList newList;
  QVariantMap entry;
  entry["value"] = QString("x");
  entry["title"] = QString("X-Ray");
  newList << entry;
  sv.setPossibleValues(newList);
  QCOMPARE(sv.possibleValues().size(), 1);
}

void TestSettings::testDescriptionsOutput()
{
  SettingsValue sv("testKey", 10);
  sv.setDisplayName("My Setting");
  sv.setHelp("Sample help");
  sv.addPossibleValue("Ten", 10);
  sv.addPossibleValue("Twenty", 20);

  QVariantMap desc = sv.descriptions();
  QCOMPARE(desc["key"].toString(), QString("testKey"));
  QCOMPARE(desc["displayName"].toString(), QString("My Setting"));
  QCOMPARE(desc["help"].toString(), QString("Sample help"));
  QVERIFY(desc.contains("options"));
  QCOMPARE(desc["options"].toList().size(), 2);
  QVERIFY(!desc.contains("inputType"));
}

void TestSettings::testDescriptionsWithInputType()
{
  SettingsValue sv("testKey", QString(""));
  sv.setDisplayName("Name");
  sv.setHelp("Help");
  sv.setInputType("numeric");

  QVariantMap desc = sv.descriptions();
  QVERIFY(desc.contains("inputType"));
  QCOMPARE(desc["inputType"].toString(), QString("numeric"));
  QVERIFY(!desc.contains("options"));
}

void TestSettings::testIsHiddenPlatformAny()
{
  SettingsValue sv("testKey", 0, PLATFORM_ANY);
  // Defaults tp hidden for PLATFORM_ANY
  QVERIFY(sv.isHidden());

  sv.setHidden(false);
  QVERIFY(!sv.isHidden());
}

void TestSettings::testIsHiddenMismatchedPlatform()
{
  // Use a platform that is not the current one.
  quint8 wrongPlatform = PLATFORM_ANY & ~Utils::CurrentPlatform();
  SettingsValue sv("testKey", 0, wrongPlatform);
  sv.setHidden(false);

  // Wrong platform should override hidden = false.
  QVERIFY(sv.isHidden());
}

/*!
  SettingsSection tests
 */

void TestSettings::testRegisterSettingMakesValueRetrievable()
{
  SettingsSection section("testSection", PLATFORM_ANY, 0);
  auto* sv = new SettingsValue("alpaca", 99);
  section.registerSetting(sv);

  QCOMPARE(section.value("alpaca"), QVariant(99));
}

void TestSettings::testDuplicateRegisterSettingRejected()
{
  SettingsSection section("testSection", PLATFORM_ANY, 0);
  auto* sv1 = new SettingsValue("llama", 10); // Original value
  auto* sv2 = new SettingsValue("llama", 20); // Duplicate value
  section.registerSetting(sv1);
  section.registerSetting(sv2);

  // Original value is retained
  QCOMPARE(section.value("llama"), QVariant(10));

  // SettingsSection will only delete sv1 so we have to delete sv2 ourselves.
  delete sv2;
}

void TestSettings::testValueForUnknownKeyReturnsInvalid()
{
  SettingsSection section("testSection", PLATFORM_ANY, 0);
  QVariant result = section.value("nonexistent");
  QVERIFY(!result.isValid());
}

void TestSettings::testDefaultValueReturnsSettingDefault()
{
  SettingsSection section("testSection", PLATFORM_ANY, 0);
  auto* sv = new SettingsValue("moose", 55);
  section.registerSetting(sv);

  // Set value directly
  sv->setValue(77);
  QCOMPARE(section.value("moose"), QVariant(77));
  QCOMPARE(section.defaultValue("moose"), QVariant(55));
}

void TestSettings::testAllValuesReturnsAllRegistered()
{
  SettingsSection section("testSection", PLATFORM_ANY, 0);
  auto* sv1 = new SettingsValue("a", 1);
  auto* sv2 = new SettingsValue("b", 2);
  section.registerSetting(sv1);
  section.registerSetting(sv2);

  QVariantMap all = section.allValues();
  QCOMPARE(all.size(), 2);
  QCOMPARE(all["a"], QVariant(1));
  QCOMPARE(all["b"], QVariant(2));
}

void TestSettings::testResetValueDescribedSetting()
{
  SettingsSection section("testSection", PLATFORM_ANY, 0);
  auto* sv = new SettingsValue("aardvark", 100);
  sv->setHasDescription(true);
  section.registerSetting(sv);

  // Change from default
  sv->setValue(999);
  QCOMPARE(section.value("aardvark"), QVariant(999));

  section.resetValue("aardvark");
  QCOMPARE(section.value("aardvark"), QVariant(100));
}

void TestSettings::testResetValueDynamicSetting()
{
  SettingsSection section("testSection", PLATFORM_ANY, 0);
  // Dynamic setting: hasDescription = false (the default)
  auto* sv = new SettingsValue("dynamic", QString("temp"));
  section.registerSetting(sv);

  QVERIFY(section.value("dynamic").isValid());

  section.resetValue("dynamic");
  // Dynamic setting is removed entirely
  QVERIFY(!section.value("dynamic").isValid());
}

void TestSettings::testResetValuesResetsAll()
{
  SettingsSection section("testSection", PLATFORM_ANY, 0);

  auto* sv1 = new SettingsValue("described", 10);
  sv1->setHasDescription(true);
  section.registerSetting(sv1);

  auto* sv2 = new SettingsValue("dynamic", QString("tmp"));
  section.registerSetting(sv2);

  // Modify the described setting.
  sv1->setValue(50);

  section.resetValues();

  // The described setting resets back to default.
  QCOMPARE(section.value("described"), QVariant(10));

  // Dynamic setting is removed.
  QVERIFY(!section.value("dynamic").isValid());
}

void TestSettings::testSectionIsHiddenPlatform()
{
  // Current platform section.
  SettingsSection visible("vis", PLATFORM_ANY, 0);
  QVERIFY(!visible.isHidden());

  // Other platform section.
  quint8 wrongPlatform = PLATFORM_ANY & ~Utils::CurrentPlatform();
  SettingsSection hidden("hid", wrongPlatform, 0);
  QVERIFY(hidden.isHidden());

  // Explicitly hidden section
  SettingsSection explicitHidden("hideMe", PLATFORM_ANY, 0);
  explicitHidden.setHidden(true);
  QVERIFY(explicitHidden.isHidden());
}

void TestSettings::testSectionOrder()
{
  SettingsSection section("mySection", PLATFORM_ANY, 5);
  QVariantMap order = section.sectionOrder();
  QCOMPARE(order["key"].toString(), QString("mySection"));
  QCOMPARE(order["order"].toInt(), 5);
}

void TestSettings::testValuesUpdatedSignalOnReset()
{
  SettingsSection section("testSection", PLATFORM_ANY, 0);
  auto* sv = new SettingsValue("delta", 42);
  sv->setHasDescription(true);
  section.registerSetting(sv);

  // Change from default.
  sv->setValue(99);

  QSignalSpy spy(&section, &SettingsSection::valuesUpdated);
  section.resetValue("delta");

  QCOMPARE(spy.count(), 1);
  QVariantMap updated = spy.at(0).at(0).toMap();
  QVERIFY(updated.contains("delta"));
  QCOMPARE(updated["delta"], QVariant(42));
}

void TestSettings::testValuesUpdatedSignalNotFiredWhenNoChange()
{
  SettingsSection section("testSection", PLATFORM_ANY, 0);
  auto* sv = new SettingsValue("epsilon", 42);
  sv->setHasDescription(true);
  section.registerSetting(sv);

  // Value is already at default — reset should be a no-op.
  QSignalSpy spy(&section, &SettingsSection::valuesUpdated);
  section.resetValue("epsilon");
  QCOMPARE(spy.count(), 0);
}

void TestSettings::testResetValueNonExistentKeyIsNoop()
{
  SettingsSection section("testSection", PLATFORM_ANY, 0);
  QSignalSpy spy(&section, &SettingsSection::valuesUpdated);

  // Should not fire signal.
  section.resetValue("ghost");
  QCOMPARE(spy.count(), 0);
}

void TestSettings::testSectionDescriptionsFiltersHidden()
{
  SettingsSection section("mySection", PLATFORM_ANY, 0);

  auto* visible = new SettingsValue("vis", 1, PLATFORM_ANY);
  visible->setHidden(false);
  visible->setHasDescription(true);
  visible->setDisplayName("Visible");
  section.registerSetting(visible);

  auto* hidden = new SettingsValue("hid", 2, PLATFORM_ANY);
  hidden->setHidden(true);
  hidden->setHasDescription(true);
  hidden->setDisplayName("Hidden");
  section.registerSetting(hidden);

  QVariantMap desc = section.descriptions();
  QCOMPARE(desc["key"].toString(), QString("mySection"));

  QVariantList settings = desc["settings"].toList();
  QCOMPARE(settings.size(), 1);
  QCOMPARE(settings[0].toMap()["key"].toString(), QString("vis"));
}

void TestSettings::testSectionDescriptionsSortsByIndexOrder()
{
  SettingsSection section("mySection", PLATFORM_ANY, 0);

  // Create settings in mixed-up order.
  auto* second = new SettingsValue("second", 2, PLATFORM_ANY);
  second->setHidden(false);
  second->setHasDescription(true);
  second->setIndexOrder(1); // 1 = second
  section.registerSetting(second);

  auto* first = new SettingsValue("first", 1, PLATFORM_ANY);
  first->setHidden(false);
  first->setHasDescription(true);
  first->setIndexOrder(0); // 0 = first
  section.registerSetting(first);

  auto* third = new SettingsValue("third", 3, PLATFORM_ANY);
  third->setHidden(false);
  third->setHasDescription(true);
  third->setIndexOrder(2); // 2 = third
  section.registerSetting(third);

  QVariantList settings = section.descriptions()["settings"].toList();
  QCOMPARE(settings.size(), 3);
  QCOMPARE(settings[0].toMap()["key"].toString(), QString("first"));
  QCOMPARE(settings[1].toMap()["key"].toString(), QString("second"));
  QCOMPARE(settings[2].toMap()["key"].toString(), QString("third"));
}

void TestSettings::testSetValueHidden()
{
  SettingsSection section("testSection", PLATFORM_ANY, 0);
  auto* sv = new SettingsValue("toggle", 0, PLATFORM_ANY);
  sv->setHidden(false);
  section.registerSetting(sv);

  QVERIFY(!section.isValueHidden("toggle"));

  section.setValueHidden("toggle", true);
  QVERIFY(section.isValueHidden("toggle"));

  section.setValueHidden("toggle", false);
  QVERIFY(!section.isValueHidden("toggle"));
}

void TestSettings::testStorageFlag()
{
  SettingsSection section("store", PLATFORM_ANY, 0);
  QVERIFY(!section.isStorage());

  section.setStorage(true);
  QVERIFY(section.isStorage());
}

QTEST_APPLESS_MAIN(TestSettings)
#include "test_settings.moc"
