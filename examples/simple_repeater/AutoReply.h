#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/AutoReplyLogic.h>
#include <helpers/IdentityStore.h>
#include "RateLimiter.h"

// the keyword that triggers a reply (matched on its own, case-insensitive)
#ifndef AUTOREPLY_KEYWORD
  #define AUTOREPLY_KEYWORD   "test"
#endif

#define AUTOREPLY_MAX_TEXT    112
// the requester's name is echoed back so they can pick their reply out of several
#define AUTOREPLY_MAX_SENDER  16
// '#test-' plus a three-character region code, rebuilt on the stack when needed
#define AUTOREPLY_MAX_CHANNEL 16
// a region name bounding how far a reply floods, or empty for the request's own scope
#define AUTOREPLY_MAX_REGION  31
// multiplier on the radio's own retransmit delay, spreading replies so the storms
// several repeaters raise do not land on top of each other
#define AUTOREPLY_DELAY_MIN   1
#define AUTOREPLY_DELAY_MAX   120
#define AUTOREPLY_DELAY_DEF   4
#define AUTOREPLY_MAX_PAYLOAD (5 + AUTOREPLY_MAX_TEXT)   // timestamp + flags + text

// how many replies this repeater will send in total, per AUTOREPLY_WINDOW_SECS
#define AUTOREPLY_MAX_REPLIES 10
// how long one sender must wait before being answered again
#define AUTOREPLY_WINDOW_SECS 300
// senders remembered for the per-sender cooldown (oldest is evicted). Sized above
// AUTOREPLY_MAX_REPLIES so a sender can never be evicted while still cooling down.
#define AUTOREPLY_MAX_SENDERS 16

/**
 * \brief  Replies to a keyword on the node's regional test channel, with a signal
 *         + path report.
 *
 * The channel is '#test-<iata>', derived from the region code set with
 * 'set mqtt.iata' rather than configured here, and its key is the first 16 bytes of
 * sha256 of that name - so there is no PSK to configure or share, and any client
 * that adds a channel of the same name can use it. Switched on with
 * 'set autoreply on'; a node with no region set stays silent either way.
 *
 * Every repeater in range answers the same trigger, so requests from further away than
 * 'autoreply.hops' are ignored, replies are rate limited per sender and in total, and
 * are staggered by a random delay.
 *
 * How a reply is sent is the requester's choice first and the node's second: a
 * request may name a mode (see AutoReplyLogic.h), and one that names none is answered
 * in the mode set with 'set autoreply.mode' -- flood, the group text every node sends
 * today, or private/direct, a text message to the requester alone.
 */
class AutoReply {
  FILESYSTEM* _fs;
  bool _enabled;
  char _iata[8];                // region the channel was derived from
  uint8_t _hops;                // max hop count of a request we will answer
  bool _direct_flood;           // flood the answer to a request that arrived direct
  uint8_t _delay_factor;        // multiplier on the radio retransmit delay
  char _region[AUTOREPLY_MAX_REGION + 1];   // scope for the reply, empty = mirror the request
  uint8_t _mode;                // how a request that names no mode is answered
  bool _ready;
  mesh::GroupChannel _channel;
  RateLimiter _limiter;

  AutoReplySender _senders[AUTOREPLY_MAX_SENDERS];
  uint8_t _next_sender;

  void refreshChannel(const char* iata);
  void load();
  void save();

public:
  AutoReply();

  void begin(FILESYSTEM* fs);

  /**
   * \brief  Handle the 'set/get autoreply[.*]' commands.
   * \param  iata  the node's region code, or NULL when the build has none
   * \returns  true if the command was ours (and 'reply' was filled in)
   */
  bool handleCommand(const char* iata, const char* command, char* reply);

  /**
   * \brief  Whether a request that arrived direct is answered with a flood.
   */
  bool directFlood() const { return _direct_flood; }
  uint8_t delayFactor() const { return _delay_factor; }
  const char* replyRegion() const { return _region; }
  /**
   * \brief  The standing reply mode, for a request that names none (AUTOREPLY_MODE_*).
   */
  uint8_t mode() const { return _mode; }

  /**
   * \brief  The derived '#test-<iata>' channel, for originating a probe on it.
   *
   * Readiness is only that the region is set and the channel derived. It is
   * deliberately independent of 'autoreply on', which governs whether this node
   * *answers* a request -- a node can be asked to originate a probe without
   * answering probes itself, and conflating the two would make the second
   * vantage point depend on a setting that has nothing to do with it.
   *
   * \param  iata  the node's region code, re-derives the channel when it changes
   * \returns  true and fills 'out' when a channel exists
   */
  bool probeChannel(const char* iata, mesh::GroupChannel* out) {
    refreshChannel(iata);
    if (!_ready || out == NULL) return false;
    *out = _channel;
    return true;
  }

  /**
   * \brief  Match our channel, for Mesh::searchChannelsByHash()
   * \param  iata  the node's region code, re-derives the channel when it changes
   */
  int searchChannelsByHash(const char* iata, const uint8_t* hash, mesh::GroupChannel channels[],
                           int max_matches);

  /**
   * \brief  Test an incoming group text for the trigger, and build the reply payload.
   * \param  dest  OUT - group datagram payload, needs AUTOREPLY_MAX_PAYLOAD bytes
   * \returns  length of the payload, or 0 to stay silent
   */
  int buildReply(const mesh::Packet* req, const uint8_t* data, size_t len,
                 const char* node_name, float rssi, uint32_t timestamp, uint8_t* dest);
};
