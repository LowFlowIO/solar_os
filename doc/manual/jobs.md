+++
id = "jobs"
title = "Background jobs"
section = "job"
summary = "Start, inspect, and stop background workers"
aliases = ["job", "background"]
keywords = "python lua jobs background task process start stop state waiting failed memory stack"
packages_any = []
+++
# Background jobs

Jobs are named background workers such as logging, acquisition, bridges, NTP,
and HTTP serving. They are not foreground applications and continue when you
switch shell or app sessions.

## Start, inspect, and stop

```text
jobs
job start batmon 60
job status batmon
job stop batmon
```

This starts battery monitoring every 60 seconds. Available jobs depend on the
board and firmware. Only one instance of each job name can run; starting it
again replaces the previous invocation.

Use `job status NAME` to see errors and the ports, files, or streams it owns.
For example, a logger can write to a file:

```text
job start log file /logs/system.log info
job status log
job stop log
```

Create the destination directory first. Jobs stop at reboot; add their start
commands to the selected startup script to restore them. Run `setterm startup`
to find that script. See [Job reference](jobs.reference.md) for each job's
arguments.

## Understand the state

| State | What to do |
| --- | --- |
| `running` | The job started successfully. Inspect its status for progress. |
| `waiting` | SolarOS retained the start request and will retry when enough worker memory is available. |
| `failed` | A start or runtime error occurred. Check `job status NAME`, resolve the error, then restart. |
| `stopped` | No job or pending start request is active. |

The `STACK` column reports worker-stack bytes, not total memory use. Use `mem`
when a job waits or reports no memory. Stop unused jobs to free their resources.

## Quick reference

solaros.jobs provides list(), count(), status(name), start(name,
optional_args), and stop(name). Status includes state, last_error,
worker_stack_bytes, worker_stack_external, tick timing, and deadline telemetry.
The shell's detailed status adds the current start-admission disposition and
reason.
Waiting means launch admission has not yet succeeded; failed records a terminal
launch or runtime error.

The `midi` job owns one named MIDI bus and moves messages in both directions.
Create the bus with `expansion bus create midi`, then use `midi status` to
inspect its counters. `midi stream add <channel> <controller>` exposes an exact
incoming CC as a scalar `midi.cc.<channel>.<controller>` stream. `midi monitor`
prints incoming CC and key identifiers to help select that mapping.

The `controls` job samples configured scalar streams at 50 Hz and sends changed
normalized values to native application parameters and MIDI CC targets. Create
the mappings with `control`; see `man controls` for the complete workflow.
