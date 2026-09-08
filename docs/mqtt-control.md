# Remote control over MQTT

An observer node can accept commands published to it over MQTT: parameter changes, and a
request to send a test now, on the channel or to one repeater alone. It is **off by default** and inert without a configured owner key.

Flashing a fleet takes months. This exists so that changing a parameter afterwards costs a
signed publish rather than another flashing round.

## The threat model in one line

`mqtt.meshat.se` and brokers like it publish their credentials. **Anyone can publish to any
topic**, so nothing here trusts the transport: a command runs only if it carries a valid
Ed25519 signature by this node's configured owner key.

## Turning it on

```
set mqtt.owner <64 hex characters>    # serial only; the public half of the signing key
set mqtt.cmd on
get mqtt.cmd                          # state, and the highest command counter seen
```

Both are needed. With `mqtt.cmd on` but no owner key, every command is refused -- and,
importantly, *discarded*. The node holds one staged command at a time and a full slot drops
the newer arrival, so a refusal that left the slot occupied would silently swallow every
later command for the rest of the boot, including the ones sent once a key was finally
provisioned. Refusing and clearing are the same act (`mqttCtrlOwnerReady`, tested in
`test_mqtt_control`).

## Topics

```
meshcore/{IATA}/{NODE}/cmd     to one node
meshcore/{IATA}/all/cmd        to every node in the region
meshcore/{IATA}/{NODE}/res     the node's answer
```

`{NODE}` is the first four bytes of the node's public key, in hex.

**The prefix must be `meshcore/`.** The broker permits that namespace and silently discards
everything else: a publish elsewhere is accepted, QoS-1 acked, and dropped, with no error the
publisher can see. This was measured against `mqtt.meshat.se` on 2026-09-01, after every signed
command to a correctly configured node went unanswered — `meshcore/JKG/<node>/cmd` arrives,
`meshhealth/v1/JKG/<node>/cmd` does not.

An earlier version used a project-specific prefix to keep control traffic out of the namespace
observer tooling watches. That separation was never needed: the tooling matches on the leaf
(`+/packets`, `+/status`), so `/cmd` and `/res` never collided with it.

## The envelope

```
payload      = <signed-part> "|" <128 hex Ed25519 signature>
signed-part  = "v1|" <counter> "|" <expiry> "|" <command>
signed bytes = <topic> "\n" <signed-part>
```

Not JSON. Signing a JSON document requires canonicalisation, and every canonicalisation bug is
a signature-bypass bug; here the signed region is a literal byte range of the payload as it
arrived. The payload is split at the **last** `|`, verified, and only then parsed.

**The topic is inside the signature.** One signature is therefore valid for exactly one
destination: a command captured from one node cannot be replayed at another, and a per-node
command cannot be replayed as a broadcast. Neither case is visible to a counter, because each
node keeps its own.

## What a node checks, in order

```
feature enabled → owner key set → signature valid
  → counter strictly greater than the stored one → not expired
  → rate limit → command on the allowlist
  → store the counter → run it → publish the result
```

- **The counter is stored before the command runs**, never after. A counter written afterwards
  is reopened by whatever crashes mid-command, and a silently repeated trigger spends
  airtime nobody asked for. The cost is that a crashed command is not retried, which is the
  right way round.
- **Expiry fails closed without a clock.** If NTP has not synced, every command is refused
  rather than accepted on a guess. A command may not be valid more than an hour ahead.
- **The rate limit applies to validly signed commands** — 20 an hour, of which 4 may transmit.
  A signing key that leaks must not become a mesh-flood weapon; the airtime is not ours.

## What may be sent

Auto-reply parameters, the two test triggers, and a set of read-only parameter gets:

```
set autoreply on|off · set autoreply.hops · set autoreply.direct.flood
set autoreply.delay · set autoreply.region · set autoreply.mode · set autoreply.private
get autoreply* · trigger test [<id>] [<F|P|D|S>] · trigger private <64 hex> [<id>]

get txdelay · get direct.txdelay · get rxdelay · get af
get cad · get int.thresh · get radio
get repeat · get flood.max · get flood.max.unscoped
```

`trigger test` originates a probe on the channel, naming a mode the way a request sent by hand
does; `trigger private` sends a test request to the one repeater whose key is given, and the
answer comes back to this node alone. Both are described in
[Repeater auto-reply](autoreply.md).

