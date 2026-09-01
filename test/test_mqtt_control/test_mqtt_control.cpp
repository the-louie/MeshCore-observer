// Host tests for the MQTT control-plane logic (src/helpers/MqttControlLogic.h):
// the envelope split, the bytes the signature covers, the replay window, the
// expiry window and the command allowlist. These are the exact functions
// examples/simple_repeater/MqttControl.cpp calls.
//
// The broker this rides on publishes its own credentials, so every one of these
// functions is reachable by anyone. Most of what follows is therefore hostile
// input rather than happy path: a bug here is a remote mesh-flood capability.
// The design is docs/architecture/decisions/001-mqtt-control-envelope.md in the
// workspace root, which is the directory above this repo, not a path inside it.
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include "helpers/MqttControlLogic.h"
#include "helpers/AutoReplyLogic.h"   // the trigger form is checked one layer down

extern "C" {
#include "ed25519/ed_25519.h"
}

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
  ASSERT_TRUE(mqttCtrlBuildSignedMessage("meshcore/JKG/AA04/cmd", &e, a, sizeof(a), &alen));
  ASSERT_TRUE(mqttCtrlBuildSignedMessage("meshcore/JKG/BB05/cmd", &e, b, sizeof(b), &blen));

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
  ASSERT_TRUE(mqttCtrlBuildSignedMessage("meshcore/JKG/AA04/cmd", &e, one, sizeof(one), &l1));
  ASSERT_TRUE(mqttCtrlBuildSignedMessage("meshcore/JKG/all/cmd", &e, all, sizeof(all), &l2));
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
  EXPECT_FALSE(mqttCtrlBuildSignedMessage("meshcore/JKG/AA04/cmd", &e, small, sizeof(small), &len));
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
  EXPECT_EQ(mqttCtrlRateResult(true, true, true), MQTTCTRL_OK);
  EXPECT_EQ(mqttCtrlRateResult(true, true, false), MQTTCTRL_ERR_RATE_LIMITED);  // trigger spent
  EXPECT_EQ(mqttCtrlRateResult(true, false, true), MQTTCTRL_ERR_RATE_LIMITED);  // general spent
}

TEST(RateLimit, ParameterChangesIgnoreTheTriggerBudget) {
  EXPECT_EQ(mqttCtrlRateResult(false, true, false), MQTTCTRL_OK);
  EXPECT_EQ(mqttCtrlRateResult(false, false, true), MQTTCTRL_ERR_RATE_LIMITED);
}

// ---- the two overloaded codes, now split -------------------------------------
//
// Both refusals used to borrow a code that already meant something else, so a
// caller watching a public broker could not tell a forgery from a broken
// publisher, nor "ask again later" from "never".

TEST(ErrorCodes, ExhaustedBudgetIsNotTheSameAsUnauthorised) {
  // NOT_ALLOWED means the command is not on the allowlist -- a permanent no.
  // RATE_LIMITED means the budget is spent -- a temporary one. A caller that
  // conflates them either retries forever or gives up on a command that works.
  EXPECT_NE(MQTTCTRL_ERR_RATE_LIMITED, MQTTCTRL_ERR_NOT_ALLOWED);
  EXPECT_EQ(mqttCtrlRateResult(true, false, false), MQTTCTRL_ERR_RATE_LIMITED);

  MqttCtrlEnvelope env;
  ASSERT_EQ(MQTTCTRL_OK, parse(envelope("v1|7|1788200000|set prv.key deadbeef"), &env));
  EXPECT_EQ(MQTTCTRL_ERR_NOT_ALLOWED, mqttCtrlAuthorise(&env, 6, 1788199000));
}

TEST(ErrorCodes, AForgedSignatureIsNotTheSameAsAMalformedOne) {
  // BAD_SIG_HEX is still produced at parse time for a field that is not 128 hex
  // characters; SIG_INVALID is reserved for one that parsed and did not verify.
  MqttCtrlEnvelope env;
  EXPECT_EQ(MQTTCTRL_ERR_BAD_SIG_LEN, parse("v1|7|1788200000|get txdelay|zzzz", &env));
  EXPECT_EQ(MQTTCTRL_ERR_BAD_SIG_HEX,
            parse("v1|7|1788200000|get txdelay|" + sig('z'), &env));
  EXPECT_NE(MQTTCTRL_ERR_SIG_INVALID, MQTTCTRL_ERR_BAD_SIG_HEX);
}

TEST(ErrorCodes, TheNewCodesAreAppendedAndNothingShifted) {
  EXPECT_EQ((int)MQTTCTRL_ERR_BAD_SIG_HEX, 5);
  EXPECT_EQ((int)MQTTCTRL_ERR_NOT_ALLOWED, 14);
  EXPECT_EQ((int)MQTTCTRL_ERR_NO_OWNER_KEY, 15);
  EXPECT_EQ((int)MQTTCTRL_ERR_SIG_INVALID, 16);
  EXPECT_EQ((int)MQTTCTRL_ERR_RATE_LIMITED, 17);
}

