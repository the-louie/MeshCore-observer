// Host tests for the repeater auto-reply logic (src/helpers/AutoReplyLogic.h): the
// '#test-<iata>' channel derived from the observer region code, the trigger parsing
// of an incoming group text, and the per-sender cooldown ring. These are the exact
// functions examples/simple_repeater/AutoReply.cpp calls.
#include <gtest/gtest.h>
#include <string>
#include "helpers/AutoReplyLogic.h"

// The firmware's buffer for a channel name (AUTOREPLY_MAX_CHANNEL).
static const size_t CHANNEL_BUF = 16;

// autoReplyParseRequest() trims in place, so tests need a mutable copy.
static void copyText(char* dest, size_t size, const char* src) {
  snprintf(dest, size, "%s", src);
}

// ---- the channel derived from the region code -----------------------------

TEST(DeriveChannel, BuildsTestDashRegion) {
  char out[CHANNEL_BUF];
  EXPECT_TRUE(autoReplyDeriveChannel("sto", out, sizeof(out)));
  EXPECT_STREQ("#test-sto", out);
}

TEST(DeriveChannel, FoldsToLowerCase) {
  // The CLI stores the region upper-cased, but clients only accept lower-case
  // channel names and the key is the hash of the name exactly as stored, so an
  // upper-case name would hash to a channel nobody could join.
  char out[CHANNEL_BUF];
  EXPECT_TRUE(autoReplyDeriveChannel("STO", out, sizeof(out)));
  EXPECT_STREQ("#test-sto", out);
  EXPECT_TRUE(autoReplyDeriveChannel("Lax", out, sizeof(out)));
  EXPECT_STREQ("#test-lax", out);
}

TEST(DeriveChannel, AcceptsAlphanumericRegions) {
  char out[CHANNEL_BUF];
  EXPECT_TRUE(autoReplyDeriveChannel("D3N", out, sizeof(out)));
  EXPECT_STREQ("#test-d3n", out);
}

TEST(DeriveChannel, RejectsAnUnsetRegion) {
  // Without a region there is no per-region channel, so the node stays silent.
  char out[CHANNEL_BUF];
  EXPECT_FALSE(autoReplyDeriveChannel("", out, sizeof(out)));
  EXPECT_STREQ("", out);
  EXPECT_FALSE(autoReplyDeriveChannel(nullptr, out, sizeof(out)));
  EXPECT_STREQ("", out);
}

TEST(DeriveChannel, RejectsThePlaceholderRegion) {
  // 'XXX' is what MQTTTopicRouter also treats as unconfigured.
  char out[CHANNEL_BUF];
  EXPECT_FALSE(autoReplyDeriveChannel("XXX", out, sizeof(out)));
  EXPECT_FALSE(autoReplyDeriveChannel("xxx", out, sizeof(out)));
  EXPECT_STREQ("", out);
}

TEST(DeriveChannel, RejectsMalformedRegions) {
  // Same rule as the 'set mqtt.iata' setter: exactly three alphanumerics.
  char out[CHANNEL_BUF];
  EXPECT_FALSE(autoReplyDeriveChannel("ST", out, sizeof(out)));
  EXPECT_FALSE(autoReplyDeriveChannel("STOC", out, sizeof(out)));
  EXPECT_FALSE(autoReplyDeriveChannel("S O", out, sizeof(out)));
  EXPECT_FALSE(autoReplyDeriveChannel("S/O", out, sizeof(out)));
  EXPECT_FALSE(autoReplyDeriveChannel("S#O", out, sizeof(out)));
}

TEST(DeriveChannel, CanNeverProduceASharedChannel) {
  // The mesh-wide channels a reply must never go to. The prefix plus a three-char
  // region cannot collide with any of them, which is why the name is derived.
  const char* shared[] = { "#public", "#test", "#bot" };
  char out[CHANNEL_BUF];
  for (int a = 0; a < 36; a++) {
    for (int b = 0; b < 36; b++) {
      for (int c = 0; c < 36; c++) {
        const char* digits = "abcdefghijklmnopqrstuvwxyz0123456789";
        char iata[4] = { digits[a], digits[b], digits[c], 0 };
        if (!autoReplyDeriveChannel(iata, out, sizeof(out))) continue;
        for (const char* s : shared) EXPECT_STRNE(s, out);
      }
    }
  }
}

