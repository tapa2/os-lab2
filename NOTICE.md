# Third-party code

The following directories come from the
[Operating Systems course by open-education-hub](https://github.com/open-education-hub/operating-systems/tree/main/content/assignments/memory-allocator)
and are distributed under the BSD-3-Clause license (see the SPDX headers in each file):

- `utils/` — library interface (`osmem.h`), block metadata (`block_meta.h`), heap-free `printf()`
- `tests/` — official test suite (`snippets/`, `ref/`, `run_tests.py`, `.ltrace.conf`)
- `src/Makefile` and the initial `src/osmem.c` skeleton
- `docs/` — assignment specification and images

The allocator implementation in `src/` is my own work.