TEST(ErrorCodes, TheCodesDocumentedInMqttControlMdKeepTheirNumbers) {
  // docs/mqtt-control.md publishes these numbers for a requester to parse, and
  // a stored result keeps its meaning only while they hold. Counting them by
  // hand got the table wrong once already.
  EXPECT_EQ((int)MQTTCTRL_OK, 0);
  EXPECT_EQ((int)MQTTCTRL_ERR_REPLAY, 9);
  EXPECT_EQ((int)MQTTCTRL_ERR_EXPIRED, 10);
  EXPECT_EQ((int)MQTTCTRL_ERR_FUTURE, 11);
  EXPECT_EQ((int)MQTTCTRL_ERR_NO_CLOCK, 12);
}

TEST(RateLimit, TriggerCapIsTighterThanTheGeneralCap) {
  // The constants themselves are the defence: a leaked key bounded only by the
  // general cap could still put 20 floods an hour on a shared network.
  EXPECT_LT(MQTTCTRL_TRIGGER_MAX, MQTTCTRL_CMD_MAX);
}

// ---- the cross-check: does the Python signer produce what this accepts? ------

// Generated by panopticon/control.py with a fixed seed and a fixed clock. Two
// independent implementations of one envelope is exactly where a project like
// this goes wrong quietly: a canonicalisation difference of one byte produces a
// signature that never verifies, and the failure looks like a broken radio.
static const char* GOLDEN_PUB =
    "C9571EEB4AA9DE1159858BC6A3D4A626C4F4845E8EEBD5F554B2EC0F50C68860";
static const char* GOLDEN_TOPIC = "meshcore/JKG/AA04792D/cmd";
static const char* GOLDEN_PAYLOAD =
    "v1|42|1788000600|set autoreply.hops 5|"
    "D6E615B85660933377ACE10C946A42DFABFFC5CF9736CA6D6FEF25B863170241"
    "A4FFA470C77D30CFD1D47F37D1E3DB380287AE108E243C452D83993B619DB408";

static void hexToBytes(const char* hex, uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; i++) out[i] = (uint8_t)mqttCtrlHexByte(hex[i * 2], hex[i * 2 + 1]);
}

TEST(GoldenEnvelope, PythonSignerVerifiesAgainstTheRealEd25519) {
  MqttCtrlEnvelope env;
  ASSERT_EQ(MQTTCTRL_OK,
            mqttCtrlParseEnvelope(GOLDEN_PAYLOAD, strlen(GOLDEN_PAYLOAD), &env));
  EXPECT_EQ(42u, env.counter);
  EXPECT_EQ(1788000600u, env.expiry);
  EXPECT_EQ(std::string("set autoreply.hops 5"), std::string(env.command, env.command_len));

  uint8_t signed_bytes[MQTTCTRL_MAX_SIGNED];
  size_t signed_len = 0;
  ASSERT_TRUE(mqttCtrlBuildSignedMessage(GOLDEN_TOPIC, &env, signed_bytes,
                                         sizeof(signed_bytes), &signed_len));

  uint8_t pub[32];
  hexToBytes(GOLDEN_PUB, pub, sizeof(pub));
  EXPECT_EQ(1, ed25519_verify(env.signature, signed_bytes, signed_len, pub));
}

TEST(GoldenEnvelope, TheSameSignatureIsRefusedAtAnotherNode) {
  MqttCtrlEnvelope env;
  ASSERT_EQ(MQTTCTRL_OK,
            mqttCtrlParseEnvelope(GOLDEN_PAYLOAD, strlen(GOLDEN_PAYLOAD), &env));
  uint8_t signed_bytes[MQTTCTRL_MAX_SIGNED];
  size_t signed_len = 0;
  ASSERT_TRUE(mqttCtrlBuildSignedMessage("meshcore/JKG/BB05DEAD/cmd", &env,
                                         signed_bytes, sizeof(signed_bytes), &signed_len));
  uint8_t pub[32];
  hexToBytes(GOLDEN_PUB, pub, sizeof(pub));
  EXPECT_EQ(0, ed25519_verify(env.signature, signed_bytes, signed_len, pub));
}

TEST(GoldenEnvelope, ATamperedCommandIsRefused) {
  std::string tampered(GOLDEN_PAYLOAD);
  tampered.replace(tampered.find("hops 5"), 6, "hops 1");
  MqttCtrlEnvelope env;
  ASSERT_EQ(MQTTCTRL_OK, mqttCtrlParseEnvelope(tampered.data(), tampered.size(), &env));
  uint8_t signed_bytes[MQTTCTRL_MAX_SIGNED];
  size_t signed_len = 0;
  ASSERT_TRUE(mqttCtrlBuildSignedMessage(GOLDEN_TOPIC, &env, signed_bytes,
                                         sizeof(signed_bytes), &signed_len));
  uint8_t pub[32];
  hexToBytes(GOLDEN_PUB, pub, sizeof(pub));
  EXPECT_EQ(0, ed25519_verify(env.signature, signed_bytes, signed_len, pub));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}


