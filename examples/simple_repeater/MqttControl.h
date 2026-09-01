#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/AutoReplyLogic.h>
#include <helpers/IdentityStore.h>
#include <helpers/MqttControlLogic.h>

#include "RateLimiter.h"

// One staged command, copied out of the MQTT event task so nothing expensive
// happens there. Sized to the logic's own bounds; both are static, because the
// DRAM fragmentation note in docs/mbedtls-tls-footprint.md makes a per-message
// allocation on the reconnect path a measured hazard rather than a style point.
struct MqttControlStaged {
  char topic[MQTTCTRL_MAX_TOPIC + 1];
  char payload[MQTTCTRL_MAX_PAYLOAD + 1];
  size_t payload_len;
  bool pending;
};

// Runs an allowlisted command through the node's CLI. An interface rather than a
// direct call on MyMesh, so this class stays free of the application it serves and
// its tests need no mesh at all.
struct MqttCommandRunner {
  virtual ~MqttCommandRunner() { }
  virtual void runCommand(uint32_t sender_timestamp, char* command, char* reply) = 0;
};

/**
 * \brief  Accepts signed commands over MQTT and runs them through the node's CLI.
 *
 * The broker this rides on publishes its own credentials, so anyone can publish to
 * any topic on it. Nothing here trusts the transport: a command is executed only if
 * it carries an Ed25519 signature by the node's configured owner key over the topic
 * and body together, its counter has never been seen before, it has not expired, it
 * is on the allowlist, and the rate limiter still has budget.
 *
 * The decisions are all in src/helpers/MqttControlLogic.h, where they are attacked
 * on the host (test/test_mqtt_control). This class is the state, the key and the I/O.
 *
 * Off unless 'set mqtt.cmd on', and inert regardless without an owner key.
 */
class MqttControl {
  FILESYSTEM* _fs;
  bool _enabled;
  uint32_t _counter;          // highest command counter accepted, persisted
  MqttControlStaged _staged;
  RateLimiter _commands;
  RateLimiter _transmits;

  void load();
  void save();

public:
  MqttControl();

  void begin(FILESYSTEM* fs);
  bool isEnabled() const { return _enabled; }
  uint32_t counter() const { return _counter; }

  // Build the topics this node answers on. 'out' must hold MQTTCTRL_MAX_TOPIC + 1.
  static bool buildTopic(const char* iata, const char* node_hex, char* out, size_t out_size);
  static bool buildBroadcastTopic(const char* iata, char* out, size_t out_size);
  static bool buildResultTopic(const char* iata, const char* node_hex,
                               char* out, size_t out_size);

  // Called on the MQTT event task. Copies and flags, nothing more: verification and
  // execution both need more stack than that task has, and executing a command
  // there would run a flash write on the network stack's own thread.
  void stage(const char* topic, const uint8_t* payload, size_t len);
  bool hasPending() const { return _staged.pending; }

  // Resolve a staged command without executing it. The slot holds one command and
  // clears only when it is disposed of, so every path that declines to run one
  // must call this or the slot latches and drops everything after it.
  void discard() { _staged.pending = false; }

  // Called on the loop task. Verifies the staged command against 'owner' and runs
  // it through 'mesh' if it passes, writing a human-readable outcome into 'reply'
  // (at least 160 bytes, the CLI's own budget). Returns the verdict either way, so
  // the caller can publish a refusal without having to guess why.
  // 'authentic', when given, is set true the moment the signature verifies and
  // stays false otherwise. It is what decides whether a result may be published:
  // verification happens before the rate limiter (deliberately -- the limiters
  // spend budget when asked, so every stateless check runs first), which means a
  // bad-signature refusal is not rate limited. Publishing those would let anyone
  // on a broker whose credentials are public drive unlimited publishes out of
  // this node by sending garbage. A flag set at the point of truth, rather than
  // a classification of result codes, so a code added later cannot drift into
  // the wrong half.
  MqttCtrlResult drain(const mesh::Identity& owner, uint32_t now,
                       MqttCommandRunner* runner, char* reply, size_t reply_size,
                       bool* authentic = NULL);

  bool handleCommand(const char* command, char* reply);
};
