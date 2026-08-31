// Host tests for the MQTT control-plane logic (src/helpers/MqttControlLogic.h):
// the envelope split, the bytes the signature covers, the replay window, the
// expiry window and the command allowlist. These are the exact functions
// examples/simple_repeater/MqttControl.cpp calls.
//
// The broker this rides on publishes its own credentials, so every one of these
// functions is reachable by anyone. Most of what follows is therefore hostile
// input rather than happy path: a bug here is a remote mesh-flood capability.
// The design is docs/architecture/decisions/001-mqtt-control-envelope.md.
#include <gtest/gtest.h>
#include <string>
#include "helpers/MqttControlLogic.h"

// 128 hex characters, the length JWTHelper already emits.
static std::string sig(char fill = 'A') { return std::string(MQTTCTRL_SIG_HEX_LEN, fill); }

static std::string envelope(const std::string& signed_part) {
  return signed_part + "|" + sig();
}

static MqttCtrlResult parse(const std::string& payload, MqttCtrlEnvelope* out) {
  return mqttCtrlParseEnvelope(payload.data(), payload.size(), out);
}

// MqttCtrlEnvelope points into the payload buffer, which must outlive it. Tests
// that keep an envelope past the statement that made it hold the bytes here --
// a temporary std::string would leave every pointer dangling.
struct Held {
  std::string bytes;
  MqttCtrlEnvelope env;
  explicit Held(const std::string& signed_part) : bytes(envelope(signed_part)) {
    EXPECT_EQ(MQTTCTRL_OK, mqttCtrlParseEnvelope(bytes.data(), bytes.size(), &env));
  }
};

// ---- the happy path, so the hostile cases mean something --------------------

TEST(ParseEnvelope, AcceptsAWellFormedCommand) {
  MqttCtrlEnvelope e;
  ASSERT_EQ(MQTTCTRL_OK, parse(envelope("v1|7|1788200000|trigger test"), &e));
  EXPECT_EQ(7u, e.counter);
  EXPECT_EQ(1788200000u, e.expiry);
  EXPECT_EQ(std::string("trigger test"), std::string(e.command, e.command_len));
}

TEST(ParseEnvelope, SignedSpanExcludesTheSignature) {
  // The signed span must be a literal byte range of what arrived: we verify
  // exactly the bytes received, with no canonicalisation step to get wrong.
  const std::string signed_part = "v1|1|1788200000|get autoreply";
  Held h(signed_part);
  EXPECT_EQ(signed_part.size(), h.env.signed_len);
  EXPECT_EQ(signed_part, std::string(h.env.signed_part, h.env.signed_len));
}

TEST(ParseEnvelope, DecodesSignatureBytes) {
  MqttCtrlEnvelope e;
  std::string hex(MQTTCTRL_SIG_HEX_LEN, '0');
  hex[0] = 'f'; hex[1] = 'f'; hex[2] = 'A'; hex[3] = 'b';
  ASSERT_EQ(MQTTCTRL_OK, parse("v1|1|1788200000|get autoreply|" + hex, &e));
  EXPECT_EQ(0xFF, e.signature[0]);
  EXPECT_EQ(0xAB, e.signature[1]);   // hex decoding is case-insensitive
  EXPECT_EQ(0x00, e.signature[63]);
}

// ---- malformed and hostile envelopes ---------------------------------------

TEST(ParseEnvelope, RejectsEmptyAndOversized) {
  MqttCtrlEnvelope e;
  EXPECT_EQ(MQTTCTRL_ERR_EMPTY, mqttCtrlParseEnvelope(NULL, 10, &e));
  EXPECT_EQ(MQTTCTRL_ERR_EMPTY, mqttCtrlParseEnvelope("x", 0, &e));
  std::string huge = "v1|1|1788200000|" + std::string(MQTTCTRL_MAX_PAYLOAD, 'a') + "|" + sig();
  EXPECT_EQ(MQTTCTRL_ERR_TOO_LONG, parse(huge, &e));
}

