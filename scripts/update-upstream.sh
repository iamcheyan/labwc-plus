#!/bin/sh

set -eu

usage()
{
	cat <<EOF
Usage: scripts/update-upstream.sh [--push]

Fetch upstream labwc, rebase the labwc-plus patch queue onto
upstream/master, and build the result.

Options:
  --push  Push the rebased master to origin after a successful build.
  -h      Show this help.

Environment:
  BUILD_DIR  Meson build directory (default: build).
EOF
}

push_after_build=false

for arg do
	case "$arg" in
	--push)
		push_after_build=true
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		printf 'error: unknown option: %s\n' "$arg" >&2
		usage >&2
		exit 2
		;;
	esac
done

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || {
	printf 'error: not inside a Git repository\n' >&2
	exit 1
}
cd "$repo_root"

current_branch=$(git branch --show-current)
if [ "$current_branch" != "master" ]; then
	printf 'error: current branch is %s; switch to master first\n' \
		"${current_branch:-detached HEAD}" >&2
	exit 1
fi

for remote in origin upstream; do
	if ! git remote get-url "$remote" >/dev/null 2>&1; then
		printf 'error: required remote "%s" is not configured\n' "$remote" >&2
		exit 1
	fi
done

rebase_merge=$(git rev-parse --git-path rebase-merge)
rebase_apply=$(git rev-parse --git-path rebase-apply)
if [ -d "$rebase_merge" ] || [ -d "$rebase_apply" ]; then
	printf 'error: a rebase is already in progress\n' >&2
	printf 'finish it with "git rebase --continue" or cancel it with '
	printf '"git rebase --abort"\n' >&2
	exit 1
fi

if ! git diff --quiet || ! git diff --cached --quiet; then
	printf 'error: tracked files contain uncommitted changes\n' >&2
	printf 'commit or stash them before updating upstream\n' >&2
	exit 1
fi

build_dir=${BUILD_DIR:-build}

printf '==> Fetching labwc-plus origin\n'
git fetch --prune origin

if ! git merge-base --is-ancestor origin/master master; then
	printf 'error: origin/master contains commits not present locally\n' >&2
	printf 'inspect the divergence before rebasing:\n' >&2
	printf '  git log --oneline --left-right master...origin/master\n' >&2
	exit 1
fi

printf '==> Fetching upstream labwc\n'
git fetch --prune upstream

old_head=$(git rev-parse HEAD)
git branch -f backup/pre-upstream-update "$old_head"

if git merge-base --is-ancestor upstream/master master; then
	printf '==> master already contains upstream/master\n'
else
	printf '==> Rebasing labwc-plus commits onto upstream/master\n'
	if ! git rebase upstream/master; then
		cat >&2 <<EOF

The rebase stopped because Git found a conflict.

1. Run: git status
2. Edit each conflicted file and remove the conflict markers.
3. Run: git add <resolved-files>
4. Run: git rebase --continue
5. Repeat until the rebase finishes.

To cancel and return to the previous state:
  git rebase --abort

A backup of the pre-update commit is available at:
  backup/pre-upstream-update
EOF
		exit 1
	fi
fi

printf '==> Checking patch whitespace\n'
git diff --check upstream/master..master

if [ ! -f "$build_dir/build.ninja" ]; then
	printf '==> Configuring build directory: %s\n' "$build_dir"
	meson setup "$build_dir"
fi

printf '==> Building: %s\n' "$build_dir"
meson compile -C "$build_dir"

if [ "$push_after_build" = true ]; then
	printf '==> Pushing labwc-plus master\n'
	git push --force-with-lease origin master
else
	cat <<EOF
==> Update and build completed without pushing.

Review and run labwc-plus, then publish with:
  git push --force-with-lease origin master

Or run this script with --push next time.
EOF
fi
