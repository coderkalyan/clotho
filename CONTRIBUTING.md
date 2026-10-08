# Contributing to Clotho

Issues and pull requests are welcome. Clotho is alpha software; expect rough
edges, and expect review to be strict about correctness.

Two things worth knowing before you start: the IEEE 1800-2023 LRM is the
source of truth for every semantic question, and `docs/PRINCIPLES.md`
describes what the codebase holds itself to. `docs/TODO.md` lists what is not
implemented yet; when a change closes a gap, delete its entry.

## Contributor License Agreement

Before a contribution can be merged, its author must agree to the Clotho
Contributor License Agreement. You keep the copyright in your work; the
agreement grants a license to use it in Clotho, including the right to offer
Clotho under other licenses in the future.

- Contributing as an individual: [docs/cla/individual.md](docs/cla/individual.md).
- Contributing on behalf of a company or other organization:
  [docs/cla/entity.md](docs/cla/entity.md).

To sign, follow the CLA Assistant prompt that appears on your first pull
request. If you cannot use it, or are signing the entity agreement, email a
signed copy to coder.kalyan@gmail.com.

If you are employed, check whether your employer has rights in your work
before you sign; the individual agreement asks you to confirm that it does
not, or that your employer has approved.

## Code you did not write

Do not submit code you do not hold the copyright to as if it were your own.
If a contribution includes third-party code, say so in the pull request, keep
the original copyright and license notices in the files, add an entry to
`THIRD_PARTY_NOTICES.md`, and put the license text in `LICENSES/`. The
third-party license must be compatible with Clotho's (GPL-3.0-only, or
MPL-2.0 for `src/runtime/`). If in doubt, ask in an issue or email
coder.kalyan@gmail.com before submitting.
