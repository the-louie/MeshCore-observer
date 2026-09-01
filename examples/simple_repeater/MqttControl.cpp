#include "MqttControl.h"

#include <helpers/TxtDataHelpers.h>

#define MQTTCTRL_CONFIG_FILE  "/mqttcontrol"
#define MQTTCTRL_CONFIG_VER   1

#define MQTTCTRL_TOPIC_PREFIX "meshhealth/v1/"

// Same shape as AutoReply's: the filesystem call differs per platform, and each
// feature keeps its own copy rather than adding a shared header upstream edits.
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

// The node's own MQTT-originated commands run with the RTC time rather than 0.
// Zero is not a clock, it is a privilege signal: CommonCLI gates 'set prv.key',
// 'erase' and 'set mqtt.owner' on it, and serial and the web CLI both rely on
// physical or password authentication as their boundary. A remote caller has
// neither, so it must never present itself as local. The allowlist in
// MqttControlLogic.h is the independent second guard on the same door.
MqttControl::MqttControl()
  : _fs(NULL), _enabled(false), _counter(0),
    _commands(MQTTCTRL_CMD_MAX, MQTTCTRL_CMD_WINDOW_SECS),
    _transmits(MQTTCTRL_TRIGGER_MAX, MQTTCTRL_TRIGGER_WINDOW)
{
  _staged.pending = false;
  _staged.payload_len = 0;
  _staged.topic[0] = 0;
  _staged.payload[0] = 0;
}

void MqttControl::begin(FILESYSTEM* fs) {
  _fs = fs;
  load();
}

void MqttControl::load() {
  if (!_fs->exists(MQTTCTRL_CONFIG_FILE)) return;

  File file = openRead(_fs, MQTTCTRL_CONFIG_FILE);
  if (file) {
    uint8_t ver = 0;
    file.read(&ver, 1);
    if (ver == MQTTCTRL_CONFIG_VER) {
      uint8_t enabled = 0;
      file.read(&enabled, 1);
      file.read((uint8_t *) &_counter, sizeof(_counter));
      _enabled = enabled != 0;
    }
    file.close();
  }
}

void MqttControl::save() {
  File file = openWrite(_fs, MQTTCTRL_CONFIG_FILE);
  if (file) {
    uint8_t ver = MQTTCTRL_CONFIG_VER;
    uint8_t enabled = _enabled ? 1 : 0;
    file.write(&ver, 1);
    file.write(&enabled, 1);
    file.write((const uint8_t *) &_counter, sizeof(_counter));
    file.close();
  }
}

static bool buildOne(const char* iata, const char* leaf, const char* suffix,
                     char* out, size_t out_size) {
  if (iata == NULL || iata[0] == 0 || leaf == NULL || out == NULL) return false;
  int n = snprintf(out, out_size, "%s%s/%s/%s", MQTTCTRL_TOPIC_PREFIX, iata, leaf, suffix);
  return n > 0 && (size_t)n < out_size;
}

bool MqttControl::buildTopic(const char* iata, const char* node_hex,
                             char* out, size_t out_size) {
  return buildOne(iata, node_hex, "cmd", out, out_size);
}

bool MqttControl::buildBroadcastTopic(const char* iata, char* out, size_t out_size) {
  return buildOne(iata, "all", "cmd", out, out_size);
}

bool MqttControl::buildResultTopic(const char* iata, const char* node_hex,
                                   char* out, size_t out_size) {
  return buildOne(iata, node_hex, "res", out, out_size);
}

void MqttControl::stage(const char* topic, const uint8_t* payload, size_t len) {
  // Runs on the client's esp-mqtt event task, which has a fraction of the stack the
  // bridge task has and none of the loop task's right to touch flash. Copy, flag,
  // and get out; MQTTBridge.cpp:1630-1636 says the same about publishing.
  if (!_enabled || _staged.pending) return;          // a full slot drops the newer
  if (topic == NULL || payload == NULL) return;
  if (len == 0 || len > MQTTCTRL_MAX_PAYLOAD) return;
  if (strlen(topic) > MQTTCTRL_MAX_TOPIC) return;

  memcpy(_staged.topic, topic, strlen(topic) + 1);
  memcpy(_staged.payload, payload, len);
  _staged.payload[len] = 0;
  _staged.payload_len = len;
  _staged.pending = true;
}

