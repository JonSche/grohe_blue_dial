# Local ESP-IDF patches

Diagnostic (or other) patches against the vendored ESP-IDF installation
(`$IDF_PATH`), kept here purely as a reproducible record -- `$IDF_PATH` is a
separate installation outside this repo, so these changes can't be committed
where they actually live. Not applied automatically; nothing in this
project's own build depends on them.

## esp_tls_mbedtls_diagnostics.patch

Adds heap diagnostics (free / largest free block / minimum-ever free, on
`MALLOC_CAP_INTERNAL`) and the symbolic MbedTLS error name, logged
unconditionally at `ESP_LOGW`, immediately before and after the
`mbedtls_ssl_setup()` call in `components/esp-tls/esp_tls_mbedtls.c`
(`esp_create_mbedtls_handle()`). Written to investigate OTA's
`MBEDTLS_ERR_SSL_ALLOC_FAILED` (`-0x7F00`) failure -- see
`docs/ARCHITECTURE.md`'s OTA section and this repo's own history around
2026-08-07 for the full root-cause trace. Purely additive logging; does not
change `ret`'s value or any control flow in that function.

Targets ESP-IDF v5.3 (commit `e0991facf5ecb362af6aac1fae972139eb38d2e4`),
MbedTLS 3.6.0. Line numbers may not match a different ESP-IDF checkout.

**Apply** (from `$IDF_PATH`):
```sh
git apply /path/to/grohe_blue_dial/patches/esp_tls_mbedtls_diagnostics.patch
```

**Revert**:
```sh
git checkout -- components/esp-tls/esp_tls_mbedtls.c
```
(or `git apply -R` the same patch file).

After applying, a full rebuild is needed (`idf.py build`) so
`esp_tls_mbedtls.c` recompiles.
