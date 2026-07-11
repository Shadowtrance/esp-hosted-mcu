# ESP-NOW bridge (Tactility)

Bridges `esp_now_init/add_peer/send/recv` over the existing custom RPC (peer
data transfer) channel, so a host with no native WiFi radio (e.g. ESP32-P4 via
esp-hosted-mcu) can drive real ESP-NOW running on this co-processor.

Off by default. To build a slave image with it enabled (e.g. for M5Stack Tab5,
P4 host + C6 co-processor):

**Important**: if `slave/sdkconfig` or `slave/build/` already exist from a
previous build (even a plain one without these flags), delete them first.
`-D CONFIG_X=y` on the `idf.py` command line only reliably takes effect on a
fresh configure — if a resolved value for that symbol already exists in
`sdkconfig`, the new `-D` override is silently ignored and you'll get a clean
build with the bridge missing (no error, no warning).

```
cd slave
rm -rf build sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6" \
    -D CONFIG_ESP_HOSTED_ESPNOW_BRIDGE=y \
    -D CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y \
    -D CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS=8 \
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
