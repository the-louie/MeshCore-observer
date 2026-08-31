#pragma once

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

// Pure, dependency-free decisions behind the MQTT control plane. Factored out of
// examples/simple_repeater/MqttControl.cpp so the envelope split, the replay
// window, the expiry window and the command allowlist can be attacked on the host
// (see test/test_mqtt_control) rather than only on a device that is hard to reach.
// The firmware holds the state, owns the key and does the I/O; every rule it
// applies is one of these functions.
//
// The broker this rides on publishes its own credentials, so anyone can publish to
// any topic. Nothing here may ever trust the transport: the signature is the only
// thing that says who sent a command, and these functions run in the order given
// in docs/architecture/decisions/001-mqtt-control-envelope.md -- cheap before
// expensive, and the signature before anything it protects.

// The envelope is deliberately not JSON. Signing a JSON document needs
// canonicalisation, and every canonicalisation bug is a signature-bypass bug.
// Here the signed region is a literal byte range of the payload as received.
#define MQTTCTRL_VERSION_TAG    "v1"
#define MQTTCTRL_FIELD_SEP      '|'

// Ed25519 signature, hex-encoded the way JWTHelper already emits one.
#define MQTTCTRL_SIG_HEX_LEN    128
#define MQTTCTRL_SIG_BYTES      64

// Bounds every buffer here. The broker's own ceiling is 896 bytes
// (MQTTBridge.cpp:4248-4251); staying well inside it keeps the staging buffer
// static, which the DRAM fragmentation note in docs/mbedtls-tls-footprint.md
// makes a requirement rather than a preference.
#define MQTTCTRL_MAX_PAYLOAD    512
#define MQTTCTRL_MAX_TOPIC      160
#define MQTTCTRL_MAX_COMMAND    160
#define MQTTCTRL_MAX_SIGNED     (MQTTCTRL_MAX_TOPIC + 1 + MQTTCTRL_MAX_PAYLOAD)

// A command may not claim to be valid indefinitely: a signer cannot mint one that
// outlives its usefulness and waits to be replayed at a worse moment.
#define MQTTCTRL_MAX_FUTURE_SECS  3600

// Two rate limits, because the two kinds of command cost different things. A
// parameter change costs a flash write; a trigger costs the whole mesh airtime
// and provokes a reply storm from every repeater in range, so it is capped far
// harder. Both apply to *validly signed* commands: a key that leaks must not
// become a flood weapon, and the airtime is not ours to spend.
#define MQTTCTRL_CMD_MAX          20
#define MQTTCTRL_CMD_WINDOW_SECS  3600
#define MQTTCTRL_TRIGGER_MAX      4
#define MQTTCTRL_TRIGGER_WINDOW   3600

// Why a command was refused. Returned rather than logged so the caller decides
// what is safe to publish back -- a rejection reason is not secret, but the
// caller is the one that knows whether the result topic is public.
enum MqttCtrlResult {
  MQTTCTRL_OK = 0,
  MQTTCTRL_ERR_EMPTY,
  MQTTCTRL_ERR_TOO_LONG,
  MQTTCTRL_ERR_NO_SIGNATURE,
  MQTTCTRL_ERR_BAD_SIG_LEN,
  MQTTCTRL_ERR_BAD_SIG_HEX,
  MQTTCTRL_ERR_BAD_VERSION,
  MQTTCTRL_ERR_BAD_FIELDS,
  MQTTCTRL_ERR_BAD_COUNTER,
  MQTTCTRL_ERR_REPLAY,
  MQTTCTRL_ERR_EXPIRED,
  MQTTCTRL_ERR_FUTURE,
  MQTTCTRL_ERR_NO_CLOCK,
  MQTTCTRL_ERR_COMMAND_CHARS,
  MQTTCTRL_ERR_NOT_ALLOWED,
};

// One parsed envelope. Pointers refer into the caller's payload buffer, which
// must outlive this struct -- the same convention AutoReplyRequest uses.
struct MqttCtrlEnvelope {
  uint32_t counter;
  uint32_t expiry;
  const char* command;
  size_t command_len;
  const char* signed_part;    // the bytes the signature covers, after the topic
  size_t signed_len;
  uint8_t signature[MQTTCTRL_SIG_BYTES];
};

// One hex pair to a byte, or -1. Rejects anything that is not strictly two hex
// digits: strtol would accept leading spaces and a sign, which would let two
// different payloads carry the same signature bytes.
static inline int mqttCtrlHexByte(char hi, char lo) {
  int out = 0;
  for (int i = 0; i < 2; i++) {
    char c = (i == 0) ? hi : lo;
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return -1;
    out = (out << 4) | v;
  }
  return out;
}

// Decode the 128 hex characters of a signature. Length is checked by the caller.
static inline bool mqttCtrlDecodeSignature(const char* hex, uint8_t* out) {
  for (int i = 0; i < MQTTCTRL_SIG_BYTES; i++) {
    int b = mqttCtrlHexByte(hex[i * 2], hex[i * 2 + 1]);
    if (b < 0) return false;
    out[i] = (uint8_t)b;
  }
  return true;
}

