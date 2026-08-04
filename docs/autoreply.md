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

Then, on the client side, add the `#test-STO` channel and give it the matching region
scope. Sending `test` to that channel gets a reply from every repeater that heard it.

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

## Why the region scope is required

A channel name does not limit propagation. Repeaters forward on the packet header alone —
route type, hop count against `flood.max`, transport code — and the channel lives in the
encrypted payload, which a repeater does not read. A message to `#test-STO` therefore costs
exactly the same airtime, over exactly the same hops, as one to `#test`. Naming the channel
per region keeps it out of other people's message lists; it does not keep it off their air.

What does limit propagation is the region transport code. So the repeater **only answers a
request that carries a region scope**, and sends its reply back into that same scope. An
unscoped `test` is silently ignored — no reply, from anyone.

Pair the channel with a region of the same name:

```
region def #test-STO
region allowf #test-STO
```

Region keys are derived from the name in the same way, so `#test-STO` needs no key
distribution either. See [CLI commands](cli_commands.md) for the full `region` syntax.

## What it costs

Be honest with yourself about the traffic before enabling this:

- One trigger produces **up to one flood reply per repeater in range**, each propagating
  across the configured scope.
- Replies are rate limited to 2 per 5 minutes per repeater.
- Replies are staggered by a random delay, scaled by `f_txdelay`, so nearby repeaters do
  not transmit on top of each other. Setting `f_txdelay` to `0` removes that stagger.
- Only an exact, case-insensitive match on the keyword triggers a reply, so ordinary chat
  on the channel is ignored — and a reply can never trigger another reply.

If several repeaters you own cover the same area, consider enabling this on only one of
them. Do not enable it on a channel that another bot already answers.
