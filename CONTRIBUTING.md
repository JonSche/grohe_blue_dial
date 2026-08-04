# Contributing to Grohe Dial

Thanks for taking the time to contribute — bug reports, fixes, and
improvements are all welcome.

## Building

See the [README](README.md#building) for the full build instructions
(ESP-IDF v5.3+, `idf.py build flash monitor`). If you're testing against a
real appliance, you'll also need your own credentials — see the README's
note on the gitignored `*_local.hpp` files.

## Coding style

Match the conventions already used throughout the codebase rather than
introducing new ones:

- One component, one concern, with dependencies pointing one way — see
  [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)'s "Goals behind the
  structure".
- C++20, PascalCase classes, snake_case namespaces/files, `kConstantName`
  for constants, a trailing underscore for private members.
- Comments explain *why*, not *what* — skip a comment if the code already
  says what it does; add one only for a non-obvious constraint, invariant,
  or reason a decision was made a particular way.
- No unnecessary abstractions. A bug fix doesn't need surrounding cleanup;
  don't build for hypothetical future requirements.

## Pull requests

- Keep pull requests small and focused on one change. Large, mixed-purpose
  PRs are harder to review and more likely to get stuck.
- Run `idf.py build` and confirm it compiles cleanly, with no new warnings,
  before submitting — this project has no separate test suite; a clean
  build plus (where practical) real hardware validation is the bar every
  milestone in [`docs/ROADMAP.md`](docs/ROADMAP.md) is held to. If you
  can't validate on real hardware, say so in the PR description rather
  than leaving it unstated.
- For anything beyond a small fix — a new component, a changed protocol
  interaction, a different architectural approach — please open an issue
  to discuss it before writing the code. It's much easier to agree on a
  direction first than to rework a finished PR.

## Reporting bugs

Open an issue with what you expected, what happened instead, and your
hardware/build details (ESP-IDF version, target board). See
[`SECURITY.md`](SECURITY.md) instead if you're reporting a security
vulnerability.
