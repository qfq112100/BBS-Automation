# Static runtime notices

The Windows x64 release uses the official LLVM-MinGW UCRT toolchain, release 20261020 (LLVM/Clang 23.1.4). C++/compiler runtimes are linked statically; only Windows system DLL imports are permitted by the build audits.

- LLVM-LICENSE.TXT: upstream LLVM Project license (Apache-2.0 WITH LLVM-exception), covering libc++, libc++abi, libunwind and compiler-rt as distributed.
- COPYING.MinGW-w64-runtime.txt, COPYING.MinGW-w64.txt and MinGW-COPYING.txt: upstream MinGW-w64 runtime notices.
- COPYING.winpthreads.txt and COPYING.winstorecompat.txt: accompanying runtime license notices.

These original license texts are copied from the verified official portable archive. Development tools and their ZIP archives are excluded from the application/source deliverable.
