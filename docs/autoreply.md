# Repeater auto-reply

A repeater can answer a keyword on one channel with a signal report, so anyone can check
coverage by sending a single message — no login, no contact setup, no administration.

```
you:    test
STO-1:  [louie] SNR 6.5 RSSI -92 2h A3,1B
STO-2:  [louie] SNR -4.0 RSSI -104 0h direct
```

The reply gives the SNR and RSSI the repeater heard you at, the hop count, and the hex
hashes of the repeaters your message travelled through.

**It is off by default, and it should stay off unless you have read this page.** Every
repeater in range answers the same trigger, so a request from more than one hop away becomes
one flood reply per repeater. `autoreply.hops` is what bounds that, and the reply carries
the requester's name in brackets so several arriving together can be told apart.

## Setup

The channel is not something you choose. It is always `#test-<iata>`, built from the region
code the node already publishes under, so **the node must have one set**:

```
set mqtt.iata STO
set autoreply on
```

`get autoreply.channel` then reports `#test-sto`. Add that channel in your client and send
`test` to it. Every repeater in direct range answers. Nothing else is needed for a range
check.

To also get answers from repeaters **more than one hop away**, the request has to carry a
region scope — see [How far the reply travels](#how-far-the-reply-travels) below.

To turn it back off:

```
set autoreply off
```

A node with no region code set — no `mqtt.iata`, or the `XXX` placeholder — has no channel
to listen on and stays silent even with `set autoreply on`. `get autoreply` says so.

## Commands

#### Switch it on or off

- `get autoreply`
- `set autoreply on|off`

**Default:** `off`

#### Read the channel the repeater listens on

- `get autoreply.channel`

**Read-only.** The channel is always `#test-` plus the node's `mqtt.iata` code, folded to
lower case; change it with `set mqtt.iata`. Deriving it rather than accepting a name is what
keeps every repeater off the mesh-wide `#test`, where a reply from each of them would be
spam — a three-character region code can never produce one of the shared channel names.

**Note:** the channel key is derived from the name — the first 16 bytes of `sha256("#name")`
— so there is no PSK to configure or share. Any client that adds a channel of the same name
gets the same key.

**Note:** the name is **lower case**. The key is the hash of the name exactly as stored, and
clients only accept lower-case channel names, so an upper-case name would produce a channel
nobody could join. `set mqtt.iata STO` therefore gives `#test-sto` — type that into your
client.

Region names are **not** folded, so if you pair the channel with a region for the multi-hop
path, spell the region exactly the same on every node.

The trigger keyword is case-insensitive: `test`, `Test` and `TEST` all work. Many phone
keyboards capitalise the first letter, so this matters in practice.

The keyword may be followed by a correlation id: one space, then exactly eight hexadecimal
characters, as in `test a1b2c3d4`. The id is echoed back in the reply, so a requester can tie
an answer to the request that provoked it — which arrival time can no longer do once replies
are delayed by minutes. Anything else after the keyword is ordinary chat and is ignored, so
`test me` and `test 123` still cost nobody any airtime.

A bare `test` keeps working and will continue to. Fleets are flashed slowly, and a firmware
that answered only the id form would drop every not-yet-updated node out of a survey.

#### Limit how far away a request can be

- `get autoreply.hops`
- `set autoreply.hops <value>`

**Parameters:**

- `value`: maximum hop count (0-63) of a request that will be answered. `0` answers only
  direct neighbours.

**Default:** `8`

#### Spread the replies out

- `get autoreply.delay`
- `set autoreply.delay <value>`

**Parameters:**

- `value`: multiplier (1-120) on the radio's own retransmit delay. The reply is sent after a
  random wait between zero and that multiple.

**Default:** `4`

Every repeater in range answers the same request, so their replies compete for the air. Field
measurement on 2026-08-30 found that a reply overlapping another transmission reached a median
of 2 relays, against 5 for one sent into clear air, and that 16 of 18 requests drawing two or
more replies contained an overlapping pair.

Raise this where several repeaters cover the same ground. For an automated survey, where
nobody is waiting for the answer, a much larger value costs only patience. The right number
depends on how many repeaters hear each other and cannot be read off one mesh, which is why it
is settable at runtime rather than compiled in.

#### Bound how far a reply travels

- `get autoreply.region`
- `set autoreply.region <name>`

**Parameters:**

- `name`: a region from the node's own region table, or empty to clear.

**Default:** empty — the reply mirrors whatever scope the request arrived under.

With a region set, every reply is scoped to it regardless of how the request arrived. This
bounds the airtime a survey costs the wider mesh while still letting a reply cross the whole
region being measured.

Note this bounds propagation by a *stated* boundary rather than by hop count. A hop cap would
answer a different question — how much comes back to whoever asked — and would quietly hide
the far side of the mesh, which is the thing a connectivity survey exists to see.

#### Choose how a direct request is answered

- `get autoreply.direct.flood`
- `set autoreply.direct.flood on|off`

**Default:** `on`

With `on`, a request that reached this repeater without passing through another one is
answered the same way any other request is: a flood, kept inside the request's region scope
when it had one. With `off` it is answered by a single zero-hop packet that no repeater will
ever relay — the cheapest possible reply, and the behaviour of releases before this setting
existed.

Prefer `on` unless airtime is tight. A repeater generally sits in much quieter RF than a
handheld, so it hears requests that its own answers cannot get back to — the link is
asymmetric even when the antennas are not, and a zero-hop reply has no second chance and no
alternative path. `off` is the right choice on dense urban sites, or where requesters are
known to be in solid two-way range.

#### Show the current state

- `get autoreply`

## How far the reply travels

A channel name does not limit propagation. Repeaters forward on the packet header alone —
route type, hop count against `flood.max`, transport code — and the channel lives in the
encrypted payload, which a repeater does not read. A message to `#test-sto` therefore costs
exactly the same airtime, over exactly the same hops, as one to `#test`. Naming the channel
per region keeps it out of other people's message lists; it does not keep it off their air.

What does limit propagation is the region transport code. So the repeater answers in one of
two ways, and stays silent if it can do neither:

| Request arrived | Reply |
|---|---|
| **0 hops**, `autoreply.direct.flood on` (default) | Flooded, scoped as below. Reaches you even where you cannot hear this repeater directly. |
| **0 hops**, `autoreply.direct.flood off` | Sent zero-hop. One packet, and no repeater will ever retransmit it. Costs the mesh nothing. |
| **1+ hops, region-scoped** | Flooded back inside that same scope only. |
| **1+ hops, unscoped** | Flooded. **This is the expensive case** — see below. |

**Why a direct request is flooded by default.** Hearing you is no promise you can hear the
answer. A repeater usually sits in far quieter RF than a handheld does, so it decodes
signals that never make it back the other way — the link is asymmetric even though the
antennas are not. A zero-hop reply gets exactly one transmission and no second chance, so
where that asymmetry exists the requester sees silence. Flooding lets a repeater you *can*
hear carry the answer back. Turn it off on dense sites where the extra packet is not worth
it, or where most requesters are known to be in solid two-way range.

**Understand the multi-hop cost before raising `autoreply.hops`.** A request that arrives
from several hops away can only be answered by flooding, and *every* repeater that heard it
answers. One `test` therefore becomes one flood packet per repeater in range, each
propagating as far as `flood.max` allows. On a busy mesh that is real airtime.

What bounds it:

* `autoreply.hops` — the single most effective control. `0` answers only direct neighbours.
  A small value such as `2` or `3` keeps replies regional.
* `autoreply.direct.flood off` — the only setting that produces no flood whatsoever, and
  only in combination with `autoreply.hops 0`. Either one alone still floods.
* The rate limits (below).
* Each repeater's own `flood.max` / `flood.max.unscoped`.

Scoping the request is still much cheaper, because the reply stays inside that scope.
Note a stock client has no per-channel scope setting — it applies a device-wide default
scope and sends unscoped when none is set (see the `// TODO: have per-channel send_scope`
in the companion firmware), so unscoped is what most meshes actually carry.

To use the cheaper scoped path, pair the channel with a region of the same name:

```
region def #test-sto
region allowf #test-sto
```

Region keys are derived from the name in the same way, so `#test-sto` needs no key
distribution either. See [CLI commands](cli_commands.md) for the full `region` syntax.

## Possible improvement: reply direct along the reverse path

The multi-hop case currently costs a flood reply per repeater in range. It should be
possible to avoid flooding entirely by replying **direct along the reverse of the path the
request arrived by** — one packet per hop, targeted, reaching the sender without touching
the rest of the mesh. That would make `autoreply.hops` a range setting rather than a cost
setting.

The pieces are already there:

* A flood packet accumulates the hash of every repeater it crosses, appended in order
  (`self_id.copyHashTo(&packet->path[n * hash_size], ...)`, `src/Mesh.cpp:349`). So an
  inbound request carries the full route from the sender to us.
* Direct routing forwards *any* payload type, group text included: a `ROUTE_TYPE_DIRECT`
  packet with a non-empty path is matched against `path[0]`, forwarded, and the hop removes
  itself (`src/Mesh.cpp:78-108`). When the path is exhausted the payload is handled
  normally, so any node in range holding the channel key will decrypt it.
* `sendDirect(pkt, path, path_len, delay)` already takes an explicit path.

What needs care before trusting it:

* **Order.** The inbound path is `[R1, R2, R3]` from the sender's side, where `R1` is the
  sender's neighbour. Sending back means `[R3, R2, R1]`, so the path must be reversed —
  unlike a client's `out_path`, which is used as-is.
* **Hash size.** `getPathHashSize()` is 1-4 bytes per entry and the reversal must move whole
  entries, not bytes. Keep within `MAX_PATH_SIZE`.
* **Asymmetry.** A route that worked one way may not work back; a flood reply finds its own
  way, a direct reply does not. A fallback is probably still wanted.
* **Verify on hardware** that a direct-routed `PAYLOAD_TYPE_GRP_TXT` is displayed by stock
  clients — group messages are normally flooded, so this path is unexercised.

## A reply can never trigger a reply

Every repeater in range answers the same trigger, so the one thing the feature must never
do is answer itself. Two repeaters that each took the other's reply for a request would
answer each other until the rate limit stopped them, every five minutes, for ever.

On the channel this holds by the shape of the text, not by any flag. A request is the
keyword at the start of the message — `test`, or `test` and an id, or those and a mode
letter — and a reply always begins with the node's name, then the bracketed requester, then
the measurements. Nothing a repeater sends starts with the keyword, however the tail is
parsed, and the host suite pins this with real reply bodies (`AReplyCanNeverTriggerAReply`
in `test/test_autoreply`), including a requester whose own node is named `test`.

A private reply is safer still, for a structural reason. It is a text message addressed to
one key, and a repeater acts on an incoming text message only from a node already in its
own access list as an admin — anything else is dropped before it is parsed, and a repeater
is never another repeater's admin. So a private reply that reaches a peer repeater is not
a request that fails to match; it is a packet that is never read as one.

## What it costs

Be honest with yourself about the traffic before enabling this:

- One trigger produces **up to one flood reply per repeater in range**, each propagating
  across the configured scope.
- Two rate limits apply, both per repeater:
  - **Per sender** — one reply per sender per 5 minutes. The last 16 senders are remembered
    in a ring; the oldest is forgotten when a 17th appears. Senders are identified by the
    name prefix a group message carries, folded to lower case, so this is a courtesy limit
    rather than a security control — a name is trivially spoofed, and two people using the
    same name share a slot.
  - **Global** — 10 replies per 5 minutes in total. The per-sender check runs first, so one
    person retrying cannot spend everyone else's share.
  - Both are fixed windows, not cooldowns: the budget refills all at once when the window
    ends, and the window starts at the first reply after an idle gap.
- Replies are staggered by a random delay, scaled by `f_txdelay`, so nearby repeaters do
  not transmit on top of each other. Setting `f_txdelay` to `0` removes that stagger.
- Only an exact, case-insensitive match on the keyword triggers a reply, so ordinary chat
  on the channel is ignored — and a reply can never trigger another reply.

If several repeaters you own cover the same area, consider enabling this on only one of
them. Do not enable it on a channel that another bot already answers.
