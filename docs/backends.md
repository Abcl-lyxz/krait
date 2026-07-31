# Backends

A backend is what a tab is connected to. Every one of them implements the same
`IBackend` contract (`src/net/ibackend.h`) and passes the same battery, so from
the terminal's side they are interchangeable — that sameness is the point, not
a coincidence.

Which one a session uses is the `backend` key in `sessions.toml`:

```toml
[[session]]
id = "web-1-prod"
name = "Web 1 (prod)"
folder = "prod/eu"
backend = "ssh"       # "conpty" | "ssh" | "telnet"
host = "web1.example.com"
port = 22
user = "deploy"
auth = "auto"         # auto | agent | password | publickey | keyboard-interactive
accent = "#f38ba8"    # prod = red is a UX invariant, not decoration
```

An unrecognised `backend` value falls back to `conpty` — which connects to
nothing — and the load reports it, rather than quietly dialling somewhere.

## conpty — the local shell

The default, and what a window with no arguments opens. Runs the shell over
the **bundled** OpenConsole (ADR-0011), never the inbox one: conhost is years
stale on any given machine and its DCS passthrough has holes.

| Key | Meaning |
|---|---|
| `command` | The shell to spawn. Empty means PowerShell. |

`command` is resolved to an absolute path before it is launched, and quoted in
the command line. That is not ceremony: `CreateProcessW`'s own search — the one
it uses when `lpApplicationName` is null — looks in the calling process's
directory and then the *current* directory before System32, so a planted
`powershell.exe` beside a downloaded file would win. Microsoft's documented
mitigation is exactly this, and `SearchPathW` gets safe search mode turned on
first because it has the same default weakness.

Reconnect: none. A shell that exits has exited; reopening it under someone
would be a bug.

## ssh — libssh

ADR-0002. One worker thread owns the whole libssh session, because a libssh
session is not thread-safe and the alternative is a lock around every call plus
the bug that shows up a week later.

| Key | Meaning |
|---|---|
| `host`, `port`, `user` | Where and as whom. Port defaults to 22. |
| `auth` | Which method to prefer. `auto` walks agent, key, keyboard-interactive, password. |
| `key_path` | Private key for `publickey`. |

Credentials are never in this file. Passwords and passphrases live in the
Windows DPAPI vault, keyed by the profile's `id` — which is pinned on first
save precisely so that renaming a session does not orphan its stored secret.

Host keys are never auto-accepted. An unknown key gets the TOFU screen with
fingerprint and randomart; a **changed** key is refused outright and shown as a
blocking banner with no Trust button, because that is not a question. There is
no setting anywhere that turns it back into one.

Reconnect: on retryable failures only, with backoff. Never after a changed host
key, a rejected key, or a bad password — retrying those is how a lockout
happens.

**Jump hosts** use the `proxy_jump` key, spelled the way OpenSSH spells
ProxyJump: `bastion`, or `me@bastion:2222,inner` for a chain. libssh walks the
chain in-process (ADR-0012) — nothing shells out to `ssh.exe`, and there are no
direct-tcpip channels of ours to get wrong.

The point of doing it this way is that Krait's own host-key and auth UX runs on
EVERY hop, through libssh's per-hop callbacks. A bastion whose key changed
matters exactly as much as the target's, and a refused key aborts the whole
chain rather than letting the session's credentials travel through a machine
nobody vouched for. The banner names which hop is asking, because "trust this
key?" is unanswerable without knowing whose.

Known gap: `mlkem768x25519-sha256` is disabled. libssh 0.12 advertises it and
OpenSSH 10 prefers it, but the two ends negotiate it and the client then fails
before sending its KEX init. `src/net/ssh/algorithms.h` records the evidence.
This is a postponement, re-tested on the next libssh bump.

### The agent, and why it needed a bridge (T60)

libssh cannot reach the agent that ships with Windows, and the way it failed is
the interesting part.

libssh's agent client has exactly one transport: read `SSH_AUTH_SOCK` (or
`SSH_OPTIONS_IDENTITY_AGENT`), then `socket(AF_UNIX)` + `connect`. There is no
named-pipe path in its `src/agent.c` on any platform. The OpenSSH agent on
Windows listens on `\\.\pipe\openssh-ssh-agent` and nothing else. So on a stock
Windows box `ssh_userauth_agent()` reached nothing — and reported that as
`SSH_AUTH_DENIED`, which is also what libssh returns when the server refused
every key. The ladder fell through to a password prompt and nothing looked
broken. That is why the code carried a comment claiming this already worked.

`src/net/ssh/agent_bridge.cpp` supplies the missing transport. libssh exposes
one seam, `ssh_set_agent_socket(session, fd)`, which drops an fd into the
session's agent socket; `ssh_agent_is_running()` is then satisfied by that
socket merely HAVING an fd. So the bridge hands libssh one end of a verified
loopback socket pair and relays the agent protocol between the other end and the
pipe. libssh owns that fd from then on and closes it in `ssh_free`.

