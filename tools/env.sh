# Sourced by the project's scripts. Builds and test runs share the machine with
# its user: by default they take a quarter of the cores, at low priority.
#
#   PSVP_JOBS=N   use N cores instead (CI sets it to all of them)

PSVP_JOBS="${PSVP_JOBS:-$(( ($(nproc) + 3) / 4 ))}"

psvp_nice() {
    if command -v taskset >/dev/null 2>&1 && taskset -c "0-$((PSVP_JOBS - 1))" true 2>/dev/null; then
        taskset -c "0-$((PSVP_JOBS - 1))" nice -n 10 "$@"
    else
        nice -n 10 "$@"
    fi
}
