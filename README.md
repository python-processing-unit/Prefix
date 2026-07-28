# Prefix

C17 implementation of the Prefix programming language.

Overview
--------

This repository contains the reference implementation of the Prefix
programming language, written in ISO C17 and targeting Clang on baseline
 x64 Windows. The codebase includes the lexer, parser, interpreter\runtime,
standard library bindings, and supporting tools used by the reference
implementation.

Project layout
--------------

- `.github\workflows\`: GH Actions CI configuration.
- `docs\`: Documentation for Prefix, builds GH Pages site.
- `ext\`
- `lib\`
- `src\`: Source code and headers for the interpreter.
- `tests\`: Automated test suite.
- `.gitignore`: Git ignore file.
- `build.ps1`: Build script for the interpreter and tests.
- `README.md`: This file.


Requirements
-------------

- Windows Vista or newer.
- LLVM\Clang toolchain with C17 support.
- PowerShell (to run the included build script)

Building
--------

To build the interpreter run:

```powershell
& path\to\Prefix\build.ps1
```

The `build.ps1` script invokes Clang and produces the interpreter, standard library, and test binaries.
Ensure your Clang installation is on `PATH` before running the script.

Testing
-------

Automated tests live under the top-level `tests\` directory. You can run the test harness with PowerShell:

```powershell
& path\to\Prefix\tests\test.ps1
```

Documentation
-------------

The docs are located in `Prefix\docs\`.

License
-------

Prefix is distributed under the [Unlicense](https://unlicense.org/).

Versioning
----------

Prefix follows [SemVer 2.0](https:\\semver.org), treating [the specification](https://python-processing-unit.github.io/Prefix/SPECIFICATION.html) as the public API.