Three things about it are load-bearing:

- **The relay follows the framing** (uint32 big-endian length, then that many
  bytes) rather than copying bytes blindly. That costs one thread where a blind
  copy needs two, and it puts the 256 KiB cap somewhere.
- **The socket pair is checked.** A loopback listener is briefly connectable by
  any process on the machine, and what sits behind it is the user's ssh-agent.
  The accepted connection is verified to be the one we dialled before either end
  is used.
- **Pipe I/O is overlapped, and cancellable.** `CancelIoEx` marks only the I/O
  already outstanding, so a relay caught between "wrote the request" and "about
  to read the reply" would issue an uncancellable read a moment later. Closing a
  tab joins the SSH worker from the GUI thread, and that worker can be blocked
  in libssh's `recv()` waiting on an agent — for a security key, "until somebody
  touches it". `AgentBridge::cancel()` is what reaches it.

`SSH_AUTH_SOCK` still wins when it is set: setting it is a deliberate act, and
the pipe is simply there on every Windows machine.

### Certificates and security keys (T61, ADR-0014)

**User certificates work.** `ssh_userauth_publickey_auto` finds a
`<key>-cert.pub` sibling by itself, and this backend does the same for a key
named by `key_path`. A certificate kept somewhere else goes in `cert_path`,
which becomes `SSH_OPTIONS_CERTIFICATE`. That option MUST be set before
`ssh_connect` — it only appends to libssh's unexpanded list, and `ssh_connect`
is what calls the internal `ssh_options_apply` that moves it to the list the
auth code reads. Set afterwards it is accepted, returns `SSH_OK`, and is never
used. The certificate is tried only after the plain key is refused, which is
libssh's own order and means a stale certificate costs nothing.

**Host certificates are NOT verified.** libssh's `knownhosts.c` skips
`@cert-authority` and `@revoked` lines at parse time, and
`ssh_session_is_known_server` is not CA-aware. A host presenting a CA-signed key
is treated as an unknown host and goes through the normal TOFU flow.

**FIDO2 security keys work through the agent, and only through the agent.** The
libssh this build links is compiled `WITH_FIDO2=OFF` — the API is declared in
the header either way, which is what makes this an expensive thing to get wrong
— so an `sk-*` key named directly by a profile is refused, by name, with a
message pointing at `ssh-add`. The agent does the FIDO2 work itself, so the
bridge above carries a security key's signature like any other. ADR-0014 has the
evidence and the conditions for revisiting. **Untested against real hardware.**

## telnet — RFC 854 and friends

Plaintext, and there is no version of this protocol that is not. Krait speaks
it because the devices that need it — switches, PDUs, console servers — are not
going to grow SSH, not because it is a good idea.

| Key | Meaning |
|---|---|
| `host`, `port` | Port defaults to 23. |

No `user` or `auth`: telnet has no authentication of its own. Whatever the far
end asks for arrives as terminal output and is typed back like anything else,
which is precisely why the password is on the wire in the clear.

Options negotiated: BINARY (RFC 856), ECHO (857), SUPPRESS-GO-AHEAD (858),
TERMINAL-TYPE (1091), NAWS (1073). Everything else is refused, which RFC 1123
makes a MUST rather than a courtesy. Two refusals are policy rather than
capability:

- **NEW-ENVIRON (39)** would ship environment variables to the far end. No RFC
  forbids a client offering it; the leak is ours to prevent.
- **AUTHENTICATION (37)** negotiates its own strength unprotected — RFC 2941
  says so itself — which over a plaintext transport is a downgrade oracle.

Negotiation follows RFC 1143's Q Method, including the rule that an option
already agreed is answered with silence: two polite implementations
acknowledging each other is the loop that rule exists to prevent.

Reconnect: on connect failures only. A telnet server closing the connection is
how a telnet session normally ends, and nothing at this layer can tell that
apart from a connection that dropped — Qt reports the same error for a graceful
FIN and a reset, and telnet has no logout message. It is treated as a clean
end, because a banner on every normal logout is how a banner teaches people to
ignore banners. `src/net/telnet/telnet_backend.cpp` records the measurement.

Specs, quoted and checked: `docs/research/t54-telnet-findings.md`.

## raw — a TCP socket and nothing else

What PuTTY calls "raw", and it earns its place: half of network debugging is
wanting to see exactly what a port says, with nothing helpfully rewriting it.

| Key | Meaning |
|---|---|
| `host`, `port` | No default port. A raw socket is always aimed somewhere specific, and inventing one would connect to a service nobody named. |