// ---- the owner key, and the staging slot it used to wedge --------------------
//
// Without a key there is nothing to verify against, so a staged command cannot
// run. It must still be resolved: the slot holds exactly one command and clears
// only when that command is disposed of. The firmware used to return early here,
// latching the slot for the life of the boot and dropping every command after it
// -- including the ones sent once a key had finally been provisioned.

TEST(OwnerReady, AValidKeyIsReady) {
  const char* key = "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef";
  EXPECT_EQ(strlen(key), 64u);
  EXPECT_EQ(mqttCtrlOwnerReady(key), MQTTCTRL_OK);
}

TEST(OwnerReady, AnUnsetKeyIsRefusedRatherThanIgnored) {
  EXPECT_EQ(mqttCtrlOwnerReady(""), MQTTCTRL_ERR_NO_OWNER_KEY);
  EXPECT_EQ(mqttCtrlOwnerReady(NULL), MQTTCTRL_ERR_NO_OWNER_KEY);
}

TEST(OwnerReady, AMalformedKeyIsTheSameCaseAsNoKey) {
  // fromHex would refuse these a moment later; refusing here keeps one rule in
  // one place, and both paths must resolve the slot.
  EXPECT_EQ(mqttCtrlOwnerReady("abc"), MQTTCTRL_ERR_NO_OWNER_KEY);
  EXPECT_EQ(mqttCtrlOwnerReady("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde"),
            MQTTCTRL_ERR_NO_OWNER_KEY);   // 63
  EXPECT_EQ(mqttCtrlOwnerReady("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0"),
            MQTTCTRL_ERR_NO_OWNER_KEY);   // 65
  EXPECT_EQ(mqttCtrlOwnerReady("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg"),
            MQTTCTRL_ERR_NO_OWNER_KEY);   // non-hex
}

TEST(OwnerReady, TheRefusalHasItsOwnCodeAndDoesNotCollide) {
  // A caller publishing the refusal must be able to say why. Reusing an existing
  // code would make "no key" indistinguishable from a malformed payload.
  EXPECT_NE(MQTTCTRL_ERR_NO_OWNER_KEY, MQTTCTRL_OK);
  EXPECT_NE(MQTTCTRL_ERR_NO_OWNER_KEY, MQTTCTRL_ERR_EMPTY);
  EXPECT_NE(MQTTCTRL_ERR_NO_OWNER_KEY, MQTTCTRL_ERR_NOT_ALLOWED);
  EXPECT_NE(MQTTCTRL_ERR_NO_OWNER_KEY, MQTTCTRL_ERR_BAD_SIG_HEX);
}

TEST(OwnerReady, ExistingCodesKeepTheirNumbers) {
  // panopticon/control.py and stored results read these as numbers, so the new
  // code is appended and nothing before it may shift.
  EXPECT_EQ((int)MQTTCTRL_OK, 0);
  EXPECT_EQ((int)MQTTCTRL_ERR_EMPTY, 1);
  EXPECT_EQ((int)MQTTCTRL_ERR_NOT_ALLOWED, 14);
  EXPECT_EQ((int)MQTTCTRL_ERR_NO_OWNER_KEY, 15);
}

// ---- the published result -----------------------------------------------------
//
// The topic names the node; the counter names the question. Without it a `get`
// answer is "> 0.5" -- true of whatever was asked, which is not enough to act on
// when one requester is polling several nodes.

TEST(FormatResult, CarriesCounterCodeCommandAndReply) {
  char out[MQTTCTRL_MAX_RESULT];
  size_t n = mqttCtrlFormatResult(out, sizeof(out), 42, 0, "get txdelay", "> 0.5");
  EXPECT_EQ(std::string("r1|42|0|get txdelay|> 0.5"), std::string(out));
  EXPECT_EQ(n, strlen(out));
}

TEST(FormatResult, ARefusalStillNamesItsRequest) {
  // No command ran, so the reply is empty -- but the counter and the code are
  // what make a refusal distinguishable from a node that never answered.
  char out[MQTTCTRL_MAX_RESULT];
  mqttCtrlFormatResult(out, sizeof(out), 9, (int)MQTTCTRL_ERR_RATE_LIMITED,
                       "trigger test", "");
  EXPECT_EQ(std::string("r1|9|17|trigger test|"), std::string(out));
}