TEST(ParseEnvelope, RejectsMissingOrWrongLengthSignature) {
  MqttCtrlEnvelope e;
  EXPECT_EQ(MQTTCTRL_ERR_NO_SIGNATURE, parse("v1 1 1788200000 trigger test", &e));
  EXPECT_EQ(MQTTCTRL_ERR_BAD_SIG_LEN, parse("v1|1|1788200000|trigger test|ABCD", &e));
  EXPECT_EQ(MQTTCTRL_ERR_BAD_SIG_LEN,
            parse("v1|1|1788200000|trigger test|" + std::string(127, 'A'), &e));
  EXPECT_EQ(MQTTCTRL_ERR_BAD_SIG_LEN,
            parse("v1|1|1788200000|trigger test|" + std::string(129, 'A'), &e));
}

TEST(ParseEnvelope, RejectsNonHexSignature) {
  MqttCtrlEnvelope e;
  std::string bad = sig();
  bad[64] = 'g';
  EXPECT_EQ(MQTTCTRL_ERR_BAD_SIG_HEX, parse("v1|1|1788200000|trigger test|" + bad, &e));
  // ' 1' would satisfy strtol; it must not satisfy this.
  std::string spaced = sig();
  spaced[0] = ' '; spaced[1] = '1';
  EXPECT_EQ(MQTTCTRL_ERR_BAD_SIG_HEX, parse("v1|1|1788200000|trigger test|" + spaced, &e));
}

TEST(ParseEnvelope, RejectsWrongVersionTag) {
  MqttCtrlEnvelope e;
  EXPECT_EQ(MQTTCTRL_ERR_BAD_VERSION, parse(envelope("v2|1|1788200000|trigger test"), &e));
  EXPECT_EQ(MQTTCTRL_ERR_BAD_VERSION, parse(envelope("|1|1788200000|trigger test"), &e));
  EXPECT_EQ(MQTTCTRL_ERR_BAD_VERSION, parse(envelope("v1x|1|1788200000|trigger test"), &e));
}

TEST(ParseEnvelope, RejectsMissingFields) {
  MqttCtrlEnvelope e;
  EXPECT_EQ(MQTTCTRL_ERR_BAD_FIELDS, parse(envelope("v1|1"), &e));
  EXPECT_EQ(MQTTCTRL_ERR_BAD_FIELDS, parse(envelope("v1|1|1788200000"), &e));
}

TEST(ParseEnvelope, RejectsCounterThatIsNotAPlainNumber) {
  MqttCtrlEnvelope e;
  EXPECT_EQ(MQTTCTRL_ERR_BAD_COUNTER, parse(envelope("v1||1788200000|trigger test"), &e));
  EXPECT_EQ(MQTTCTRL_ERR_BAD_COUNTER, parse(envelope("v1|-1|1788200000|trigger test"), &e));
  EXPECT_EQ(MQTTCTRL_ERR_BAD_COUNTER, parse(envelope("v1|+1|1788200000|trigger test"), &e));
  EXPECT_EQ(MQTTCTRL_ERR_BAD_COUNTER, parse(envelope("v1| 1|1788200000|trigger test"), &e));
  EXPECT_EQ(MQTTCTRL_ERR_BAD_COUNTER, parse(envelope("v1|0x10|1788200000|trigger test"), &e));
  // A counter that wrapped would silently reopen the replay window.
  EXPECT_EQ(MQTTCTRL_ERR_BAD_COUNTER, parse(envelope("v1|4294967296|1788200000|trigger test"), &e));
  EXPECT_EQ(MQTTCTRL_ERR_BAD_COUNTER, parse(envelope("v1|99999999999|1788200000|trigger test"), &e));
}

TEST(ParseEnvelope, SplitsAtTheLastSeparatorNotTheFirst) {
  // A command containing '|' must not be able to manufacture a second split
  // point and steer which bytes the signature is taken to cover.
  MqttCtrlEnvelope e;
  EXPECT_EQ(MQTTCTRL_ERR_COMMAND_CHARS,
            parse("v1|1|1788200000|trigger test|" + sig() + "|" + sig(), &e));
}

TEST(ParseEnvelope, RejectsControlCharactersInCommand) {
  MqttCtrlEnvelope e;
  EXPECT_EQ(MQTTCTRL_ERR_COMMAND_CHARS, parse(envelope("v1|1|1788200000|get autoreply\n"), &e));
  EXPECT_EQ(MQTTCTRL_ERR_COMMAND_CHARS, parse(envelope("v1|1|1788200000|get\tautoreply"), &e));
  EXPECT_EQ(MQTTCTRL_ERR_COMMAND_CHARS, parse(envelope("v1|1|1788200000|"), &e));

  // An embedded NUL must not truncate the command into something else.
  std::string with_nul = "v1|1|1788200000|get autoreply";
  with_nul[20] = '\0';
  EXPECT_EQ(MQTTCTRL_ERR_COMMAND_CHARS, parse(with_nul + "|" + sig(), &e));
}

