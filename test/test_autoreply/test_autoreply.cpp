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

// ---- the mode letter: how the requester asks to be answered ----------------

TEST(ParseRequest, NoModeMeansTheNodeDefault) {
  // Both older forms keep working unchanged, and neither names a mode: the node
  // answers the way it is configured to. This is the transition guarantee -- a
  // requester that knows nothing about modes is answered exactly as before.
  const char* older[] = { "Louie: test", "Louie: test a1b2c3d4" };
  for (const char* s : older) {
    char text[128];
    copyText(text, sizeof(text), s);
    AutoReplyRequest r = autoReplyParseRequest(text, "test");
    EXPECT_TRUE(r.is_trigger) << s;
    EXPECT_EQ(AUTOREPLY_MODE_DEFAULT, r.mode) << s;
  }
}

TEST(ParseRequest, ModeLetterAfterTheId) {
  struct { const char* letter; uint8_t mode; } cases[] = {
    { "F", AUTOREPLY_MODE_FLOOD }, { "P", AUTOREPLY_MODE_PRIVATE },
    { "D", AUTOREPLY_MODE_DIRECT }, { "S", AUTOREPLY_MODE_SILENT },
  };
  for (const auto& c : cases) {
    char text[128];
    copyText(text, sizeof(text), (std::string("Louie: test a1b2c3d4 ") + c.letter).c_str());
    AutoReplyRequest r = autoReplyParseRequest(text, "test");
    ASSERT_TRUE(r.is_trigger) << c.letter;
    EXPECT_EQ(c.mode, r.mode) << c.letter;
    EXPECT_EQ(std::string("a1b2c3d4"), std::string(r.id, r.id_len)) << c.letter;
  }
}

TEST(ParseRequest, ModeLetterWithoutAnId) {
  // 'F' and 'D' are hex digits, so a lone one must read as a mode and not as a
  // one-character id: an id is exactly AUTOREPLY_ID_LEN characters, never shorter.
  const char* letters[] = { "F", "D", "P", "S" };
  for (const char* l : letters) {
    char text[128];
    copyText(text, sizeof(text), (std::string("Louie: test ") + l).c_str());
    AutoReplyRequest r = autoReplyParseRequest(text, "test");
    EXPECT_TRUE(r.is_trigger) << l;
    EXPECT_EQ(nullptr, r.id) << l;
    EXPECT_NE(AUTOREPLY_MODE_DEFAULT, r.mode) << l;
  }
}

TEST(ParseRequest, ModeLetterIsCaseInsensitive) {
  char text[128];
  copyText(text, sizeof(text), "Louie: test a1b2c3d4 s");
  AutoReplyRequest r = autoReplyParseRequest(text, "test");
  ASSERT_TRUE(r.is_trigger);
  EXPECT_EQ(AUTOREPLY_MODE_SILENT, r.mode);
}

TEST(ParseRequest, ModeMustBeExactlyOneKnownLetter) {
  // The strict-match discipline holds for the third part as it does for the id:
  // 'test a1b2c3d4 x' is chat, and so is anything with a second letter or a second
  // space. A reply costs airtime; a near-miss must not spend it.
  const char* bad[] = {
    "Louie: test a1b2c3d4 X",     // not a mode letter
    "Louie: test a1b2c3d4 SS",    // two letters
    "Louie: test a1b2c3d4  S",    // two spaces
    "Louie: test a1b2c3d4 S x",   // trailing text
    "Louie: test S S",            // a mode is not an id
    "Louie: test SS",
    "Louie: test 123 S",          // a short id is chat, and stays chat with a mode
  };
  for (const char* s : bad) {
    char text[128];
    copyText(text, sizeof(text), s);
    EXPECT_FALSE(autoReplyParseRequest(text, "test").is_trigger) << s;
  }
}

// ---- the requester's key: where a private reply is addressed ---------------

// A 32-byte key as 64 hex characters, the form `set mqtt.owner` also takes.
static const char KEY[] =
  "AA04792D7804529FABF230B32DC8F7FA494D1B3280F153B2215C305E4FD654DC";