// Parse an unsigned decimal that occupies the whole span. Rejects empty spans,
// non-digits, leading '+'/'-'/whitespace and anything that overflows: a counter
// that wrapped or a length-prefixed sneak would each break the replay window.
static inline bool mqttCtrlParseU32(const char* start, size_t len, uint32_t* out) {
  if (len == 0 || len > 10) return false;
  uint64_t acc = 0;
  for (size_t i = 0; i < len; i++) {
    if (start[i] < '0' || start[i] > '9') return false;
    acc = acc * 10 + (uint64_t)(start[i] - '0');
    if (acc > 0xFFFFFFFFULL) return false;
  }
  *out = (uint32_t)acc;
  return true;
}

// A command may hold only printable ASCII, and never the field separator, so the
// split that found the signature cannot be steered by the command itself.
static inline bool mqttCtrlCommandCharsOk(const char* cmd, size_t len) {
  if (len == 0 || len > MQTTCTRL_MAX_COMMAND) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)cmd[i];
    if (c < 0x20 || c > 0x7E) return false;      // control chars, NUL, high bytes
    if (c == MQTTCTRL_FIELD_SEP) return false;
  }
  return true;
}

// Split "<signed-part>|<sig-hex>" and decode the signature, without yet trusting
// any of it. Splits at the LAST separator so a command cannot manufacture a
// second split point, and the signed span is a literal byte range of what
// arrived -- we verify exactly the bytes we received.
//
// Nothing here is authenticated. Everything this fills in stays untrusted until
// mqttCtrlBuildSignedMessage feeds it to Identity::verify.
static inline MqttCtrlResult mqttCtrlParseEnvelope(const char* payload, size_t len,
                                                   MqttCtrlEnvelope* out) {
  if (payload == NULL || out == NULL || len == 0) return MQTTCTRL_ERR_EMPTY;
  if (len > MQTTCTRL_MAX_PAYLOAD) return MQTTCTRL_ERR_TOO_LONG;

  size_t split = len;
  for (size_t i = len; i > 0; i--) {
    if (payload[i - 1] == MQTTCTRL_FIELD_SEP) { split = i - 1; break; }
  }
  if (split == len) return MQTTCTRL_ERR_NO_SIGNATURE;

  size_t sig_len = len - split - 1;
  if (sig_len != MQTTCTRL_SIG_HEX_LEN) return MQTTCTRL_ERR_BAD_SIG_LEN;
  if (!mqttCtrlDecodeSignature(payload + split + 1, out->signature)) {
    return MQTTCTRL_ERR_BAD_SIG_HEX;
  }

  out->signed_part = payload;
  out->signed_len = split;

  // "v1|<counter>|<expiry>|<command>" -- three separators inside the signed part.
  size_t tag_len = strlen(MQTTCTRL_VERSION_TAG);
  if (split < tag_len + 1) return MQTTCTRL_ERR_BAD_VERSION;
  if (memcmp(payload, MQTTCTRL_VERSION_TAG, tag_len) != 0) return MQTTCTRL_ERR_BAD_VERSION;
  if (payload[tag_len] != MQTTCTRL_FIELD_SEP) return MQTTCTRL_ERR_BAD_VERSION;

  size_t p = tag_len + 1;
  size_t c_end = p;
  while (c_end < split && payload[c_end] != MQTTCTRL_FIELD_SEP) c_end++;
  if (c_end >= split) return MQTTCTRL_ERR_BAD_FIELDS;
  if (!mqttCtrlParseU32(payload + p, c_end - p, &out->counter)) return MQTTCTRL_ERR_BAD_COUNTER;

  size_t e_start = c_end + 1;
  size_t e_end = e_start;
  while (e_end < split && payload[e_end] != MQTTCTRL_FIELD_SEP) e_end++;
  if (e_end >= split) return MQTTCTRL_ERR_BAD_FIELDS;
  if (!mqttCtrlParseU32(payload + e_start, e_end - e_start, &out->expiry)) {
    return MQTTCTRL_ERR_BAD_FIELDS;
  }

  out->command = payload + e_end + 1;
  out->command_len = split - (e_end + 1);
  if (!mqttCtrlCommandCharsOk(out->command, out->command_len)) {
    return MQTTCTRL_ERR_COMMAND_CHARS;
  }
  return MQTTCTRL_OK;
}

// Assemble the exact bytes the signature covers: the topic, a newline, then the
// signed part of the payload.
//
// The topic is inside the signature so one signature is valid for exactly one
// destination. Without it, a command signed for one node could be captured and
// republished on another node's topic, or a per-node command replayed as a
// broadcast -- the two cases the counter alone cannot see, because each node
// keeps its own counter.
static inline bool mqttCtrlBuildSignedMessage(const char* topic, const MqttCtrlEnvelope* env,
                                              uint8_t* out, size_t out_size, size_t* out_len) {
  if (topic == NULL || env == NULL || out == NULL || out_len == NULL) return false;
  size_t topic_len = strlen(topic);
  if (topic_len == 0 || topic_len > MQTTCTRL_MAX_TOPIC) return false;
  size_t total = topic_len + 1 + env->signed_len;
  if (total > out_size) return false;

  memcpy(out, topic, topic_len);
  out[topic_len] = '\n';
  memcpy(out + topic_len + 1, env->signed_part, env->signed_len);
  *out_len = total;
  return true;
}

