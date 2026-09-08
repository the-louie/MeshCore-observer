#pragma once

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

#include "MQTTObserverValidation.h"   // mqttOwnerKeyValid

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
// in the workspace root's docs/architecture/decisions/001-mqtt-control-envelope.md
// (that tree is outside this repo, alongside it) -- cheap before
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
#define MQTTCTRL_MAX_REPLY      160
// r1|counter|code|command|reply, plus the separators and a terminator.
#define MQTTCTRL_MAX_RESULT     (2 + 12 + 6 + MQTTCTRL_MAX_COMMAND + MQTTCTRL_MAX_REPLY + 8)
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
  // Appended, never renumbered: test_mqtt_control and panopticon/control.py both
  // read these as numbers, so an inserted value would silently re-label every
  // stored result.
  MQTTCTRL_ERR_NO_OWNER_KEY,
  // A signature that parsed as hex but did not verify. Distinct from
  // MQTTCTRL_ERR_BAD_SIG_HEX, which means the field was malformed: one is a
  // forgery or the wrong key, the other is a broken publisher, and a caller
  // watching a public broker wants to tell those apart.
  MQTTCTRL_ERR_SIG_INVALID,
  // Budget exhausted. Distinct from MQTTCTRL_ERR_NOT_ALLOWED, which means the
  // command is not on the allowlist: one is "ask again later", the other is
  // "never". Reporting both as NOT_ALLOWED made a throttled node look like a
  // node refusing the command outright.
  MQTTCTRL_ERR_RATE_LIMITED,
  // A transmit command addressed to every node in the region at once. Refused on
  // principle rather than on budget -- see mqttCtrlTransmitTopicResult.
  MQTTCTRL_ERR_BROADCAST_TRANSMIT,
};

// Can a staged command be examined at all?
//
// Without a provisioned owner key there is nothing to verify a signature against,
// so the command cannot be executed -- but it must still be *resolved*. The
// caller owns a single staging slot that only clears when the command is
// disposed of, and returning early here left it latched: every later command was
// dropped for the life of the boot, including after a key was finally
// provisioned. The bug was silent, and the documentation said the opposite
// ("every command is refused"), which is why the rule now lives in a function
// with a name and a test rather than in an early return.
//
// Unset, empty and malformed are one case, not three: none of them can verify a
// signature, and `mesh::Utils::fromHex` would refuse the last a moment later.
// `mqttOwnerKeyValid` already rejects NULL and the empty string, so guarding
// those separately here was dead code -- a mutation that deleted the guard left
// every test green, which is how it was found.
static inline MqttCtrlResult mqttCtrlOwnerReady(const char* owner_hex) {
  return mqttOwnerKeyValid(owner_hex) ? MQTTCTRL_OK : MQTTCTRL_ERR_NO_OWNER_KEY;
}

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
// How one allowlist entry matches.
//
// A *family* entry uses the boundary rule, so `set autoreply` covers
// `set autoreply.hops 8` and every other autoreply setting -- one entry instead
// of five, which is why the boundary rule exists at all.
//
// An *exact* entry admits the bare command and nothing else. The cost of a family
// is that it admits children nobody listed: `get radio` silently admitted
// `get radio.rxgain` and `get radio.fem.rxgain`, and `get radio foo` besides.
// Those are harmless read-only values, but this list exists to be *reviewed*, and
// an entry that admits more than it says defeats the review. None of the reads
// have children worth reaching, so all of them are exact -- and a `get af.thing`
// added to the CLI later cannot reach the mesh without someone adding it here too.
struct MqttCtrlAllowEntry {
  const char* text;
  bool exact;
};

// The bare command, and nothing after it.
static inline bool mqttCtrlExactCommand(const char* command, size_t len, const char* text) {
  size_t tlen = strlen(text);
  return len == tlen && strncasecmp(command, text, tlen) == 0;
}