TEST(FormatResult, TruncatesRatherThanDiscarding) {
  // The existing payload builders drop a payload over budget rather than
  // shortening it, which would mean the longest replies never arrive. Here the
  // reply is last and every earlier field is bounded, so a clamp leaves a line
  // that still parses.
  std::string huge(400, 'x');
  char out[64];
  size_t n = mqttCtrlFormatResult(out, sizeof(out), 7, 0, "get radio", huge.c_str());
  EXPECT_EQ(n, sizeof(out) - 1);
  EXPECT_EQ(strlen(out), sizeof(out) - 1);
  EXPECT_EQ(std::string("r1|7|0|get radio|"), std::string(out).substr(0, 17));
}

TEST(FormatResult, TheFieldsBeforeTheReplyCannotBeAmbiguous) {
  // mqttCtrlCommandCharsOk refuses a command containing the separator, so only
  // the trailing reply can hold one and a parser takes the rest of the line.
  EXPECT_FALSE(mqttCtrlCommandCharsOk("get a|b", 7));

  char out[MQTTCTRL_MAX_RESULT];
  mqttCtrlFormatResult(out, sizeof(out), 1, 0, "get radio", "a|b|c");
  std::string s(out);
  EXPECT_EQ(std::string("r1|1|0|get radio|a|b|c"), s);

  // Split on the first four separators; whatever follows is the reply, however
  // many separators it happens to contain.
  size_t pos = 0;
  for (int i = 0; i < 4; i++) pos = s.find('|', pos) + 1;
  EXPECT_EQ(std::string("a|b|c"), s.substr(pos));
  EXPECT_EQ(std::string("get radio"), s.substr(7, pos - 8));
}

TEST(FormatResult, RefusesABufferItCannotUse) {
  char out[8];
  EXPECT_EQ(0u, mqttCtrlFormatResult(NULL, sizeof(out), 1, 0, "x", "y"));
  EXPECT_EQ(0u, mqttCtrlFormatResult(out, 0, 1, 0, "x", "y"));
}

TEST(FormatResult, ANullCommandOrReplyIsAnEmptyField) {
  char out[MQTTCTRL_MAX_RESULT];
  mqttCtrlFormatResult(out, sizeof(out), 3, 0, NULL, NULL);
  EXPECT_EQ(std::string("r1|3|0||"), std::string(out));
}

TEST(FormatResult, TheBufferFitsTheLongestPossibleLine) {
  std::string cmd(MQTTCTRL_MAX_COMMAND, 'c');
  std::string rep(MQTTCTRL_MAX_REPLY - 1, 'r');
  char out[MQTTCTRL_MAX_RESULT];
  size_t n = mqttCtrlFormatResult(out, sizeof(out), 4294967295u, 17,
                                  cmd.c_str(), rep.c_str());
  EXPECT_LT(n, sizeof(out) - 1) << "MQTTCTRL_MAX_RESULT must not clamp a legal line";
}

// ---- guard rails on the trigger -----------------------------------------------

TEST(BroadcastGuard, RecognisesTheBroadcastTopic) {
  EXPECT_TRUE(mqttCtrlTopicIsBroadcast("meshcore/JKG/all/cmd"));
  EXPECT_FALSE(mqttCtrlTopicIsBroadcast("meshcore/JKG/a1b2c3d4/cmd"));
  EXPECT_FALSE(mqttCtrlTopicIsBroadcast("meshcore/JKG/all/res"));
  EXPECT_FALSE(mqttCtrlTopicIsBroadcast("all/cmd"));   // no region segment
  EXPECT_FALSE(mqttCtrlTopicIsBroadcast(NULL));
}

TEST(BroadcastGuard, ATransmitCommandMayNotBeAddressedToEveryNode) {
  // The rate limits are per node -- four triggers an hour each. That bounds one
  // node and says nothing about one publish reaching every node in a region, each
  // then transmitting inside the same few seconds and each within budget. The
  // workspace rule is that no design may scale transmissions with the node count.
  EXPECT_EQ(MQTTCTRL_ERR_BROADCAST_TRANSMIT,
            mqttCtrlTransmitTopicResult(true, "meshcore/JKG/all/cmd"));
  EXPECT_EQ(MQTTCTRL_OK, mqttCtrlTransmitTopicResult(true, "meshcore/JKG/a1b2c3d4/cmd"));
}

TEST(BroadcastGuard, ABroadcastParameterChangeIsStillAllowed) {
  // Setting a value costs no airtime, so fleet-wide configuration stays possible.
  // Only transmitting is refused.
  EXPECT_EQ(MQTTCTRL_OK, mqttCtrlTransmitTopicResult(false, "meshcore/JKG/all/cmd"));
  EXPECT_FALSE(mqttCtrlIsTransmitCommand("set autoreply.delay 8", 21));
  EXPECT_TRUE(mqttCtrlIsTransmitCommand("trigger test", 12));
}

