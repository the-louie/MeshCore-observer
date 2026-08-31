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

// A group text, split into who sent it and whether it asked for a reply. 'sender',
// 'message' and 'id' point into the text buffer passed to autoReplyParseRequest().
struct AutoReplyRequest {
  bool is_trigger;
  const char* sender;
  size_t sender_len;
  const char* message;
  const char* id;        // NULL for a bare keyword
  size_t id_len;
};

// Does the message ask for a reply, and does it carry a correlation id?
//
// Two forms are accepted: the bare keyword, and the keyword followed by one space
// and exactly AUTOREPLY_ID_LEN hex characters. **The bare form must keep working
// indefinitely.** Flashing this fleet takes months, so probes stay bare until
// adoption is high; a firmware that only answered the id form would silently drop
// every un-flashed node out of the measurement, and those are exactly the nodes
// whose connectivity is least understood.
//
// The tail is matched strictly -- one space, then hex, then end of string. Anything
// looser ('test me', 'test 123', trailing text) stays a non-trigger, which is what
// keeps ordinary chat on the channel from costing everyone airtime.
static inline bool autoReplyMatchTrigger(const char* msg, const char* keyword,
                                         const char** id, size_t* id_len) {
  *id = NULL;
  *id_len = 0;
  if (msg == NULL || keyword == NULL) return false;

  size_t klen = strlen(keyword);
  if (strncasecmp(msg, keyword, klen) != 0) return false;
  if (msg[klen] == 0) return true;                 // the bare keyword
  if (msg[klen] != ' ') return false;

  const char* tail = msg + klen + 1;
  size_t n = 0;
  while (n < AUTOREPLY_ID_LEN && isxdigit((unsigned char)tail[n])) n++;
  if (n != AUTOREPLY_ID_LEN || tail[n] != 0) return false;

  *id = tail;
  *id_len = n;
  return true;
}

// Split a group text into its name prefix and message, and test the message against
// the trigger keyword. Group texts are "<sender>: <message>" (see
// BaseChatMesh::sendGroupMessage), and a text without that prefix is treated as all
// message and no sender. 'text' is modified in place: whitespace around the message
// is trimmed before the comparison, so " test " still triggers.
//
// A reply can never trigger a reply: a reply body begins with the bracketed
// requester name, or with "SNR" when there was none, and neither can match the
// keyword. Widening the match to accept a correlation id does not change that,
// and test/test_autoreply pins it.
static inline AutoReplyRequest autoReplyParseRequest(char* text, const char* keyword) {
  AutoReplyRequest req = { false, text, 0, text, NULL, 0 };
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
  req.is_trigger = autoReplyMatchTrigger(msg, keyword, &req.id, &req.id_len);
  return req;
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
