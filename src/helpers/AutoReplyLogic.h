#pragma once

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "MQTTObserverValidation.h"

// Pure, dependency-free decisions behind the repeater's auto-reply. Factored out
// of examples/simple_repeater/AutoReply.cpp so the channel derivation, the
// per-sender cooldown and the trigger matching can be unit-tested on the host
// (see test/test_autoreply) rather than only over the air. The firmware keeps the
// state and does the I/O; every rule it applies is one of these functions.

// The channel is not configurable: it is this prefix plus the node's region code.
#ifndef AUTOREPLY_CHANNEL_PREFIX
  #define AUTOREPLY_CHANNEL_PREFIX  "#test-"
#endif

// Build the auto-reply channel name for a region: '#test-' plus the node's IATA
// code, folded to lower case because clients only accept lower-case channel
// names. Returns false, leaving 'out' empty, when the node has no region set:
// 'XXX' is the placeholder the MQTT topic router also treats as unconfigured, and
// without a region there is no per-region channel to answer on. Deriving the name
// rather than accepting one is what keeps every repeater off the mesh-wide
// '#test', where a reply from each of them would be spam.
static inline bool autoReplyDeriveChannel(const char* iata, char* out, size_t out_size) {
  if (out == NULL || out_size == 0) return false;
  out[0] = 0;
  if (!mqttIataValid(iata) || strcasecmp(iata, "XXX") == 0) return false;

  size_t prefix_len = strlen(AUTOREPLY_CHANNEL_PREFIX);
  if (prefix_len + strlen(iata) >= out_size) return false;

  memcpy(out, AUTOREPLY_CHANNEL_PREFIX, prefix_len);
  size_t i = 0;
  for (; iata[i]; i++) out[prefix_len + i] = tolower(iata[i]);
  out[prefix_len + i] = 0;
  return true;
}

// A sender that was recently answered. The firmware holds a fixed ring of these,
// so one person retrying cannot spend everyone else's share of the global limit.
struct AutoReplySender {
  uint32_t id;
  uint32_t last_reply;
};

// FNV-1a over the case-folded name, so 'Louie' and 'louie' are one sender. The
// name prefix of a group text is the only sender identity the packet carries.
static inline uint32_t autoReplySenderId(const char* name, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint8_t)tolower(name[i]);
    h *= 16777619u;
  }
  return h;
}

// The same hash over a public key, for a private request: the packet names its
// sender by key, which nobody else can type, so the cooldown is charged to the key
// and not to a display name. Bytes are hashed as they are -- folding is for text.
static inline uint32_t autoReplyKeyId(const uint8_t* key, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= key[i];
    h *= 16777619u;
  }
  return h;
}

// Charge a reply to 'id' against the per-sender cooldown. Returns false while that
// sender is still inside its window; otherwise records the reply and returns true.
// A sender not in the ring takes the next slot, evicting the oldest entry.
static inline bool autoReplySenderAllowed(AutoReplySender* senders, uint8_t count,
                                          uint8_t& next_slot, uint32_t id, uint32_t now,
                                          uint32_t window_secs) {
  if (senders == NULL || count == 0) return true;

  for (uint8_t i = 0; i < count; i++) {
    if (senders[i].id == id) {
      if (now < senders[i].last_reply + window_secs) return false;
      senders[i].last_reply = now;
      return true;
    }
  }

  senders[next_slot % count].id = id;
  senders[next_slot % count].last_reply = now;
  next_slot = (next_slot + 1) % count;
  return true;
}

// A correlation id is exactly this many hex characters. Fixed rather than ranged
// so the reply budget can be reasoned about, and so a half-typed id is a
// non-trigger rather than a shorter id.
#define AUTOREPLY_ID_LEN  8

