#include "AutoReply.h"

#include <helpers/TxtDataHelpers.h>

#define AUTOREPLY_CONFIG_FILE   "/autoreply"
#define AUTOREPLY_CONFIG_VER    1

#define MAX_PATH_HASHES_SHOWN   8    // keep the reply short, it is airtime

// channels that are shared mesh-wide, where a reply from every repeater would be spam
static const char* banned_channels[] = { "#public", "#test", "#bot", NULL };

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

static void toLowerStr(char* s) {
  for (; *s; s++) {
    if (*s >= 'A' && *s <= 'Z') *s += 32;
  }
}

static bool equalsIgnoreCase(const char* a, const char* b) {
  while (*a && *b) {
    char ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
    char cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
    if (ca != cb) return false;
    a++; b++;
  }
  return *a == 0 && *b == 0;
}

AutoReply::AutoReply()
  : _fs(NULL), _hops(8), _ready(false), _limiter(AUTOREPLY_MAX_REPLIES, AUTOREPLY_WINDOW_SECS)
{
  _channel_name[0] = 0;
  memset(_senders, 0, sizeof(_senders));
  _next_sender = 0;
}

// case-folded FNV-1a, so 'Louie' and 'louie' are the same sender
static uint32_t hashName(const char* s, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    char c = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];
    h ^= (uint8_t) c;
    h *= 16777619u;
  }
  return h;
}

bool AutoReply::senderAllowed(const char* name, size_t name_len, uint32_t now) {
  uint32_t id = hashName(name, name_len);

  for (int i = 0; i < AUTOREPLY_MAX_SENDERS; i++) {
    if (_senders[i].id == id) {
      // guard against the clock going backwards, which would look like a huge gap
      if (now >= _senders[i].last_reply && now - _senders[i].last_reply < AUTOREPLY_WINDOW_SECS) {
        return false;
      }
      _senders[i].last_reply = now;
      return true;
    }
  }

  // not seen before, so take the next slot in the ring, evicting whoever is oldest
  _senders[_next_sender].id = id;
  _senders[_next_sender].last_reply = now;
  _next_sender = (_next_sender + 1) % AUTOREPLY_MAX_SENDERS;
  return true;
}

void AutoReply::begin(FILESYSTEM* fs) {
  _fs = fs;
  load();
  deriveChannel();
}

// hashtag channel key is the first 16 bytes of sha256 of the channel name
void AutoReply::deriveChannel() {
  _ready = false;
  if (_channel_name[0] == 0) return;

  memset(_channel.secret, 0, sizeof(_channel.secret));
  mesh::Utils::sha256(_channel.secret, 16, (const uint8_t *) _channel_name, strlen(_channel_name));
  mesh::Utils::sha256(_channel.hash, sizeof(_channel.hash), _channel.secret, 16);
  _ready = true;

  MESH_DEBUG_PRINTLN("AutoReply: listening on '%s' (channel hash %02X), keyword '%s', max %d hops",
                     _channel_name, (uint32_t) _channel.hash[0], AUTOREPLY_KEYWORD, (uint32_t) _hops);
}

void AutoReply::load() {
  if (_fs == NULL || !_fs->exists(AUTOREPLY_CONFIG_FILE)) return;

  File file = openRead(_fs, AUTOREPLY_CONFIG_FILE);
  if (file) {
    uint8_t ver = 0;
    file.read(&ver, 1);
    if (ver == AUTOREPLY_CONFIG_VER) {
      file.read((uint8_t *) _channel_name, sizeof(_channel_name));
      file.read(&_hops, 1);
      _channel_name[sizeof(_channel_name) - 1] = 0;
      toLowerStr(_channel_name);   // migrate a name saved before names were folded
    }
    file.close();
  }
}