// ---- the bytes the signature covers ----------------------------------------

TEST(SignedMessage, BindsTheTopicSoOneSignatureFitsOneDestination) {
  Held h("v1|1|1788200000|trigger test");
  MqttCtrlEnvelope& e = h.env;

  uint8_t a[MQTTCTRL_MAX_SIGNED], b[MQTTCTRL_MAX_SIGNED];
  size_t alen = 0, blen = 0;
  ASSERT_TRUE(mqttCtrlBuildSignedMessage("meshhealth/v1/JKG/AA04/cmd", &e, a, sizeof(a), &alen));
  ASSERT_TRUE(mqttCtrlBuildSignedMessage("meshhealth/v1/JKG/BB05/cmd", &e, b, sizeof(b), &blen));

  // Same payload, different node: the verified bytes must differ, so a command
  // captured from one node cannot be replayed at another.
  ASSERT_EQ(alen, blen);
  EXPECT_NE(0, memcmp(a, b, alen));
}

TEST(SignedMessage, PerNodeAndBroadcastAreDistinct) {
  Held h("v1|1|1788200000|trigger test");
  MqttCtrlEnvelope& e = h.env;
  uint8_t one[MQTTCTRL_MAX_SIGNED], all[MQTTCTRL_MAX_SIGNED];
  size_t l1 = 0, l2 = 0;
  ASSERT_TRUE(mqttCtrlBuildSignedMessage("meshhealth/v1/JKG/AA04/cmd", &e, one, sizeof(one), &l1));
  ASSERT_TRUE(mqttCtrlBuildSignedMessage("meshhealth/v1/JKG/all/cmd", &e, all, sizeof(all), &l2));
  EXPECT_FALSE(l1 == l2 && memcmp(one, all, l1) == 0);
}

TEST(SignedMessage, LaysOutTopicNewlineSignedPart) {
  Held h("v1|1|1788200000|trigger test");
  MqttCtrlEnvelope& e = h.env;
  uint8_t out[MQTTCTRL_MAX_SIGNED];
  size_t len = 0;
  ASSERT_TRUE(mqttCtrlBuildSignedMessage("t/x", &e, out, sizeof(out), &len));
  EXPECT_EQ(std::string("t/x\nv1|1|1788200000|trigger test"), std::string((char*)out, len));
}

TEST(SignedMessage, RefusesToOverrunOrTakeAnEmptyTopic) {
  Held h("v1|1|1788200000|trigger test");
  MqttCtrlEnvelope& e = h.env;
  uint8_t small[8];
  size_t len = 0;
  EXPECT_FALSE(mqttCtrlBuildSignedMessage("meshhealth/v1/JKG/AA04/cmd", &e, small, sizeof(small), &len));
  uint8_t out[MQTTCTRL_MAX_SIGNED];
  EXPECT_FALSE(mqttCtrlBuildSignedMessage("", &e, out, sizeof(out), &len));
  EXPECT_FALSE(mqttCtrlBuildSignedMessage(std::string(MQTTCTRL_MAX_TOPIC + 1, 'x').c_str(),
                                          &e, out, sizeof(out), &len));
}

// ---- the replay window -----------------------------------------------------

TEST(Replay, RequiresStrictlyGreaterNeverEqual) {
  EXPECT_TRUE(mqttCtrlCounterAccepted(5, 6));
  EXPECT_FALSE(mqttCtrlCounterAccepted(5, 5));   // an equal counter is a replay
  EXPECT_FALSE(mqttCtrlCounterAccepted(5, 4));
  EXPECT_FALSE(mqttCtrlCounterAccepted(5, 0));
}

TEST(Replay, AFreshNodeAcceptsTheFirstCommandButNotZero) {
  EXPECT_TRUE(mqttCtrlCounterAccepted(0, 1));
  EXPECT_FALSE(mqttCtrlCounterAccepted(0, 0));
}