// How a request asks to be answered. A requester picks one with a single letter
// after the id; a request that names none takes the node's configured default,
// which is what AUTOREPLY_MODE_DEFAULT stands for in a parsed request.
//
// FLOOD is the reply every node makes today, a group text flooded on the channel.
// PRIVATE and DIRECT are a text message addressed to the requester alone -- the
// first flooded so it finds them wherever they are, the second sent down a known
// return path. SILENT asks for no reply at all: the node still hears the request
// and its observer uplink still reports it, so a probe that wants only "who heard
// me" costs the mesh nothing but its own transmission.
enum {
  AUTOREPLY_MODE_DEFAULT = 0,
  AUTOREPLY_MODE_FLOOD,
  AUTOREPLY_MODE_PRIVATE,
  AUTOREPLY_MODE_DIRECT,
  AUTOREPLY_MODE_SILENT,
};

// The letter a requester types, either case. Anything else is DEFAULT, which the
// trigger parser treats as "not a mode token" rather than as a choice.
static inline uint8_t autoReplyModeFromLetter(char c) {
  switch (tolower((unsigned char)c)) {
    case 'f': return AUTOREPLY_MODE_FLOOD;
    case 'p': return AUTOREPLY_MODE_PRIVATE;
    case 'd': return AUTOREPLY_MODE_DIRECT;
    case 's': return AUTOREPLY_MODE_SILENT;
    default:  return AUTOREPLY_MODE_DEFAULT;
  }
}

// The letter for a mode, for a probe this node originates; 0 for DEFAULT, which
// names nothing and so has no letter to send.
static inline char autoReplyModeLetter(uint8_t mode) {
  switch (mode) {
    case AUTOREPLY_MODE_FLOOD:   return 'F';
    case AUTOREPLY_MODE_PRIVATE: return 'P';
    case AUTOREPLY_MODE_DIRECT:  return 'D';
    case AUTOREPLY_MODE_SILENT:  return 'S';
    default:                     return 0;
  }
}

// A group text, split into who sent it and whether it asked for a reply. 'sender',
// 'message' and 'id' point into the text buffer passed to autoReplyParseRequest().
struct AutoReplyRequest {
  bool is_trigger;
  const char* sender;
  size_t sender_len;
  const char* message;
  const char* id;        // NULL for a bare keyword
  size_t id_len;
  uint8_t mode;          // AUTOREPLY_MODE_DEFAULT unless the request named one
  const char* pubkey_hex; // 64 hex characters, or NULL: where a private reply goes
};

// Does the message ask for a reply, and does it carry a correlation id or a mode?
//
// The forms accepted are the bare keyword, the keyword and exactly AUTOREPLY_ID_LEN
// hex characters, and either of those followed by one mode letter -- each part
// separated by a single space. A private mode (P or D) may then carry the
// requester's public key as 64 hex characters, because a group text names its
// sender only by an unauthenticated display name and a private reply has to be
// addressed to a key. **The bare form must keep working indefinitely.** Flashing
// this fleet takes months, so probes stay bare until adoption is high; a firmware
// that only answered the id form would silently drop every un-flashed node out of
// the measurement, and those are exactly the nodes whose connectivity is least
// understood.
//
// The tail is matched strictly -- one space between parts, then end of string.
// Anything looser ('test me', 'test 123', a second letter, trailing text, a key
// after F or S) stays a non-trigger, which is what keeps ordinary chat on the
// channel from costing everyone airtime. A mode letter and an id cannot be
// confused: an id is exactly eight hex digits, and a mode is exactly one letter.
//
// 'mode' may be NULL for a caller that has no use for one. Such a caller keeps
// exactly the two older forms: a request that names a mode is a non-trigger there,
// never a trigger with the mode quietly dropped. 'pubkey_hex' likewise: a caller
// that cannot address a private reply treats a request carrying a key as chat.
static inline bool autoReplyMatchTrigger(const char* msg, const char* keyword,
                                         const char** id, size_t* id_len,
                                         uint8_t* mode = NULL,
                                         const char** pubkey_hex = NULL) {
  *id = NULL;
  *id_len = 0;
  if (mode) *mode = AUTOREPLY_MODE_DEFAULT;
  if (pubkey_hex) *pubkey_hex = NULL;
  if (msg == NULL || keyword == NULL) return false;

  size_t klen = strlen(keyword);
  if (strncasecmp(msg, keyword, klen) != 0) return false;
  if (msg[klen] == 0) return true;                 // the bare keyword
  if (msg[klen] != ' ') return false;

  const char* tail = msg + klen + 1;
  size_t n = 0;
  while (n < AUTOREPLY_ID_LEN && isxdigit((unsigned char)tail[n])) n++;
  if (n == AUTOREPLY_ID_LEN && (tail[n] == 0 || tail[n] == ' ')) {
    *id = tail;
    *id_len = n;
    if (tail[n] == 0) return true;                 // keyword and id
    tail += n + 1;
  }

  uint8_t m = autoReplyModeFromLetter(tail[0]);
  bool ok = mode != NULL && m != AUTOREPLY_MODE_DEFAULT;
  if (ok && tail[1] == ' ') {
    // Only a private reply needs an address, so only P and D may carry one.
    bool addressable = m == AUTOREPLY_MODE_PRIVATE || m == AUTOREPLY_MODE_DIRECT;
    ok = addressable && pubkey_hex != NULL && mqttOwnerKeyValid(tail + 2);
    if (ok) *pubkey_hex = tail + 2;
  } else if (ok) {
    ok = tail[1] == 0;
  }
  if (!ok) {
    *id = NULL;                                    // a non-trigger carries nothing
    *id_len = 0;
    return false;
  }
  *mode = m;
  return true;
}