TEST(ParseRequest, APrivateModeMayCarryTheRequestersKey) {
  // A group text names its sender only by a display name anyone can type, so a
  // reply addressed to a key has to be told the key. It follows the mode letter.
  const char* forms[] = {
    "Louie: test a1b2c3d4 P ", "Louie: test a1b2c3d4 D ", "Louie: test P ", "Louie: test D ",
  };
  for (const char* f : forms) {
    char text[192];
    copyText(text, sizeof(text), (std::string(f) + KEY).c_str());
    AutoReplyRequest r = autoReplyParseRequest(text, "test");
    ASSERT_TRUE(r.is_trigger) << f;
    ASSERT_NE(nullptr, r.pubkey_hex) << f;
    EXPECT_EQ(std::string(KEY), std::string(r.pubkey_hex)) << f;
  }
}

TEST(ParseRequest, APrivateModeWithoutAKeyStillTriggers) {
  // The request is well formed; it just cannot be answered privately. Resolution
  // (below) turns it into a flood reply rather than dropping it.
  char text[128];
  copyText(text, sizeof(text), "Louie: test a1b2c3d4 P");
  AutoReplyRequest r = autoReplyParseRequest(text, "test");
  ASSERT_TRUE(r.is_trigger);
  EXPECT_EQ(AUTOREPLY_MODE_PRIVATE, r.mode);
  EXPECT_EQ(nullptr, r.pubkey_hex);
}

TEST(ParseRequest, TheKeyIsExactly64HexCharacters) {
  std::string key(KEY);
  const std::string bad[] = {
    "Louie: test a1b2c3d4 P " + key.substr(0, 63),      // short
    "Louie: test a1b2c3d4 P " + key + "A",               // long
    "Louie: test a1b2c3d4 P " + key.substr(0, 63) + "G", // not hex
    "Louie: test a1b2c3d4 P  " + key,                    // two spaces
    "Louie: test a1b2c3d4 P " + key + " x",              // trailing text
  };
  for (const std::string& s : bad) {
    char text[192];
    copyText(text, sizeof(text), s.c_str());
    EXPECT_FALSE(autoReplyParseRequest(text, "test").is_trigger) << s;
  }
}

TEST(ParseRequest, OnlyAPrivateModeMayCarryAKey) {
  // A flood reply and a silent one have nowhere to send a key to. A key after F or
  // S is not a slightly odd request, it is chat.
  const char* forms[] = { "Louie: test a1b2c3d4 F ", "Louie: test a1b2c3d4 S " };
  for (const char* f : forms) {
    char text[192];
    copyText(text, sizeof(text), (std::string(f) + KEY).c_str());
    EXPECT_FALSE(autoReplyParseRequest(text, "test").is_trigger) << f;
  }
}

// ---- the mode a reply is actually sent in ----------------------------------

TEST(ResolveMode, ARequestThatNamesNothingTakesTheNodeDefault) {
  EXPECT_EQ(AUTOREPLY_MODE_FLOOD,
            autoReplyResolveMode(AUTOREPLY_MODE_DEFAULT, AUTOREPLY_MODE_FLOOD, false));
  EXPECT_EQ(AUTOREPLY_MODE_PRIVATE,
            autoReplyResolveMode(AUTOREPLY_MODE_DEFAULT, AUTOREPLY_MODE_PRIVATE, true));
  // An unconfigured default is the flood reply every node makes today.
  EXPECT_EQ(AUTOREPLY_MODE_FLOOD,
            autoReplyResolveMode(AUTOREPLY_MODE_DEFAULT, AUTOREPLY_MODE_DEFAULT, false));
}

TEST(ResolveMode, ARequestThatNamesAModeWins) {
  EXPECT_EQ(AUTOREPLY_MODE_SILENT,
            autoReplyResolveMode(AUTOREPLY_MODE_SILENT, AUTOREPLY_MODE_FLOOD, false));
  EXPECT_EQ(AUTOREPLY_MODE_FLOOD,
            autoReplyResolveMode(AUTOREPLY_MODE_FLOOD, AUTOREPLY_MODE_PRIVATE, true));
  EXPECT_EQ(AUTOREPLY_MODE_DIRECT,
            autoReplyResolveMode(AUTOREPLY_MODE_DIRECT, AUTOREPLY_MODE_FLOOD, true));
}