TEST(BroadcastGuard, TheRefusalHasItsOwnAppendedCode) {
  EXPECT_EQ((int)MQTTCTRL_ERR_BROADCAST_TRANSMIT, 18);
  EXPECT_NE(MQTTCTRL_ERR_BROADCAST_TRANSMIT, MQTTCTRL_ERR_NOT_ALLOWED);
  EXPECT_NE(MQTTCTRL_ERR_BROADCAST_TRANSMIT, MQTTCTRL_ERR_RATE_LIMITED);
}

TEST(BroadcastGuard, ARefusalIsPublishableRatherThanSilent) {
  // It is refused after the signature verified, so T-05 publishes it: a requester
  // learns the node exists and declined, which is not what silence would say.
  char out[MQTTCTRL_MAX_RESULT];
  size_t n = mqttCtrlFormatResult(out, sizeof(out), 5,
                                  (int)MQTTCTRL_ERR_BROADCAST_TRANSMIT, "trigger test", "");
  EXPECT_GT(n, 0u);
  EXPECT_EQ(std::string("r1|5|18|trigger test|"), std::string(out));
}

// ---- the read-only additions to the allowlist ---------------------------------

TEST(Allowlist, AdmitsTheReadOnlyParameterGets) {
  EXPECT_TRUE(allowed("get txdelay"));
  EXPECT_TRUE(allowed("get direct.txdelay"));
  EXPECT_TRUE(allowed("get rxdelay"));
  EXPECT_TRUE(allowed("get af"));
  EXPECT_TRUE(allowed("get cad"));
  EXPECT_TRUE(allowed("get int.thresh"));
  EXPECT_TRUE(allowed("get radio"));
}

TEST(Allowlist, AReadDoesNotAdmitTheMatchingWrite) {
  // The whole argument for widening is that a read changes nothing. If any of
  // these let a `set` through, that argument is gone.
  EXPECT_FALSE(allowed("set txdelay 0.5"));
  EXPECT_FALSE(allowed("set direct.txdelay 0.3"));
  EXPECT_FALSE(allowed("set rxdelay 0"));
  EXPECT_FALSE(allowed("set af 9.0"));
  EXPECT_FALSE(allowed("set cad on"));
  EXPECT_FALSE(allowed("set int.thresh 1"));
  EXPECT_FALSE(allowed("set radio 869.618,62.5,8,8"));
}

TEST(Allowlist, StaysTighterThanTheCliItGuards) {
  // CommonCLI's handleGetCmd matches with an unbounded memcmp, so `get afxyz`
  // would answer if it ever reached there. The allowlist requires end-of-string,
  // a space or a dot, and is the tighter gate -- keep it that way rather than
  // relaxing the boundary to save entries.
  EXPECT_FALSE(allowed("get afxyz"));
  EXPECT_FALSE(allowed("get radiofoo"));
  EXPECT_FALSE(allowed("get txdelayx"));
  EXPECT_FALSE(allowed("get int.threshold"));
}

TEST(Allowlist, TheWideningAddedNoPrivilegedCommand) {
  // Re-pinned after the widening: these must still be refused, and the reads
  // must not have opened a path to them.
  EXPECT_FALSE(allowed("set prv.key 00112233"));
  EXPECT_FALSE(allowed("erase"));
  EXPECT_FALSE(allowed("set mqtt.owner AABB"));
  EXPECT_FALSE(allowed("get prv.key"));
  EXPECT_FALSE(allowed("get mqtt.owner"));
  EXPECT_FALSE(allowed("get"));
  EXPECT_FALSE(allowed("get "));
}

// ---- the boundary rule itself -------------------------------------------------
//
// Tested directly rather than only through the allowlist, because
// docs/mqtt-control.md now names it as the reason the allowlist is the tighter of
// the two gates: CommonCLI's own handleGetCmd matches with an unbounded memcmp.

TEST(PrefixBoundary, AcceptsAnExactMatch) {
  EXPECT_TRUE(mqttCtrlPrefixWithBoundary("get af", 6, "get af"));
}

TEST(PrefixBoundary, AcceptsASpaceOrDotAfterThePrefix) {
  EXPECT_TRUE(mqttCtrlPrefixWithBoundary("get af 1", 8, "get af"));
  EXPECT_TRUE(mqttCtrlPrefixWithBoundary("set autoreply.hops 8", 20, "set autoreply"));
  EXPECT_TRUE(mqttCtrlPrefixWithBoundary("set autoreply on", 16, "set autoreply"));
}

TEST(PrefixBoundary, RefusesAPrefixThatRunsIntoAnotherWord) {
  // The whole point: `get afxyz` must not ride in on `get af`.
  EXPECT_FALSE(mqttCtrlPrefixWithBoundary("get afxyz", 9, "get af"));
  EXPECT_FALSE(mqttCtrlPrefixWithBoundary("set autoreplyx", 14, "set autoreply"));
  EXPECT_FALSE(mqttCtrlPrefixWithBoundary("triggerx", 8, "trigger"));
}

