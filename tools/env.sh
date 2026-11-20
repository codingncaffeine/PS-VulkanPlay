# Sourced by the project's scripts. Builds and test runs share the machine with
# its user: by default they take a quarter of the cores, at low priority.
#
#   PSVP_JOBS=N         use N cores instead (CI sets it to all of them)
#   PSVP_FIRST_CORE=N   the first of them (default 0): runs that go on at once share the cores out

PSVP_JOBS="${PSVP_JOBS:-$(( ($(nproc) + 3) / 4 ))}"
PSVP_FIRST_CORE="${PSVP_FIRST_CORE:-0}"

psvp_nice() {
    local cores="$PSVP_FIRST_CORE-$((PSVP_FIRST_CORE + PSVP_JOBS - 1))"
    if command -v taskset >/dev/null 2>&1 && taskset -c "$cores" true 2>/dev/null; then
        taskset -c "$cores" nice -n 10 "$@"
    else
        nice -n 10 "$@"
    fi
}