TEST(DeriveChannel, RejectsABufferItCannotFill) {
  char small[sizeof(AUTOREPLY_CHANNEL_PREFIX) + 2];   // prefix + NUL + 2 of the 3 chars
  EXPECT_FALSE(autoReplyDeriveChannel("sto", small, sizeof(small)));
  EXPECT_STREQ("", small);

  char exact[sizeof(AUTOREPLY_CHANNEL_PREFIX) + 3];   // prefix + NUL + all 3
  EXPECT_TRUE(autoReplyDeriveChannel("sto", exact, sizeof(exact)));
  EXPECT_STREQ("#test-sto", exact);
}

TEST(DeriveChannel, RejectsNullOrZeroBuffer) {
  char out[CHANNEL_BUF];
  EXPECT_FALSE(autoReplyDeriveChannel("sto", nullptr, sizeof(out)));
  EXPECT_FALSE(autoReplyDeriveChannel("sto", out, 0));
}

// ---- request parsing: "<sender>: <message>" and the trigger ---------------

TEST(ParseRequest, SplitsSenderFromMessage) {
  char text[128];
  copyText(text, sizeof(text), "Louie: test");
  AutoReplyRequest r = autoReplyParseRequest(text, "test");
  EXPECT_TRUE(r.is_trigger);
  EXPECT_EQ(5u, r.sender_len);
  EXPECT_EQ(0, strncmp(r.sender, "Louie", 5));
  EXPECT_STREQ("test", r.message);
}

TEST(ParseRequest, KeywordIsCaseInsensitive) {
  const char* spellings[] = { "test", "Test", "TEST", "tEsT" };
  for (const char* s : spellings) {
    char text[128];
    copyText(text, sizeof(text), (std::string("Louie: ") + s).c_str());
    EXPECT_TRUE(autoReplyParseRequest(text, "test").is_trigger) << s;
  }
}

TEST(ParseRequest, TrimsSpacesAndLineEndings) {
  char text[128];
  copyText(text, sizeof(text), "Louie:   test  \r\n");
  AutoReplyRequest r = autoReplyParseRequest(text, "test");
  EXPECT_TRUE(r.is_trigger);
  EXPECT_STREQ("test", r.message);
  EXPECT_EQ(5u, r.sender_len);
}

TEST(ParseRequest, WorksWithoutANamePrefix) {
  char text[128];
  copyText(text, sizeof(text), "test");
  AutoReplyRequest r = autoReplyParseRequest(text, "test");
  EXPECT_TRUE(r.is_trigger);
  EXPECT_EQ(0u, r.sender_len);   // nothing to echo back in the reply
}

TEST(ParseRequest, RejectsOtherMessages) {
  const char* others[] = { "Louie: hello", "Louie: testing", "Louie: test me",
                           "Louie: a test", "Louie: ", "" };
  for (const char* s : others) {
    char text[128];
    copyText(text, sizeof(text), s);
    EXPECT_FALSE(autoReplyParseRequest(text, "test").is_trigger) << s;
  }
}

TEST(ParseRequest, KeywordStandsAloneOrCarriesAnId) {
  // The reply costs airtime, so the tail is matched strictly: the bare keyword, or
  // the keyword plus exactly AUTOREPLY_ID_LEN hex characters. 'test 123' is still
  // ordinary chat and still costs nobody anything.
  char text[128];
  copyText(text, sizeof(text), "Louie: test 123");
  AutoReplyRequest r = autoReplyParseRequest(text, "test");
  EXPECT_FALSE(r.is_trigger);
  EXPECT_STREQ("test 123", r.message);
}

