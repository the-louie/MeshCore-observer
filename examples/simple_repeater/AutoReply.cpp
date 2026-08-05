#include "AutoReply.h"

#include <helpers/TxtDataHelpers.h>

#define AUTOREPLY_CONFIG_FILE   "/autoreply"
#define AUTOREPLY_CONFIG_VER    2
#define AUTOREPLY_CONFIG_VER_1  1    // channel name + hops, before the channel was derived

#define MAX_PATH_HASHES_SHOWN   8    // keep the reply short, it is airtime

static File openRead(FILESYSTEM* fs, const char* filename) {
  #if defined(RP2040_PLATFORM)
    return fs->open(filename, "r");
  #else
    return fs->open(filename);
  #endif
}

static File openWrite(FILESYSTEM* fs, const char* filename) {
  #if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    fs->remove(filename);
    return fs->open(filename, FILE_O_WRITE);
  #elif defined(RP2040_PLATFORM)
    return fs->open(filename, "w");
  #else
    return fs->open(filename, "w", true);
  #endif
}

AutoReply::AutoReply()
  : _fs(NULL), _enabled(false), _hops(8), _ready(false),
    _limiter(AUTOREPLY_MAX_REPLIES, AUTOREPLY_WINDOW_SECS)
{
  _iata[0] = 0;
  _channel_name[0] = 0;
  memset(_senders, 0, sizeof(_senders));
  _next_sender = 0;
}

void AutoReply::begin(FILESYSTEM* fs) {
  _fs = fs;
  load();
}

// The region is owned by the observer config and can change at any time, so the
// channel is re-derived whenever it has. The hashtag channel key is the first 16
// bytes of sha256 of the name.
void AutoReply::refreshChannel(const char* iata) {
  if (iata == NULL) iata = "";
  if (strcmp(iata, _iata) == 0) return;

  StrHelper::strncpy(_iata, iata, sizeof(_iata));
  _ready = autoReplyDeriveChannel(_iata, _channel_name, sizeof(_channel_name));
  if (!_ready) {
    MESH_DEBUG_PRINTLN("AutoReply: no region set, staying silent - use 'set mqtt.iata'");
    return;
  }

  memset(_channel.secret, 0, sizeof(_channel.secret));
  mesh::Utils::sha256(_channel.secret, 16, (const uint8_t *) _channel_name, strlen(_channel_name));
  mesh::Utils::sha256(_channel.hash, sizeof(_channel.hash), _channel.secret, 16);

  MESH_DEBUG_PRINTLN("AutoReply: channel '%s' (hash %02X), keyword '%s', max %d hops, %s",
                     _channel_name, (uint32_t)_channel.hash[0], AUTOREPLY_KEYWORD,
                     (uint32_t)_hops, _enabled ? "on" : "off");
}

void AutoReply::load() {
  if (!_fs->exists(AUTOREPLY_CONFIG_FILE)) return;

  File file = openRead(_fs, AUTOREPLY_CONFIG_FILE);
  if (file) {
    uint8_t ver = 0;
    file.read(&ver, 1);
    if (ver == AUTOREPLY_CONFIG_VER) {
      uint8_t enabled = 0;
      file.read(&enabled, 1);
      file.read(&_hops, 1);
      _enabled = enabled != 0;
    } else if (ver == AUTOREPLY_CONFIG_VER_1) {
      // v1 named the channel to switch the feature on, so a node that had one
      // named wanted it on. The name itself is dropped: it is derived now.
      char old_name[32];
      file.read((uint8_t *) old_name, sizeof(old_name));
      file.read(&_hops, 1);
      old_name[sizeof(old_name) - 1] = 0;
      _enabled = old_name[0] != 0;
    }
    file.close();
  }
}

void AutoReply::save() {
  File file = openWrite(_fs, AUTOREPLY_CONFIG_FILE);
  if (file) {
    uint8_t ver = AUTOREPLY_CONFIG_VER;
    uint8_t enabled = _enabled ? 1 : 0;
    file.write(&ver, 1);
    file.write(&enabled, 1);
    file.write(&_hops, 1);
    file.close();
  }
}

int AutoReply::searchChannelsByHash(const char* iata, const uint8_t* hash,
                                    mesh::GroupChannel channels[], int max_matches) {
  refreshChannel(iata);
  if (!_enabled || !_ready) return 0;
  if (memcmp(hash, _channel.hash, sizeof(_channel.hash)) != 0) {
    // the name is hashed exactly as stored, so a mismatch is usually a spelling one
    MESH_DEBUG_PRINTLN("AutoReply: not our channel - heard hash %02X, '%s' is %02X",
                       (uint32_t)hash[0], _channel_name, (uint32_t)_channel.hash[0]);
    return 0;
  }

  MESH_DEBUG_PRINTLN("AutoReply: channel '%s' matched (hash %02X)", _channel_name,
                     (uint32_t)_channel.hash[0]);
  channels[0] = _channel;
  return 1;
}