TEST(ResolveMode, APrivateReplyWithNowhereToGoFloodsInstead) {
  // Never silence: a probe unanswered for an addressing gap looks exactly like a
  // dead repeater, and this system exists to tell those apart.
  EXPECT_EQ(AUTOREPLY_MODE_FLOOD,
            autoReplyResolveMode(AUTOREPLY_MODE_PRIVATE, AUTOREPLY_MODE_FLOOD, false));
  EXPECT_EQ(AUTOREPLY_MODE_FLOOD,
            autoReplyResolveMode(AUTOREPLY_MODE_DIRECT, AUTOREPLY_MODE_FLOOD, false));
  // ...including when it is the node's own default that asked for privacy.
  EXPECT_EQ(AUTOREPLY_MODE_FLOOD,
            autoReplyResolveMode(AUTOREPLY_MODE_DEFAULT, AUTOREPLY_MODE_PRIVATE, false));
  // Silence needs no address, so a key is irrelevant to it.
  EXPECT_EQ(AUTOREPLY_MODE_SILENT,
            autoReplyResolveMode(AUTOREPLY_MODE_SILENT, AUTOREPLY_MODE_FLOOD, true));
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
    "SE-JKG-Rep: [test a1b2c3d4 S] SNR 1.0 RSSI -100 0h direct",
    "SE-JKG-Rep: [test S] SNR 1.0 RSSI -100 0h direct",
    // A private reply has the same body as a channel one, and a requester who
    // names their own node after the keyword still gets an answer that cannot
    // come back around -- even with a key in the brackets.
    "SE-JKG-Rep: [test a1b2c3d4 P AA04792D7804529FABF230B32DC8F7FA494D1B3280F153B2215C305E4FD654DC] "
      "#a1b2c3d4 SNR 1.0 RSSI -100 2h AA,BB",
    "SE-JKG-Rep: [test] #a1b2c3d4 SNR 1.0 RSSI -100 0h direct",
    // The reply seen by a node with no name prefix of its own is the whole body.
    "[test a1b2c3d4] SNR 1.0 RSSI -100 0h direct",
  };
  for (const char* s : replies) {
    char text[256];
    copyText(text, sizeof(text), s);
    EXPECT_FALSE(autoReplyParseRequest(text, "test").is_trigger) << s;
  }
}