TEST(ParseRequest, BareKeywordStillTriggersAndCarriesNoId) {
  // Load-bearing for months: flashing this fleet is slow, so probes stay bare
  // until adoption is high. A firmware that only answered the id form would drop
  // every un-flashed node out of the measurement.
  char text[128];
  copyText(text, sizeof(text), "Louie: test");
  AutoReplyRequest r = autoReplyParseRequest(text, "test");
  EXPECT_TRUE(r.is_trigger);
  EXPECT_EQ(nullptr, r.id);
  EXPECT_EQ((size_t)0, r.id_len);
}

TEST(ParseRequest, KeywordWithACorrelationIdTriggers) {
  char text[128];
  copyText(text, sizeof(text), "Louie: test a1b2c3d4");
  AutoReplyRequest r = autoReplyParseRequest(text, "test");
  ASSERT_TRUE(r.is_trigger);
  ASSERT_NE(nullptr, r.id);
  EXPECT_EQ((size_t)AUTOREPLY_ID_LEN, r.id_len);
  EXPECT_EQ(std::string("a1b2c3d4"), std::string(r.id, r.id_len));
}

TEST(ParseRequest, CorrelationIdIsHexOfExactlyTheRightLength) {
  const char* bad[] = {
    "Louie: test a1b2c3d",     // seven
    "Louie: test a1b2c3d4e",   // nine
    "Louie: test a1b2c3dg",    // not hex
    "Louie: test  a1b2c3d4",   // two spaces
    "Louie: test a1b2c3d4 ",   // trailing space is trimmed, so this is the id form
    "Louie: test a1b2c3d4x",   // trailing junk
    "Louie: testa1b2c3d4",     // no separator
    // A *wrong* separator followed by eight valid hex characters. Without the
    // explicit space check these ride in on the length rule alone.
    "Louie: test-a1b2c3d4",
    "Louie: test:a1b2c3d4",
    "Louie: test_a1b2c3d4",
    "Louie: test.a1b2c3d4",
  };
  for (const char* s : bad) {
    char text[128];
    copyText(text, sizeof(text), s);
    bool trigger = autoReplyParseRequest(text, "test").is_trigger;
    // The trailing-space case is trimmed before matching, so it *is* a trigger.
    if (std::string(s) == "Louie: test a1b2c3d4 ") EXPECT_TRUE(trigger) << s;
    else EXPECT_FALSE(trigger) << s;
  }
}

TEST(ParseRequest, CorrelationIdIsCaseInsensitiveHex) {
  char text[128];
  copyText(text, sizeof(text), "Louie: TEST A1B2C3D4");
  AutoReplyRequest r = autoReplyParseRequest(text, "test");
  ASSERT_TRUE(r.is_trigger);
  EXPECT_EQ(std::string("A1B2C3D4"), std::string(r.id, r.id_len));
}

TEST(ParseRequest, AReplyCanNeverTriggerAReply) {
  // The invariant the whole-string match used to guarantee for free. A reply body
  // begins with the bracketed requester name, or with "SNR" when there was none,
  // and neither can match the keyword however the tail is parsed. Widening the
  // match must not cost this, or two repeaters could answer each other forever.
  const char* replies[] = {
    "SE-JKG-Rep: [SE-JKG-LouHome-A] #a1b2c3d4 SNR 1.0 RSSI -100 2h AA,BB",
    "SE-JKG-Rep: [SE-JKG-LouHome-A] SNR 1.0 RSSI -100 0h direct",
    "SE-JKG-Rep: SNR 1.0 RSSI -100 0h direct",
    "SE-JKG-Rep: [test] SNR 1.0 RSSI -100 0h direct",
    "SE-JKG-Rep: [test a1b2c3d4] SNR 1.0 RSSI -100 0h direct",
  };
  for (const char* s : replies) {
    char text[256];
    copyText(text, sizeof(text), s);
    EXPECT_FALSE(autoReplyParseRequest(text, "test").is_trigger) << s;
  }
}

