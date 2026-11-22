# Contributing

Thanks for your interest in RTL Wiring Inspector.

## Before you start

Open an issue describing the change you plan to make. For anything beyond a
small fix, agree on the approach first so the work is not rejected after it is
done.

## Workflow

1. Create a topic branch off the default branch.
2. Keep a pull request to one logical change.
3. Make sure the project builds, the tests pass, and the documentation still
   builds.
4. In the pull request description, state what changed and how you verified it.

## Code style

Match the formatting of the surrounding code. C++ sources follow the
`.clang-format` configuration in this repository; Python sources follow the
configured formatter. Run both before committing.

New behaviour needs corresponding tests. Existing tests live in `tests/` and
are registered with CTest.

## License

This project is a derivative work; its provenance and license terms are
recorded in [NOTICE](NOTICE) and [LICENSE](LICENSE). By submitting a
contribution you agree that it is licensed under the terms in LICENSE and that
those provenance notes are kept intact.
