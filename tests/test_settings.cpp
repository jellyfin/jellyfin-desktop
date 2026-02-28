#include <QtTest/QtTest>
#include "../src/settings/SettingsValue.h"
#include "../src/settings/SettingsSection.h"

class TestSettings : public QObject
{
  Q_OBJECT

private slots:
  // SettingsValue tests
  void testDefaultValueReturnedWhenNoExplicitValue();
  void testExplicitValueOverridesDefault();
  void testInvalidValueRestoresDefault();
  void testPossibleValues();
  void testDescriptionsOutput();
  void testDescriptionsWithInputType();
  void testIsHiddenPlatformAny();
  void testIsHiddenMismatchedPlatform();

  // SettingsSection tests
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
};


/*!
  SettingsValue tests
 */

void TestSettings::testDefaultValueReturnedWhenNoExplicitValue()
{
  SettingsValue sv("keyTest", 42);
  QCOMPARE(sv.value(), QVariant(42));
}

void TestSettings::testExplicitValueOverridesDefault()
{
  SettingsValue sv("keyTest", 42);
  sv.setValue(123);
  QCOMPARE(sv.value(), QVariant(123));
}

void TestSettings::testInvalidValueRestoresDefault()
{
  SettingsValue sv("keyTest", 42);
  sv.setValue(456);
  QCOMPARE(sv.value(), QVariant(456));

  sv.setValue(QVariant()); // invalid value
  QCOMPARE(sv.value(), QVariant(42));
}

void TestSettings::testPossibleValues()
{
  SettingsValue sv("keyTest", QString("a"));
  sv.addPossibleValue("Option A", QString("a"));
  sv.addPossibleValue("Option B", QString("b"));

  QVariantList possible = sv.possibleValues();
  QCOMPARE(possible.size(), 2);

  QVariantMap first = possible[0].toMap();
  QCOMPARE(first["title"].toString(), QString("Option A"));
  QCOMPARE(first["value"].toString(), QString("a"));

  QVariantMap second = possible[1].toMap();
  QCOMPARE(second["title"].toString(), QString("Option B"));
  QCOMPARE(second["value"].toString(), QString("b"));

  // setPossibleValues replaces the list
  QVariantList newList;
  QVariantMap entry;
  entry["value"] = QString("x");
  entry["title"] = QString("Option X");
  newList << entry;
  sv.setPossibleValues(newList);
  QCOMPARE(sv.possibleValues().size(), 1);
}

void TestSettings::testDescriptionsOutput()
{
  SettingsValue sv("myKey", 10);
  sv.setDisplayName("My Setting");
  sv.setHelp("Sample help text");
  sv.addPossibleValue("Ten", 10);
  sv.addPossibleValue("Twenty", 20);

  QVariantMap desc = sv.descriptions();
  QCOMPARE(desc["key"].toString(), QString("myKey"));
  QCOMPARE(desc["displayName"].toString(), QString("My Setting"));
  QCOMPARE(desc["help"].toString(), QString("Sample help text"));
  QVERIFY(desc.contains("options"));
  QCOMPARE(desc["options"].toList().size(), 2);
  QVERIFY(!desc.contains("inputType"));
}

void TestSettings::testDescriptionsWithInputType()
{
  SettingsValue sv("myKey", QString(""));
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
  SettingsValue sv("key", 0, PLATFORM_ANY);
  // default hidden=true
  QVERIFY(sv.isHidden());

  sv.setHidden(false);
  QVERIFY(!sv.isHidden());
}

void TestSettings::testIsHiddenMismatchedPlatform()
{
  // Use a platform that is not the current one.
  quint8 wrongPlatform = PLATFORM_ANY & ~Utils::CurrentPlatform();
  SettingsValue sv("key", 0, wrongPlatform);
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

  // Set a value on the SettingsValue directly
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

  // Described setting resets to default.
  QCOMPARE(section.value("described"), QVariant(10));

  // Dynamic setting removed.
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

QTEST_APPLESS_MAIN(TestSettings)
#include "test_settings.moc"