MqttCtrlResult MqttControl::drain(const mesh::Identity& owner, uint32_t now,
                                  MqttCommandRunner* runner, char* reply, size_t reply_size,
                                  MqttCtrlOutcome* out) {
  if (reply != NULL && reply_size > 0) reply[0] = 0;
  if (out != NULL) { out->authentic = false; out->counter = 0; out->command[0] = 0; }
  if (!_staged.pending) return MQTTCTRL_OK;

  MqttCtrlResult result = MQTTCTRL_OK;
  MqttCtrlEnvelope env;

  do {
    result = mqttCtrlParseEnvelope(_staged.payload, _staged.payload_len, &env);
    if (result != MQTTCTRL_OK) break;

    // The signature is checked before anything it protects, and over the topic as
    // well as the body: without the topic bound in, a command signed for one node
    // could be captured and republished on another's, and each node's own counter
    // would never notice.
    uint8_t signed_bytes[MQTTCTRL_MAX_SIGNED];
    size_t signed_len = 0;
    if (!mqttCtrlBuildSignedMessage(_staged.topic, &env, signed_bytes,
                                    sizeof(signed_bytes), &signed_len)) {
      result = MQTTCTRL_ERR_TOO_LONG;
      break;
    }
    if (!owner.verify(env.signature, signed_bytes, signed_len)) {
      result = MQTTCTRL_ERR_SIG_INVALID;   // parsed as hex, did not verify
      break;
    }
    // Past this line the sender held the owner key, so the outcome is safe to
    // publish however it turns out -- and the counter and command it signed are
    // what let the requester tell which question the answer belongs to.
    if (out != NULL) {
      out->authentic = true;
      out->counter = env.counter;
      size_t n = env.command_len < MQTTCTRL_MAX_COMMAND ? env.command_len : MQTTCTRL_MAX_COMMAND;
      memcpy(out->command, env.command, n);
      out->command[n] = 0;
    }

    result = mqttCtrlAuthorise(&env, _counter, now);
    if (result != MQTTCTRL_OK) break;

    // The limiters are stateful and spend budget when asked, so they come after
    // every stateless check: otherwise anyone could drain the budget with garbage
    // that was going to be refused anyway.
    bool transmit = mqttCtrlIsTransmitCommand(env.command, env.command_len);

    // Stateless, so it runs before the limiters spend anything.
    result = mqttCtrlTransmitTopicResult(transmit, _staged.topic);
    if (result != MQTTCTRL_OK) break;

    result = mqttCtrlRateResult(transmit, _commands.allow(now),
                                transmit ? _transmits.allow(now) : true);
    if (result != MQTTCTRL_OK) break;

    // Persisted before the command runs, never after. A counter written afterwards
    // is reopened by whatever crashes mid-command, and a silently repeated
    // 'trigger test' spends airtime nobody asked for. The cost is that a crashed
    // command is not retried, which is the right way round.
    _counter = env.counter;
    save();

    char command[MQTTCTRL_MAX_COMMAND + 1];
    memcpy(command, env.command, env.command_len);
    command[env.command_len] = 0;
    if (runner != NULL) {
      runner->runCommand(mqttCtrlSenderStamp(now), command, reply);
    }
  } while (false);

  _staged.pending = false;
  return result;
}

bool MqttControl::handleCommand(const char* command, char* reply) {
  if (memcmp(command, "set mqtt.cmd ", 13) == 0) {
    bool on = false;
    if (!autoReplyParseOnOff(&command[13], &on)) {
      strcpy(reply, "Err - use: set mqtt.cmd on|off");
      return true;
    }
    _enabled = on;
    save();
    strcpy(reply, "OK");
    return true;
  }

  if (strcmp(command, "get mqtt.cmd") == 0) {
    sprintf(reply, "> %s, counter %d", _enabled ? "on" : "off", (uint32_t)_counter);
    return true;
  }

  return false;
}