// The mode a reply is actually sent in, given what the request asked for and how
// the node is configured. A request that named nothing takes the node's default.
// A private reply needs somewhere to go: with no key to address it to, P and D
// fall back to the flood reply rather than to silence, because a probe that goes
// unanswered for an addressing gap looks exactly like a dead repeater.
static inline uint8_t autoReplyResolveMode(uint8_t requested, uint8_t node_default,
                                           bool has_pubkey) {
  uint8_t m = requested == AUTOREPLY_MODE_DEFAULT ? node_default : requested;
  if (m == AUTOREPLY_MODE_DEFAULT) m = AUTOREPLY_MODE_FLOOD;
  if ((m == AUTOREPLY_MODE_PRIVATE || m == AUTOREPLY_MODE_DIRECT) && !has_pubkey) {
    return AUTOREPLY_MODE_FLOOD;
  }
  return m;
}

// Parse `trigger <keyword>` / `trigger <keyword> <8 hex>`, the command a signed
// MQTT envelope carries to make this node originate a probe.
//
// The id is **echoed, never generated here**. The requester mints it, so the
// requester is the one who can tie the replies that come back to the request it
// sent -- the same decision already taken for the auto-reply correlation id, and
// for the same reason. A node inventing its own id would produce a number nobody
// asked for and nobody can match.
//
// Reuses autoReplyMatchTrigger for the tail so the accepted id and mode grammar is
// defined once: whatever a node accepts in a request, it accepts in a trigger. A
// key is the one thing a trigger never carries -- the probe's requester is this
// node, and it knows its own key -- so a command with one is refused.
static inline bool autoReplyParseTrigger(const char* command, const char* keyword,
                                         const char** id, size_t* id_len,
                                         uint8_t* mode = NULL) {
  *id = NULL;
  *id_len = 0;
  if (mode) *mode = AUTOREPLY_MODE_DEFAULT;
  if (command == NULL || keyword == NULL) return false;

  static const char PREFIX[] = "trigger ";
  const size_t plen = sizeof(PREFIX) - 1;
  if (strncasecmp(command, PREFIX, plen) != 0) return false;

  return autoReplyMatchTrigger(command + plen, keyword, id, id_len, mode);
}

