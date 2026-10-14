#!/bin/bash
# Commits and pushes the generated website (https://accerberus.github.io).
# Run by `make site-push` after tools/gen-site.py has written the site.
#
# usage: push-site.sh <site-dir> <message>
#
# Works from WSL and from Git Bash. From WSL it prefers the Windows git
# (git.exe), which holds the owner's GitHub credentials; WSL's own git has
# none. Nothing is pushed when there is nothing to commit.
set -u
SITE="$1"
MSG="${2:-site: update}"
GIT=git
for g in "/mnt/c/Program Files/Git/cmd/git.exe" "/c/Program Files/Git/cmd/git.exe"; do
    if [ -x "$g" ] && [ -d /mnt/c ] && [ -n "${WSL_DISTRO_NAME:-}" ]; then GIT="$g"; break; fi
done
if [ -n "${WSL_DISTRO_NAME:-}" ] && [ "$GIT" != git ]; then
    DIR=$(wslpath -w "$SITE")
elif command -v cygpath >/dev/null 2>&1; then
    DIR=$(cygpath -w "$SITE")   # Git Bash: a Windows path survives MSYS_NO_PATHCONV=1
else
    DIR="$SITE"
fi
cd "$SITE" || { echo "push-site: $SITE not found"; exit 1; }
"$GIT" -C "$DIR" add -A >/dev/null
if "$GIT" -C "$DIR" diff --cached --quiet; then
    echo "push-site: nothing changed"
    exit 0
fi
"$GIT" -C "$DIR" commit -q -m "$MSG" || exit 1
"$GIT" -C "$DIR" push -q origin HEAD:main && echo "push-site: pushed $("$GIT" -C "$DIR" rev-parse --short HEAD)"
