# T54 — Telnet, verified against the RFCs

Checked 2026-07-31 against rfc-editor.org / datatracker.ietf.org before any
code was written (rules/mcp-first.md). What is written here is quoted or
directly derived; the "ours to decide" section is the part the RFCs leave open,
which matters more than the parts they settle.

## Commands (RFC 854)

| Name | Dec | Name | Dec |
|---|---|---|---|
| SE | 240 | GA | 249 |
| NOP | 241 | SB | 250 |
| DM (Data Mark) | 242 | WILL | 251 |
| BRK | 243 | WONT | 252 |
| IP | 244 | DO | 253 |
| AO | 245 | DONT | 254 |
| AYT | 246 | IAC | 255 |
| EC | 247 | | |
| EL | 248 | | |

SB is 250 and SE is 240 — **not adjacent**, and the obvious off-by-one is to
assume they bracket each other numerically.

## Options we implement

| Option | Code | RFC | Notes |
|---|---|---|---|
| BINARY | 0 | 856 | "must be negotiated separately for each direction of data flow" |
| ECHO | 1 | 857 | never WILL it to a server that echoes — the RFC warns it "loops back and forth indefinitely" |
| SGA | 3 | 858 | "suppressed in both directions independently" |
| TERMINAL-TYPE | 24 | 1091 | IS = 0, SEND = 1; names are <= 40 chars and case-insensitive |
| NAWS | 31 | 1073 | `IAC SB 31 W1 W0 H1 H0 IAC SE`, 16-bit **big-endian** |

Everything else is refused. RFC 1123 section 3.2 is a MUST: "A host MUST refuse
(i.e, reply WONT/DONT to a DO/WILL) an unsupported option", and "A host MUST be
able to receive and ignore any Telnet control functions that it does not
support" — so an unknown two-byte command is consumed and dropped, while an
unknown *option* is answered.

## Deliberately refused

- **NEW-ENVIRON (39, RFC 1572).** Nothing in the RFC forbids a client offering
  it; the leak is ours to prevent. RFC 1572 section 7 only warns the *server*
  side. Krait answers WONT 39.
- **AUTHENTICATION (37, RFC 2941).** "the negotiation of the authentication
  type pair is not protected, thus allowing an attacker to force the result of
  the authentication to the weakest mutually acceptable method." Over plaintext
  telnet there is no version of this worth having.
- Note RFC 1408's option code 36 is dead: 1572 moved NEW-ENVIRON to 39 because
  1408 "incorrectly reversed the values for VAR and VALUE".

## Escaping

- A literal 0xFF in the data stream is doubled. RFC 854: "only the IAC need be
  doubled to be sent as data". RFC 1123 makes it a MUST.
- Inside a subnegotiation too: RFC 855, "if parameters in an option
  'subnegotiation' include a byte with a value of 255, it is necessary to
  double this byte". NAWS spells this out for width/height bytes equal to 255.
- **Only `IAC SE` ends a subnegotiation.** RFC 855: "the receiver may locate the
  end of a parameter string by searching for the SE command (i.e., the string
  IAC SE)". A lone 240 in parameter data is data — terminating on it is a
  parser that a hostile server can cut short at will.

## Line endings (RFC 854, RFC 1123)

NVT mode: "the sequence CR LF must be treated as a single new line character";
"the sequence CR NUL must be used where a carriage return alone is actually
desired". RFC 1123: "A User Telnet MUST be able to send any of the forms: CR LF,
CR NUL, and LF". Under BINARY the convention is gone — RFC 1123 section 3.2.7:
"there is no end-of-line convention ... in binary mode" — but IAC doubling still
applies (RFC 856).

## The anti-loop rule — RFC 1143's Q Method

RFC 854: "If a party receives what appears to be a request to enter some mode it
is already in, the request should not be acknowledged. This non-response is
essential to prevent endless loops in the negotiation." RFC 1143 restates it as
a MUST and gives the state machine we implement: states NO / YES / WANTNO /
WANTYES, each WANT state carrying a queue bit (EMPTY or OPPOSITE).

The published table, for `him` on receiving WILL/WONT:

| State | recv WILL | recv WONT |
|---|---|---|
| NO | if we agree he should enable: him=YES, send DO; else send DONT | ignore |
| YES | ignore | him=NO, send DONT |
| WANTNO/EMPTY | error (DONT answered by WILL), him=NO | him=NO |
| WANTNO/OPPOSITE | error, him=YES, himq=EMPTY | him=WANTYES, himq=EMPTY, send DO |
| WANTYES/EMPTY | him=YES | him=NO |
| WANTYES/OPPOSITE | him=WANTNO, himq=EMPTY, send DONT | him=NO, himq=EMPTY |

DO/DONT are not a separate table — RFC 1143: "We handle the option on our side
by the same procedures, with DO-WILL, DONT-WONT, him-us, himq-usq swapped."

Also normative and easy to miss: "During the negotiation state, any effects of
having the option enabled MUST NOT be used."

## Ours to decide — the RFCs impose nothing

Recorded because the temptation is to assume a spec answer exists.

- **No subnegotiation length limit exists anywhere.** RFC 855 has no
  parameter-length or buffering text at all. Per-option bounds only: NAWS is
  four payload bytes, a TERMINAL-TYPE name is <= 40 characters. The cap is ours,
  and a parser without one is a remote allocation primitive.
- **An unterminated `IAC SB` is unspecified.** So is a new `IAC SB` arriving
  while one is open.
- Receiver-side IAC un-doubling is never stated in those words; it is the only
  reading consistent with the sender-side MUST.

Krait's choices are in `src/net/telnet/telnet_negotiation.h`, next to the code
that implements them.

## Wire examples, checked

```
NAWS 80x24:           FF FA 1F 00 50 00 18 FF F0
NAWS width 255:       FF FA 1F 00 FF FF 00 18 FF F0    (payload FF doubled)
TERMINAL-TYPE reply:  FF FA 18 00 'x' 't' 'e' 'r' 'm' FF F0   (IS = 0)
Refuse option N:      FF FC N (WONT) to a DO N;  FF FE N (DONT) to a WILL N
```