// Parse `trigger private <64 hex key> [<8 hex id>]`, the command that makes this node
// send a private test request to the node with that key -- the requester's side of
// the private exchange, over the same signed control plane the channel probe uses.
// The key is the target's, not ours: our own goes in the packet by construction.
// The id is echoed, never generated, for the reason autoReplyParseTrigger gives.
// Matched as strictly as everything else here: one space between parts, exactly
// 64 hex, then either the end or one space and exactly AUTOREPLY_ID_LEN hex.
static inline bool autoReplyParsePrivateTrigger(const char* command, const char** pubkey_hex,
                                                const char** id, size_t* id_len) {
  *pubkey_hex = NULL;
  *id = NULL;
  *id_len = 0;
  if (command == NULL) return false;

  static const char PREFIX[] = "trigger private ";
  const size_t plen = sizeof(PREFIX) - 1;
  if (strncasecmp(command, PREFIX, plen) != 0) return false;

  const char* key = command + plen;
  size_t klen = 0;
  while (isxdigit((unsigned char)key[klen])) klen++;
  if (klen != 64) return false;
  if (key[klen] == 0) {
    *pubkey_hex = key;
    return true;
  }
  if (key[klen] != ' ') return false;

  const char* tail = key + klen + 1;
  size_t n = 0;
  while (n < AUTOREPLY_ID_LEN && isxdigit((unsigned char)tail[n])) n++;
  if (n != AUTOREPLY_ID_LEN || tail[n] != 0) return false;

  *pubkey_hex = key;
  *id = tail;
  *id_len = n;
  return true;
}

// Build the probe body: the keyword alone, or the keyword and the echoed id, and
// after either a mode letter and, for a private mode, the key the reply should
// come back to -- this node's own.
//
// This is the same text a person sends by hand, deliberately: the probe has to be
// indistinguishable from an ordinary request, or the nodes that answer it would be
// answering something else and the measurement would not compare. A probe names
// no mode unless asked to, for the reason a request does not: an un-flashed
// repeater matches the bare forms only.
//
// Returns the length written, or 0 if it would not fit.
static inline size_t autoReplyBuildProbe(char* out, size_t out_size, const char* sender,
                                         const char* keyword, const char* id, size_t id_len,
                                         uint8_t mode = AUTOREPLY_MODE_DEFAULT,
                                         const char* pubkey_hex = NULL) {
  if (out == NULL || out_size == 0 || keyword == NULL) return 0;
  out[0] = 0;

  // The sender prefix is not decoration. autoReplyParseRequest splits the text at
  // ": " to find who asked, and a repeater echoes that name back inside the
  // reply's [brackets]. Without it every reply to this probe comes back
  // anonymous, and a reply that cannot name the node it answers is worth much
  // less than one that can -- with two probes in flight nothing tells them apart.
  //
  // Measured on air 2026-09-01: a triggered probe sent as bare `test` drew
  // `SNR 7.75 RSSI -106 1h 70` where a client's probe draws
  // `[SE-JKG-LouHome-A] SNR ...`. The probe has to look like the one a person
  // sends, prefix included, or it is not measuring the same thing.
  char letter = autoReplyModeLetter(mode);
  bool keyed = letter && (mode == AUTOREPLY_MODE_PRIVATE || mode == AUTOREPLY_MODE_DIRECT)
               && pubkey_hex != NULL;
  size_t slen = (sender != NULL && sender[0] != 0) ? strlen(sender) : 0;
  size_t klen = strlen(keyword);
  size_t hlen = keyed ? strlen(pubkey_hex) : 0;
  size_t need = (slen ? slen + 2 : 0) + klen + (id != NULL && id_len > 0 ? 1 + id_len : 0)
                + (letter ? 2 : 0) + (keyed ? 1 + hlen : 0);
  if (need == 0 || need + 1 > out_size) return 0;   // never truncate a probe

  size_t n = 0;
  if (slen) {
    memcpy(out, sender, slen);
    n = slen;
    out[n++] = ':';
    out[n++] = ' ';
  }
  memcpy(out + n, keyword, klen);
  n += klen;
  if (id != NULL && id_len > 0) {
    out[n++] = ' ';
    memcpy(out + n, id, id_len);
    n += id_len;
  }
  if (letter) {
    out[n++] = ' ';
    out[n++] = letter;
  }
  if (keyed) {
    out[n++] = ' ';
    memcpy(out + n, pubkey_hex, hlen);
    n += hlen;
  }
  out[n] = 0;
  return n;
}