TEST(PrefixBoundary, RefusesSomethingShorterThanThePrefix) {
  EXPECT_FALSE(mqttCtrlPrefixWithBoundary("get a", 5, "get af"));
  EXPECT_FALSE(mqttCtrlPrefixWithBoundary("", 0, "get af"));
}

TEST(PrefixBoundary, IsCaseInsensitive) {
  EXPECT_TRUE(mqttCtrlPrefixWithBoundary("GET AF", 6, "get af"));
  EXPECT_TRUE(mqttCtrlPrefixWithBoundary("Trigger Test", 12, "trigger"));
}

TEST(PrefixBoundary, UsesTheGivenLengthNotTheTerminator) {
  // The command is a byte range inside the envelope, not a C string, so a prefix
  // must not be judged by what happens to follow the range in memory.
  EXPECT_TRUE(mqttCtrlPrefixWithBoundary("get afxyz", 6, "get af"));
}

TEST(PrefixBoundary, ShorterThanThePrefixNeverReadsPastTheCommand) {
  // The command points into the received payload and is NOT terminated: the byte
  // after it is the next field. Without the length guard, comparing the full
  // prefix walks past the command's own end into whatever followed it on the
  // wire -- and if those bytes happen to complete the prefix, a five-byte
  // command matches a six-byte allowlist entry.
  //
  // Written as an explicit range because a C string literal cannot show this:
  // the terminator hides the bug that the guard exists to prevent.
  const char payload[] = "get af get autoreply";
  EXPECT_FALSE(mqttCtrlPrefixWithBoundary(payload, 5, "get af")) << "matched past the command";
  EXPECT_TRUE(mqttCtrlPrefixWithBoundary(payload, 6, "get af"));
}

// ---- the exact command T-12 sends ---------------------------------------------
//
// The golden fixture above covers `set autoreply.hops 5`, which existed before this
// sprint. T-12 reads a value back with `get txdelay` -- a command that only became
// sendable when T-09 widened the allowlist -- so it gets its own fixture, produced
// by panopticon/control.py and verified here by the firmware's own Ed25519.
//
// Proving this offline removes the whole class of on-hardware surprises where a
// signature never verifies and the node simply stays silent, which is
// indistinguishable from a node that was never listening.

static const char* READ_PUB =
    "6B734A8EFF246FE734B38D4046C148EEE5F04FE87B3A0A423955A77956DE066B";
static const char* READ_TOPIC = "meshcore/JKG/AA04792D/cmd";
static const char* READ_PAYLOAD =
    "v1|43|1788000600|get txdelay|"
    "2C22F81168EE96B8598B70B29B951DF365876413AC6C1B6CE1C14CB3E7D3E2BD"
    "563F7F0905EE12EE5E41273B3CA515E50504CFD96D57D16A12DBD07D0BCDFD04";

TEST(GoldenRead, TheCommandT12SendsVerifiesAndIsAllowed) {
  MqttCtrlEnvelope env;
  ASSERT_EQ(MQTTCTRL_OK,
            mqttCtrlParseEnvelope(READ_PAYLOAD, strlen(READ_PAYLOAD), &env));
  EXPECT_EQ(43u, env.counter);
  EXPECT_EQ(std::string("get txdelay"), std::string(env.command, env.command_len));

  uint8_t signed_bytes[MQTTCTRL_MAX_SIGNED];
  size_t signed_len = 0;
  ASSERT_TRUE(mqttCtrlBuildSignedMessage(READ_TOPIC, &env, signed_bytes,
                                         sizeof(signed_bytes), &signed_len));
  uint8_t pub[32];
  hexToBytes(READ_PUB, pub, sizeof(pub));
  EXPECT_EQ(1, ed25519_verify(env.signature, signed_bytes, signed_len, pub));

  // Verifying is not enough -- it must also survive authorisation, which is where
  // a command absent from the allowlist would be refused.
  EXPECT_EQ(MQTTCTRL_OK, mqttCtrlAuthorise(&env, 42, 1788000100));

  // And it must not be treated as a transmit, or it would spend a trigger slot
  // and be refused on the broadcast topic.
  EXPECT_FALSE(mqttCtrlIsTransmitCommand(env.command, env.command_len));
  EXPECT_EQ(MQTTCTRL_OK, mqttCtrlTransmitTopicResult(false, READ_TOPIC));
}

TEST(GoldenRead, TheSameReadIsRefusedAtAnotherNode) {
  // The topic is inside the signature, so a read captured off the public broker
  // cannot be replayed at a different node.
  MqttCtrlEnvelope env;
  ASSERT_EQ(MQTTCTRL_OK,
            mqttCtrlParseEnvelope(READ_PAYLOAD, strlen(READ_PAYLOAD), &env));

  uint8_t signed_bytes[MQTTCTRL_MAX_SIGNED];
  size_t signed_len = 0;
  ASSERT_TRUE(mqttCtrlBuildSignedMessage("meshcore/JKG/BB05C3D1/cmd", &env,
                                         signed_bytes, sizeof(signed_bytes), &signed_len));
  uint8_t pub[32];
  hexToBytes(READ_PUB, pub, sizeof(pub));
  EXPECT_EQ(0, ed25519_verify(env.signature, signed_bytes, signed_len, pub));
}