The reads change nothing and cannot be replayed into anything, which is what keeps the security
argument simple. There is deliberately **no `set`** among them: we do not yet know which values
we would write, and a write would need an argument this design has not had to make.

**The two groups match differently, and the difference is the point.** The first four are
*families*: the boundary rule lets `set autoreply` cover `set autoreply.hops 8` and the rest,
one entry instead of five, and `trigger test` its optional id and mode letter. The reads are
*exact* — each admits the command it names and nothing else. A family silently admits children
nobody listed, and `get radio` was admitting
`get radio.rxgain`, `get radio.fem.rxgain` and `get radio foo` that way. Harmless values, but
this list exists to be read by an operator deciding what a node exposes, and an entry that
admits more than it says defeats that. Adding `get af.thing` to `CommonCLI` later cannot reach
the mesh without someone also adding it here.

Everything else is refused before it reaches the CLI. The allowlist is deliberately independent
of the second guard: MQTT commands run with the node's clock rather than the `0` that marks a
locally-privileged caller, so `set prv.key`, `erase` and `set mqtt.owner` refuse them anyway.
Either alone should be enough; both together mean one of them can be wrong.

**The allowlist is the tighter of the two gates and must stay that way.** It requires a match to
end at end-of-string, a space or a dot; `CommonCLI`'s own `handleGetCmd` matches with an
unbounded `memcmp`, so `get afxyz` would answer if it ever reached there. Do not relax the
boundary rule to save allowlist entries.

A node on older firmware will not know some of these and answers that it does not. That is the
mixed fleet working as intended, and a reader must record it as "did not answer" rather than as
an absent setting.

### A transmit command may not be broadcast

`trigger test` or `trigger private` sent to `meshcore/{IATA}/all/cmd` is refused, on principle
rather than on budget.

The rate limits are per node — four triggers an hour each. That bounds what one node does and
says nothing about one publish reaching every node in a region, each of them then transmitting
inside the same few seconds, every one within its own budget. The workspace rule is explicit
that any design which scales transmissions with the number of nodes is wrong, and a broadcast
trigger is exactly that shape: the more of the mesh adopts this firmware, the worse the storm
it enables.

Broadcasting a *parameter* change stays allowed — setting a value costs no airtime. Only
transmitting is refused, and the check is stateless so it runs before the limiters spend
anything.

## What is not defended

- **Confidentiality.** Commands and results are public on a public broker. Nothing secret may
  appear in either — which is what the allowlist keeps true.
- **Availability.** Anyone can flood the broker. The node's rate limit bounds the airtime
  consequence; it cannot make the broker quiet.
- **A compromised owner key.** Whoever holds it can do anything on the allowlist. Rotation
  currently needs serial access.

## The result

A node that verified a command publishes the outcome to its own result topic:

```
meshcore/{IATA}/{NODE}/res
r1|<counter>|<code>|<command>|<reply>
```

The counter is the one the requester chose, which makes it the correlation id: the topic says
which node answered, the counter says which question it answered. Split on the first four
separators and take the remainder as the reply — a command can never contain a separator
(`mqttCtrlCommandCharsOk` refuses it), so only the reply can, and only at the end.

A line too long for the buffer is **truncated, never dropped**. The reply is last and every
earlier field is bounded, so a clamped line still parses.

**Only an outcome whose signature verified is published.** Verification runs before the rate
limiter — the limiters spend budget when asked, so every stateless check comes first — which
means a bad-signature refusal is not rate limited, and publishing those would let anyone on a
broker with public credentials drive unlimited publishes out of the node. A legitimate
requester holds the owner key, so its commands always draw an answer whether they succeed or
are refused; nothing readable is lost.

| Code | Meaning |
|---|---|
| 0 | ran; `reply` holds the output |
| 9 | replayed — counter not greater than the last one stored |
| 10 | expired |
| 11 | expiry too far in the future |
| 12 | no trusted clock; expiry fails closed |
| 14 | not on the allowlist — a permanent no |
| 17 | rate limited — ask again later |
| 18 | a transmit command addressed to every node at once |

Codes are appended to the enum and never renumbered, so a stored result keeps its meaning.