// Split a group text into its name prefix and message, and test the message against
// the trigger keyword. Group texts are "<sender>: <message>" (see
// BaseChatMesh::sendGroupMessage), and a text without that prefix is treated as all
// message and no sender. 'text' is modified in place: whitespace around the message
// is trimmed before the comparison, so " test " still triggers.
//
// A reply can never trigger a reply: a reply body begins with the bracketed
// requester name, or with "SNR" when there was none, and neither can match the
// keyword. Widening the match to accept a correlation id and a mode letter does not
// change that, and test/test_autoreply pins it.
static inline AutoReplyRequest autoReplyParseRequest(char* text, const char* keyword) {
  AutoReplyRequest req = { false, text, 0, text, NULL, 0, AUTOREPLY_MODE_DEFAULT, NULL };
  if (text == NULL || keyword == NULL) return req;

  char* msg = strstr(text, ": ");
  if (msg) {
    req.sender_len = msg - text;
    msg += 2;
  } else {
    msg = text;
  }

  while (*msg == ' ') msg++;
  char* end = msg + strlen(msg);
  while (end > msg && (end[-1] == ' ' || end[-1] == '\n' || end[-1] == '\r')) end--;
  *end = 0;

  req.message = msg;
  req.is_trigger = autoReplyMatchTrigger(msg, keyword, &req.id, &req.id_len, &req.mode,
                                         &req.pubkey_hex);
  return req;
}

// The body of a private test request -- the text an anonymous request carries after
// its sub-type byte -- against the trigger keyword. Only the bare keyword and the
// keyword with an id are requests here: the packet already names its sender by key
// and already makes the reply private, so a mode letter or a key in the body has
// nothing to say and is refused as chat, exactly as a mode-less caller refuses them
// on the channel. There is no name prefix to split either; a key is the sender.
// 'text' is trimmed in place like a channel request, so a stray line ending or
// trailing space does not turn a request into chat.
static inline bool autoReplyParsePrivateTest(char* text, const char* keyword,
                                             const char** id, size_t* id_len) {
  *id = NULL;
  *id_len = 0;
  if (text == NULL || keyword == NULL) return false;

  char* msg = text;
  while (*msg == ' ') msg++;
  char* end = msg + strlen(msg);
  while (end > msg && (end[-1] == ' ' || end[-1] == '\n' || end[-1] == '\r')) end--;
  *end = 0;

  return autoReplyMatchTrigger(msg, keyword, id, id_len);
}

// Read an 'on' or 'off' argument, the form every boolean setting in this feature
// takes. The token ends at the first space or line ending, so a value typed with
// trailing whitespace still reads, while a word that merely starts with 'on' does
// not. 'out' is left alone unless a whole valid token was found.
static inline bool autoReplyParseOnOff(const char* arg, bool* out) {
  if (arg == NULL || out == NULL) return false;

  while (*arg == ' ') arg++;
  size_t len = 0;
  while (arg[len] && arg[len] != ' ' && arg[len] != '\r' && arg[len] != '\n') len++;

  if (len == 2 && strncasecmp(arg, "on", 2) == 0) {
    *out = true;
    return true;
  }
  if (len == 3 && strncasecmp(arg, "off", 3) == 0) {
    *out = false;
    return true;
  }
  return false;
}

// The name a mode is configured and reported by. Silent is deliberately absent:
// it is something a request asks for, never a node's standing answer -- a node
// that should answer nobody is switched off, not configured to stay quiet while
// still parsing every request. Anything unrecognised is DEFAULT.
static inline const char* autoReplyModeName(uint8_t mode) {
  switch (mode) {
    case AUTOREPLY_MODE_FLOOD:   return "flood";
    case AUTOREPLY_MODE_PRIVATE: return "private";
    case AUTOREPLY_MODE_DIRECT:  return "direct";
    case AUTOREPLY_MODE_SILENT:  return "silent";
    default:                     return "default";
  }
}

