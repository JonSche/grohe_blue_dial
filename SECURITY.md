# Security Policy

## Reporting a vulnerability

If you find a security vulnerability in Grohe Dial, please report it
privately rather than opening a public issue.

**Preferred**: use GitHub's private vulnerability reporting — open this
repository's **Security** tab and select **Report a vulnerability**. This
creates a private advisory visible only to the maintainer.

If that isn't available to you, open a regular issue asking to be
contacted privately, without any vulnerability details in the issue
itself, and a private channel will be set up from there.

Please do not disclose the vulnerability publicly — including in a GitHub
issue, pull request, or discussion — until it has been addressed. This
gives anyone running the firmware a chance to update before details become
public.

Include as much detail as you can: the affected component or file, steps
to reproduce, and the potential impact.

## Supported versions

This project does not yet have tagged releases with a formal support
window. Security fixes are made against the `main` branch — please build
and report against the latest commit there.

## Scope

Security reports are in scope for:

- **BLE**: the NimBLE-based connection handling and the Grohe protocol
  implementation ([`components/grohe_ble/`](components/grohe_ble/)) —
  authentication (HMAC/credential handling), payload parsing, and the
  connection state machine.
- **OTA**: the firmware update mechanism
  ([`components/ota/`](components/ota/)) — image verification, rollback
  behavior, and how update sources are handled.
- **Firmware generally**: memory safety and credential handling anywhere
  else in this firmware's own code — e.g.
  [`components/grohe_ble/`](components/grohe_ble/) (Grohe appliance
  credentials) and [`components/time_service/`](components/time_service/)
  (Wi-Fi credentials, used only as a one-shot SNTP time source).

Out of scope: vulnerabilities in ESP-IDF, NimBLE, mbedTLS, or any other
upstream dependency this firmware builds on — please report those to their
own maintainers. The GROHE Blue Home appliance's own firmware/cloud
services are also out of scope; this policy covers the dial firmware in
this repository only.
