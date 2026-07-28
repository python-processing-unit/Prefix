# Prefix

C17 implementation of the Prefix programming language.

Overview
--------

This repository contains the reference implementation of the Prefix
programming language, written in ISO C17 and targeting Clang on baseline
x64. The codebase includes the lexer, parser, interpreter/runtime,
standard library bindings, and supporting tools used by the reference
implementation.

Supported platforms:

- Windows Vista or newer.
- Ubuntu 22.04 or newer (x86_64).

Project layout
--------------

- `.github/workflows/`: GH Actions CI configuration.
- `docs/`: Documentation for Prefix, builds GH Pages site.
- `ext/`
- `lib/`
- `src/`: Source code and headers for the interpreter.
- `tests/`: Automated test suite.
- `.gitignore`: Git ignore file.
- `build.ps1`: Build script for the interpreter and tests.
- `README.md`: This file.


Requirements
-------------

- LLVM/Clang toolchain with C17 support.
- LLD linker (installed with LLVM on most platforms; `lld` package on Linux).
- PowerShell (`pwsh`) to run the included build and test scripts.

On Ubuntu 22.04+:

```bash
sudo apt-get install clang lld powershell
```

On Windows: install [LLVM/Clang](https://releases.llvm.org/) and
[PowerShell](https://github.com/PowerShell/PowerShell) (or use the
built-in Windows PowerShell).

Building
--------

To build the interpreter run:

```powershell
pwsh ./build.ps1
```

The `build.ps1` script invokes Clang and produces the interpreter, standard library, and test binaries.
Ensure your Clang installation is on `PATH` before running the script.

Testing
-------

Automated tests live under the top-level `tests/` directory. You can run the test harness with PowerShell:

```powershell
pwsh ./tests/test.ps1
```

Documentation
-------------

The docs are located in `docs/`.

License
-------

Prefix is distributed under the [Unlicense](https://unlicense.org/).

Versioning
----------

Prefix follows [SemVer 2.0](https://semver.org), treating [the specification](https://python-processing-unit.github.io/Prefix/SPECIFICATION.html) as the public API.