// Read a mode name as `set autoreply.mode` takes it. Measured the same way an
// on/off argument is, so 'flooding' does not pass as 'flood'; and only the three
// standing modes are accepted, for the reason autoReplyModeName gives. 'out' is
// left alone unless a whole valid token was found.
static inline bool autoReplyParseModeName(const char* arg, uint8_t* out) {
  if (arg == NULL || out == NULL) return false;

  while (*arg == ' ') arg++;
  size_t len = 0;
  while (arg[len] && arg[len] != ' ' && arg[len] != '\r' && arg[len] != '\n') len++;

  static const uint8_t standing[] = {
    AUTOREPLY_MODE_FLOOD, AUTOREPLY_MODE_PRIVATE, AUTOREPLY_MODE_DIRECT,
  };
  for (size_t i = 0; i < sizeof(standing); i++) {
    const char* name = autoReplyModeName(standing[i]);
    if (len == strlen(name) && strncasecmp(arg, name, len) == 0) {
      *out = standing[i];
      return true;
    }
  }
  return false;
}

// The route back to a requester whose flood arrived along 'path'. A flood collects
// the hash of each repeater that carried it, first to last, so read backwards it
// is the path from here to the requester -- and that is the only route a private
// direct reply has. 'path_len' is the packet's encoded length byte (hash size in
// the top two bits, count in the rest), returned unchanged so the caller can hand
// both straight to sendDirect(). Hashes are reversed whole, never their bytes.
//
// The same links carried the request the other way, and this mesh is asymmetric
// as a matter of course, so a reply sent down the reverse path can fail where a
// flood would not. That is the trade the requester makes by asking for D.
static inline uint8_t autoReplyReversePath(const uint8_t* path, uint8_t path_len,
                                           uint8_t* out) {
  uint8_t size = (path_len >> 6) + 1;
  uint8_t count = path_len & 63;
  for (uint8_t i = 0; i < count; i++) {
    memcpy(out + i * size, path + (count - 1 - i) * size, size);
  }
  return path_len;
}

// A request from further away than 'max_hops' is ignored. Answering it means
// flooding, and every repeater that heard it floods a reply of its own, so this is
// the setting that bounds what one 'test' costs the mesh.
static inline bool autoReplyHopsAllowed(uint8_t hop_count, uint8_t max_hops) {
  return hop_count <= max_hops;
}

// A request that arrived direct can be answered with a single packet no repeater
// will retransmit, which costs the mesh nothing but reaches only what this node can
// reach. Hearing a request is no promise the requester can hear the answer - it may
// sit in far worse noise than a repeater does - so 'direct_flood' trades that packet
// for a flooded one that a neighbouring repeater can carry back.
static inline bool autoReplyReplyIsZeroHop(uint8_t hop_count, bool direct_flood) {
  return hop_count == 0 && !direct_flood;
}

// Mode M's answer rides the same block that answers a login, a regions query, an
// owner query and a clock query, and that block sends everything on one fixed
// delay. Only the test is an auto-reply. Only the test is staggered.
//
// The distinction matters in one direction: a login, regions, owner or clock
// request has a person waiting at a keyboard for it, and making them sit through a
// mesh-wide stagger measured in tens of seconds would read as a dead node, not as
// politeness. A test has nobody waiting, and every repeater in range answers the
// same request, which is exactly the case the stagger exists for.
//
// The sub-type is data[4] of the request. A login is identified by that byte being
// 0 (an empty password) or any printable character, so the safe rule is the narrow
// one: stagger the test sub-type and nothing else.
#define AUTOREPLY_ANON_TEST_SUBTYPE 0x04

static inline bool autoReplyAnonReplyIsStaggered(uint8_t sub_type) {
  return sub_type == AUTOREPLY_ANON_TEST_SUBTYPE;
}
