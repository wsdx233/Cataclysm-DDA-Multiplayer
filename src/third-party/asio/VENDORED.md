# standalone Asio vendoring record

All Asio headers in this directory and `../asio.hpp` are the unmodified contents of the upstream
`include/` directory from standalone Asio 1.38.1. This record is the only additional file.

- Repository: `https://github.com/chriskohlhoff/asio`
- Tag: `asio-1-38-1`
- Commit: `dfd7b3e3145bac5d0e91a99fde69c6ae1442f971`
- Source archive: `https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-38-1.tar.gz`
- Source archive SHA-256: `2827b229972be80cdb14e5497962fa393d1adf036b5869e2b9c99f644daadacc`
- License: Boost Software License 1.0, reproduced in `LICENSE_1_0.txt`
- License SHA-256: `c9bff75738922193e67fa726fa225535870d2aa1059f91452c411736284ad566`

Do not patch these headers locally. An update must change the pin intentionally, rerun the Linux GCC/Clang,
Windows MSVC and Android NDK transport gates, compare the vendored headers byte-for-byte with the pinned
archive, and update ADR-0003 plus the build baseline.