TEST(ParseRequest, PrefixNeedsColonAndSpace) {
  // Documents the split: a bare colon is not a name prefix, so the whole text is
  // the message and there is no sender to echo.
  char text[128];
  copyText(text, sizeof(text), "Louie:test");
  AutoReplyRequest r = autoReplyParseRequest(text, "test");
  EXPECT_FALSE(r.is_trigger);
  EXPECT_EQ(0u, r.sender_len);
  EXPECT_STREQ("Louie:test", r.message);
}

TEST(ParseRequest, SplitsOnTheFirstPrefixOnly) {
  char text[128];
  copyText(text, sizeof(text), "Louie: re: test");
  AutoReplyRequest r = autoReplyParseRequest(text, "test");
  EXPECT_FALSE(r.is_trigger);
  EXPECT_EQ(5u, r.sender_len);
  EXPECT_STREQ("re: test", r.message);
}

TEST(ParseRequest, HandlesNulls) {
  char text[128];
  copyText(text, sizeof(text), "Louie: test");
  EXPECT_FALSE(autoReplyParseRequest(nullptr, "test").is_trigger);
  EXPECT_FALSE(autoReplyParseRequest(text, nullptr).is_trigger);
}

TEST(ParseRequest, KeywordIsConfigurable) {
  // AUTOREPLY_KEYWORD can be overridden at build time.
  char text[128];
  copyText(text, sizeof(text), "Louie: ping");
  EXPECT_TRUE(autoReplyParseRequest(text, "ping").is_trigger);
  copyText(text, sizeof(text), "Louie: ping");
  EXPECT_FALSE(autoReplyParseRequest(text, "test").is_trigger);
}

// ---- sender identity ------------------------------------------------------

TEST(SenderId, IsCaseFolded) {
  EXPECT_EQ(autoReplySenderId("Louie", 5), autoReplySenderId("louie", 5));
  EXPECT_EQ(autoReplySenderId("Louie", 5), autoReplySenderId("LOUIE", 5));
}

TEST(SenderId, DistinguishesNames) {
  EXPECT_NE(autoReplySenderId("louie", 5), autoReplySenderId("bertil", 6));
  EXPECT_NE(autoReplySenderId("louie", 5), autoReplySenderId("louis", 5));
}

TEST(SenderId, UsesOnlyTheGivenLength) {
  // The caller passes sender_len, which spans the name prefix and not the message.
  EXPECT_EQ(autoReplySenderId("Louie", 5), autoReplySenderId("Louie: test", 5));
}

TEST(SenderId, EmptyNamesShareOneIdentity) {
  EXPECT_EQ(autoReplySenderId("", 0), autoReplySenderId("anything", 0));
}

// ---- per-sender cooldown --------------------------------------------------

TEST(SenderAllowed, FirstRequestIsAnswered) {
  AutoReplySender ring[4] = {};
  uint8_t next = 0;
  EXPECT_TRUE(autoReplySenderAllowed(ring, 4, next, 111, 1000, 300));
}

TEST(SenderAllowed, WindowBoundary) {
  AutoReplySender ring[4] = {};
  uint8_t next = 0;
  EXPECT_TRUE(autoReplySenderAllowed(ring, 4, next, 111, 1000, 300));
  EXPECT_FALSE(autoReplySenderAllowed(ring, 4, next, 111, 1000, 300));
  EXPECT_FALSE(autoReplySenderAllowed(ring, 4, next, 111, 1299, 300));
  EXPECT_TRUE(autoReplySenderAllowed(ring, 4, next, 111, 1300, 300));
}

TEST(SenderAllowed, DeniedRequestsDoNotExtendTheWindow) {
  // Retrying must not push the next allowed reply further away, or a persistent
  // sender could lock themselves out indefinitely.
  AutoReplySender ring[4] = {};
  uint8_t next = 0;
  EXPECT_TRUE(autoReplySenderAllowed(ring, 4, next, 111, 1000, 300));
  for (uint32_t t = 1001; t < 1300; t += 50) {
    EXPECT_FALSE(autoReplySenderAllowed(ring, 4, next, 111, t, 300));
  }
  EXPECT_TRUE(autoReplySenderAllowed(ring, 4, next, 111, 1300, 300));
}

