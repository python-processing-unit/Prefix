# Prefix

Prefix is a programming language focused on explicit, readable code.

```prefix
! LCG cracker.
! Recovers the modulus m, multiplier a, and constant c of a linear
! congruential generator from a short run of consecutive outputs, then
! predicts the next value.

! ---------- modular inverse via the extended Euclidean algorithm ----------
func int modinv(int a, int m){
    a = mod(a, m)
    if(lt(a, 0)){ a = +(a, m) }
    int r0 = m
    int r1 = a
    int t0 = 0
    int t1 = 1
    while(neq(r1, 0)){
        int q = /(r0, r1)
        int nr = -(r0, *(q, r1))
        int nt = -(t0, *(q, t1))
        r0 = r1
        r1 = nr
        t0 = t1
        t1 = nt
    }
    if(neq(r0, 1)){
        throw("modinv: arguments not coprime")
    }
    int inv = mod(t0, m)
    if(lt(inv, 0)){ inv = +(inv, m) }
    return(inv)
}

! ---------- the hidden LCG (the secret being cracked) ----------
! x_{n+1} = (a * x_n + c) mod m
! The modulus is kept below 2^31 so that products of two consecutive
! differences (each < m) never overflow Prefix's signed 64-bit int.
int m = 0x186A1
int a = 16807
int c = 12345
int seed = 48271

func int step(int s){
    return(mod(+( *(a, s), c), m))
}

! ---------- collect observed outputs (all the attacker sees) ----------
map obs
int s = seed
for(i, 8){
    obs<i> = s
    s = step(s)
}
del("seed")
del("s")

! ---------- recover the modulus m ----------
! t_i = x_{i+1} - x_i  follows  t_{i+1} = a * t_i (mod m), so each
! u_k = t_{k+1} * t_{k-1} - t_k^2 is a multiple of m.  gcd of several
! |u_k| yields m.
map d
for(i, 7){
    d<i> = -(obs<+(i, 1)>, obs<i>)
}

int mrec = 0
int k = 2
while(lte(k, 6)){
    int lo = -(k, 1)
    int hi = +(k, 1)
    int tk = d<k>
    mrec = gcd(mrec, abs(-( *(d<hi>, d<lo>), *(tk, tk) )))
    k = +(k, 1)
}
del("d")
del("k")

! ---------- recover a and c ----------
! a = (x2 - x1) * (x1 - x0)^{-1}  (mod m)
! c = x1 - a * x0                  (mod m)
int x0 = obs<0d1>
int x1 = obs<0d2>
int x2 = obs<0d3>
int arec = mod(*( -(x2, x1), modinv(-(x1, x0), mrec) ), mrec)
int crec = mod(-(x1, *(arec, x0)), mrec)
if(lt(arec, 0d0)){ arec = +(arec, mrec) }
if(lt(crec, 0d0)){ crec = +(crec, mrec) }
del("r0")
del("r1")
del("t0")
del("t1")
del("q")
del("nr")
del("nt")
del("inv")
del("x2")
del("x1")
del("modinv")

! ---------- verify by regenerating the whole sequence ----------
int ok = 1
int v = x0
for(i, 8){
    if(neq(v, obs<i>)){
        ok = 0
    }
    v = mod(+( *(arec, v), crec), mrec)
}
del("x0")
del("v")

! ---------- report ----------
print("hidden : m=", m, " a=", a, " c=", c)
print("cracked: m=", mrec, " a=", arec, " c=", crec)
print("sequence reproduced: ", ok)

! ---------- predict the next output ----------
int next_pred = mod(+( *(arec, obs<0d8>), crec), mrec)
int next_true = step(obs<0d8>)
del("obs")
del("step")
print("next output predicted: ", next_pred)
print("next output actual  : ", next_true)

assert(eq(ok, 1))
assert(eq(mrec, m))
assert(eq(arec, a))
assert(eq(crec, c))
assert(eq(next_pred, next_true))
```

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

Prefix follows [SemVer 2.0](https://semver.org), treating [the specification](https://python-processing-unit.github.io/Prefix/specification/0-toc-and-preamble.html) as the public API.