// The replay window. Strictly greater, never equal: unlike the LoRa admin path
// there is no delivery uncertainty here to forgive, so an equal counter is a
// replay rather than a retry.
//
// The caller must persist the new value BEFORE executing the command. A counter
// held only in RAM reopens the whole window on the next reboot, and reboots are
// routine; a counter persisted after execution reopens it for whatever crashed
// mid-command. The cost is that a crashed command is not retried, which is the
// right trade: a silently repeated 'trigger test' spends airtime nobody asked for.
static inline bool mqttCtrlCounterAccepted(uint32_t last_seen, uint32_t offered) {
  return offered > last_seen;
}

// Expiry, bounded at both ends. 'now' of 0 means the clock has never synced, and
// then this fails closed: without a trustworthy clock the expiry check means
// nothing, and accepting on a guess would be worse than refusing.
static inline MqttCtrlResult mqttCtrlCheckExpiry(uint32_t now, uint32_t expiry) {
  if (now == 0) return MQTTCTRL_ERR_NO_CLOCK;
  if (expiry < now) return MQTTCTRL_ERR_EXPIRED;
  if (expiry - now > MQTTCTRL_MAX_FUTURE_SECS) return MQTTCTRL_ERR_FUTURE;
  return MQTTCTRL_OK;
}

// Does 'command' start with 'prefix' and then stop at a boundary -- end of
// string, a space, or a '.'? The dot matters: the settings are 'set autoreply',
// 'set autoreply.hops', 'set autoreply.direct.flood', so a space-only boundary
// would admit the bare command and reject every parameter under it.
//
// Requiring *some* boundary is what stops 'set autoreplyX' riding in on
// 'set autoreply'. Allowing '.' deliberately opens the whole 'autoreply.'
// namespace, which is ours and which the CLI validates itself; it does not open
// any other namespace, because the prefix before the dot still has to match.
static inline bool mqttCtrlPrefixWithBoundary(const char* command, size_t len,
                                              const char* prefix) {
  size_t plen = strlen(prefix);
  if (len < plen) return false;
  if (strncasecmp(command, prefix, plen) != 0) return false;
  if (len == plen) return true;
  return command[plen] == ' ' || command[plen] == '.';
}

// The allowlist. Only auto-reply parameters and the test trigger travel this way.
//
// This is the security boundary, and it is deliberately independent of the
// sender_timestamp policy: passing a non-zero timestamp already makes the
// locally-privileged commands refuse a remote caller, and this list means a
// mistake there is still not a compromise. Either alone should keep 'set prv.key'
// unreachable; both together mean one of them can be wrong.
static inline bool mqttCtrlCommandAllowed(const char* command, size_t len) {
  if (!mqttCtrlCommandCharsOk(command, len)) return false;

  static const char* const allowed[] = {
    "set autoreply",              // covers on|off and every autoreply.* setting
    "get autoreply",
    "trigger test",
  };
  for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
    if (mqttCtrlPrefixWithBoundary(command, len, allowed[i])) return true;
  }
  return false;
}

// Does this command put a packet on the air? Triggers are limited separately and
// much more tightly than parameter changes, so the caller needs to know which
// budget to spend before it executes anything.
static inline bool mqttCtrlIsTransmitCommand(const char* command, size_t len) {
  return mqttCtrlPrefixWithBoundary(command, len, "trigger");
}

// The whole decision except the signature itself, which needs the node's key and
// so belongs to the firmware. Runs the order from the ADR, stopping at the first
// refusal. The caller checks the signature between parsing and this call, then
// persists the counter, then executes.
static inline MqttCtrlResult mqttCtrlAuthorise(const MqttCtrlEnvelope* env,
                                               uint32_t last_counter, uint32_t now) {
  if (env == NULL) return MQTTCTRL_ERR_EMPTY;
  if (!mqttCtrlCounterAccepted(last_counter, env->counter)) return MQTTCTRL_ERR_REPLAY;

  MqttCtrlResult expiry = mqttCtrlCheckExpiry(now, env->expiry);
  if (expiry != MQTTCTRL_OK) return expiry;

  if (!mqttCtrlCommandAllowed(env->command, env->command_len)) return MQTTCTRL_ERR_NOT_ALLOWED;
  return MQTTCTRL_OK;
}

// Rate limiting, as a decision the caller makes with limiters it owns. Split out
// rather than folded into mqttCtrlAuthorise because the limiters are stateful and
// consume budget when asked -- so the caller must run every stateless check
// first, and only then spend. Asking a limiter about a command that was going to
// be refused anyway would let anyone drain the budget with garbage.
//
// 'allow_general' and 'allow_transmit' are the results of RateLimiter::allow on
// the caller's two limiters; a transmit command must satisfy both.
static inline bool mqttCtrlRateAccepted(bool is_transmit, bool allow_general,
                                        bool allow_transmit) {
  if (!allow_general) return false;
  return is_transmit ? allow_transmit : true;
}
