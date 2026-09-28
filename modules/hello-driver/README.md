# SolarOS native hello driver

This acceptance module registers a zero-resource utility driver. It does not
claim or touch any GPIO, bus, or peripheral. Install and exercise it with:

```text
pkg install hello-driver
expansion drivers
expansion attach hello-driver hello0
expansion devices
pkg remove hello-driver
expansion detach hello0
```

Removal must be rejected while `hello0` remains attached. Before removing it,
reboot once with no attached instance and confirm that `hello-driver` still
appears in `expansion drivers`, then run `pkg remove hello-driver`.