TEST(ParseRequest, TheKeywordAloneTriggersOnlyAtTheStartOfTheMessage) {
  // The invariant rests on where the keyword sits, not on what surrounds it: a
  // message whose keyword is preceded by anything -- a node name, a bracket, a
  // measurement -- is not a request. Pinned separately so a future match that
  // searched for the keyword anywhere would fail here before it failed on air.
  const char* not_at_start[] = {
    "Louie: SNR 1.0 test", "Louie: [x] test", "Louie: #a1b2c3d4 test", "Louie: -test",
  };
  for (const char* s : not_at_start) {
    char text[128];
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

// ---- charging a private request to its key ---------------------------------

TEST(KeyId, SameKeySameIdDifferentKeyDifferentId) {
  uint8_t a[32], b[32];
  memset(a, 0x5A, sizeof(a));
  memcpy(b, a, sizeof(b));
  EXPECT_EQ(autoReplyKeyId(a, 32), autoReplyKeyId(b, 32));
  b[31] ^= 1;                                     // one bit of the last byte
  EXPECT_NE(autoReplyKeyId(a, 32), autoReplyKeyId(b, 32));
}

TEST(KeyId, BytesAreNotFoldedLikeAName) {
  // 0x41 and 0x61 are 'A' and 'a' as text and one sender to autoReplySenderId;
  // as key bytes they are two different keys and must stay so.
  uint8_t upper[32], lower[32];
  memset(upper, 0x41, sizeof(upper));
  memset(lower, 0x61, sizeof(lower));
  EXPECT_NE(autoReplyKeyId(upper, 32), autoReplyKeyId(lower, 32));
  EXPECT_EQ(autoReplySenderId((const char*)upper, 32), autoReplySenderId((const char*)lower, 32));
}

TEST(KeyId, TheCooldownRingWorksOnKeysUnchanged) {
  // The same ring the channel uses, keyed by key id: a retry inside the window
  // is refused, a second key is admitted, the first is admitted again after it.
  AutoReplySender ring[4] = {};
  uint8_t next = 0;
  uint8_t k1[32], k2[32];
  memset(k1, 1, 32); memset(k2, 2, 32);
  uint32_t id1 = autoReplyKeyId(k1, 32), id2 = autoReplyKeyId(k2, 32);
  EXPECT_TRUE(autoReplySenderAllowed(ring, 4, next, id1, 1000, 300));
  EXPECT_FALSE(autoReplySenderAllowed(ring, 4, next, id1, 1100, 300));
  EXPECT_TRUE(autoReplySenderAllowed(ring, 4, next, id2, 1100, 300));
  EXPECT_TRUE(autoReplySenderAllowed(ring, 4, next, id1, 1300, 300));
}

// ---- a private test request: the body an anonymous request carries ---------

TEST(ParsePrivateTest, BareAndIdForms) {
  char text[64];
  const char* id = NULL; size_t id_len = 0;
  copyText(text, sizeof(text), "test");
  EXPECT_TRUE(autoReplyParsePrivateTest(text, "test", &id, &id_len));
  EXPECT_EQ(NULL, id);
  copyText(text, sizeof(text), "TEST a1b2c3d4");
  EXPECT_TRUE(autoReplyParsePrivateTest(text, "test", &id, &id_len));
  EXPECT_EQ(std::string("a1b2c3d4"), std::string(id, id_len));
}

TEST(ParsePrivateTest, TrimsWhatATerminalOrClientMayAdd) {
  char text[64];
  const char* id = NULL; size_t id_len = 0;
  copyText(text, sizeof(text), "  test a1b2c3d4 \r\n");
  EXPECT_TRUE(autoReplyParsePrivateTest(text, "test", &id, &id_len));
  EXPECT_EQ(std::string("a1b2c3d4"), std::string(id, id_len));
}

TEST(ParsePrivateTest, AModeOrAKeyHasNothingToSayHere) {
  // The packet already makes the reply private and already names the sender by
  // key, so a mode letter or a key in the body is refused as chat -- the same
  // refusal a mode-less caller makes on the channel, from the same function.
  const std::string bad[] = {
    "test S", "test a1b2c3d4 P", std::string("test a1b2c3d4 P ") + KEY, "test a1b2c3d4 F",
  };
  for (const std::string& s : bad) {
    char text[160];
    const char* id = NULL; size_t id_len = 0;
    copyText(text, sizeof(text), s.c_str());
    EXPECT_FALSE(autoReplyParsePrivateTest(text, "test", &id, &id_len)) << s;
    EXPECT_EQ(NULL, id) << s;
  }
}

TEST(ParsePrivateTest, NoNamePrefixToSplit) {
  // A channel request may say "Louie: test"; a private one is from a key, and a
  // name prefix would only be a way to type a request that is not one.
  char text[64];
  const char* id = NULL; size_t id_len = 0;
  copyText(text, sizeof(text), "Louie: test");
  EXPECT_FALSE(autoReplyParsePrivateTest(text, "test", &id, &id_len));
}

TEST(ParsePrivateTest, HandlesNullsAndChat) {
  char text[64];
  const char* id = NULL; size_t id_len = 0;
  EXPECT_FALSE(autoReplyParsePrivateTest(NULL, "test", &id, &id_len));
  copyText(text, sizeof(text), "test");
  EXPECT_FALSE(autoReplyParsePrivateTest(text, NULL, &id, &id_len));
  copyText(text, sizeof(text), "test me");
  EXPECT_FALSE(autoReplyParsePrivateTest(text, "test", &id, &id_len));
  copyText(text, sizeof(text), "");
  EXPECT_FALSE(autoReplyParsePrivateTest(text, "test", &id, &id_len));
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

// ---- reading a mode name, as `set autoreply.mode` takes it -----------------

TEST(ParseModeName, AcceptsTheThreeStandingModesInAnyCase) {
  uint8_t m = AUTOREPLY_MODE_DEFAULT;
  EXPECT_TRUE(autoReplyParseModeName("flood", &m));
  EXPECT_EQ(AUTOREPLY_MODE_FLOOD, m);
  EXPECT_TRUE(autoReplyParseModeName("Private", &m));
  EXPECT_EQ(AUTOREPLY_MODE_PRIVATE, m);
  EXPECT_TRUE(autoReplyParseModeName("DIRECT", &m));
  EXPECT_EQ(AUTOREPLY_MODE_DIRECT, m);
}

TEST(ParseModeName, SilentIsNotAStandingMode) {
  // A node that should answer nobody is switched off. Silence is what a request
  // asks for, one probe at a time, never a setting the node keeps.
  uint8_t m = AUTOREPLY_MODE_FLOOD;
  EXPECT_FALSE(autoReplyParseModeName("silent", &m));
  EXPECT_EQ(AUTOREPLY_MODE_FLOOD, m);   // a rejected argument leaves the setting alone
}

TEST(ParseModeName, MeasuresTheTokenLikeAnOnOffArgument) {
  uint8_t m = AUTOREPLY_MODE_FLOOD;
  EXPECT_FALSE(autoReplyParseModeName("flooding", &m));
  EXPECT_FALSE(autoReplyParseModeName("priv", &m));
  EXPECT_FALSE(autoReplyParseModeName("", &m));
  EXPECT_FALSE(autoReplyParseModeName("   ", &m));
  EXPECT_FALSE(autoReplyParseModeName(NULL, &m));
  EXPECT_FALSE(autoReplyParseModeName("flood", NULL));
  EXPECT_EQ(AUTOREPLY_MODE_FLOOD, m);
  EXPECT_TRUE(autoReplyParseModeName("  direct\r\n", &m));
  EXPECT_EQ(AUTOREPLY_MODE_DIRECT, m);
}

TEST(ParseModeName, NamesRoundTrip) {
  // What `get autoreply.mode` prints is what `set autoreply.mode` accepts.
  const uint8_t standing[] = { AUTOREPLY_MODE_FLOOD, AUTOREPLY_MODE_PRIVATE, AUTOREPLY_MODE_DIRECT };
  for (uint8_t mode : standing) {
    uint8_t back = AUTOREPLY_MODE_DEFAULT;
    EXPECT_TRUE(autoReplyParseModeName(autoReplyModeName(mode), &back));
    EXPECT_EQ(mode, back);
  }
  EXPECT_STREQ("silent", autoReplyModeName(AUTOREPLY_MODE_SILENT));
  EXPECT_STREQ("default", autoReplyModeName(AUTOREPLY_MODE_DEFAULT));
}

// ---- the route back: a flood's path, read backwards ------------------------

TEST(ReversePath, ReversesWholeHashesNotBytes) {
  // Three 2-byte hashes: the flood came AA11 -> BB22 -> CC33, so the way back
  // starts at CC33. The bytes inside each hash keep their order.
  const uint8_t path[] = { 0xAA, 0x11, 0xBB, 0x22, 0xCC, 0x33 };
  const uint8_t path_len = (1 << 6) | 3;      // hash size 2, count 3
  uint8_t back[sizeof(path)];
  EXPECT_EQ(path_len, autoReplyReversePath(path, path_len, back));
  const uint8_t want[] = { 0xCC, 0x33, 0xBB, 0x22, 0xAA, 0x11 };
  EXPECT_EQ(0, memcmp(want, back, sizeof(want)));
}

TEST(ReversePath, OneByteHashesAndASingleHop) {
  const uint8_t path[] = { 0x01, 0x02, 0x03, 0x04 };
  uint8_t back[4];
  autoReplyReversePath(path, 4, back);          // hash size 1, count 4
  const uint8_t want[] = { 0x04, 0x03, 0x02, 0x01 };
  EXPECT_EQ(0, memcmp(want, back, 4));

  const uint8_t one[] = { 0x7A, 0x15, 0xAA };
  uint8_t same[3];
  EXPECT_EQ((2 << 6) | 1, autoReplyReversePath(one, (2 << 6) | 1, same));
  EXPECT_EQ(0, memcmp(one, same, 3));           // a single hop is its own reverse
}

TEST(ReversePath, AnEmptyPathStaysEmpty) {
  uint8_t back[1] = { 0xEE };
  EXPECT_EQ(0, autoReplyReversePath(NULL, 0, back));
  EXPECT_EQ(0xEE, back[0]);                     // nothing written
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

TEST(TriggerCommand, ACallerWithNoUseForAModeRejectsOne) {
  // The trigger command parses without a mode out-param today. A request naming
  // a mode must then be a non-trigger, and carry nothing out -- never a trigger
  // with the mode quietly dropped, which would probe in a mode nobody chose.
  const char* id = NULL; size_t id_len = 0;
  EXPECT_FALSE(autoReplyParseTrigger("trigger test A1B2C3D4 S", "test", &id, &id_len));
  EXPECT_EQ(NULL, id);
  EXPECT_EQ(0u, id_len);
  EXPECT_FALSE(autoReplyParseTrigger("trigger test S", "test", &id, &id_len));
}

TEST(TriggerCommand, CarriesAModeWhenAskedFor) {
  const char* id = NULL; size_t id_len = 0; uint8_t mode = AUTOREPLY_MODE_DEFAULT;
  EXPECT_TRUE(autoReplyParseTrigger("trigger test A1B2C3D4 S", "test", &id, &id_len, &mode));
  EXPECT_EQ(AUTOREPLY_MODE_SILENT, mode);
  EXPECT_EQ(std::string("A1B2C3D4"), std::string(id, id_len));
  EXPECT_TRUE(autoReplyParseTrigger("trigger test p", "test", &id, &id_len, &mode));
  EXPECT_EQ(AUTOREPLY_MODE_PRIVATE, mode);
  EXPECT_EQ(NULL, id);
  // and none when it was not
  EXPECT_TRUE(autoReplyParseTrigger("trigger test", "test", &id, &id_len, &mode));
  EXPECT_EQ(AUTOREPLY_MODE_DEFAULT, mode);
}

TEST(TriggerCommand, NeverCarriesAKey) {
  // The probe's requester is this node, which knows its own key; a command that
  // tries to supply one is refused rather than trusted.
  const char* id = NULL; size_t id_len = 0; uint8_t mode = AUTOREPLY_MODE_DEFAULT;
  std::string cmd = std::string("trigger test A1B2C3D4 P ") + KEY;
  EXPECT_FALSE(autoReplyParseTrigger(cmd.c_str(), "test", &id, &id_len, &mode));
  EXPECT_EQ(NULL, id);
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

TEST(BuildProbe, NamesAModeOnlyWhenAsked) {
  // The bare and id forms are untouched by the mode work: a probe that named a
  // mode by default would be one an un-flashed repeater ignores.
  char out[64];
  autoReplyBuildProbe(out, sizeof(out), NULL, "test", "A1B2C3D4", 8, AUTOREPLY_MODE_DEFAULT, KEY);
  EXPECT_EQ(std::string("test A1B2C3D4"), std::string(out));

  autoReplyBuildProbe(out, sizeof(out), NULL, "test", "A1B2C3D4", 8, AUTOREPLY_MODE_SILENT);
  EXPECT_EQ(std::string("test A1B2C3D4 S"), std::string(out));
  autoReplyBuildProbe(out, sizeof(out), NULL, "test", NULL, 0, AUTOREPLY_MODE_FLOOD);
  EXPECT_EQ(std::string("test F"), std::string(out));
}

TEST(BuildProbe, APrivateModeCarriesTheKeyAndTheOthersDoNot) {
  char out[160];
  autoReplyBuildProbe(out, sizeof(out), NULL, "test", "A1B2C3D4", 8, AUTOREPLY_MODE_PRIVATE, KEY);
  EXPECT_EQ(std::string("test A1B2C3D4 P ") + KEY, std::string(out));
  autoReplyBuildProbe(out, sizeof(out), NULL, "test", NULL, 0, AUTOREPLY_MODE_DIRECT, KEY);
  EXPECT_EQ(std::string("test D ") + KEY, std::string(out));
  // F and S have nowhere to send a key, and the parser would refuse one.
  autoReplyBuildProbe(out, sizeof(out), NULL, "test", NULL, 0, AUTOREPLY_MODE_FLOOD, KEY);
  EXPECT_EQ(std::string("test F"), std::string(out));
  autoReplyBuildProbe(out, sizeof(out), NULL, "test", NULL, 0, AUTOREPLY_MODE_SILENT, KEY);
  EXPECT_EQ(std::string("test S"), std::string(out));
}

TEST(BuildProbe, ARealRepeaterParsesEveryModeWeSend) {
  // Round trip through the request parser, which is what the receiving node runs.
  struct { uint8_t mode; bool keyed; } cases[] = {
    { AUTOREPLY_MODE_FLOOD, false }, { AUTOREPLY_MODE_PRIVATE, true },
    { AUTOREPLY_MODE_DIRECT, true }, { AUTOREPLY_MODE_SILENT, false },
  };
  for (const auto& c : cases) {
    char body[160];
    autoReplyBuildProbe(body, sizeof(body), "SE-JKG-Rep", "test", "A1B2C3D4", 8, c.mode, KEY);
    AutoReplyRequest r = autoReplyParseRequest(body, "test");
    ASSERT_TRUE(r.is_trigger) << body;
    EXPECT_EQ(c.mode, r.mode) << body;
    EXPECT_EQ(c.keyed, r.pubkey_hex != NULL) << body;
    if (c.keyed) EXPECT_EQ(std::string(KEY), std::string(r.pubkey_hex));
  }
}

TEST(BuildProbe, RefusesRatherThanTruncatingAKey) {
  char out[70];   // room for "test A1B2C3D4 P " but not the 64-character key
  EXPECT_EQ(0u, autoReplyBuildProbe(out, sizeof(out), NULL, "test", "A1B2C3D4", 8,
                                    AUTOREPLY_MODE_PRIVATE, KEY));
  EXPECT_EQ(std::string(""), std::string(out));
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