Nothing is sent on connect and nothing is ever answered — a backend that spoke
unprompted would be writing bytes the user never typed into a session they are
using to find out what the other end does. In particular 0xFF is **not**
doubled and CR is **not** given a companion NUL: those are telnet's rules, and
applying them here would corrupt the one protocol whose contract is that
nothing is applied.

## serial — a COM port

| Key | Meaning |
|---|---|
| `host` | The port name, "COM7". PuTTY keeps the serial line in its host field and so do we: a profile has one "where", and a second field for it would mean every importer and every editor learning both. |
| `baud` | Default 115200. 8-N-1 with no flow control unless changed. |

Ports are enumerated through the COM port **device interface** class, which is
what gives the picker a friendly name — "COM7" tells nobody which of the three
adapters on the desk it is. The USB vendor and product ids come from the same
place, and they are what makes replug work.

**Auto-reconnect on replug** is the point of all that. When the adapter
disappears, the read fails and the session waits for a device with the SAME
VID/PID to come back — possibly on a different COM number, because Windows
hands out whatever is free. Matching on the port name instead would be worse
than useless: unplug a console cable, plug in a different adapter, Windows
gives it COM7, and the session silently reattaches to hardware nobody chose.

Two adapters of the same model are indistinguishable this way. That is the
honest limit of what Windows exposes without opening each device, and it is
recorded rather than papered over.

A device going away is reported as `deviceRemoved`, not as an error: on a USB
adapter it is a normal Tuesday, and a banner for something the next second
fixes is a banner people learn to dismiss without reading.

**DTR, RTS and break** are actions rather than settings, because each is a
momentary or level change on a wire: toggling DTR is how you reset an Arduino,
and a break is how you get a Cisco console's attention. RTS is refused while
hardware flow control owns the line — Windows documents `EscapeCommFunction` as
an error in that state, so issuing it anyway would look like it worked.

## telnet and raw share their socket

Both are `TcpBackend` (`src/net/tcp_backend.h`) with a codec on top: telnet's
is the negotiator, raw's is a copy. The split happened when the second one
arrived and not before — an abstraction with a single implementation is
something `.claude/rules/cpp.md` bans — and it matters because the socket half
is where the lifetime bugs live. Written twice, every fix has to be made twice
on the code path that handles remote input.

Three properties belong to that shared half:

- **`stop()` hops to the object's own thread if called from another.** The app
  tears backends down on a thread pool, because `SshBackend::stop()` can block
  inside libssh. Everything in `TcpBackend` is a GUI-thread QObject, and
  `QTimer::stop()` from another thread is refused silently — so without the hop
  a pending reconnect survives the tab closing.
- **The write queue is capped.** Qt caps reads but has no write-buffer limit at
  all, and telnet's answers amplify: six bytes of subnegotiation in, twenty
  out. A server that floods and stops reading would otherwise grow the queue
  until allocation failed.
- **The connect timeout aborts the socket**, rather than only reporting. Left
  running, the OS gives up twenty seconds later and raises a second banner for
  the same failure — or connects, and the tab comes alive under a banner saying
  nothing answered.

## Seeing and keeping what crossed the wire

Two per-session toggles, both in the command palette. They are actions rather
than settings on purpose: a hexdump is something you turn on for the device
that is misbehaving, not a preference.

**Toggle hexdump** (Ctrl+Shift+H) replaces the interpreted output with a
canonical dump — offset, sixteen bytes, printable column. The offset counts the
STREAM, not each read, so a line can be matched against a packet capture; reads
arrive at the mercy of TCP and an offset that restarted every read would be
decorative. The text column decodes nothing, not even valid UTF-8, because a
hexdump that renders text hides the byte being looked for. Input still goes out
unchanged: this is a view, not a mode.

**Start or stop logging this session** writes a timestamped capture to
`<config>/logs/<session>-<when>.log`, with `>` for what was typed and `<` for
what arrived — a log that cannot tell those apart cannot settle the argument it
was started to settle. Control bytes are escaped, so the file does not reformat
itself when it is opened, and each chunk is flushed, because the tail is
precisely the part worth having when a session ends badly.

It captures the session byte for byte and does not try to filter. If a password
is typed while it is running, the password is in the file. That is why it is off
until someone turns it on, per session, and why the banner names the path — an
invisible log is how a secret ends up somewhere nobody remembers.

## What every backend owes

From `.claude/rules/net.md`, and enforced by the contract tests:

- All remote input is hostile. Length-check before every read, cap anything a
  remote party influences the size of, and ship fuzz seeds with any new parsing.
- Errors map to the shared taxonomy in `src/net/error.h`, which the UI turns
  into a per-tab banner. A backend inventing its own error strings breaks that.
- Every wait has a timeout and a cancel path wired to closing the tab.
- No blocking network call on the UI thread — by using a worker thread where the
  library is blocking (ssh), or by using an asynchronous API where one exists
  (telnet).
