# Contributing to Jellyfin Desktop

## Running Tests

Jellyfin Desktop uses Qt Test for unit testing.
JavaScript player tests also run through CTest when Node.js 18 or newer is available.

### Building Tests

Tests are built automatically when you build the project:

```sh
cmake -B build
cmake --build build
```

### Running Tests

Run all tests using CTest:

```sh
cd build
ctest
```

Or run individual test executables directly:

```sh
cd build
./tests/test_systemcomponent
```

Run the JavaScript player tests directly from the repository root:

```sh
node --test tests/test_mpvVideoPlayer.js
```

### Writing Tests

Tests are located in the `tests/` directory. To add a new test:

1. Create a test file in `tests/` (e.g., `test_mycomponent.cpp`)
2. Use `QTEST_APPLESS_MAIN` for headless unit tests
3. Add the test to `tests/CMakeLists.txt`

Example test structure:

```cpp
#include <QtTest/QtTest>
#include "../src/mycomponent/MyComponent.h"

class TestMyComponent : public QObject
{
  Q_OBJECT

private slots:
  void testMyFunction_data();
  void testMyFunction();
};

void TestMyComponent::testMyFunction_data()
{
  QTest::addColumn<QString>("input");
  QTest::addColumn<QString>("expected");

  QTest::newRow("test case 1") << "input1" << "output1";
  QTest::newRow("test case 2") << "input2" << "output2";
}

void TestMyComponent::testMyFunction()
{
  QFETCH(QString, input);
  QFETCH(QString, expected);

  QString result = MyComponent::myFunction(input);
  QCOMPARE(result, expected);
}

QTEST_APPLESS_MAIN(TestMyComponent)
#include "test_mycomponent.moc"
```

For more information on Qt Test, see the [Qt Test documentation](https://doc.qt.io/qt-6/qtest-overview.html).
