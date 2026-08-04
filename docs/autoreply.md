# Repeater auto-reply

A repeater can answer a keyword on one channel with a signal report, so anyone can check
coverage by sending a single message — no login, no contact setup, no administration.

```
you:    test
STO-1:  SNR 6.5 RSSI -92 2h A3,1B
STO-2:  SNR -4.0 RSSI -104 0h direct
```

The reply gives the SNR and RSSI the repeater heard you at, the hop count, and the hex
hashes of the repeaters your message travelled through.

**It is off by default, and it should stay off unless you have read this page.** Every
repeater in range answers the same trigger, so one message becomes one flood reply per
repeater. That is why replies are only sent for region-scoped requests.

## Setup

```
set autoreply.channel #test-STO
set autoreply.hops 8
```

Then add the `#test-STO` channel in your client and send `test` to it. Every repeater in
direct range answers. Nothing else is needed for a range check.

To also get answers from repeaters **more than one hop away**, the request has to carry a
region scope — see [How far the reply travels](#how-far-the-reply-travels) below.

To turn it back off:

```
set autoreply.channel
```

## Commands

#### Set the channel the repeater listens on

- `get autoreply.channel`
- `set autoreply.channel <#name>`

**Parameters:**

- `#name`: a hashtag channel name, up to 31 characters, starting with `#`. An empty value
  disables the feature.

**Default:** empty (disabled)

**Note:** the channel key is derived from the name — the first 16 bytes of `sha256("#name")`
— so there is no PSK to configure or share. Any client that adds a channel of the same name
gets the same key.

**Note:** the name is folded to **lower case** when you set it. The key is the hash of the
name exactly as stored, and clients only accept lower-case channel names, so an upper-case
name would produce a channel nobody could join. `set autoreply.channel #test-STO` therefore
stores `#test-sto`, and the command echoes back what it saved — type that into your client.

Region names are **not** folded, so if you pair the channel with a region for the multi-hop
path, spell the region exactly the same on every node.

The trigger keyword is case-insensitive: `test`, `Test` and `TEST` all work. Many phone
keyboards capitalise the first letter, so this matters in practice.

**Note:** `#public`, `#test` and `#bot` are rejected. They are shared mesh-wide, so a reply
from every repeater on them is spam. Use a regional name, such as your IATA code.

#### Limit how far away a request can be

- `get autoreply.hops`
- `set autoreply.hops <value>`

**Parameters:**

- `value`: maximum hop count (0-63) of a request that will be answered. `0` answers only
  direct neighbours.

**Default:** `8`

#### Show the current state

- `get autoreply`

## How far the reply travels

A channel name does not limit propagation. Repeaters forward on the packet header alone —
route type, hop count against `flood.max`, transport code — and the channel lives in the
encrypted payload, which a repeater does not read. A message to `#test-STO` therefore costs
exactly the same airtime, over exactly the same hops, as one to `#test`. Naming the channel
per region keeps it out of other people's message lists; it does not keep it off their air.

What does limit propagation is the region transport code. So the repeater answers in one of
two ways, and stays silent if it can do neither:

| Request arrived | Reply |
|---|---|
| **0 hops** (you are in direct range) | Sent zero-hop. One packet, and no repeater will ever retransmit it. Costs the mesh nothing. |
| **1+ hops, region-scoped** | Flooded back inside that same scope only. |
| **1+ hops, unscoped** | Flooded. **This is the expensive case** — see below. |

**Understand the multi-hop cost before raising `autoreply.hops`.** A request that arrives
from several hops away can only be answered by flooding, and *every* repeater that heard it
answers. One `test` therefore becomes one flood packet per repeater in range, each
propagating as far as `flood.max` allows. On a busy mesh that is real airtime.

What bounds it:

* `autoreply.hops` — the single most effective control. `0` answers only direct neighbours
  and can never flood at all. A small value such as `2` or `3` keeps replies regional.
* The rate limits (below).
* Each repeater's own `flood.max` / `flood.max.unscoped`.

Scoping the request is still much cheaper, because the reply stays inside that scope.
Note a stock client has no per-channel scope setting — it applies a device-wide default
scope and sends unscoped when none is set (see the `// TODO: have per-channel send_scope`
in the companion firmware), so unscoped is what most meshes actually carry.

To use the cheaper scoped path, pair the channel with a region of the same name:

```
region def #test-STO
region allowf #test-STO
```

Region keys are derived from the name in the same way, so `#test-STO` needs no key
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

## What it costs

Be honest with yourself about the traffic before enabling this:

- One trigger produces **up to one flood reply per repeater in range**, each propagating
  across the configured scope.
- Two rate limits apply, both per repeater:
  - **Per sender** — one reply per sender per 5 minutes. The last 32 senders are remembered
    in a ring; the oldest is forgotten when a 33rd appears. Senders are identified by the
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