void AutoReply::save() {
  if (_fs == NULL) return;

  File file = openWrite(_fs, AUTOREPLY_CONFIG_FILE);
  if (file) {
    uint8_t ver = AUTOREPLY_CONFIG_VER;
    file.write(&ver, 1);
    file.write((const uint8_t *) _channel_name, sizeof(_channel_name));
    file.write(&_hops, 1);
    file.close();
  }
}

int AutoReply::searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) {
  if (!_ready || max_matches < 1) return 0;
  if (memcmp(hash, _channel.hash, sizeof(_channel.hash)) != 0) {
    // by far the most common cause of "it does not answer": the name differs,
    // and the name is hashed exactly as typed, so case matters
    MESH_DEBUG_PRINTLN("AutoReply: not our channel - heard hash %02X, '%s' is %02X",
                       (uint32_t) hash[0], _channel_name, (uint32_t) _channel.hash[0]);
    return 0;
  }

  MESH_DEBUG_PRINTLN("AutoReply: channel '%s' matched (hash %02X)", _channel_name,
                     (uint32_t) _channel.hash[0]);
  channels[0] = _channel;
  return 1;
}

bool AutoReply::handleCommand(const char* command, char* reply) {
  // trailing space is optional, so a bare 'set autoreply.channel' turns it off
  if (memcmp(command, "set autoreply.channel", 21) == 0 &&
      (command[21] == 0 || command[21] == ' ')) {
    const char* name = &command[21];
    while (*name == ' ') name++;

    if (*name == 0) {   // blank name turns the feature off
      _channel_name[0] = 0;
      deriveChannel();
      save();
      strcpy(reply, "OK - autoreply off");
      return true;
    }
    if (*name != '#') {
      strcpy(reply, "Err - name must start with #");
      return true;
    }
    if (strlen(name) >= sizeof(_channel_name)) {
      strcpy(reply, "Err - name too long");
      return true;
    }

    // Clients only accept lower-case channel names, so fold the name here: an
    // upper-case name would hash to a channel nobody can actually join.
    char tmp[sizeof(_channel_name)];
    StrHelper::strncpy(tmp, name, sizeof(tmp));
    toLowerStr(tmp);

    for (int i = 0; banned_channels[i]; i++) {
      if (strcmp(tmp, banned_channels[i]) == 0) {
        sprintf(reply, "Err - %s is shared, use a regional name", banned_channels[i]);
        return true;
      }
    }
    StrHelper::strncpy(_channel_name, tmp, sizeof(_channel_name));
    deriveChannel();
    save();
    sprintf(reply, "OK - %s", _channel_name);
    return true;
  }

  if (memcmp(command, "set autoreply.hops ", 19) == 0) {
    int hops = atoi(&command[19]);
    if (hops < 0 || hops > 63) {
      strcpy(reply, "Err - must be 0..63");
    } else {
      _hops = (uint8_t) hops;
      save();
      strcpy(reply, "OK");
    }
    return true;
  }

  if (strcmp(command, "get autoreply.channel") == 0) {
    sprintf(reply, "> %s", _channel_name[0] ? _channel_name : "(off)");
    return true;
  }

  if (strcmp(command, "get autoreply.hops") == 0) {
    sprintf(reply, "> %d", (uint32_t) _hops);
    return true;
  }

  if (strcmp(command, "get autoreply") == 0) {
    if (_ready) {
      sprintf(reply, "> %s '%s' max %d hops", _channel_name, AUTOREPLY_KEYWORD, (uint32_t) _hops);
    } else {
      // there is no 'set autoreply on' - naming a channel is what enables it
      strcpy(reply, "> off - use: set autoreply.channel #name");
    }
    return true;
  }

  return false;   // not one of ours
}