TEST(SenderAllowed, SendersAreIndependent) {
  // One person retrying cannot spend everyone else's share of the global limit.
  AutoReplySender ring[4] = {};
  uint8_t next = 0;
  EXPECT_TRUE(autoReplySenderAllowed(ring, 4, next, 111, 1000, 300));
  EXPECT_FALSE(autoReplySenderAllowed(ring, 4, next, 111, 1010, 300));
  EXPECT_TRUE(autoReplySenderAllowed(ring, 4, next, 222, 1010, 300));
  EXPECT_TRUE(autoReplySenderAllowed(ring, 4, next, 333, 1020, 300));
}

TEST(SenderAllowed, RingEvictsTheOldestEntry) {
  // A full ring forgets the earliest sender, who is then answered again at once.
  AutoReplySender ring[2] = {};
  uint8_t next = 0;
  EXPECT_TRUE(autoReplySenderAllowed(ring, 2, next, 111, 1000, 300));
  EXPECT_TRUE(autoReplySenderAllowed(ring, 2, next, 222, 1001, 300));
  EXPECT_FALSE(autoReplySenderAllowed(ring, 2, next, 111, 1002, 300));   // still remembered
  EXPECT_TRUE(autoReplySenderAllowed(ring, 2, next, 333, 1003, 300));    // evicts 111
  EXPECT_TRUE(autoReplySenderAllowed(ring, 2, next, 111, 1004, 300));    // forgotten, so answered
}

TEST(SenderAllowed, RingIndexWraps) {
  AutoReplySender ring[2] = {};
  uint8_t next = 0;
  for (uint32_t id = 1; id <= 10; id++) {
    EXPECT_TRUE(autoReplySenderAllowed(ring, 2, next, id, 1000 + id, 300));
    EXPECT_LT(next, 2);
  }
}

TEST(SenderAllowed, WithoutARingEverythingIsAllowed) {
  uint8_t next = 0;
  AutoReplySender ring[1] = {};
  EXPECT_TRUE(autoReplySenderAllowed(nullptr, 4, next, 111, 1000, 300));
  EXPECT_TRUE(autoReplySenderAllowed(ring, 0, next, 111, 1000, 300));
}

TEST(SenderAllowed, MirrorsTheFirmwareSettings) {
  // AUTOREPLY_MAX_SENDERS = 16, AUTOREPLY_WINDOW_SECS = 300.
  AutoReplySender ring[16] = {};
  uint8_t next = 0;
  for (uint32_t id = 1; id <= 16; id++) {
    EXPECT_TRUE(autoReplySenderAllowed(ring, 16, next, id, 1000, 300));
  }
  for (uint32_t id = 1; id <= 16; id++) {
    EXPECT_FALSE(autoReplySenderAllowed(ring, 16, next, id, 1299, 300));
  }
  for (uint32_t id = 1; id <= 16; id++) {
    EXPECT_TRUE(autoReplySenderAllowed(ring, 16, next, id, 1300, 300));
  }
}

// ---- how far a request may have travelled ---------------------------------

TEST(HopsAllowed, AcceptsUpToAndIncludingTheLimit) {
  EXPECT_TRUE(autoReplyHopsAllowed(0, 8));
  EXPECT_TRUE(autoReplyHopsAllowed(7, 8));
  EXPECT_TRUE(autoReplyHopsAllowed(8, 8));
  EXPECT_FALSE(autoReplyHopsAllowed(9, 8));
}

TEST(HopsAllowed, ZeroAnswersDirectNeighboursOnly) {
  EXPECT_TRUE(autoReplyHopsAllowed(0, 0));
  EXPECT_FALSE(autoReplyHopsAllowed(1, 0));
}

