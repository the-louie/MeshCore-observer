#pragma once

#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include <helpers/IdentityStore.h>
#include "RateLimiter.h"

// the keyword that triggers a reply (matched on its own, case-insensitive)
#ifndef AUTOREPLY_KEYWORD
  #define AUTOREPLY_KEYWORD   "test"
#endif

#define AUTOREPLY_MAX_TEXT    96
#define AUTOREPLY_MAX_PAYLOAD (5 + AUTOREPLY_MAX_TEXT)   // timestamp + flags + text

// how many replies this repeater will send in total, per AUTOREPLY_WINDOW_SECS
#define AUTOREPLY_MAX_REPLIES 10
// how long one sender must wait before being answered again
#define AUTOREPLY_WINDOW_SECS 300
// senders remembered for the per-sender cooldown (oldest is evicted)
#define AUTOREPLY_MAX_SENDERS 32

/**
 * \brief  Replies to a keyword on one hashtag channel, with a signal + path report.
 *
 * The channel key is derived from the channel name (first 16 bytes of sha256 of the
 * name), so no PSK needs to be configured or shared. Disabled while the channel name
 * is empty.
 *
 * Every repeater in range answers the same trigger, so replies are only ever sent for
 * region-scoped requests (checked by the caller), are rate limited, and are staggered
 * by a random delay.
 */
class AutoReply {
  FILESYSTEM* _fs;
  char _channel_name[32];
  uint8_t _hops;                // max hop count of a request we will answer
  bool _ready;
  mesh::GroupChannel _channel;
  RateLimiter _limiter;

  // ring of recently answered senders, so one person retrying cannot use up
  // everyone else's share of the global limit
  struct SenderEntry {
    uint32_t id;          // hash of the sender name, 0 = unused slot
    uint32_t last_reply;
  };
  SenderEntry _senders[AUTOREPLY_MAX_SENDERS];
  uint8_t _next_sender;

  void deriveChannel();
  void load();
  void save();
  bool senderAllowed(const char* name, size_t name_len, uint32_t now);

public:
  AutoReply();

  void begin(FILESYSTEM* fs);

  /**
   * \brief  Handle the 'set/get autoreply.*' commands.
   * \returns  true if the command was ours (and 'reply' was filled in)
   */
  bool handleCommand(const char* command, char* reply);

  /**
   * \brief  Match our channel, for Mesh::searchChannelsByHash()
   */
  int searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches);

  /**
   * \brief  Test an incoming group text for the trigger, and build the reply payload.
   * \param  dest  OUT - group datagram payload, needs AUTOREPLY_MAX_PAYLOAD bytes
   * \returns  length of the payload, or 0 to stay silent
   */
  int buildReply(const mesh::Packet* req, const uint8_t* data, size_t len,
                 const char* node_name, float rssi, uint32_t timestamp, uint8_t* dest);
};