int AutoReply::buildReply(const mesh::Packet* req, const uint8_t* data, size_t len,
                          const char* node_name, float rssi, uint32_t timestamp, uint8_t* dest) {
  if (!_ready) return 0;
  if (len < 6) {                                      // timestamp + flags + at least one char
    MESH_DEBUG_PRINTLN("AutoReply: ignored, message too short (len=%d)", (uint32_t) len);
    return 0;
  }
  if ((data[4] >> 2) != TXT_TYPE_PLAIN) {             // only plain chat text
    MESH_DEBUG_PRINTLN("AutoReply: ignored, not plain text (txt_type=%d)", (uint32_t)(data[4] >> 2));
    return 0;
  }

  uint8_t hop_count = req->getPathHashCount();
  if (hop_count > _hops) {                            // came from too far away
    MESH_DEBUG_PRINTLN("AutoReply: ignored, %d hops away (max %d)", (uint32_t) hop_count,
                       (uint32_t) _hops);
    return 0;
  }

  // group text is "<sender>: <message>", see BaseChatMesh::sendGroupMessage()
  char text[AUTOREPLY_MAX_TEXT];
  size_t text_len = len - 5;
  if (text_len >= sizeof(text)) text_len = sizeof(text) - 1;
  memcpy(text, &data[5], text_len);
  text[text_len] = 0;

  // the name prefix is the only sender identity a group text carries
  const char* sender = text;
  size_t sender_len = 0;
  char* msg = strstr(text, ": ");
  if (msg) {
    sender_len = msg - text;
    msg += 2;
  } else {
    msg = text;
  }

  while (*msg == ' ') msg++;                          // trim, so " test " still triggers
  char* end = msg + strlen(msg);
  while (end > msg && (end[-1] == ' ' || end[-1] == '\n' || end[-1] == '\r')) end--;
  *end = 0;

  if (!equalsIgnoreCase(msg, AUTOREPLY_KEYWORD)) {
    MESH_DEBUG_PRINTLN("AutoReply: not the keyword - got '%s', want '%s'", msg, AUTOREPLY_KEYWORD);
    return 0;
  }

  // Per-sender first, so someone retrying cannot spend everyone else's share of
  // the global budget. Only real triggers are charged against either limit.
  if (!senderAllowed(sender, sender_len, timestamp)) {
    MESH_DEBUG_PRINTLN("AutoReply: '%.*s' was already answered in the last %d s",
                       (int) sender_len, sender, (uint32_t) AUTOREPLY_WINDOW_SECS);
    return 0;
  }
  if (!_limiter.allow(timestamp)) {
    MESH_DEBUG_PRINTLN("AutoReply: rate limited, %d replies already sent in the last %d s",
                       (uint32_t) AUTOREPLY_MAX_REPLIES, (uint32_t) AUTOREPLY_WINDOW_SECS);
    return 0;
  }

  // the repeaters this request came through (flood routing appends each hop)
  char path_hex[MAX_PATH_HASHES_SHOWN * 9 + 4];
  uint8_t hash_size = req->getPathHashSize();
  uint8_t shown = hop_count > MAX_PATH_HASHES_SHOWN ? MAX_PATH_HASHES_SHOWN : hop_count;
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

  // group text payload is: timestamp, flags, then "<sender>: <message>"
  memcpy(dest, &timestamp, 4);
  dest[4] = (TXT_TYPE_PLAIN << 2);

  // echo the requester's name back, so they can pick their reply out of several
  // arriving together. Bounded, as the name and the path can both be long.
  char who[AUTOREPLY_MAX_SENDER + 4];
  if (sender_len > 0) {
    int n = sender_len > AUTOREPLY_MAX_SENDER ? AUTOREPLY_MAX_SENDER : (int) sender_len;
    snprintf(who, sizeof(who), "[%.*s] ", n, sender);
  } else {
    who[0] = 0;   // no name prefix in the request, so nothing to echo
  }

  char* out = (char *) &dest[5];
  snprintf(out, AUTOREPLY_MAX_TEXT, "%s: %sSNR %s RSSI %d %dh %s", node_name, who,
           StrHelper::ftoa(req->getSNR()), (int) rssi, (uint32_t) hop_count, path_hex);

  MESH_DEBUG_PRINTLN("AutoReply: replying '%s'", out);
  return 5 + strlen(out);
}
