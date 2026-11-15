# Contributing to Jellyfin Desktop

## Running Tests

Jellyfin Desktop uses Qt Test for unit testing.

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

On Windows, `dev\windows\test.bat` selects the native build and sets the Qt/mpv
runtime paths. Windows ARM64 builds also run `test_mpvd3d11`, which opens a test
window and checks video pixels, orientation, resizing and renderer recreation.
It requires a working Direct3D 11 device (hardware or WARP).

### Fork CI

The `build` and `test` workflows can be dispatched manually on a fork branch.
Fork builds upload package artifacts but skip upstream package signing and
repository deployment.

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
