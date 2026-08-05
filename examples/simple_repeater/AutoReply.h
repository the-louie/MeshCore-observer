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
 */
class AutoReply {
  FILESYSTEM* _fs;
  bool _enabled;
  char _iata[8];                // region the channel was derived from
  uint8_t _hops;                // max hop count of a request we will answer
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
