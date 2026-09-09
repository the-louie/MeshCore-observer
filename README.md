# 📡 MeshCore Observer — with repeater auto-reply and signed remote control

**This is the standard [MeshCore Observer](https://observer.gessaman.com/) firmware, plus two
features.** Everything Observer does is still here and unchanged — MQTT uplink to all six
broker slots, WiFi and timezone config, fault alerts, the web flasher config portal. If you
already run an Observer repeater, nothing you have set up changes.

The two additions:

1. **Auto-reply.** Your repeaters answer the word `test` with a signal report. Anyone in
   range can check coverage by sending one message. No login, no contact setup, no admin
   rights, nothing to install.
2. **Remote control over MQTT.** You can change a repeater's settings, or ask it to send a
   test, with a signed message published to the broker — instead of driving to it. **Off by
   default**, and inert until you provision a key. Read the next section before you turn it
   on; it is the part of this firmware that deserves your attention.

```
you        test
STO-1      [louie] SNR 6.5 RSSI -92 0h direct
STO-2      [louie] SNR -4.0 RSSI -104 2h A3,1B
```

Each repeater that hears you replies with the SNR and RSSI it heard you at, how far away it
is in hops, and which repeaters your message travelled through. Your name comes back in
brackets, so when several people test at once everyone can find their own reply.

---

## Remote control, and the key that secures it

Flashing a fleet takes months. This exists so that changing a setting afterwards costs a
signed publish rather than another round of climbing towers.

**Start from the threat model, because it is unusual.** Public MeshCore MQTT brokers publish
their credentials — `mqtt.meshat.se` does. **Anyone can publish to any topic on them.** So
this feature trusts the transport with nothing at all. Reaching a node's command topic is not
a permission; it is the starting position everyone already has.

### Two separate keypairs. Do not confuse them.

| | **Your node's mesh identity** | **Your owner signing key** |
|---|---|---|
| What it is | The repeater's own keypair, the one it has always had | A keypair **you** generate, for commanding your fleet |
| What it does | Mesh addressing, encryption, direct messages | Signs remote commands |
| Where the **private** half lives | On the node | **With you. Never on a node, never at the broker.** |
| Where the **public** half lives | Advertised on the mesh | On each node you provision, as `mqtt.owner` |

**A node never holds a secret that can command anything — not even itself.** It stores 64 hex
characters: the *public* half of your signing key. That is enough to check a signature and
useless for making one. Somebody who steals a repeater, dumps its flash, or reads every byte
the broker has ever carried still cannot issue a single command to any node, including the one
they took apart.

This is a different key from the node's mesh identity, on purpose. The mesh identity is
published — it is how the node is addressed. A published key cannot also be the key that
authorises commands.

### Turning it on

```
set mqtt.owner <64 hex characters>    # serial only — the PUBLIC half of your signing key
set mqtt.cmd on
get mqtt.cmd                          # state, and the highest command counter seen
```

Both are needed. `mqtt.cmd on` with no owner key refuses everything. **`set mqtt.owner` cannot
be done remotely** — only over serial or the local console — so a remote attacker cannot point
a node at their own key.

To turn it off again: `set mqtt.cmd off`.

### What a node checks before it obeys

In this order, and it stops at the first failure:

1. the feature is on, and an owner key is provisioned
2. the **Ed25519 signature** verifies against that key — over the topic *and* the payload
   together, so a command signed for one node cannot be replayed at another, and a per-node
   command cannot be replayed as a broadcast
3. the **counter** is strictly greater than the highest one this node has stored — a captured
   command cannot be replayed later. The counter is stored *before* the command runs
4. the command has **not expired**, and a node with no trusted clock fails closed
5. the **rate limit** — 20 commands an hour, of which at most 4 may transmit. This applies to
   *validly signed* commands: even your own key cannot turn a repeater into a flood weapon
6. the command is on a short **allowlist** — the auto-reply settings, a few read-only `get`s,
   and the two `trigger` commands. Everything else is refused before it reaches the CLI.
   There is no general `set`, no `erase`, and no way to change the owner key

Two things are deliberately out of reach of the remote path no matter what it carries:
`set prv.key`, `erase` and `set mqtt.owner`, which require a locally-privileged caller; and a
broadcast that would make every node transmit at once.

### What this does not defend against

**Availability.** Anyone can flood the broker, and the node's rate limit is what bounds the
airtime that costs you — not the broker traffic. **Confidentiality of commands.** The envelope
is signed, not encrypted: anyone watching the broker can read what you asked a node to do.
Nothing secret should travel this way, and nothing does.

Full detail, including the wire format and every refusal code, is in
[`docs/mqtt-control.md`](docs/mqtt-control.md).

---

## Auto-reply: turning it on — three steps

**1. Flash a repeater with this firmware.** Grab the file for your board from this repo's
[Releases](../../releases) page.

* **Upgrading a node you already run?** Use the plain `….bin` at offset `0x10000` — it keeps
  your settings, node identity, MQTT slots and WiFi credentials.
* **Starting fresh, or unsure?** Use `…-merged.bin` at offset `0x0`. It wipes the device and
  you set it up from scratch.

**2. Connect a console.** USB serial at **115200 baud**, or the repeater's admin console in
your MeshCore client app if you are already logged in to it remotely.

**3. Type one command:**

```
set autoreply on
```

That is it. Check it took:

```
get autoreply
> on #test-sto 'test' max 8 hops, direct flood
```

Now add the channel it named — `#test-sto` above — in your MeshCore client, send `test` to
it, and every repeater in range answers.

> **If `get autoreply` says `no region - set mqtt.iata`**, the node has no region code yet.
> Set the airport code nearest you and the channel appears: `set mqtt.iata STO`. Observer
> nodes that already publish to MQTT have this set, because it is what they publish under.

To turn it off again: `set autoreply off`.

## Settings

| Command | Default | What it does |
|---|---|---|
| `set autoreply on\|off` | `off` | Whether this repeater answers the trigger. |
| `set autoreply.hops <0-63>` | `8` | How many hops away a request may be and still get an answer. `0` = direct neighbours only. |
| `set autoreply.direct.flood on\|off` | `on` | Whether a request that arrived direct is answered with a flood. `off` sends a single zero-hop packet that no repeater relays — cheapest, but it never reaches anyone who cannot hear this repeater directly. |
| `set autoreply.delay <1-120>` | `4` | How widely replies are spread in time. Every repeater that hears a request is about to answer it, and without a spread their replies land on top of each other. The wait is random, up to roughly 2.3 s x this number — so `4` spreads answers over about 9 seconds and `120` over about four and a half minutes. Nobody is waiting on a test, so err high: with a dozen repeaters answering, `4` is far too tight. |
| `set autoreply.region <code>` | *(request scope)* | Which region replies are scoped to. Empty mirrors whatever scope the request arrived under. |
| `set autoreply.mode flood\|private\|direct` | `flood` | How a request that names no mode is answered. `private` and `direct` answer with a text message to the requester alone, and only when the request carries their key. |
| `set autoreply.private on\|off` | `off` | Whether a test request sent to this repeater alone, off the channel, is answered. Off because it is an unauthenticated packet that makes the node transmit. |
| `get autoreply` | — | Current state, in one line. |
| `get autoreply.channel` | — | The channel name. **Read-only** — it follows `set mqtt.iata`. |
| `get autoreply.hops` | — | Read the hop limit back. |
| `get autoreply.direct.flood` | — | Read the direct-reply mode back. |
| `get autoreply.delay` | — | Read the spread back. |
| `get autoreply.region` | — | Read the reply scope back. |
| `get autoreply.mode` | — | Read the standing mode back. |
| `get autoreply.private` | — | Read the private-request switch back. |

The trigger word is `test`, case-insensitive — `test`, `Test` and `TEST` all work, which
matters because phone keyboards like to capitalise the first letter. Override it at build
time with `-D AUTOREPLY_KEYWORD='"..."'`.

Remote control reaches all of these: `set autoreply` and `get autoreply` are on the allowlist
as families, so `set autoreply.delay 30` can be published to a node instead of typed into it.

---

## Reply modes — asking for the answer you want

A bare `test` is answered the way the node is configured, and that is all most people need.
A request can also **name how it wants to be answered**, which is what the modes are for:

```
test                     answered the node's standing way
test A1B2C3D4            the same, with an id echoed back so you can match reply to request
test A1B2C3D4 F          answer on the channel, where everyone sees it
test A1B2C3D4 P          answer me privately
test A1B2C3D4 D          answer me privately, straight back down the path this came in on
test A1B2C3D4 S          do not answer at all
```

| Mode | What the repeater does | What it costs the mesh |
|---|---|---|
| **F — Flood** | Answers on `#test-<iata>`, where everyone in the region sees it. The default, and what every node did before modes existed. | The most: one flooded reply per repeater that heard you. |
| **P — Private** | Answers with a text message addressed to you alone, flood-routed so it finds you wherever you are. | One reply, but it still floods to find a path. |
| **D — Private Direct** | The same private answer, sent straight back down the path your request arrived on. | The least of the three that answer — no flood at all. The trade: the reverse path can fail where a flood would not, because this mesh is asymmetric. |
| **S — Silent** | Answers nothing. The node still *heard* you, and still reports the reception over its MQTT uplink. | Nothing on the air. |

**P and D need your public key**, appended to the request, because a channel message carries
no sender key and a repeater has no name-to-key table to look you up in. A `P` or `D` request
without one is answered as **F** rather than dropped — a probe is never left unanswered
because it asked for something the node could not do.

**S is per-request only.** There is no standing "silent" mode, because that is what
`set autoreply off` already is.

The standing mode is `set autoreply.mode flood|private|direct`. A request that names a mode
overrides it for that request only.

### Mode M — a test that never touches the channel

The modes above all *arrive* on `#test-<iata>`, in the open. **Mode M is the private
counterpart**: the request is sent to one repeater alone, off the channel entirely, and is
answered the same way. Nothing about the exchange appears on the test channel.

This sidesteps the key problem that P and D have, because the request packet carries the
sender's key by construction — there is nothing to append and nothing to get wrong.

It is **off by default** (`set autoreply.private on`), and it is the one switch here worth
thinking about before you flip it: unlike a channel request, it is an unauthenticated packet
sent to your node alone that makes it transmit. The guards are a cooldown charged to the
requester's *key* rather than a name — one answer per key per five minutes — and a budget for
the private path of its own, ten answers per five minutes, which is what bounds somebody
cycling through fresh keys. A request refused by either gets nothing back.

Full detail on all five in [`docs/autoreply.md`](docs/autoreply.md).

---

## Why the channel is `#test-<iata>` and nothing else

You cannot choose the channel. It is always `#test-` followed by your node's region code, in
lower case — `#test-sto`, `#test-jkg`. That is a deliberate restriction, for three reasons.

**One badly chosen channel would spam the whole mesh.** Every repeater in range answers the
same message. Point that at `#public`, `#test` or `#bot` — channels everybody carries — and
one person typing `test` sets off a reply from every repeater that heard it, mesh-wide,
forever. A region code can never produce one of those names, so the mistake is impossible to
make rather than merely discouraged.

**A per-region channel keeps the traffic where it is useful.** Your coverage test is
interesting to people near you and noise to everyone else. Naming the channel after the
region keeps it out of other people's message lists.

**There is nothing to distribute.** The channel key is the first 16 bytes of
`sha256("#test-sto")`, so there is no PSK to generate, share or keep in sync. Anyone who
knows the region code can join by typing the name into their client — that is the whole
point, since the person testing coverage is usually a stranger to the repeater's owner.

Lower case is not a style choice: clients only accept lower-case channel names, and the key
is the hash of the name exactly as stored, so an upper-case name would hash to a channel
nobody could join.

## Why it will not flood your mesh

Every repeater in range answers the same message, which is exactly the shape of a broadcast
storm. Five mitigations keep it cheap:

**A direct reply you can make free.** If your request arrived directly, the repeater can
answer zero-hop: a single packet that no repeater will ever retransmit, costing the mesh one
transmission and nothing more. That is `set autoreply.direct.flood off`.

It is not the default, because hearing you is no promise you can hear the answer. A repeater
usually sits in far quieter radio noise than a handheld does, so it decodes signals that
never make it back the other way — the link is asymmetric even when the antennas are not. A
zero-hop reply gets one transmission, no acknowledgement and no alternative path, so where
that gap exists you simply see silence. Flooding lets a repeater you *can* hear carry the
answer back. Turn it off where airtime is tight, or where the people testing are known to be
in solid two-way range.

**A hop limit on what gets answered at all.** A request from several hops away can only be
answered by flooding, and *every* repeater that heard it floods its own reply — so one `test`
becomes one flood packet per repeater, each propagating as far as `flood.max` allows.
`set autoreply.hops` bounds this, and it is the setting that matters. `0` answers only direct
neighbours, and paired with `autoreply.direct.flood off` it puts no flood on the mesh at all.

**A per-sender cooldown.** One reply per sender per five minutes, with the last 16 senders
remembered. Someone hammering `test` gets one answer and then silence, and — because the
per-sender check runs *before* the shared limit — their retries cannot use up anyone else's
share. A group standing in a field testing together all get answers.

**A global rate limit.** Ten replies per repeater per five minutes, whatever the source.
Whatever else happens, that is the ceiling on what one repeater will put on the air.

**Random stagger.** Replies are delayed by a random interval, so neighbouring repeaters
answering the same message do not transmit on top of each other and corrupt each other's
packets.

If several of your repeaters cover the same area, consider enabling this on only one of them.

📖 Full reference, the multi-hop region-scope details and the test procedure:
**[docs/autoreply.md](./docs/autoreply.md)**

---

## About MeshCore

MeshCore is a lightweight, portable C++ library that enables multi-hop packet routing for embedded projects using LoRa and other packet radios. It is designed for developers who want to create resilient, decentralized communication networks that work without the internet.

## 🔍 What is MeshCore?

MeshCore now supports a range of LoRa devices, allowing for easy flashing without the need to compile firmware manually. Users can flash a pre-built binary using tools like Adafruit ESPTool and interact with the network through a serial console.
MeshCore provides the ability to create wireless mesh networks, similar to Meshtastic and Reticulum but with a focus on lightweight multi-hop packet routing for embedded projects. Unlike Meshtastic, which is tailored for casual LoRa communication, or Reticulum, which offers advanced networking, MeshCore balances simplicity with scalability, making it ideal for custom embedded solutions, where devices (nodes) can communicate over long distances by relaying messages through intermediate nodes. This is especially useful in off-grid, emergency, or tactical situations where traditional communication infrastructure is unavailable.

> **MQTT Observer Setup** — Prebuilt observer firmware, docs, and a changelog are at
> [observer.gessaman.com](https://observer.gessaman.com/). See the
> [MQTT Implementation Guide](./MQTT_IMPLEMENTATION.md) for configuration, CLI commands, and
> troubleshooting.

## ⚡ Key Features

* Multi-Hop Packet Routing
  * Devices can forward messages across multiple nodes, extending range beyond a single radio's reach.
  * Supports up to a configurable number of hops to balance network efficiency and prevent excessive traffic.
  * Nodes use fixed roles where "Companion" nodes are not repeating messages at all to prevent adverse routing paths from being used.
* Supports LoRa Radios – Works with Heltec, RAK Wireless, and other LoRa-based hardware.
* Decentralized & Resilient – No central server or internet required; the network is self-healing.
* Low Power Consumption – Ideal for battery-powered or solar-powered devices.
* Simple to Deploy – Pre-built example applications make it easy to get started.

## 🎯 What Can You Use MeshCore For?

* Off-Grid Communication: Stay connected even in remote areas.
* Emergency Response & Disaster Recovery: Set up instant networks where infrastructure is down.
* Outdoor Activities: Hiking, camping, and adventure racing communication.
* Tactical & Security Applications: Military, law enforcement, and private security use cases.
* IoT & Sensor Networks: Collect data from remote sensors and relay it back to a central location.

## 🚀 How to Get Started

- Watch the [MeshCore QuickStart Playlist](https://www.youtube.com/watch?v=iaFltojJrAc&list=PLshzThxhw4O4WU_iZo3NmNZOv6KMrUuF9) by The Comms Channel
- Watch the [MeshCore Technical Presentation](https://www.youtube.com/watch?v=OwmkVkZQTf4) by Liam Cottle.
- Read through our [Frequently Asked Questions](./docs/faq.md) and [Documentation](https://docs.meshcore.io).
- Flash the MeshCore firmware on a supported device.
- Connect with a supported client.

For developers:

- Install [PlatformIO](https://docs.platformio.org) in [Visual Studio Code](https://code.visualstudio.com).
- Clone and open the MeshCore repository in Visual Studio Code.
- See the example applications you can modify and run:
  - [Companion Radio](./examples/companion_radio) - For use with an external chat app, over BLE, USB or Wi-Fi.
  - [KISS Modem](./examples/kiss_modem) - Serial KISS protocol bridge for host applications. ([protocol docs](./docs/kiss_modem_protocol.md))
  - [Simple Repeater](./examples/simple_repeater) - Extends network coverage by relaying messages.
  - [Simple Room Server](./examples/simple_room_server) - A simple BBS server for shared Posts.
  - [Simple Secure Chat](./examples/simple_secure_chat) - Secure terminal based text communication between devices.
  - [Simple Sensor](./examples/simple_sensor) - Remote sensor node with telemetry and alerting.

The Simple Secure Chat example can be interacted with through the Serial Monitor in Visual Studio Code, or with a Serial USB Terminal on Android.

## ⚡️ MeshCore Flasher

We have prebuilt firmware ready to flash on supported devices.

- Launch https://meshcore.io/flasher
- Select a supported device
- Flash one of the firmware types:
  - Companion, Repeater or Room Server
- Once flashing is complete, you can connect with one of the MeshCore clients below.

## 📱 MeshCore Clients

**Companion Firmware**

The companion firmware can be connected to via BLE, USB or Wi-Fi depending on the firmware type you flashed.

- Web: https://app.meshcore.nz
- Android: https://play.google.com/store/apps/details?id=com.liamcottle.meshcore.android
- iOS: https://apps.apple.com/us/app/meshcore/id6742354151?platform=iphone
- NodeJS: https://github.com/liamcottle/meshcore.js
- Python: https://github.com/fdlamotte/meshcore-cli

**Repeater and Room Server Firmware**

The repeater and room server firmware can be set up via USB in the web config tool.

- https://config.meshcore.io

They can also be managed via LoRa in the mobile app by using the Remote Management feature.

## 🛠 Hardware Compatibility

MeshCore is designed for devices listed in the [MeshCore Flasher](https://meshcore.io/flasher)

## 📜 License

MeshCore is open-source software released under the MIT License. You are free to use, modify, and distribute it for personal and commercial projects.

## Contributing

Please submit PR's using 'dev' as the base branch!
For minor changes just submit your PR and we'll try to review it, but for anything more 'impactful' please open an Issue first and start a discussion. It is better to sound out what it is you want to achieve first, and try to come to a consensus on what the best approach is, especially when it impacts the structure or architecture of this codebase.

Here are some general principles you should try to adhere to:
* Keep it simple. Please, don't think like a high-level lang programmer. Think embedded, and keep code concise, without any unnecessary layers.
* No dynamic memory allocation, except during setup/begin functions.
* Use the same brace and indenting style that's in the core source modules. (A .clang-format is probably going to be added soon, but please do NOT retroactively re-format existing code. This just creates unnecessary diffs that make finding problems harder)

Help us prioritize! Please react with thumbs-up to issues/PRs you care about most. We look at reaction counts when planning work.

### Running unit tests

To run unit tests, run the following command:

```bash
pio test --environment native --verbose
```

## Road-Map / To-Do

There are a number of fairly major features in the pipeline, with no particular time-frames attached yet. In very rough chronological order:
- [X] Companion radio: UI redesign
- [X] Repeater + Room Server: add ACL's (like Sensor Node has)
- [X] Standardise Bridge mode for repeaters
- [ ] Repeater/Bridge: Standardise the Transport Codes for zoning/filtering
- [X] Core + Repeater: enhanced zero-hop neighbour discovery
- [ ] Core: round-trip manual path support
- [ ] Companion + Apps: support for multiple sub-meshes (and 'off-grid' client repeat mode)
- [ ] Core + Apps: support for LZW message compression
- [ ] Core: dynamic CR (Coding Rate) for weak vs strong hops
- [ ] Core: new framework for hosting multiple virtual nodes on one physical device
- [ ] V2 protocol spec: discussion and consensus around V2 packet protocol, including path hashes, new encryption specs, etc

## 📞 Get Support

- Report bugs and request features on the [GitHub Issues](https://github.com/ripplebiz/MeshCore/issues) page.
- Find additional guides and components on [my site](https://buymeacoffee.com/ripplebiz).
- Join [MeshCore Discord](https://meshcore.gg) to chat with the developers and get help from the community.