// ---- the private key is not reachable over MQTT -------------------------------
//
// Operator requirement, 2026-09-01: the node's identity key must be neither
// readable nor writable over MQTT. Two independent guards enforce it, and each is
// pinned separately here -- the point of having two is that either alone suffices,
// so a test that only proves their conjunction would not notice one of them dying.

TEST(PrivateKey, GuardOne_AnMqttCommandNeverCarriesTheLocalPrivilegeMarker) {
  // sender_timestamp 0 is not a clock reading, it is "the caller is physically
  // present". It gates get/set prv.key, set mqtt.owner and erase in CommonCLI.
  // If an MQTT command could execute with 0, every one of those gates would open.
  EXPECT_NE(0u, mqttCtrlSenderStamp(0));

  // The case that matters: a node whose clock has never synced reports 0, so
  // passing `now` through unchanged would hand a remote caller full local
  // privilege exactly when NTP is down -- an outage becoming an escalation.
  EXPECT_EQ(1u, mqttCtrlSenderStamp(0));

  // Every real reading is passed through untouched.
  EXPECT_EQ(1u, mqttCtrlSenderStamp(1));
  EXPECT_EQ(1788000600u, mqttCtrlSenderStamp(1788000600));
  EXPECT_EQ(0xFFFFFFFFu, mqttCtrlSenderStamp(0xFFFFFFFFu));
}

TEST(PrivateKey, GuardTwo_NeitherReadingNorWritingItIsOnTheAllowlist) {
  EXPECT_FALSE(allowed("get prv.key"));
  EXPECT_FALSE(allowed("set prv.key 00112233"));

  // And not by any spelling the boundary rule might let through.
  EXPECT_FALSE(allowed("GET PRV.KEY"));
  EXPECT_FALSE(allowed("SET PRV.KEY 00112233"));
  EXPECT_FALSE(allowed("get prv"));
  EXPECT_FALSE(allowed("set prv.key"));
  EXPECT_FALSE(allowed("get prv.keyx"));

  // Nor smuggled in behind a command that is allowed.
  EXPECT_FALSE(allowed("get autoreply; get prv.key"));
}

TEST(PrivateKey, ATrailingKeyCommandRidesThroughTheAllowlistAndDiesAtTheHandler) {
  // Worth stating exactly, because the obvious guess is wrong and I guessed it:
  // the allowlist admits `trigger test set prv.key 00`. Entries match as prefixes
  // with a boundary, so anything beginning "trigger test " is admitted -- the same
  // way "set autoreply" admits "set autoreply.hops 8". The allowlist decides which
  // *family* of command may arrive, not whether one is well formed.
  EXPECT_TRUE(allowed("trigger test set prv.key 00"));

  // The form is checked one layer down, and that is what refuses it: the trigger
  // parser accepts a bare keyword or a keyword and eight hex characters, nothing
  // else. So the string reaches no handler that would act on the tail.
  const char* id = NULL; size_t id_len = 0;
  EXPECT_FALSE(autoReplyParseTrigger("trigger test set prv.key 00", "test", &id, &id_len));

  // It is not a `set` to any CLI either -- it begins "trigger", so CommonCLI's
  // handleSetCmd is never reached and the whole string falls through as unknown.
  EXPECT_NE(0, memcmp("trigger test set prv.key 00", "set ", 4));

  // Residual, recorded rather than fixed: it is classed as a transmit and so
  // spends a trigger token before being refused. Not an escalation -- sending it
  // at all requires the owner key, and whoever holds that can trigger legitimately
  // -- but it is why the allowlist is not the only thing standing here.
  EXPECT_TRUE(mqttCtrlIsTransmitCommand("trigger test set prv.key 00", 26));
}

TEST(PrivateKey, ASignedRequestForItIsRefusedAtAuthorisation) {
  // End of the chain: even a perfectly signed, fresh, unexpired envelope asking
  // for the key is refused -- and refused as NOT_ALLOWED, which is permanent,
  // rather than as anything a requester should retry.
  MqttCtrlEnvelope env;
  ASSERT_EQ(MQTTCTRL_OK, parse(envelope("v1|9|1788200000|get prv.key"), &env));
  EXPECT_EQ(MQTTCTRL_ERR_NOT_ALLOWED, mqttCtrlAuthorise(&env, 8, 1788199000));

  ASSERT_EQ(MQTTCTRL_OK, parse(envelope("v1|9|1788200000|set prv.key deadbeef"), &env));
  EXPECT_EQ(MQTTCTRL_ERR_NOT_ALLOWED, mqttCtrlAuthorise(&env, 8, 1788199000));
}

