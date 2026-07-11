# ESP-NOW bridge (Tactility)

Bridges `esp_now_init/add_peer/send/recv` over the existing custom RPC (peer
data transfer) channel, so a host with no native WiFi radio (e.g. ESP32-P4 via
esp-hosted-mcu) can drive real ESP-NOW running on this co-processor.

Off by default. To build a slave image with it enabled (e.g. for M5Stack Tab5,
P4 host + C6 co-processor):

**Important**: `-D CONFIG_X=y` on the `idf.py` command line does NOT work for
this — ESP-IDF 5.5.2's kconfig generation (`tools/cmake/kconfig.cmake`) only
ever reads `--defaults` files; it never looks at `CONFIG_*` CMake cache
variables (confirmed by reading the actual `kconfgen` invocation and by
reproducing the failure on a clean tree — the flags land in `CMakeCache.txt`
but are silently dropped before reaching the generated `sdkconfig`, no error).
Use the checked-in `sdkconfig.defaults.espnow_bridge` file instead:

```
cd slave
rm -rf build sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.espnow_bridge" \
    set-target esp32c6 build
```

Verify it actually took before flashing:
```
grep ESPNOW_BRIDGE sdkconfig                                # expect: CONFIG_ESP_HOSTED_ESPNOW_BRIDGE=y
grep -c slave_espnow_bridge build/compile_commands.json      # expect: nonzero
```

`CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS` must be raised from the default of
3 — this bridge alone registers 4 handlers (init/deinit/add_peer/send), and
the host side (Tactility's `EspNowBackendHosted.cpp`) registers 6 more on its
end (4 response + 2 event). Each side's cap only needs to cover its own
handler count, but 8 leaves headroom for other custom-RPC users.

See `slave/main/slave_espnow_bridge.c` for the implementation and
`common/espnow_bridge/esp_hosted_espnow_bridge_proto.h` for the wire format
(kept in sync by hand with a duplicate copy in the Tactility repo).
