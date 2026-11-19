#!/usr/bin/env bash
# Every author and committer in the history must be the project identity.
# Exit 0 = clean, 1 = another identity found.
set -euo pipefail
cd "$(dirname "$0")/.."

allowed='codingncaffeine <codingncaffeine@users.noreply.github.com>'
# GitHub's web editor signs the same account with its numbered address.
web='codingncaffeine <19158626+codingncaffeine@users.noreply.github.com>'
# GitHub stamps commits made on its site with its own committer identity.
github='GitHub <noreply@github.com>'

if ! git rev-parse --verify -q HEAD >/dev/null; then
    echo "identity-audit: no commits yet"
    exit 0
fi

mapfile -t found < <(git log --all --format='%an <%ae>%n%cn <%ce>' | sort -u)
bad=0
for id in "${found[@]}"; do
    if [[ "$id" != "$allowed" && "$id" != "$web" && "$id" != "$github" ]]; then
        echo "identity-audit: another identity in the history: $id"
        bad=1
    fi
done
[ $bad -eq 0 ] && echo "identity-audit: ${#found[@]} identity line(s), all the project identity"
exit $bad
