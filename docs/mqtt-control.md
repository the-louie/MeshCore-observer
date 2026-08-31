# Remote control over MQTT

An observer node can accept commands published to it over MQTT: parameter changes, and a
request to send a test now. It is **off by default** and inert without a configured owner key.

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

Both are needed. With `mqtt.cmd on` but no owner key, every command is refused.

## Topics

```
meshhealth/v1/{IATA}/{NODE}/cmd     to one node
meshhealth/v1/{IATA}/all/cmd        to every node in the region
meshhealth/v1/{IATA}/{NODE}/res     the node's answer
```

`{NODE}` is the first four bytes of the node's public key, in hex. A project-specific prefix,
not `meshcore/`, so control traffic stays out of the namespace observer tooling watches.

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
  is reopened by whatever crashes mid-command, and a silently repeated `trigger test` spends
  airtime nobody asked for. The cost is that a crashed command is not retried, which is the
  right way round.
- **Expiry fails closed without a clock.** If NTP has not synced, every command is refused
  rather than accepted on a guess. A command may not be valid more than an hour ahead.
- **The rate limit applies to validly signed commands** — 20 an hour, of which 4 may transmit.
  A signing key that leaks must not become a mesh-flood weapon; the airtime is not ours.

## What may be sent

Only auto-reply parameters and the test trigger:

```
set autoreply on|off · set autoreply.hops · set autoreply.direct.flood
set autoreply.delay · set autoreply.region · get autoreply* · trigger test
```

Everything else is refused before it reaches the CLI. The allowlist is deliberately independent
of the second guard: MQTT commands run with the node's clock rather than the `0` that marks a
locally-privileged caller, so `set prv.key`, `erase` and `set mqtt.owner` refuse them anyway.
Either alone should be enough; both together mean one of them can be wrong.

## What is not defended

- **Confidentiality.** Commands and results are public on a public broker. Nothing secret may
  appear in either — which is what the allowlist keeps true.
- **Availability.** Anyone can flood the broker. The node's rate limit bounds the airtime
  consequence; it cannot make the broker quiet.
- **A compromised owner key.** Whoever holds it can do anything on the allowlist. Rotation
  currently needs serial access.
