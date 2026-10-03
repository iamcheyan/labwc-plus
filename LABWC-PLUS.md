# labwc-plus

labwc-plus is an unofficial downstream fork of
[labwc](https://github.com/labwc/labwc). The project regularly rebases its
changes on top of upstream `master` to retain labwc fixes and improvements
while maintaining a small set of downstream changes.

labwc-plus is maintained independently and is not supported by the upstream
labwc project. Report issues that occur with this fork to the
[labwc-plus issue tracker](https://github.com/iamcheyan/labwc-plus/issues).

## Downstream Behavior

labwc-plus keeps workspaces independent per output. Each monitor can show and
switch its own workspace without changing the active workspace on other
outputs. See [the design notes](docs/labwc-plus/design/per-output-workspaces.md)
for implementation details.

## Building

labwc-plus uses the same build process and dependencies as upstream labwc:

```sh
meson setup build/
meson compile -C build/
```

See [README.md](README.md) for the full upstream build, configuration, and
usage documentation.

## Relationship With Upstream

The repository layout is:

- `upstream/master`: upstream labwc
- `origin/master`: upstream labwc plus the labwc-plus patch queue

Upstream documentation and behavior apply unless this document states
otherwise. Downstream changes may evolve as upstream evolves.

For upstream labwc bugs that can also be reproduced without the labwc-plus
changes, report them to the upstream project. For regressions or behavior
specific to downstream changes, report them to labwc-plus.

## Updating From Upstream

The maintenance script fetches upstream labwc, rebases the labwc-plus commits,
checks the resulting patch, and builds it:

```sh
scripts/update-upstream.sh
```

After manually reviewing and testing the result, publish it with:

```sh
git push --force-with-lease origin master
```

To push automatically after a successful build:

```sh
scripts/update-upstream.sh --push
```

If Git finds a conflict, the script stops without pushing and prints the
commands needed to resolve or abort the rebase.
