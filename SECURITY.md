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
- **Firmware generally**: memory safety and credential handling anywhere
  else in this firmware's own code — e.g.
  [`components/grohe_ble/`](components/grohe_ble/) (Grohe appliance
  credentials) and [`components/time_service/`](components/time_service/)
  (Wi-Fi credentials, used only as a one-shot SNTP time source).
- **OTA** ([`components/ota/`](components/ota/)): the local-network Wi-Fi
  firmware-update endpoint — shared-secret handling, and any memory-safety
  issue in request parsing or the flash-write path.
  **By design, this endpoint is plain HTTP with no TLS and a single
  shared secret** — appropriate for a trusted local Wi-Fi network, not an
  Internet-facing one; see
  [ARCHITECTURE.md](docs/ARCHITECTURE.md#ota-m12) for the full rationale
  and threat model. Reports that this isn't TLS-encrypted are already
  known and out of scope; reports of a way to bypass the shared-secret
  check entirely, or a memory-safety bug in the upload handler, are very
  much in scope.
- **Provisioning** ([`components/provisioning/`](components/provisioning/)):
  the local-network HTTP endpoint (`POST /provision`) that writes Grohe
  BLE credentials into NVS — shared-secret handling (a token separate
  from the OTA one above), JSON request parsing, and the NVS write path.
  **Same posture as OTA: plain HTTP, no TLS, a single shared secret,
  permanently reachable whenever Wi-Fi is connected** — see
  [ARCHITECTURE.md](docs/ARCHITECTURE.md#provisioning-m132) for the full
  rationale and threat model. This endpoint is more consequential than
  OTA's if compromised — the request body itself carries the Grohe BLE
  credentials in the clear, not just an update payload — so reports that
  this isn't TLS-encrypted are already known and out of scope for the
  same documented reason OTA's are, but reports of a way to bypass the
  provisioning-token check, reuse the OTA token to provision (it must
  not work — the two are deliberately separate secrets), or a
  memory-safety/parsing bug in the request handler are very much in
  scope.

Out of scope: vulnerabilities in ESP-IDF, NimBLE, mbedTLS, or any other
upstream dependency this firmware builds on — please report those to their
own maintainers. The GROHE Blue Home appliance's own firmware/cloud
services are also out of scope; this policy covers the dial firmware in
this repository only.
