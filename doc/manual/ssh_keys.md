+++
id = "ssh_keys"
title = "SSH identity keys"
section = "network"
summary = "Inspect, share, generate, and remove the default SSH key pair"
aliases = ["sshkeys", "sshkey"]
keywords = "python lua ssh scp keys identity private public generate remove credentials"
packages_any = ["service_ssh"]
+++
# SSH identity keys

SolarOS can keep a default SSH key pair for `ssh` and `scp`. The public key may
be copied to remote hosts; the private key must remain protected.

## Inspect or create a key

```text
sshkey status
sshkey gen 2048
sshkey pub
```

Generating a key may take time and memory. Without `-f`, an existing key is not
overwritten.

## Protect private material

Copy only the public key to the remote host's `authorized_keys`. Keep the
private key on the device. The agent's storage tools cannot access `.ssh`.

## Quick reference

solaros.ssh_keys provides default_paths, default_exists, status, public_key,
generate (optional bits and overwrite), and remove when SSH is installed.
`public_key()` returns only the default OpenSSH public-key line. Share that
line with the remote host; keep the private key on the device.
