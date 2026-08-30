# Realtime privileges

The engine is only as good as the scheduling class its audio thread gets. `avc`
reports what it actually received on startup; anything other than SCHED_FIFO or
SCHED_RR means the graph will glitch as soon as the machine is busy.

PipeWire's `libpipewire-module-rt` tries two paths, in order:

1. `sched_setscheduler()` directly, which needs `RLIMIT_RTPRIO` on the login
   session. This is the reliable path.
2. rtkit over D-Bus, used when path 1 fails. rtkit caps the priority at 20 and
   can demote the thread again afterwards, so a grant here is not a guarantee.

PipeWire already ships `/etc/security/limits.d/25-pw-rlimits.conf`, which gives
the `@pipewire` group `rtprio 95`. Distributions create the group but leave it
empty, so path 1 fails until someone is added to it:

```bash
sudo usermod -aG pipewire "$USER"
# log out and back in -- PAM limits are applied at login, not at exec
```

Verify:

```bash
ulimit -r          # want 95, not 0
./build/avc --list # then run the engine and read the "audio thread:" line
```

To check any running process by hand:

```bash
for t in /proc/$(pgrep -x avc)/task/*; do
    printf '%-14s %s\n' "$(cat "$t"/comm)" \
        "$(sed 's/.*) //' "$t"/stat | awk '{print "rt_prio="$38" policy="$39}')"
done
# policy: 0=OTHER 1=FIFO 2=RR
```
