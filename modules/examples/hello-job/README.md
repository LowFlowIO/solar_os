# SolarOS native hello job

This acceptance module registers a background job through the versioned native
job ABI. Install it with `pkg install hello-job`, then exercise the lifecycle:

```text
job start hello-job
job status hello-job
pkg remove hello-job
job stop hello-job
```

The status tick count should increase while the job is running. Removal must be
rejected until the job is stopped. Before removing it, reboot once and confirm
that `job status hello-job` still lists the installed job, then stop it if
necessary and run `pkg remove hello-job`.