TEST(Replay, SurvivesARebootOnlyIfTheCallerPersisted) {
  // The window is only as good as the stored value. A counter kept in RAM comes
  // back as 0 after a power cycle and every past command becomes replayable --
  // this test documents the requirement the firmware side has to meet.
  const uint32_t persisted = 42;
  EXPECT_FALSE(mqttCtrlCounterAccepted(persisted, 42));
  const uint32_t ram_only_after_reboot = 0;
  EXPECT_TRUE(mqttCtrlCounterAccepted(ram_only_after_reboot, 42));
}

// ---- expiry ----------------------------------------------------------------

TEST(Expiry, FailsClosedWhenTheClockHasNeverSynced) {
  EXPECT_EQ(MQTTCTRL_ERR_NO_CLOCK, mqttCtrlCheckExpiry(0, 1788200000));
}

TEST(Expiry, AcceptsInsideTheWindowAndRejectsOutside) {
  const uint32_t now = 1788200000;
  EXPECT_EQ(MQTTCTRL_OK, mqttCtrlCheckExpiry(now, now));
  EXPECT_EQ(MQTTCTRL_OK, mqttCtrlCheckExpiry(now, now + 60));
  EXPECT_EQ(MQTTCTRL_OK, mqttCtrlCheckExpiry(now, now + MQTTCTRL_MAX_FUTURE_SECS));
  EXPECT_EQ(MQTTCTRL_ERR_EXPIRED, mqttCtrlCheckExpiry(now, now - 1));
  EXPECT_EQ(MQTTCTRL_ERR_EXPIRED, mqttCtrlCheckExpiry(now, 0));
}

TEST(Expiry, RefusesACommandMintedToLastForever) {
  // Bounding the future end stops a signer minting a command that waits
  // indefinitely for a worse moment to be delivered.
  const uint32_t now = 1788200000;
  EXPECT_EQ(MQTTCTRL_ERR_FUTURE, mqttCtrlCheckExpiry(now, now + MQTTCTRL_MAX_FUTURE_SECS + 1));
  EXPECT_EQ(MQTTCTRL_ERR_FUTURE, mqttCtrlCheckExpiry(now, 0xFFFFFFFF));
}

// ---- the allowlist ---------------------------------------------------------

static bool allowed(const std::string& cmd) {
  return mqttCtrlCommandAllowed(cmd.data(), cmd.size());
}

TEST(Allowlist, AcceptsAutoReplyParametersAndTheTrigger) {
  EXPECT_TRUE(allowed("trigger test"));
  EXPECT_TRUE(allowed("set autoreply on"));
  EXPECT_TRUE(allowed("set autoreply off"));
  EXPECT_TRUE(allowed("set autoreply.hops 8"));
  EXPECT_TRUE(allowed("set autoreply.direct.flood off"));
  EXPECT_TRUE(allowed("get autoreply"));
  EXPECT_TRUE(allowed("get autoreply.channel"));
}

TEST(Allowlist, BlocksTheCommandsThatWouldOwnTheNode) {
  // The whole point of the list. Each of these is reachable from the CLI and
  // must never be reachable from a public broker.
  EXPECT_FALSE(allowed("set prv.key 00112233"));
  EXPECT_FALSE(allowed("erase"));
  EXPECT_FALSE(allowed("set mqtt.owner AABB"));
  EXPECT_FALSE(allowed("set freq 869.618"));
  EXPECT_FALSE(allowed("set password hunter2"));
  EXPECT_FALSE(allowed("start webconfig"));
  EXPECT_FALSE(allowed("set repeat off"));
  EXPECT_FALSE(allowed("reboot"));
}

TEST(Allowlist, IsNotFooledByPrefixTricks) {
  // Boundary matching: an allowed prefix must be followed by end-of-string or a
  // space, or 'set autoreply.hopsX' rides in on 'set autoreply'.
  EXPECT_FALSE(allowed("set autoreplyX 1"));
  EXPECT_FALSE(allowed("get autoreplyZZZ"));
  EXPECT_FALSE(allowed("trigger tests"));
  EXPECT_FALSE(allowed("trigger testing"));
}

