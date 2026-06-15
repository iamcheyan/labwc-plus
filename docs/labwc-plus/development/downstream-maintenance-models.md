# Downstream Maintenance Models: labwc-plus and SwayFX

> Written on 2026-06-15 as a maintenance reference. Repository workflows may
> change over time, so verify current upstream practice before relying on the
> specific examples below.

## Purpose

Both labwc-plus and SwayFX are downstream projects:

- labwc-plus is based on [labwc](https://github.com/labwc/labwc).
- [SwayFX](https://github.com/WillPower3309/swayfx) is based on
  [Sway](https://github.com/swaywm/sway).

Both projects need to preserve downstream features while incorporating
upstream fixes and development. The appropriate workflow depends on the size
of the downstream difference and the number of maintainers involved.

## The Current labwc-plus Model

labwc-plus maintains a small, ordered patch queue on top of upstream labwc:

```text
upstream/master
    |
    +-- labwc-plus feature commit
    +-- labwc-plus feature commit
    +-- labwc-plus documentation
    +-- labwc-plus maintenance tools
         |
         +-- origin/master
```

The important references are:

- `upstream/master`: the fetched upstream labwc branch
- local `master`: upstream labwc plus the labwc-plus commits
- `origin/master`: the published labwc-plus branch

Updating currently uses:

```sh
scripts/update-upstream.sh
```

Conceptually, this performs:

```sh
git fetch upstream
git rebase upstream/master
meson compile -C build
```

During the rebase, Git temporarily removes the downstream commits, advances
the base to the new upstream commit, and reapplies each downstream commit in
order:

```text
Before:

A--B--C                 upstream
       \
        P1--P2--P3      labwc-plus

After upstream adds D and E:

A--B--C--D--E           upstream
             \
              P1'--P2'--P3'  labwc-plus
```

The primed commits represent the same logical changes with new commit IDs
because their parent history changed.

This model is useful while:

- the downstream patch count remains manageable;
- each feature has a clear commit boundary;
- one maintainer controls the published branch;
- rewriting `origin/master` with `--force-with-lease` is acceptable.

## The Observed SwayFX Model

SwayFX has a much larger difference from Sway, including a custom renderer,
effects, configuration commands, packaging, branding, and extensive
documentation changes.

Its repository history shows version-oriented upstream updates rather than a
fully automated update for every Sway commit. Examples include:

- [Sway 1.8 rebase PR #78](https://github.com/WillPower3309/swayfx/pull/78)
- [Sway 1.10 rebase commit](https://github.com/WillPower3309/swayfx/commit/82fe097b3109b7869272ccb487a9ed691c788b96)
- [Sway 1.10.1 rebase commit](https://github.com/WillPower3309/swayfx/commit/50d4cf45ab77ea1476860b993ed34bb32d3f434a)
- [Sway 1.11 rebase PR #431](https://github.com/WillPower3309/swayfx/pull/431)

A typical SwayFX update is closer to:

```text
Create a version update branch
    |
Import or rebase onto the new Sway version
    |
Resolve code, renderer, dependency, packaging, and documentation conflicts
    |
Build and test the integrated result
    |
Review the update through a SwayFX pull request
    |
Merge the reviewed update into SwayFX master
```

Despite the use of "rebase" in update titles, the public result is often a
large reviewed integration commit produced through a pull request. This makes
the version update itself visible and reviewable without requiring every
downstream feature to remain a small patch directly above the latest Sway
commit.

The inspected GitHub Actions workflow automatically updates Nix flake inputs,
but it does not automatically integrate Sway source changes. Upstream source
integration remains a maintainer-driven operation.

## Documentation and Branding

SwayFX treats its README, branding, packaging, and feature documentation as
normal downstream-owned files. They are not removed before an upstream
update and restored afterward.

When both upstream and downstream modify the same document, maintainers
resolve the conflict by deciding how to combine:

- current upstream instructions;
- downstream branding;
- downstream feature documentation;
- downstream support and packaging information.

SwayFX contributors have explicitly discussed minimizing unnecessary changes
to reduce upstream conflicts. See
[SwayFX PR #257](https://github.com/WillPower3309/swayfx/pull/257).

The same principle applies to labwc-plus:

- use `LABWC-PLUS.md` and `docs/labwc-plus/` for downstream-only material;
- keep modifications to upstream documents small where practical;
- change upstream files when the product genuinely requires it;
- resolve real conflicts based on desired final behavior, not by blindly
  choosing one side.

## Comparison

| Property | labwc-plus today | SwayFX observed workflow |
| --- | --- | --- |
| Downstream size | Small | Large |
| Update frequency | Can follow upstream closely | Often version-oriented |
| Primary integration | Rebase patch queue | Dedicated update branch and PR |
| History rewriting | Expected on `master` | Less central to public workflow |
| Conflict handling | Commit-by-commit during rebase | Integration work reviewed as a PR |
| Automation | Fetch, rebase checks, and build script | Dependency automation; source integration is manual |
| Best fit | Small project or single maintainer | Larger project and multiple maintainers |

## When labwc-plus Should Change Models

The current patch-queue model should remain while it keeps updates simple and
understandable. Consider adopting a SwayFX-style update branch when:

- upstream updates produce many conflicts across several features;
- the downstream commit queue becomes difficult to review or reorder;
- multiple maintainers contribute to the integration;
- an upstream release requires dependency or API migration work;
- rewriting the public `master` branch becomes disruptive;
- an update needs runtime testing and review before publication.

A future version-oriented workflow could look like:

```sh
git fetch upstream
git switch -c sync/labwc-0.21 master
git rebase upstream/master

# Resolve conflicts, adapt features, build, and test.
# Push sync/labwc-0.21 and open a PR into labwc-plus master.
```

This is not necessary merely because labwc-plus modifies documentation or
upstream source files. The deciding factor is maintenance complexity.

## Practical Rule

Use the simplest model that keeps integration understandable:

```text
Small, clear downstream patch queue
    -> rebase directly onto upstream

Large, cross-cutting downstream product
    -> dedicated version update branch and reviewed integration PR
```

labwc-plus currently belongs in the first category. SwayFX is a useful example
of how the project can evolve if the downstream difference becomes much
larger.
