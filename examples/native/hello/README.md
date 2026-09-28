# SolarOS native hello module

This is a minimal ESP32-S3 ELF module for exercising the SolarOS `load`
command. It imports only the versioned `solar_os_native_host_v1` function table.

Build it with an ESP-IDF 5.5 environment:

```sh
idf.py set-target esp32s3
idf.py build
```

The module is written to `build/solaros_native_hello.app.elf`. Copy that file
to device storage and run:

```text
load solaros_native_hello.app.elf SolarOS
```

The expected output is `Hello from a SolarOS native module, SolarOS!` followed
by the loader completion line. The initial runtime unloads the module as soon
as `main` returns, so this example does not create tasks or retain callbacks.