// ---- reading an on/off argument --------------------------------------------

TEST(ParseOnOff, AcceptsBothValuesInAnyCase) {
  bool v = false;
  EXPECT_TRUE(autoReplyParseOnOff("on", &v));
  EXPECT_TRUE(v);
  EXPECT_TRUE(autoReplyParseOnOff("off", &v));
  EXPECT_FALSE(v);
  EXPECT_TRUE(autoReplyParseOnOff("ON", &v));
  EXPECT_TRUE(v);
  EXPECT_TRUE(autoReplyParseOnOff("Off", &v));
  EXPECT_FALSE(v);
}

TEST(ParseOnOff, ToleratesWhitespaceATerminalMayAdd) {
  bool v = false;
  EXPECT_TRUE(autoReplyParseOnOff("on\r\n", &v));
  EXPECT_TRUE(v);
  EXPECT_TRUE(autoReplyParseOnOff("  off  ", &v));
  EXPECT_FALSE(v);
}

TEST(ParseOnOff, RejectsAWordThatMerelyStartsWithTheValue) {
  bool v = true;
  EXPECT_FALSE(autoReplyParseOnOff("onions", &v));
  EXPECT_FALSE(autoReplyParseOnOff("offset", &v));
  EXPECT_TRUE(v);   // a rejected argument leaves the setting alone
}

TEST(ParseOnOff, RejectsEmptyAndDegenerateInput) {
  bool v = true;
  EXPECT_FALSE(autoReplyParseOnOff("", &v));
  EXPECT_FALSE(autoReplyParseOnOff("   ", &v));
  EXPECT_FALSE(autoReplyParseOnOff("o", &v));
  EXPECT_FALSE(autoReplyParseOnOff(NULL, &v));
  EXPECT_FALSE(autoReplyParseOnOff("on", NULL));
  EXPECT_TRUE(v);
}

// ---- direct request: one packet, or one a repeater can carry back -----------

TEST(ReplyIsZeroHop, DirectRequestOnlyWhenDirectFloodIsOff) {
  EXPECT_TRUE(autoReplyReplyIsZeroHop(0, false));
  EXPECT_FALSE(autoReplyReplyIsZeroHop(0, true));
}

TEST(ReplyIsZeroHop, AnythingThatTravelledIsAlwaysFlooded) {
  // A reply can only skip the flood when the request needed no repeater to reach us.
  for (uint8_t hops = 1; hops <= 63; hops++) {
    EXPECT_FALSE(autoReplyReplyIsZeroHop(hops, false));
    EXPECT_FALSE(autoReplyReplyIsZeroHop(hops, true));
  }
}

