<div align="center">
  <!--
  <meta name="description" content="C++ logging library. High-performance, asynchronous logging for low-latency real-time applications.">
  <meta name="keywords" content="C++, logging library, asynchronous logging, high performance, low latency">
  -->

  <br>
  <img src="docs/logo.png" alt="Quill C++ Logging Library" width="200" height="auto" />
  <h1>Quill</h1>
  <p><b>Asynchronous Low Latency C++ Logging Library</b></p>

  <div>
    <a href="https://github.com/odygrd/quill/actions?query=workflow%3Afedora">
      <img src="https://img.shields.io/github/actions/workflow/status/odygrd/quill/fedora.yml?branch=master&label=Fedora&style=flat-square&logo=fedora" alt="fedora-ci" />
    </a>
    <a href="https://github.com/odygrd/quill/actions?query=workflow%3Aubuntu">
      <img src="https://img.shields.io/github/actions/workflow/status/odygrd/quill/ubuntu.yml?branch=master&label=Ubuntu&style=flat-square&logo=ubuntu" alt="ubuntu-ci" />
    </a>
    <a href="https://github.com/odygrd/quill/actions?query=workflow%3Absd">
      <img src="https://img.shields.io/github/actions/workflow/status/odygrd/quill/bsd.yml?branch=master&label=BSD&style=flat-square&logo=openbsd" alt="bsd-ci" />
    </a>
    <a href="https://github.com/odygrd/quill/actions?query=workflow%3Amacos">
      <img src="https://img.shields.io/github/actions/workflow/status/odygrd/quill/macos.yml?branch=master&label=macOS&logoColor=white&style=flat-square&logo=apple" alt="macos-ci" />
    </a>
    <a href="https://github.com/odygrd/quill/actions?query=workflow%3Awindows">
      <img src="https://img.shields.io/github/actions/workflow/status/odygrd/quill/windows.yml?branch=master&label=Windows&logoColor=blue&style=flat-square&logo=data:image/svg%2bxml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIGhlaWdodD0iODgiIHdpZHRoPSI4OCIgeG1sbnM6dj0iaHR0cHM6Ly92ZWN0YS5pby9uYW5vIj48cGF0aCBkPSJNMCAxMi40MDJsMzUuNjg3LTQuODYuMDE2IDM0LjQyMy0zNS42Ny4yMDN6bTM1LjY3IDMzLjUyOWwuMDI4IDM0LjQ1M0wuMDI4IDc1LjQ4LjAyNiA0NS43em00LjMyNi0zOS4wMjVMODcuMzE0IDB2NDEuNTI3bC00Ny4zMTguMzc2em00Ny4zMjkgMzkuMzQ5bC0uMDExIDQxLjM0LTQ3LjMxOC02LjY3OC0uMDY2LTM0LjczOXoiIGZpbGw9IiMwMGFkZWYiLz48L3N2Zz4=" alt="windows-ci" />
    </a>
    <a href="https://github.com/odygrd/quill/actions?query=workflow%3Aintelllvm">
      <img src="https://img.shields.io/github/actions/workflow/status/odygrd/quill/intelllvm.yml?branch=master&label=Intel%20LLVM&style=flat-square&logo=intel" alt="intel-llvm-ci" />
    </a>
  </div>

  <div>
    <a href="https://codecov.io/gh/odygrd/quill">
      <img src="https://img.shields.io/codecov/c/gh/odygrd/quill/master.svg?logo=codecov&style=flat-square" alt="Codecov" />
    </a>
    <a href="https://github.com/odygrd/quill/actions/workflow/status/odygrd/quill/fuzz.yml">
      <img src="https://img.shields.io/github/actions/workflow/status/odygrd/quill/fuzz.yml?branch=master&label=Fuzz&style=flat-square" alt="fuzz-ci" />
    </a>
    <a href="https://app.codacy.com/gh/odygrd/quill/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade">
      <img src="https://img.shields.io/codacy/grade/cd387bc34658475d98bff84db3ad5287?logo=codacy&style=flat-square" alt="Codacy" />
    </a>
    <a href="https://www.codefactor.io/repository/github/odygrd/quill">
      <img src="https://img.shields.io/codefactor/grade/github/odygrd/quill?logo=codefactor&style=flat-square" alt="CodeFactor" />
    </a>
  </div>

  <div>
    <a href="https://opensource.org/licenses/MIT">
      <img src="https://img.shields.io/badge/license-MIT-blue.svg?style=flat-square" alt="license" />
    </a>
    <a href="https://en.wikipedia.org/wiki/C%2B%2B17">
      <img src="https://img.shields.io/badge/language-C%2B%2B17-red.svg?style=flat-square" alt="language" />
    </a>
  </div>

  <h4>
    <a href="https://quillcpp.readthedocs.io" title="Explore the full documentation">📚 Documentation</a>
    <span> · </span>
    <a href="https://quillcpp.readthedocs.io/en/latest/cheat_sheet.html" title="Quick reference for common tasks">⚡ Cheat Sheet</a>
    <span> · </span>
    <a href="https://quillcpp.readthedocs.io/en/latest/faq.html" title="Frequently asked questions">❓ FAQ</a>
    <span> · </span>
    <a href="https://github.com/odygrd/quill/issues/new?assignees=&labels=&projects=&template=bug-report.md&title=" title="Report a bug or issue">🐛 Report Bug</a>
    <span> · </span>
    <a href="https://github.com/odygrd/quill/issues/new?assignees=&labels=&projects=&template=feature_request.md&title=">💡 Request Feature</a>
  </h4>

  <div align="center"><img src="docs/quill_demo.gif" alt="Logging Demo" width="75%" /></div>
