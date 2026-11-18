# EfiMirage

**UEFI boot-chain patching, Secure Boot and VBS state emulation for x64 Windows.**

EfiMirage is a fork of [EfiGuard](https://github.com/Mattiwatti/EfiGuard) by
[Mattiwatti](https://github.com/Mattiwatti). It builds on upstream's PatchGuard and
Driver Signature Enforcement (DSE) patching, adding configurable Secure Boot and VBS/HVCI state
emulation, unattended configuration, and a Linux build and test workflow.

## What it provides

- In-memory patching of the Windows boot manager, loader, and kernel.
- Secure Boot state emulation, enabled by default and configurable per boot.
- VBS and memory-integrity state emulation, off by default and configurable per boot.
- A configurable UEFI loader and a Windows command-line utility.
- Cross-compiled EFI and Windows binaries, verified bundles, regression tests, and OVMF tests.

Both emulations change reported state, not firmware validation, the secure kernel, or TPM
measurements. Cryptographic authorization of the runtime memory-access interface is not
implemented.

## Documentation

| Guide | Contents |
|---|---|
| [Installation and usage](docs/usage.md) | Boot methods, setup, and compatibility limits |
| [Migrating from EfiGuard](docs/usage.md#migrating-from-efiguard) | Renamed files, configuration, and source interfaces |
| [Secure Boot emulation](docs/secureboot.md) | Configuration, behavior, limitations, and verification |
| [VBS and memory integrity emulation](docs/vbs.md) | Reports, limits, registry state, and verification |
| [Build and development](docs/build.md) | Toolchains, tests, CI/CD, and releases |
| [Architecture and features](docs/architecture.md) | Boot flow, debugging, and upstream history |
| [Changelog](CHANGELOG.md) | Release history |

Report EfiMirage-specific bugs in this repository's issue tracker rather than upstream's.

## Attributions

- [EfiGuard](https://github.com/Mattiwatti/EfiGuard) by
  [Mattiwatti](https://github.com/Mattiwatti), the upstream project.
- [UPGDSED](https://github.com/hfiref0x/UPGDSED) by
  [hfiref0x](https://github.com/hfiref0x) and [Fyyre](https://github.com/Fyyre).
- [Zydis](https://github.com/zyantific/zydis) by [zyantific](https://zydis.re).
- [Uninformed](http://uninformed.org/) articles on PatchGuard
  [v1](http://uninformed.org/index.cgi?v=3&a=3&t=pdf),
  [v2](http://www.uninformed.org/?v=6&a=1&t=pdf), and
  [v3](http://uninformed.org/index.cgi?v=8&a=5&t=pdf) by Skywing.
- [UEFI-Bootkit](https://github.com/ajkhoury/UEFI-Bootkit) by
  [dude719](https://github.com/ajkhoury).
- [ReactOS](https://reactos.org).

## License

EfiMirage is licensed under [GPLv3](LICENSE). The Zydis submodule at
`EfiMirageDxe/Zydis` is licensed under MIT.