TEST(ReplyIsZeroHop, SilenceOnTheMeshNeedsBothSettings) {
  // 'autoreply.hops' bounds which requests are answered, not how they are answered,
  // so only the pair of them keeps a reply off the mesh entirely.
  EXPECT_TRUE(autoReplyHopsAllowed(0, 0));
  EXPECT_FALSE(autoReplyReplyIsZeroHop(0, true));
  EXPECT_TRUE(autoReplyReplyIsZeroHop(0, false));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// ---- `trigger test` -----------------------------------------------------------
//
// The command a signed MQTT envelope carries to make this node originate a probe.
// It had no handler at all: it was on the allowlist and classified as a transmit
// command, so it verified, authorised, spent a trigger slot, persisted the replay
// counter -- and then fell through to "Unknown command", sending nothing.

TEST(TriggerCommand, BareFormIsAccepted) {
  const char* id = NULL; size_t id_len = 0;
  EXPECT_TRUE(autoReplyParseTrigger("trigger test", "test", &id, &id_len));
  EXPECT_EQ(NULL, id);
  EXPECT_EQ(0u, id_len);
}

TEST(TriggerCommand, IdFormIsAcceptedAndEchoedNotGenerated) {
  const char* id = NULL; size_t id_len = 0;
  EXPECT_TRUE(autoReplyParseTrigger("trigger test A1B2C3D4", "test", &id, &id_len));
  ASSERT_TRUE(id != NULL);
  EXPECT_EQ(std::string("A1B2C3D4"), std::string(id, id_len));
}

TEST(TriggerCommand, TheIdGrammarIsTheSameOneARequestUses) {
  // Defined once, in autoReplyMatchTrigger: whatever a node accepts in a request
  // it accepts in a trigger, so the two cannot drift into disagreeing.
  const char* id = NULL; size_t id_len = 0;
  EXPECT_FALSE(autoReplyParseTrigger("trigger test A1B2C3", "test", &id, &id_len));    // 6
  EXPECT_FALSE(autoReplyParseTrigger("trigger test A1B2C3D4E", "test", &id, &id_len)); // 9
  EXPECT_FALSE(autoReplyParseTrigger("trigger test ZZZZZZZZ", "test", &id, &id_len));  // not hex
  EXPECT_FALSE(autoReplyParseTrigger("trigger test A1B2C3D4 x", "test", &id, &id_len));
}

TEST(TriggerCommand, IsCaseInsensitiveLikeTheKeywordItself) {
  // The trigger word is documented case-insensitive, so the command that carries
  // it is too -- a requester should not have to guess which half cares.
  const char* id = NULL; size_t id_len = 0;
  EXPECT_TRUE(autoReplyParseTrigger("TRIGGER TEST", "test", &id, &id_len));
  EXPECT_TRUE(autoReplyParseTrigger("Trigger Test", "test", &id, &id_len));
  EXPECT_TRUE(autoReplyParseTrigger("trigger test a1b2c3d4", "test", &id, &id_len));
  EXPECT_EQ(std::string("a1b2c3d4"), std::string(id, id_len));
}

TEST(TriggerCommand, RefusesAnythingThatIsNotATrigger) {
  const char* id = NULL; size_t id_len = 0;
  EXPECT_FALSE(autoReplyParseTrigger("test", "test", &id, &id_len));
  EXPECT_FALSE(autoReplyParseTrigger("triggertest", "test", &id, &id_len));
  EXPECT_FALSE(autoReplyParseTrigger("trigger", "test", &id, &id_len));
  EXPECT_FALSE(autoReplyParseTrigger("trigger other", "test", &id, &id_len));
  EXPECT_FALSE(autoReplyParseTrigger(NULL, "test", &id, &id_len));
  EXPECT_FALSE(autoReplyParseTrigger("trigger test", NULL, &id, &id_len));
}

TEST(TriggerCommand, ATriggeredProbeCannotItselfTriggerAProbe) {
  // The probe body is bare `test`, and `trigger ` is required, so a probe this
  // node originates can never be read back as a command to originate another.
  char body[64];
  size_t n = autoReplyBuildProbe(body, sizeof(body), NULL, "test", NULL, 0);
  ASSERT_GT(n, 0u);
  const char* id = NULL; size_t id_len = 0;
  EXPECT_FALSE(autoReplyParseTrigger(body, "test", &id, &id_len));
}

TEST(BuildProbe, BareAndIdForms) {
  char out[64];
  EXPECT_EQ(4u, autoReplyBuildProbe(out, sizeof(out), NULL, "test", NULL, 0));
  EXPECT_EQ(std::string("test"), std::string(out));

  EXPECT_EQ(13u, autoReplyBuildProbe(out, sizeof(out), NULL, "test", "A1B2C3D4", 8));
  EXPECT_EQ(std::string("test A1B2C3D4"), std::string(out));
}

TEST(BuildProbe, ProbeBodyIsExactlyWhatAPersonWouldSend) {
  // A probe that differed from a hand-sent request would be answered by different
  // nodes under different rules, and the two measurements would not compare.
  char out[64];
  autoReplyBuildProbe(out, sizeof(out), NULL, "test", "DEADBEEF", 8);

  char text[128];
  copyText(text, sizeof(text), (std::string("alice: ") + out).c_str());
  AutoReplyRequest req = autoReplyParseRequest(text, "test");
  EXPECT_TRUE(req.is_trigger);
  ASSERT_TRUE(req.id != NULL);
  EXPECT_EQ(std::string("DEADBEEF"), std::string(req.id, req.id_len));
}

TEST(BuildProbe, RefusesRatherThanTruncating) {
  // A truncated probe would be a different message: `test A1B2` is not a request
  // any node answers, so sending one would spend airtime for nothing.
  char out[8];
  EXPECT_EQ(0u, autoReplyBuildProbe(out, sizeof(out), NULL, "test", "A1B2C3D4", 8));
  EXPECT_EQ(0u, autoReplyBuildProbe(out, 4, NULL, "test", NULL, 0));   // no room for NUL
  EXPECT_EQ(4u, autoReplyBuildProbe(out, 5, NULL, "test", NULL, 0));   // exactly fits
  EXPECT_EQ(0u, autoReplyBuildProbe(NULL, sizeof(out), NULL, "test", NULL, 0));
}

// ---- the probe carries the sender's name -------------------------------------
//
// Found on air, not in a test: a triggered probe sent as bare `test` drew replies
// reading `SNR 7.75 RSSI -106 1h 70`, where a client's probe draws
// `[SE-JKG-LouHome-A] SNR ...`. autoReplyParseRequest splits at ": " to find the
// requester, and a repeater echoes that name into its reply. Without the prefix
// the second vantage's replies are anonymous, and with two probes in flight
// nothing distinguishes them.

TEST(BuildProbe, CarriesTheSenderSoRepliesCanNameIt) {
  char out[96];
  size_t n = autoReplyBuildProbe(out, sizeof(out), "SE-JKG-LOUTEST", "test", NULL, 0);
  EXPECT_EQ(std::string("SE-JKG-LOUTEST: test"), std::string(out));
  EXPECT_EQ(n, strlen(out));
}

TEST(BuildProbe, CarriesBothSenderAndId) {
  char out[96];
  autoReplyBuildProbe(out, sizeof(out), "SE-JKG-LOUTEST", "test", "A1B2C3D4", 8);
  EXPECT_EQ(std::string("SE-JKG-LOUTEST: test A1B2C3D4"), std::string(out));
}

TEST(BuildProbe, ARealRepeaterCanParseWhatWeSend) {
  // The round trip that matters: what we build must come back out of the parser
  // as a trigger, with the sender and id intact.
  char out[96];
  autoReplyBuildProbe(out, sizeof(out), "SE-JKG-LOUTEST", "test", "DEADBEEF", 8);

  char text[128];
  copyText(text, sizeof(text), out);
  AutoReplyRequest req = autoReplyParseRequest(text, "test");
  EXPECT_TRUE(req.is_trigger);
  EXPECT_EQ(std::string("SE-JKG-LOUTEST"), std::string(req.sender, req.sender_len));
  ASSERT_TRUE(req.id != NULL);
  EXPECT_EQ(std::string("DEADBEEF"), std::string(req.id, req.id_len));
}

TEST(BuildProbe, AnUnnamedNodeStillSendsAUsableProbe) {
  // A node with no name set must still be able to probe; it just cannot be named
  // in the replies. Silence would be worse than anonymity.
  char out[96];
  EXPECT_EQ(4u, autoReplyBuildProbe(out, sizeof(out), "", "test", NULL, 0));
  EXPECT_EQ(std::string("test"), std::string(out));
  EXPECT_EQ(4u, autoReplyBuildProbe(out, sizeof(out), NULL, "test", NULL, 0));
}

TEST(BuildProbe, RefusesRatherThanTruncatingAName) {
  // A truncated name would be attributed to the wrong node, which is worse than
  // no probe at all.
  char out[16];
  EXPECT_EQ(0u, autoReplyBuildProbe(out, sizeof(out), "SE-JKG-LOUTEST", "test", "A1B2C3D4", 8));
}