TEST(Allowlist, RequiresTheCommandToStartWithTheAllowedText) {
  // A denied command must not be smuggled in by putting an allowed one in front
  // of it or behind it.
  EXPECT_FALSE(allowed(" set autoreply on"));
  EXPECT_FALSE(allowed("erase; set autoreply on"));
  EXPECT_FALSE(allowed("set prv.key 00 set autoreply on"));
}

TEST(Allowlist, MatchesCaseInsensitivelyLikeTheCli) {
  EXPECT_TRUE(allowed("SET AUTOREPLY ON"));
  EXPECT_TRUE(allowed("Trigger Test"));
  // Case folding must not open a hole either.
  EXPECT_FALSE(allowed("SET PRV.KEY 00"));
}

TEST(Allowlist, RejectsEmptyOverlongAndNonPrintable) {
  EXPECT_FALSE(allowed(""));
  EXPECT_FALSE(allowed("set autoreply " + std::string(MQTTCTRL_MAX_COMMAND, '1')));
  EXPECT_FALSE(allowed("set autoreply on\n"));
  EXPECT_FALSE(allowed(std::string("set autoreply o\0n", 17)));
}

// ---- the whole decision, in order ------------------------------------------

TEST(Authorise, AcceptsAFreshAllowedCommand) {
  Held h("v1|9|1788200060|trigger test");
  EXPECT_EQ(MQTTCTRL_OK, mqttCtrlAuthorise(&h.env, 8, 1788200000));
}

TEST(Authorise, StopsAtTheFirstRefusalInOrder) {
  // Replay is checked before expiry, and expiry before the allowlist: a replayed
  // command reports replay even when it is also expired and also forbidden, so a
  // caller publishing the reason cannot be used to probe the later checks.
  Held replayed("v1|5|1|erase");
  EXPECT_EQ(MQTTCTRL_ERR_REPLAY, mqttCtrlAuthorise(&replayed.env, 5, 1788200000));

  Held expired("v1|9|1|erase");
  EXPECT_EQ(MQTTCTRL_ERR_EXPIRED, mqttCtrlAuthorise(&expired.env, 8, 1788200000));

  Held forbidden("v1|9|1788200060|erase");
  EXPECT_EQ(MQTTCTRL_ERR_NOT_ALLOWED, mqttCtrlAuthorise(&forbidden.env, 8, 1788200000));
}

TEST(Authorise, RefusesEverythingWithoutAClock) {
  Held h("v1|9|1788200060|trigger test");
  EXPECT_EQ(MQTTCTRL_ERR_NO_CLOCK, mqttCtrlAuthorise(&h.env, 8, 0));
}

TEST(Authorise, HandlesANullEnvelope) {
  EXPECT_EQ(MQTTCTRL_ERR_EMPTY, mqttCtrlAuthorise(NULL, 0, 1788200000));
}

// ---- rate limiting ---------------------------------------------------------

TEST(RateLimit, TriggersAreClassifiedApartFromParameterChanges) {
  EXPECT_TRUE(mqttCtrlIsTransmitCommand("trigger test", 12));
  EXPECT_FALSE(mqttCtrlIsTransmitCommand("set autoreply.hops 8", 20));
  EXPECT_FALSE(mqttCtrlIsTransmitCommand("get autoreply", 13));
  // A parameter change costs a flash write; a trigger costs the whole mesh
  // airtime, so the two must never share one budget.
  EXPECT_FALSE(mqttCtrlIsTransmitCommand("triggerX", 8));
}

TEST(RateLimit, TransmitCommandsMustSatisfyBothBudgets) {
  EXPECT_TRUE(mqttCtrlRateAccepted(true, true, true));
  EXPECT_FALSE(mqttCtrlRateAccepted(true, true, false));   // trigger budget spent
  EXPECT_FALSE(mqttCtrlRateAccepted(true, false, true));   // general budget spent
}

TEST(RateLimit, ParameterChangesIgnoreTheTriggerBudget) {
  EXPECT_TRUE(mqttCtrlRateAccepted(false, true, false));
  EXPECT_FALSE(mqttCtrlRateAccepted(false, false, true));
}

TEST(RateLimit, TriggerCapIsTighterThanTheGeneralCap) {
  // The constants themselves are the defence: a leaked key bounded only by the
  // general cap could still put 20 floods an hour on a shared network.
  EXPECT_LT(MQTTCTRL_TRIGGER_MAX, MQTTCTRL_CMD_MAX);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