bool AutoReply::handleCommand(const char* iata, const char* command, char* reply) {
  refreshChannel(iata);

  if (memcmp(command, "set autoreply ", 14) == 0) {
    if (memcmp(&command[14], "on", 2) == 0) {
      _enabled = true;
    } else if (memcmp(&command[14], "off", 3) == 0) {
      _enabled = false;
    } else {
      strcpy(reply, "Err - use: set autoreply on|off");
      return true;
    }
    save();
    if (_enabled && !_ready) {
      strcpy(reply, "OK - on, but no region yet: set mqtt.iata");
    } else {
      sprintf(reply, "OK - %s", _enabled ? _channel_name : "off");
    }
    return true;
  }

  if (memcmp(command, "set autoreply.channel", 21) == 0) {
    strcpy(reply, "Err - channel is automatic, set mqtt.iata");
    return true;
  }

  if (memcmp(command, "set autoreply.hops ", 19) == 0) {
    int hops = atoi(&command[19]);
    if (hops < 0 || hops > 63) {
      strcpy(reply, "Err - must be 0..63");
    } else {
      _hops = (uint8_t)hops;
      save();
      strcpy(reply, "OK");
    }
    return true;
  }

  if (strcmp(command, "get autoreply.channel") == 0) {
    sprintf(reply, "> %s", _ready ? _channel_name : "(no region - set mqtt.iata)");
    return true;
  }

  if (strcmp(command, "get autoreply.hops") == 0) {
    sprintf(reply, "> %d", _hops);
    return true;
  }

  if (strcmp(command, "get autoreply") == 0) {
    if (!_ready) {
      sprintf(reply, "> %s, no region - set mqtt.iata", _enabled ? "on" : "off");
    } else {
      sprintf(reply, "> %s %s '%s' max %d hops", _enabled ? "on" : "off", _channel_name,
              AUTOREPLY_KEYWORD, _hops);
    }
    return true;
  }

  return false;   // not one of ours
}

int AutoReply::buildReply(const mesh::Packet* req, const uint8_t* data, size_t len,
                          const char* node_name, float rssi, uint32_t timestamp, uint8_t* dest) {
  if (!_enabled || !_ready) return 0;
  if (len < 6) {                                      // timestamp + flags + at least one char
    MESH_DEBUG_PRINTLN("AutoReply: ignored, message too short (len=%d)", (uint32_t)len);
    return 0;
  }
  if ((data[4] >> 2) != TXT_TYPE_PLAIN) {             // only plain chat text
    MESH_DEBUG_PRINTLN("AutoReply: ignored, not plain text (txt_type=%d)", (uint32_t)(data[4] >> 2));
    return 0;
  }

  uint8_t hop_count = req->getPathHashCount();
  if (!autoReplyHopsAllowed(hop_count, _hops)) {
    MESH_DEBUG_PRINTLN("AutoReply: ignored, %d hops away (max %d)", (uint32_t)hop_count,
                       (uint32_t)_hops);
    return 0;
  }

  char text[AUTOREPLY_MAX_TEXT];
  size_t text_len = min(len - 5, sizeof(text) - 1);
  memcpy(text, &data[5], text_len);
  text[text_len] = 0;

  AutoReplyRequest request = autoReplyParseRequest(text, AUTOREPLY_KEYWORD);
  if (!request.is_trigger) {
    MESH_DEBUG_PRINTLN("AutoReply: not the keyword - got '%s', want '%s'", request.message,
                       AUTOREPLY_KEYWORD);
    return 0;
  }

  // Per-sender first, so someone retrying cannot spend everyone else's share of
  // the global budget. Only real triggers are charged against either limit.
  uint32_t sender_id = autoReplySenderId(request.sender, request.sender_len);
  if (!autoReplySenderAllowed(_senders, AUTOREPLY_MAX_SENDERS, _next_sender, sender_id,
                              timestamp, AUTOREPLY_WINDOW_SECS)) {
    MESH_DEBUG_PRINTLN("AutoReply: '%.*s' was already answered in the last %d s",
                       (int)request.sender_len, request.sender, (uint32_t)AUTOREPLY_WINDOW_SECS);
    return 0;
  }
  if (!_limiter.allow(timestamp)) {
    MESH_DEBUG_PRINTLN("AutoReply: rate limited, %d replies already sent in the last %d s",
                       (uint32_t)AUTOREPLY_MAX_REPLIES, (uint32_t)AUTOREPLY_WINDOW_SECS);
    return 0;
  }

  // the repeaters this request came through (flood routing appends each hop)
  char path_hex[MAX_PATH_HASHES_SHOWN * 9 + 4];
  uint8_t hash_size = req->getPathHashSize();
  uint8_t shown = min(hop_count, (uint8_t)MAX_PATH_HASHES_SHOWN);
  if (shown == 0) {
    strcpy(path_hex, "direct");
  } else {
    char* wp = path_hex;
    for (uint8_t i = 0; i < shown; i++) {
      if (i > 0) *wp++ = ',';
      mesh::Utils::toHex(wp, &req->path[i * hash_size], hash_size);
      wp += hash_size * 2;
    }
    if (shown < hop_count) { strcpy(wp, ".."); wp += 2; }
    *wp = 0;
  }

  // reply uses the same payload layout as the request
  memcpy(dest, &timestamp, 4);
  dest[4] = (TXT_TYPE_PLAIN << 2);

  // bounded, as the name and the path can both be long
  char who[AUTOREPLY_MAX_SENDER + 4];
  if (request.sender_len > 0) {
    int n = min((int)request.sender_len, AUTOREPLY_MAX_SENDER);
    snprintf(who, sizeof(who), "[%.*s] ", n, request.sender);
  } else {
    who[0] = 0;
  }

  char* out = (char *) &dest[5];
  snprintf(out, AUTOREPLY_MAX_TEXT, "%s: %sSNR %s RSSI %d %dh %s", node_name, who,
           StrHelper::ftoa(req->getSNR()), (int)rssi, (uint32_t)hop_count, path_hex);

  MESH_DEBUG_PRINTLN("AutoReply: replying '%s'", out);
  return 5 + strlen(out);
}