static inline bool mqttCtrlCommandAllowed(const char* command, size_t len) {
  if (!mqttCtrlCommandCharsOk(command, len)) return false;

  static const MqttCtrlAllowEntry allowed[] = {
    // Families. The boundary rule is load-bearing on these three: make them exact
    // and `set autoreply.hops 8` and `trigger test a1b2c3d4` both stop working.
    { "set autoreply", false },   // covers on|off and every autoreply.* setting
    { "get autoreply", false },
    { "trigger test",  false },   // covers the optional 8-hex correlation id and mode
    { "trigger private", false }, // the target key, and the optional id after it

    // Reads. Each admits exactly the command it names. All read-only: they change
    // nothing and cannot be replayed into anything, which is what keeps the
    // argument for having them simple. Deliberately no `set` here.
    { "get txdelay",        true },
    { "get direct.txdelay", true },
    { "get rxdelay",        true },
    { "get af",             true },
    { "get cad",            true },
    { "get int.thresh",     true },
    { "get radio",          true },

    // How this node takes part in flooding, which the topology model otherwise
    // has to infer from traffic. That inference has been wrong before: every node
    // was once treated as a repeater, which counted phones and companions as
    // relays. A node that can be asked settles it directly.
    { "get repeat",              true },   // does it forward at all
    { "get flood.max",           true },
    { "get flood.max.unscoped",  true },
  };

  for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
    bool ok = allowed[i].exact
                ? mqttCtrlExactCommand(command, len, allowed[i].text)
                : mqttCtrlPrefixWithBoundary(command, len, allowed[i].text);
    if (ok) return true;
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

// The published result: `r1|<counter>|<code>|<command>|<reply>`.
//
// A `get` reply on its own is "> 0.5" -- true of whatever was asked, which with
// one requester polling several nodes is not enough to act on. The topic names
// the node, so what is missing is the question. The counter answers it: it is
// already persisted per node, already strictly increasing, and already what the
// requester chose, which makes it the natural correlation id -- the same role the
// 8-hex id plays on the auto-reply path.
//
// Flat rather than JSON, and clamped rather than discarded. The existing payload
// builders have a hard 768-byte budget and *drop* a payload that exceeds it
// (MQTTPayloadBuilder.cpp:9-21); inheriting that would mean the longest replies --
// the interesting ones -- silently never arrive. Truncation here yields a shorter
// line that still parses, because the reply is last and every earlier field is
// bounded.
//
// Splitting on the separator is unambiguous: mqttCtrlCommandCharsOk refuses a
// command containing one, so only the trailing reply can, and a parser takes the
// remainder of the line as the reply rather than splitting it further.
//
// Returns the number of bytes written, never more than out_size - 1.
static inline size_t mqttCtrlFormatResult(char* out, size_t out_size, uint32_t counter,
                                          int code, const char* command, const char* reply) {
  if (out == NULL || out_size == 0) return 0;
  out[0] = 0;
  int n = snprintf(out, out_size, "r1%c%u%c%d%c%s%c%s",
                   MQTTCTRL_FIELD_SEP, (unsigned)counter,
                   MQTTCTRL_FIELD_SEP, code,
                   MQTTCTRL_FIELD_SEP, command != NULL ? command : "",
                   MQTTCTRL_FIELD_SEP, reply != NULL ? reply : "");
  if (n < 0) { out[0] = 0; return 0; }
  return ((size_t)n >= out_size) ? out_size - 1 : (size_t)n;
}

// Is this the broadcast topic, meshcore/{IATA}/all/cmd, rather than one node's?
// The per-node leaf is 8 hex characters, which can never spell "all", so matching
// the tail is unambiguous.
static inline bool mqttCtrlTopicIsBroadcast(const char* topic) {
  if (topic == NULL) return false;
  static const char SUFFIX[] = "/all/cmd";
  size_t tlen = strlen(topic);
  size_t slen = sizeof(SUFFIX) - 1;
  return tlen >= slen && strcmp(topic + tlen - slen, SUFFIX) == 0;
}

// A transmit command may be addressed to one node, never to all of them.
//
// The rate limits are per node: four triggers an hour each. That bounds what any
// single node does and says nothing about what happens when one publish reaches
// every node in a region at once -- each of them then transmits, inside the same
// few seconds, every one of them within budget. The workspace rule is explicit
// that "any design that scales transmissions with the number of nodes is wrong",
// and a broadcast trigger is exactly that shape: the more of the mesh adopts this
// firmware, the worse the storm it enables.
//
// So this is refused on principle, not on budget, and the refusal is stateless --
// it runs before the limiters, which spend when asked.
//
// Reversible if a real use appears: broadcasting a *parameter* change is fine and
// stays allowed, because setting a value costs no airtime. Only transmitting does.
static inline MqttCtrlResult mqttCtrlTransmitTopicResult(bool is_transmit, const char* topic) {
  if (is_transmit && mqttCtrlTopicIsBroadcast(topic)) return MQTTCTRL_ERR_BROADCAST_TRANSMIT;
  return MQTTCTRL_OK;
}

// The sender_timestamp an MQTT command executes with. **Never 0.**
//
// Zero is not a clock reading in this CLI, it is a privilege marker: it means the
// caller is physically present (serial, ethernet, the web portal), and it is what
// gates `get prv.key`, `set prv.key`, `set mqtt.owner` and `erase`. A remote
// command that reached the CLI with 0 would be handed exactly the privilege those
// gates exist to withhold.
//
// The dangerous case is not the ordinary one. A node whose clock has never synced
// reports 0, so the naive `runCommand(now, ...)` grants full local privilege to a
// remote caller precisely when NTP is down -- an outage becomes a privilege
// escalation. Mapping 0 to 1 costs a second of accuracy and removes that entirely.
//
// Lives here rather than as a static in MqttControl.cpp so it can be tested: this
// is the single line that keeps every privilege gate in CommonCLI meaningful
// against MQTT, and it was previously unreachable from any test.
static inline uint32_t mqttCtrlSenderStamp(uint32_t now) {
  return now == 0 ? 1 : now;
}

// Rate limiting, as a decision the caller makes with limiters it owns. Split out
// rather than folded into mqttCtrlAuthorise because the limiters are stateful and
// consume budget when asked -- so the caller must run every stateless check
// first, and only then spend. Asking a limiter about a command that was going to
// be refused anyway would let anyone drain the budget with garbage.
//
// 'allow_general' and 'allow_transmit' are the results of RateLimiter::allow on
// the caller's two limiters; a transmit command must satisfy both.
//
// Returns a result rather than a bool so the refusal carries its own code. The
// name changed with the return type on purpose: the old bool was read as
// `if (!mqttCtrlRateAccepted(...))`, and MQTTCTRL_OK is 0, so a same-named
// function returning an enum would have inverted every call site in silence.
static inline MqttCtrlResult mqttCtrlRateResult(bool is_transmit, bool allow_general,
                                                bool allow_transmit) {
  if (!allow_general) return MQTTCTRL_ERR_RATE_LIMITED;
  if (is_transmit && !allow_transmit) return MQTTCTRL_ERR_RATE_LIMITED;
  return MQTTCTRL_OK;
}
