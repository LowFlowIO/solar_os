# SolarOS native hello module

This is the minimal official native module used to exercise the SolarOS module
build, publication, installation, and `load` paths. It imports only the
versioned `solar_os_native_host_v1` function table.

Build it with an ESP-IDF 5.5 environment:

```sh
idf.py set-target esp32s3
idf.py build
```

The module is written to `build/solaros_native_hello.app.elf`. A manually copied
artifact can be run directly:

```text
load solaros_native_hello.app.elf SolarOS
```

After publication through `solar_os_cid`, install and run it with:

```text
pkg available
pkg install hello
hello SolarOS
```

The expected output is `Hello from a SolarOS native module, SolarOS!`. Installed
application modules are resolved as `/modules/<command>.app.elf`; built-in
commands, compiled applications, and aliases take precedence. The initial
runtime unloads the module as soon as `main` returns, so this example does not
create tasks or retain callbacks. Use `load /modules/hello.app.elf SolarOS` to
exercise the low-level loader directly.
