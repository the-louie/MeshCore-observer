# Host unit tests

Fast, hardware-free unit tests for the fork's pure logic, run on the host with
GoogleTest via PlatformIO's `native` environment. They cover the extractable
observer/WebConfig logic (validation, preset table, topic templates, key
parsing) — the parts that don't depend on the ESP32, radio, or network stack.
Integration behavior (AsyncTCP transport, WiFi/MQTT, SoftAP) is exercised
separately; see "Local testing without hardware" in `MQTT_IMPLEMENTATION.md`.

## Running

```sh
pio test -e native                      # all suites
pio test -e native -f test_webconfig_keys   # a single suite
```

A green `[PASSED]` per suite means GoogleTest returned 0 (all assertions
passed). PlatformIO's "0 test cases" line is just its Unity-style counter and
does not reflect the GoogleTest count — run the built binary directly
(`.pio/build/native/program`) to see the per-assertion breakdown.

## Suites

| Suite | Source under test | Covers |
|-------|-------------------|--------|
| `test_mqtt_presets` | `src/helpers/MQTTPresets.h` | preset lookup; table integrity (unique names, non-empty URLs, JWT-audience invariant, names fit the slot buffer); `mqttPresetNeedsSlotCredentials`; slot-count constants |
| `test_observer_validation` | `src/helpers/MQTTObserverValidation.h` | IATA (exactly 3 alphanumerics), owner key (64 hex), NTP hostname, and the buffer-fit check behind the #17 length validation — including boundaries and nulls |
| `test_webconfig_keys` | `src/helpers/WebConfigKeys.h` | POST-key allowlist, secret detection, admin-password classification/validation, slot-index bounds, and the short-key out-of-bounds guard (attacker-supplied keys) |
| `test_topic_template` | `src/helpers/MQTTTopicTemplate.h` | `{iata}/{device}/{token}/{type}` expansion, overflow/NUL-termination, and a buffer-size fuzz |
| `test_mqtt_topic_router` | `src/helpers/MQTTTopicRouter.h` | complete preset/custom topic-routing contract; MeshRank all types except raw; required identifiers; invalid inputs/slots; exact buffer boundaries |
| `test_mqtt_connection_policy` | `src/helpers/MQTTConnectionPolicy.h` | reconnect guard/backoff/stagger and breaker transitions; stable reset; JWT lifetime/renewal policy; exact timing boundaries and 32-bit `millis()` rollover |
| `test_mqtt_packet_queue_policy` | `src/helpers/MQTTPacketQueuePolicy.h` | queue-full eviction; stale-disconnect flush; adaptive drain limits; bounded QoS0 retries; exact timing boundaries and 32-bit `millis()` rollover |
| `test_mqtt_packet_filter` | `src/helpers/MQTTPacketFilter.h` | per-slot 0-15 allowlist parsing/formatting, numeric and named spellings; exact bounds; membership; candidate/eligible split and retry-completion policy; pre-queue union gate; default-mask detection |
| `test_mqtt_runtime_buffer_lifecycle` | `src/helpers/MQTTRuntimeBufferLifecycle.h` | idempotent allocation/release; partial-allocation degradation; retry of only missing buffers |
| `test_mqtt_prefs_codec` | `src/helpers/MQTTPrefsStorage.h`, `src/helpers/MQTTPrefsCodec.h` | binary pre-slot/3-slot/6-slot migration fixtures; v1 header integrity; downgrade preservation; shortest-payload write policy (default filters stay downgrade-readable) |
| `test_mqtt_prefs_atomic_store` | `src/helpers/MQTTPrefsAtomicStore.h` | transactional MQTT writes and legacy `/node_prefs` handoff; exact short-write detection; begin/finish/rename failure cleanup; original-file preservation |
| `test_mqtt_payload_builder` | `src/helpers/MQTTPayloadBuilder.cpp` | status/packet/raw JSON contracts; optional fields; escaping; RX metrics and path; score handling; exact buffer bounds; maximum representative payloads |
| `test_autoreply` | `src/helpers/AutoReplyLogic.h` | repeater auto-reply: the `#test-<iata>` channel derived from the region code (folding, unset/placeholder regions, exhaustive proof it cannot produce a shared channel); `"<sender>: <message>"` splitting and standalone-keyword matching; per-sender cooldown ring with window boundaries and eviction |
| `test_utils` | `src/Utils.cpp` | `Utils::toHex` (upstream) |
| `test_mqtt_control` | `src/helpers/MqttControlLogic.h` | the signed control envelope: field split, the exact byte range the signature covers, replay counter, expiry and clock-fails-closed, the command allowlist and its prefix-boundary rule, the transmit/general rate split, owner-key readiness, and the numeric stability of the result codes |
| `test_mqtt_lifecycle` | `src/helpers/MQTTLifecycle.h` | bridge start/run/stop as a state machine: idempotent start and stop, stop before full initialisation, resource rollback on start failure, restart after stop, and the stop-timeout fallback |
| `test_mqtt_reply_format` | `src/helpers/MQTTReplyFormat.h` | `replyAppendf`, the bounded clamping printf-append behind the status/stats/diag CLI formatters — proves the out-of-bounds-write bound holds for any input size rather than by input-size accident |
| `test_mqtt_wire_scratch` | `src/Packet.cpp` wire sizing | the accept/reject edges of `canSerialize()`. `Packet::writeTo()` returns a `uint8_t` and so cannot report an overrun, and it trusts the packet's own length fields — these cases are what keep it in bounds |
| `test_config_serializer` | `src/helpers/ConfigSerializer.h` | serial config save/load round-trips: escaped characters, leading and trailing whitespace, unmatched braces and missing commas |
| `test_mesh_tables` | `src/helpers/SimpleMeshTables.h` | duplicate suppression: `wasSeen` is a pure query that never inserts, `markSeen` affects only its own packet, and the flood/direct duplicate counters increment on the right path |
| `test_utf8_helpers` | `src/helpers/UTF8Helpers.h` | truncation that never splits a code point, and rejection of malformed, truncated and unexpected-continuation sequences |
| `test_webconfig_batch` | `src/helpers/WebConfigBatch.h` | the config-batch / reboot / stop state machine: reqid replay without re-applying, busy only while pending, 25 ms inter-command pacing with inclusive release, and `millis()` rollover. Mirrors `WebConfigServer.cpp`; see the scope note in the header |
| `test_kiss_modem` | `examples/kiss_modem/KissModem.h` | KISS framing over the modem path (`native_kiss_modem` environment) |

## Conventions (and how to add a suite)

- Each `test/test_<name>/` directory builds into its **own** GoogleTest program
  and must define its own `main()` (`::testing::InitGoogleTest` + `RUN_ALL_TESTS`).
- Tests are **host-only**: include only pure headers. Arduino/crypto stubs live
  in `test/mocks/` (on the include path via `-I test/mocks`).
- Firmware headers are included from `src` (via `-I src`, e.g.
  `#include "helpers/MQTTPresets.h"`). Some are guarded or ESP-flavored, so a
  suite may need shims **before** the include — e.g. `test_mqtt_presets` does
  `#define WITH_MQTT_BRIDGE 1` (the preset table is behind that flag) and
  `#define PROGMEM` (the embedded CA-cert strings are PROGMEM-qualified).
- To add a suite: create `test/test_<name>/test_<name>.cpp` with a `main()`, and
  add any host-only source it links to the `native` env's `build_src_filter` in
  `platformio.ini` (header-only code needs no source entry). No other wiring.
- Keep logic testable by extracting pure functions into headers (as
  `MQTTObserverValidation.h` / `WebConfigKeys.h` / `MQTTTopicTemplate.h` do) and
  having the firmware call the same functions.