</div>

---

## 🧭 Table of Contents

- [Introduction](#-introduction)
- [Quick Start](#-quick-start)
- [Features](#-features)
- [Performance](#-performance)
- [Usage](#-usage)
- [Design](#-design)
- [Caveats](#-caveats)
- [License](#-license)

---

## ✨ Introduction

**Quill** is a **high-performance asynchronous logging library** written in **C++**. It is designed for low-latency,
performance-critical applications where every microsecond counts.

- **Performance-Focused**: Quill consistently outperforms many popular logging libraries.
- **Feature-Rich**: Packed with advanced features to meet diverse logging needs.
- **Battle-Tested**: Proven in demanding production environments. Extensively tested with sanitizers (ASan, UBSan, LSan) and fuzzed across a wide range of inputs.
- **Extensive Documentation**: Comprehensive guides and examples available.
- **Community-Driven**: Open to contributions, feedback, and feature requests.

Try it on [Compiler Explorer](https://godbolt.org/z/n68asK7bY)

---

## ⏩ Quick Start

Getting started is easy and straightforward. Follow these steps to integrate the library into your project:

### Installation

You can install Quill using the package manager of your choice:

| Package Manager |              Installation Command              |
|:---------------:|:----------------------------------------------:|
|      vcpkg      |             `vcpkg install quill`              |
|      Conan      |             `conan install quill`              |
|    Homebrew     |              `brew install quill`              |
|  Meson WrapDB   |           `meson wrap install quill`           |
|      Conda      |      `conda install -c conda-forge quill`      |
|     Bzlmod      | `bazel_dep(name = "quill", version = "x.y.z")` |
|      xmake      |             `xrepo install quill`              |
|       nix       |            `nix-shell -p quill-log`            |
|     build2      |                   `libquill`                   |

### Setup

#### Quickest Setup

For the quickest and simplest setup use `simple_logger()`:

```c++
#include "quill/SimpleSetup.h"
#include "quill/LogFunctions.h"

int main()
{
  // log to the console
  auto* logger = quill::simple_logger();
  quill::info(logger, "Hello from {}!", "Quill");

  // log to a file
  auto* logger2 = quill::simple_logger("test.log");
  quill::warning(logger2, "This message goes to a file");
}
```

#### Detailed Setup

For more detailed control and configuration options, use the `Backend` and `Frontend` APIs:

```c++
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"
#include <string_view>

int main()
{
  quill::Backend::start();

  quill::Logger* logger = quill::Frontend::create_or_get_logger(
    "root", quill::Frontend::create_or_get_sink<quill::ConsoleSink>("sink_id_1"));

  LOG_INFO(logger, "Hello from {}!", std::string_view{"Quill"});
}
```

Alternatively, you can use the macro-free mode.
See [here](https://quillcpp.readthedocs.io/en/latest/macro_free_mode.html) for details on performance
trade-offs.

```c++
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogFunctions.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"
#include <string_view>

int main()
{
  quill::Backend::start();

  quill::Logger* logger = quill::Frontend::create_or_get_logger(
    "root", quill::Frontend::create_or_get_sink<quill::ConsoleSink>("sink_id_1"));

  quill::info(logger, "Hello from {}!", std::string_view{"Quill"});
}
```

---

## 🎯 Features

- **High-Performance**: Ultra-low latency performance. View [Benchmarks](https://github.com/odygrd/quill#performance)
- **Asynchronous Processing**: Background thread handles formatting and I/O, keeping your main thread responsive.
- **Minimal Header Includes**:
    - **Frontend**: Only `Logger.h` and `LogMacros.h` needed for logging. Lightweight with minimal dependencies.
    - **Backend**: Single `.cpp` file inclusion. No backend code injection into other translation units.
- **Compile-Time Optimization**: Eliminate specific log levels at compile time.
- **Custom Formatters**: Define your own log output patterns.
  See [Formatters](https://quillcpp.readthedocs.io/en/latest/formatters.html).
- **Timestamp-Ordered Logs**: Simplify debugging of multithreaded applications with chronologically ordered logs.
- **Flexible Timestamps**: Support for `rdtsc`, `chrono`, or `custom clocks` - ideal for simulations and more.
- **Backtrace Logging**: Store messages in a ring buffer for on-demand display.
  See [Backtrace Logging](https://quillcpp.readthedocs.io/en/latest/backtrace_logging.html)
- **Multiple Output Sinks**: Console (with color), files (with rotation), JSON, ability to create custom sinks and more.
- **Log Filtering**: Process only relevant messages.
  See [Filters](https://quillcpp.readthedocs.io/en/latest/filters.html).
- **JSON Logging**: Structured log output.
  See [JSON Logging](https://quillcpp.readthedocs.io/en/latest/json_logging.html)
- **Configurable Queue Modes**: `bounded/unbounded` and `blocking/dropping` options with monitoring on dropped messages,
  queue reallocations, and blocked hot threads.
- **Crash Handling**: Built-in signal handler for log preservation during crashes.
- **Huge Pages Support (Linux)**: Leverage huge pages on the hot path for optimized performance.
- **Wide Character Support (Windows)**: Compatible with ASCII-encoded wide strings and STL containers consisting of wide
  strings.
- **Exception-Free Option**: Configurable builds with or without exception handling.
- **Clean Codebase**: Maintained to high standards, warning-free even at strict levels.
- **Type-Safe API**: Built on [{fmt}](https://github.com/fmtlib/fmt) library.

---

## 🚀 Performance

### System Configuration

- **OS:** Linux RHEL 9.4
- **CPU:** Intel Core i5-12600 (12th Gen) @ 4.8 GHz
- **Compiler:** GCC 13.1
- **Benchmark-Tuned System:** The system is specifically tuned for benchmarking.

- **Command Line Parameters:**
  ```shell
  $ cat /proc/cmdline
  BOOT_IMAGE=(hd0,gpt2)/vmlinuz-5.14.0-427.13.1.el9_4.x86_64 root=/dev/mapper/rhel-root ro crashkernel=1G-4G:192M,4G-64G:256M,64G-:512M resume=/dev/mapper/rhel-swap rd.lvm.lv=rhel/root rd.lvm.lv=rhel/swap rhgb quiet nohz=on nohz_full=1-5 rcu_nocbs=1-5 isolcpus=1-5 mitigations=off transparent_hugepage=never intel_pstate=disable nosoftlockup irqaffinity=0 processor.max_cstate=1 nosoftirqd sched_tick_offload=0 spec_store_bypass_disable=off spectre_v2=off iommu=pt
  ```

You can find the benchmark code on the [logger_benchmarks](https://github.com/odygrd/logger_benchmarks) repository.

### Latency

The results presented in the tables below are measured in `nanoseconds (ns)`.

The tables are sorted by the 95th percentile (lower is better).

#### Logging Numbers

`LOG_INFO(logger, "Logging int: {}, int: {}, double: {}", i, j, d)`.

##### 1 Thread Logging

| Library                                                                  | 50th | 75th | 90th | 95th | 99th | 99.9th |
|--------------------------------------------------------------------------|:----:|:----:|:----:|:----:|:----:|:------:|
| [Quill Bounded Dropping Queue](https://github.com/odygrd/quill)          |  8   |  8   |  9   |  9   |  10  |   12   |
| [Quill Unbounded Queue](https://github.com/odygrd/quill)                 |  8   |  8   |  9   |  9   |  11  |   13   |
| [fmtlog](https://github.com/MengRao/fmtlog)                              |  8   |  9   |  9   |  10  |  12  |   13   |
| [PlatformLab NanoLog](https://github.com/PlatformLab/NanoLog)            |  13  |  14  |  16  |  18  |  21  |   24   |
| [MS BinLog](https://github.com/Morgan-Stanley/binlog)                    |  21  |  21  |  21  |  22  |  59  |   93   |
| [Quill Unbounded Queue (Log Functions)](https://github.com/odygrd/quill) |  27  |  28  |  29  |  30  |  31  |   33   |
| [XTR](https://github.com/choll/xtr)                                      |  7   |  7   |  29  |  31  |  33  |   55   |
| [Reckless](https://github.com/mattiasflodin/reckless)                    |  26  |  28  |  31  |  32  |  34  |   40   |
| [BqLog](https://github.com/Tencent/BqLog)                                |  42  |  42  |  43  |  75  |  83  |   94   |
| [Iyengar NanoLog](https://github.com/Iyengar111/NanoLog)                 |  87  |  98  | 122  | 129  | 209  |  381   |
| [spdlog](https://github.com/gabime/spdlog)                               | 145  | 148  | 151  | 154  | 162  |  171   |
| [g3log](https://github.com/KjellKod/g3log)                               | 775  | 781  | 787  | 791  | 805  |  967   |
| [Boost.Log](https://www.boost.org)                                       | 838  | 845  | 853  | 859  | 897  |  959   |

![numbers_1_thread_logging.webp](docs%2Fcharts%2Fnumbers_1_thread_logging.webp)

##### 4 Threads Logging Simultaneously

| Library                                                                  | 50th | 75th | 90th | 95th | 99th | 99.9th |
|--------------------------------------------------------------------------|:----:|:----:|:----:|:----:|:----:|:------:|
| [Quill Unbounded Queue](https://github.com/odygrd/quill)                 |  8   |  9   |  9   |  9   |  12  |   15   |
| [fmtlog](https://github.com/MengRao/fmtlog)                              |  8   |  9   |  9   |  10  |  12  |   13   |
| [XTR](https://github.com/choll/xtr)                                      |  7   |  8   |  9   |  10  |  32  |   39   |
| [Quill Bounded Dropping Queue](https://github.com/odygrd/quill)          |  8   |  9   |  10  |  11  |  13  |   15   |
| [PlatformLab NanoLog](https://github.com/PlatformLab/NanoLog)            |  9   |  13  |  15  |  17  |  22  |   26   |
| [MS BinLog](https://github.com/Morgan-Stanley/binlog)                    |  21  |  22  |  22  |  23  |  61  |   98   |
| [Reckless](https://github.com/mattiasflodin/reckless)                    |  19  |  23  |  26  |  27  |  31  |   57   |
| [Quill Unbounded Queue (Log Functions)](https://github.com/odygrd/quill) |  28  |  29  |  30  |  31  |  33  |   44   |
| [BqLog](https://github.com/Tencent/BqLog)                                |  47  |  49  |  50  | 122  | 143  |  4248  |
| [Iyengar NanoLog](https://github.com/Iyengar111/NanoLog)                 |  58  |  90  | 124  | 132  | 178  |  1478  |
| [spdlog](https://github.com/gabime/spdlog)                               | 214  | 260  | 327  | 359  | 452  |  754   |
| [Boost.Log](https://www.boost.org)                                       | 1640 | 2538 | 2793 | 2896 | 4150 |  4962  |
| [g3log](https://github.com/KjellKod/g3log)                               | 1336 | 4102 | 4501 | 4641 | 6079 |  7363  |

![numbers_4_thread_logging.webp](docs%2Fcharts%2Fnumbers_4_thread_logging.webp)

#### Logging Large Strings

Logging `std::string` over 35 characters to prevent the short string optimization.

`LOG_INFO(logger, "Logging int: {}, int: {}, string: {}", i, j, large_string)`.

##### 1 Thread Logging

| Library                                                                  | 50th | 75th | 90th | 95th | 99th | 99.9th |
|--------------------------------------------------------------------------|:----:|:----:|:----:|:----:|:----:|:------:|
| [fmtlog](https://github.com/MengRao/fmtlog)                              |  10  |  12  |  13  |  13  |  15  |   17   |
| [Quill Unbounded Queue](https://github.com/odygrd/quill)                 |  11  |  12  |  14  |  14  |  16  |   18   |
| [Quill Bounded Dropping Queue](https://github.com/odygrd/quill)          |  12  |  13  |  14  |  15  |  16  |   19   |
| [MS BinLog](https://github.com/Morgan-Stanley/binlog)                    |  23  |  23  |  24  |  25  |  62  |   96   |
| [XTR](https://github.com/choll/xtr)                                      |  8   |  8   |  28  |  29  |  33  |   52   |
| [PlatformLab NanoLog](https://github.com/PlatformLab/NanoLog)            |  15  |  19  |  29  |  32  |  37  |   43   |
| [Quill Unbounded Queue (Log Functions)](https://github.com/odygrd/quill) |  31  |  33  |  34  |  35  |  36  |   38   |
| [BqLog](https://github.com/Tencent/BqLog)                                |  44  |  45  |  46  |  76  |  85  |   93   |
| [Reckless](https://github.com/mattiasflodin/reckless)                    |  89  | 105  | 111  | 114  | 120  |  131   |
| [Iyengar NanoLog](https://github.com/Iyengar111/NanoLog)                 |  89  | 100  | 123  | 132  | 205  |  367   |
| [spdlog](https://github.com/gabime/spdlog)                               | 129  | 133  | 137  | 141  | 149  |  157   |
| [g3log](https://github.com/KjellKod/g3log)                               | 565  | 568  | 571  | 573  | 582  |  740   |
| [Boost.Log](https://www.boost.org)                                       | 638  | 641  | 645  | 647  | 651  |  659   |

![large_strings_1_thread_logging.webp](docs%2Fcharts%2Flarge_strings_1_thread_logging.webp)

##### 4 Threads Logging Simultaneously

| Library                                                                  | 50th | 75th | 90th | 95th | 99th | 99.9th |
|--------------------------------------------------------------------------|:----:|:----:|:----:|:----:|:----:|:------:|
| [fmtlog](https://github.com/MengRao/fmtlog)                              |  10  |  12  |  13  |  13  |  16  |   19   |
| [XTR](https://github.com/choll/xtr)                                      |  8   |  11  |  13  |  15  |  30  |   40   |
| [Quill Unbounded Queue](https://github.com/odygrd/quill)                 |  13  |  14  |  15  |  16  |  19  |   21   |
| [Quill Bounded Dropping Queue](https://github.com/odygrd/quill)          |  13  |  15  |  16  |  17  |  20  |   22   |
| [PlatformLab NanoLog](https://github.com/PlatformLab/NanoLog)            |  11  |  16  |  20  |  24  |  38  |   46   |
| [MS BinLog](https://github.com/Morgan-Stanley/binlog)                    |  23  |  24  |  25  |  26  |  65  |  104   |
| [Quill Unbounded Queue (Log Functions)](https://github.com/odygrd/quill) |  31  |  33  |  35  |  36  |  39  |   46   |
| [Reckless](https://github.com/mattiasflodin/reckless)                    |  77  |  92  | 101  | 105  | 112  |  126   |
| [BqLog](https://github.com/Tencent/BqLog)                                |  51  |  55  |  59  | 119  | 136  |  2973  |
| [Iyengar NanoLog](https://github.com/Iyengar111/NanoLog)                 |  55  |  86  | 103  | 128  | 170  |  1364  |
| [spdlog](https://github.com/gabime/spdlog)                               | 196  | 227  | 281  | 310  | 395  |  690   |
| [Boost.Log](https://www.boost.org)                                       | 1357 | 2420 | 2607 | 2651 | 3939 |  5682  |
| [g3log](https://github.com/KjellKod/g3log)                               | 1050 | 3886 | 4271 | 4442 | 5693 |  6831  |

![large_strings_4_thread_logging.webp](docs%2Fcharts%2Flarge_strings_4_thread_logging.webp)

#### Logging Complex Types

Logging `std::vector<std::string>` containing 16 large strings, each ranging from 50 to 60 characters.

Note: some of the previous loggers do not support passing a `std::vector` as an argument.

`LOG_INFO(logger, "Logging int: {}, int: {}, vector: {}", i, j, v)`.

##### 1 Thread Logging

| Library                                                         | 50th  | 75th  | 90th  | 95th  | 99th  | 99.9th |
|-----------------------------------------------------------------|:-----:|:-----:|:-----:|:-----:|:-----:|:------:|
| [Quill Bounded Dropping Queue](https://github.com/odygrd/quill) |  46   |  49   |  52   |  55   |  59   |   65   |
| [MS BinLog](https://github.com/Morgan-Stanley/binlog)           |  67   |  69   |  71   |  73   |  79   |  279   |
| [Quill Unbounded Queue](https://github.com/odygrd/quill)        |  139  |  150  |  160  |  165  |  173  |  183   |
| [XTR](https://github.com/choll/xtr)                             |  286  |  297  |  341  |  346  |  353  |  589   |
| [fmtlog](https://github.com/MengRao/fmtlog)                     |  646  |  660  |  685  |  705  |  733  |  767   |
| [spdlog](https://github.com/gabime/spdlog)                      | 6301  | 6363  | 6419  | 6455  | 6802  |  7414  |
| [Boost.Log](https://www.boost.org)                              | 34432 | 34623 | 34797 | 34928 | 35703 | 36233  |

![vector_1_thread_logging.webp](docs%2Fcharts%2Fvector_1_thread_logging.webp)

##### 4 Threads Logging Simultaneously

| Library                                                         | 50th  | 75th  |  90th  |  95th  |  99th  | 99.9th |
|-----------------------------------------------------------------|:-----:|:-----:|:------:|:------:|:------:|:------:|
| [Quill Bounded Dropping Queue](https://github.com/odygrd/quill) |  49   |  53   |   57   |   60   |   65   |   79   |
| [MS BinLog](https://github.com/Morgan-Stanley/binlog)           |  69   |  72   |   74   |   76   |   82   |  292   |
| [Quill Unbounded Queue](https://github.com/odygrd/quill)        |  84   |  91   |   98   |  103   |  113   |  130   |
| [fmtlog](https://github.com/MengRao/fmtlog)                     |  675  |  701  |  740   |  756   |  779   |  809   |
| [XTR](https://github.com/choll/xtr)                             |  627  | 1225  |  1317  |  1376  | 205300 | 287631 |
| [spdlog](https://github.com/gabime/spdlog)                      | 6593  | 6671  |  6751  |  6815  |  7610  |  8794  |
| [Boost.Log](https://www.boost.org)                              | 36322 | 80294 | 151775 | 177681 | 272649 | 362401 |

![vector_4_thread_logging.webp](docs%2Fcharts%2Fvector_4_thread_logging.webp)

The benchmark methodology involves logging 20 messages in a loop, calculating and storing the average latency for those
20 messages, then waiting around ~2 milliseconds, and repeating this process for a specified number of iterations.

_In the `Quill Bounded Dropping` benchmarks, the dropping queue size is set to `262,144` bytes, which is double the
default size of `131,072` bytes._

### Throughput

Throughput is measured by calculating the maximum number of log messages the backend logging thread can write to a log
file per second (higher is better).

The tests were run on the same system used for the latency benchmarks.

Although Quill’s primary focus is not on maximizing throughput, it efficiently manages log messages across multiple
threads. Benchmarking throughput of asynchronous logging libraries presents certain challenges. Some libraries may drop
log messages, leading to smaller-than-expected log files, while others only provide asynchronous flushing, making it
difficult to verify when the backend thread has fully processed all messages.

For comparison, we benchmark against other asynchronous logging libraries that offer guaranteed logging with a
flush-and-wait mechanism.

Note that `MS BinLog` writes log data to a binary file, which requires offline formatting with an additional
program—this makes it an unfair comparison, but it is included for reference.

Similarly, `BqLog (binary log)` uses the compressed binary log appender, and its log files are not human-readable unless
processed offline. However, it is included for reference. The other version of `BqLog` is using a text appender and
produces human-readable log files.

In the same way, `Platformlab Nanolog` also outputs binary logs and is expected to deliver high throughput. However, for
reasons unexplained, the benchmark runs significantly slower (10x longer) than the other libraries, so it is excluded
from the table.

Logging 4 million times the message `"Iteration: {} int: {} double: {}"`

| Library                                                            | million msg/second | elapsed time |
|--------------------------------------------------------------------|:------------------:|:------------:|
| [MS BinLog (binary log)](https://github.com/Morgan-Stanley/binlog) |       62.93        |    62 ms     |
| [BqLog (binary log)](https://github.com/Tencent/BqLog)             |       14.71        |    271 ms    |
| [XTR](https://github.com/choll/xtr)                                |        8.16        |    490 ms    |
| [Quill](https://github.com/odygrd/quill)                           |        5.24        |    763 ms    |
| [spdlog](https://github.com/gabime/spdlog)                         |        4.32        |    925 ms    |
| [fmtlog](https://github.com/MengRao/fmtlog)                        |        2.82        |   1417 ms    |
| [Reckless](https://github.com/mattiasflodin/reckless)              |        2.72        |   1471 ms    |
| [Quill - Macro Free Mode](https://github.com/choll/xtr)            |        2.65        |   1510 ms    |
| [BqLog](https://github.com/Tencent/BqLog)                          |        2.60        |   1537 ms    |
| [Boost.Log](https://www.boost.org)                                 |        0.39        |   10102 ms   |

![throughput.webp](docs%2Fcharts%2Fthroughput.webp)

### Compilation Time

Compile times are measured using `clang 17` and for `Release` build.

Below, you can find the additional headers that the library will include when you need to log, following
the [recommended_usage](https://github.com/odygrd/quill/blob/master/examples/recommended_usage/recommended_usage.cpp)
example

![quill_v10_0_compiler_profile.speedscope.png](docs%2Fquill_v10_0_compiler_profile.speedscope.png)

There is also a compile-time benchmark measuring the compilation time of 2000 auto-generated log statements with
various arguments. You can find
it [here](https://github.com/odygrd/quill/blob/master/benchmarks/compile_time/compile_time_bench.cpp). It takes
approximately 30 seconds to compile.

![quill_v10_0_compiler_bench.speedscope.png](docs%2Fquill_v10_0_compiler_bench.speedscope.png)

### Verdict

Quill excels in hot path latency benchmarks and supports high throughput, offering a rich set of features that outshines
other logging libraries.

The human-readable log files facilitate easier debugging and analysis. While initially larger, they compress
efficiently, with the size difference between human-readable and binary logs becoming minimal once zipped.

For example, for the same amount of messages:

```
ms_binlog_backend_total_time.blog (binary log): 177 MB
ms_binlog_backend_total_time.zip (zipped binary log): 35 MB
```

```
quill_backend_total_time.log (human-readable log): 448 MB
quill_backend_total_time.zip (zipped human-readable log): 47 MB
```

If Quill were not available, MS BinLog would be a strong alternative. It delivers great latency on the hot path and
generates smaller binary log files. However, the binary logs necessitate offline processing with additional tools, which
can be less convenient.

---

## 🧩 Usage

Also, see the [Quick Start Guide](https://quillcpp.readthedocs.io/en/latest/quick_start.html) for a brief introduction.

```c++
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/std/Array.h"

#include <string>
#include <utility>

int main()
{
  // Backend  
  quill::BackendOptions backend_options;
  quill::Backend::start(backend_options);

  // Frontend
  auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("sink_id_1");
  quill::Logger* logger = quill::Frontend::create_or_get_logger("root", std::move(console_sink));

  // Change the LogLevel to print everything
  logger->set_log_level(quill::LogLevel::TraceL3);

  // A log message with number 123
  int a = 123;
  std::string l = "log";
  LOG_INFO(logger, "A {} message with number {}", l, a);

  // libfmt formatting language is supported 3.14e+00
  double pi = 3.141592653589793;
  LOG_INFO(logger, "libfmt formatting language is supported {:.2e}", pi);

  // Logging STD types is supported [1, 2, 3]
  std::array<int, 3> arr = {1, 2, 3};
  LOG_INFO(logger, "Logging STD types is supported {}", arr);

  // Logging STD types is supported [arr: [1, 2, 3]]
  LOGV_INFO(logger, "Logging STD types is supported", arr);

  // A message with two variables [a: 123, b: 3.17]
  double b = 3.17;
  LOGV_INFO(logger, "A message with two variables", a, b);

  for (uint32_t i = 0; i < 10; ++i)
  {
    // Will only log the message once per second
    LOG_INFO_LIMIT(std::chrono::seconds{1}, logger, "A {} message with number {}", l, a);
    LOGV_INFO_LIMIT(std::chrono::seconds{1}, logger, "A message with two variables", a, b);
  }

  LOG_TRACE_L3(logger, "Support for floats {:03.2f}", 1.23456);
  LOG_TRACE_L2(logger, "Positional arguments are {1} {0} ", "too", "supported");
  LOG_TRACE_L1(logger, "{:>30}", std::string_view {"right aligned"});
  LOG_DEBUG(logger, "Debugging foo {}", 1234);
  LOG_INFO(logger, "Welcome to Quill!");
  LOG_WARNING(logger, "A warning message.");
  LOG_ERROR(logger, "An error message. error code {}", 123);
  LOG_CRITICAL(logger, "A critical error.");
}
```

### Output

![example_output.png](docs%2Fexample_output.png)

### External CMake

#### Building and Installing Quill

To get started with Quill, clone the repository and install it using CMake:

```bash
git clone https://github.com/odygrd/quill.git
cd quill
mkdir cmake_build
cd cmake_build
cmake ..
make install
```

- **Custom Installation**: Specify a custom directory with `-DCMAKE_INSTALL_PREFIX=/path/to/install/dir`.
- **Build Examples**: Include examples with `-DQUILL_BUILD_EXAMPLES=ON`.

Next, add Quill to your project using `find_package()`:

```cmake
find_package(quill REQUIRED)
target_link_libraries(your_target PUBLIC quill::quill)
```

#### Sample Directory Structure

Organize your project directory like this:

```
my_project/
├── CMakeLists.txt
├── main.cpp
```

#### Sample CMakeLists.txt

Here’s a sample `CMakeLists.txt` to get you started:

```cmake
# If Quill is in a non-standard directory, specify its path.
set(CMAKE_PREFIX_PATH /path/to/quill)

# Find and link the Quill library.
find_package(quill REQUIRED)
add_executable(example main.cpp)
target_link_libraries(example PUBLIC quill::quill)
```

### Embedded CMake

For a more integrated approach, embed Quill directly into your project:

#### Sample Directory Structure

```
my_project/
├── quill/            # Quill repo folder
├── CMakeLists.txt
├── main.cpp
```

#### Sample CMakeLists.txt

Use this `CMakeLists.txt` to include Quill directly:

```cmake
cmake_minimum_required(VERSION 3.1.0)
project(my_project)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(quill)
add_executable(my_project main.cpp)
target_link_libraries(my_project PUBLIC quill::quill)
```

### Android NDK

When building Quill for Android, you might need to add this flag during configuration, but in most cases, it works
without it:

```bash
-DQUILL_NO_THREAD_NAME_SUPPORT:BOOL=ON
```

For timestamps, use `quill::ClockSourceType::System`. Quill also includes an `AndroidSink`, which integrates with
Android's logging system.

#### Minimal Example to Start Logging on Android

```c++
quill::Backend::start();

auto sink = quill::Frontend::create_or_get_sink<quill::AndroidSink>("app", [](){
    quill::AndroidSinkConfig asc;
    asc.set_tag("app");
    asc.set_format_message(true);
    return asc;
}());

auto logger = quill::Frontend::create_or_get_logger("root", std::move(sink),
                                                    quill::PatternFormatterOptions {}, 
                                                    quill::ClockSourceType::System);

LOG_INFO(logger, "Test {}", 123);
```

### Meson

#### Using WrapDB

Easily integrate Quill with Meson’s `wrapdb`:

```bash
meson wrap install quill
```

#### Manual Integration

Copy the repository contents to your `subprojects` directory and add the following to your `meson.build`:

```meson
quill = subproject('quill')
quill_dep = quill.get_variable('quill_dep')
my_build_target = executable('name', 'main.cpp', dependencies : [quill_dep], install : true)
```

### Bazel

#### Using Blzmod

Quill is available on `BLZMOD` for easy integration.

#### Manual Integration

For manual setup, add Quill to your `BUILD.bazel` file like this:

```bazel
cc_binary(name = "app", srcs = ["main.cpp"], deps = ["//quill_path:quill"])
```

---

## 📐 Design

### Frontend (caller-thread)

When invoking a `LOG_` macro:

1. Creates a static constexpr metadata object to store `Metadata` such as the format string and source location.

2. Pushes the data SPSC lock-free queue. For each log message, the following variables are pushed

| Variable   |                                                  Description                                                   |
|------------|:--------------------------------------------------------------------------------------------------------------:|
| timestamp  |                                               Current timestamp                                                |
| Metadata*  |                                        Pointer to metadata information                                         |
| Logger*    |                                         Pointer to the logger instance                                         |
| DecodeFunc | A pointer to a templated function containing all the log message argument types, used for decoding the message |
| Args...    |           A serialized binary copy of each log message argument that was passed to the `LOG_` macro            |

### Backend

Consumes each message from the SPSC queue, retrieves all the necessary information and then formats the message.
Subsequently, forwards the log message to all Sinks associated with the Logger.

![design.jpg](docs%2Fdesign.jpg)

---

## 🚨 Caveats

Quill may not work well with `fork()` since it spawns a background thread and `fork()` doesn't work well with
multithreading.

If your application uses `fork()` and you want to log in the child processes as well, you should call `quill::start()`
after the `fork()` call. Additionally, you should ensure that you write to different files in the parent and child
processes to avoid conflicts.

For example :

```c++
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/FileSink.h"

int main()
{
  // DO NOT CALL THIS BEFORE FORK
  // quill::Backend::start();

  if (fork() == 0)
  {
    quill::Backend::start();
        
    // Get or create a handler to the file - Write to a different file
    auto file_sink = quill::Frontend::create_or_get_sink<quill::FileSink>(
      "child.log");
    
    quill::Logger* logger = quill::Frontend::create_or_get_logger("root", std::move(file_sink));

    QUILL_LOG_INFO(logger, "Hello from Child {}", 123);
  }
  else
  {
    quill::Backend::start();
          
    // Get or create a handler to the file - Write to a different file
    auto file_sink = quill::Frontend::create_or_get_sink<quill::FileSink>(
      "parent.log");
    
    quill::Logger* logger = quill::Frontend::create_or_get_logger("root", std::move(file_sink));
    
    QUILL_LOG_INFO(logger, "Hello from Parent {}", 123);
  }
}
```

---

## 📝 License

Quill is licensed under the [MIT License](https://opensource.org/licenses/MIT)

Quill depends on third party libraries with separate copyright notices and license terms.
Your use of the source code for these subcomponents is subject to the terms and conditions of the following licenses.

- ([MIT License](https://opensource.org/licenses/MIT)) [{fmt}](https://github.com/fmtlib/fmt/blob/master/LICENSE)
- ([MIT License](https://opensource.org/licenses/MIT)) [doctest](https://github.com/onqtam/doctest/blob/master/LICENSE.txt)