// ---- exact entries admit the command they name, and nothing else --------------
//
// Operator review, 2026-09-01: the allowlist is read to decide what a node exposes,
// so an entry admitting more than it says defeats the reading. `get radio` was
// admitting two children and a junk tail through the boundary rule.

TEST(ExactEntries, AReadAdmitsOnlyTheBareCommand) {
  EXPECT_TRUE(allowed("get radio"));
  EXPECT_TRUE(allowed("get af"));
  EXPECT_TRUE(allowed("get int.thresh"));       // the dot is in the name, not a boundary
  EXPECT_TRUE(allowed("get direct.txdelay"));
}

TEST(ExactEntries, ChildrenOfAReadAreNoLongerAdmitted) {
  // These exist in CommonCLI and were reachable over MQTT purely because the
  // boundary rule treats '.' as a separator.
  EXPECT_FALSE(allowed("get radio.rxgain"));
  EXPECT_FALSE(allowed("get radio.fem.rxgain"));

  // And one that does not exist yet: adding `get af.thing` to the CLI later must
  // not publish it to the mesh as a side effect.
  EXPECT_FALSE(allowed("get af.anything"));
  EXPECT_FALSE(allowed("get txdelay.x"));
}

TEST(ExactEntries, ATrailingArgumentOnAReadIsRefused) {
  // A get takes no argument. The CLI would ignore the tail and answer anyway, so
  // refusing here keeps the allowlist the tighter gate.
  EXPECT_FALSE(allowed("get radio foo"));
  EXPECT_FALSE(allowed("get af 1"));
  EXPECT_FALSE(allowed("get txdelay 0.5"));
}

TEST(ExactEntries, TheFamiliesStillMatchAsPrefixes) {
  // The boundary rule is load-bearing on these three; making them exact would
  // break the sprint's own features.
  EXPECT_TRUE(allowed("set autoreply on"));
  EXPECT_TRUE(allowed("set autoreply.hops 8"));
  EXPECT_TRUE(allowed("set autoreply.delay 12"));
  EXPECT_TRUE(allowed("set autoreply.direct.flood off"));
  EXPECT_TRUE(allowed("set autoreply.region JKG"));
  EXPECT_TRUE(allowed("get autoreply"));
  EXPECT_TRUE(allowed("get autoreply.channel"));
  EXPECT_TRUE(allowed("trigger test"));
  EXPECT_TRUE(allowed("trigger test a1b2c3d4"));
}

TEST(ExactEntries, ExactMatchingIsStillCaseInsensitive) {
  EXPECT_TRUE(allowed("GET RADIO"));
  EXPECT_TRUE(allowed("Get Af"));
}

TEST(ExactEntries, ExactMatchingUsesTheGivenLengthNotATerminator) {
  // Same trap as the boundary rule: the command is a byte range in the payload,
  // not a C string, so a short command must not match by reading past its end.
  const char payload[] = "get radio.rxgain";
  EXPECT_FALSE(mqttCtrlExactCommand(payload, 5, "get radio"));   // "get r"
  EXPECT_TRUE(mqttCtrlExactCommand(payload, 9, "get radio"));    // "get radio"
  EXPECT_FALSE(mqttCtrlExactCommand(payload, 16, "get radio"));  // the whole thing
}

// ---- reads that describe how a node floods -----------------------------------
//
// Added after a bench node was set `repeat off` and there was no way to confirm
// it remotely. Whether a node forwards is not a detail: the topology model infers
// it from traffic, and that inference once counted every phone and companion as a
// relay.

TEST(FloodReads, TheFloodBehaviourGetsAreAdmitted) {
  EXPECT_TRUE(allowed("get repeat"));
  EXPECT_TRUE(allowed("get flood.max"));
  EXPECT_TRUE(allowed("get flood.max.unscoped"));
}

TEST(FloodReads, ExactMatchingKeepsTheTwoFloodEntriesApart) {
  // `get flood.max` is exact, so it does not swallow `.unscoped` -- which is why
  // that one needs its own entry rather than riding in on a prefix.
  EXPECT_FALSE(allowed("get flood.max.anything"));
  EXPECT_FALSE(allowed("get repeater"));
  EXPECT_FALSE(allowed("get repeat on"));
}

TEST(FloodReads, TheMatchingWritesAreStillRefused) {
  // `set repeat off` silences a node's forwarding. Doing that remotely to someone
  // else's repeater would take it out of the mesh, so it stays off the allowlist.
  EXPECT_FALSE(allowed("set repeat off"));
  EXPECT_FALSE(allowed("set flood.max 3"));
  EXPECT_FALSE(allowed("set flood.max.unscoped 2"));
}
