# DMFTP - DMOD FTP

## Overview

`dmftp` implements RFC 959 File Transfer Protocol over [`dmtcp`][dmtcp], in
both roles: a **server** that exposes this device's filesystem, and a
**client** that talks to somebody else's. Both halves are built out of the
same three pieces - one control-line codec, one jail-safe path resolver, one
transfer engine - so a fix in any of them lands in both.

Deliberately in scope: the full RFC 959 command set a real client needs
(including LIST/NLST, RETR/STOR/APPE, DELE/MKD/RMD, RNFR/RNTO, REST, ABOR),
RFC 2389's `FEAT`, RFC 3659's `SIZE` and `REST STREAM`, both passive (PASV)
and active (PORT) data connections in both roles, and genuine ASCII-mode
line-ending translation.

Deliberately out of scope: FTPS/TLS (no TLS module exists in this tree),
RFC 2428's EPSV/EPRT (they exist for IPv6, which `dmtcp` cannot originate),
block and compressed transfer modes (RFC 959 §3.4.2/3.4.3 - no client anyone
still runs speaks them), and `MDTM` reporting a real timestamp (see "What the
SAL does not give us").

## Files come from the SAL, not from dmvfs

Every filesystem operation goes through dmod's own SAL - `Dmod_FileOpen()`,
`Dmod_FileRead()`/`_FileWrite()`/`_FileSeek()`/`_FileSize()`,
`Dmod_OpenDir()`/`_ReadDirEx()`/`_CloseDir()`, `Dmod_MakeDir()`,
`Dmod_RemoveDir()`, `Dmod_FileRemove()`, `Dmod_Rename()`,
`Dmod_FileAvailable()` - which is exactly what the `dmell` commands (`ls`,
`cat`, `cp`) use.

The alternative would have been to depend on `dmvfs` directly and call
`dmvfs_stat()`. That is not possible: `dmvfs` publishes no `dmf-get` package,
so it cannot be a `dmod_link_modules()` dependency at all. Going through the
SAL reaches whatever `dmvfs` has mounted anyway, and costs `dmftp` one fewer
dependency.

### What the SAL does not give us

`Dmod_DirEntry_t` carries a **name and a type, and nothing else**. There is no
`stat()` anywhere in the SAL. Two visible consequences:

| Command | Consequence |
|---|---|
| `LIST` | Each entry's size costs an open + `Dmod_FileSize()` + close. The timestamp column is a fixed `Jan  1  1970` placeholder - clients treat that field as advisory and none of them refuse a listing over it. |
| `MDTM` | There is no modification time to report. `MDTM` answers `550` with an explicit "not available", and is **deliberately absent from the `FEAT` list** so a well-behaved client never asks. |

Fabricating a plausible-looking timestamp would have been the other option.
It was rejected: a wrong mtime silently corrupts every client-side "is my
copy newer" decision, whereas a missing feature is something a client
already knows how to handle.

## Nothing here blocks, and nothing here has a thread

`dmftp` owns no thread. Control lines, inbound data, and connection events
all arrive inline on whatever thread pumps the interface - `dmtcp`'s
documented delivery context - and outbound file data is pushed from
`dmtcp`'s `on_writable` callback. A `RETR` of a 4 MB file costs no thread,
no timer, and no polling loop.

### The transfer pump

`dmtcp`'s per-connection outbound buffer is 4 KB, so any transfer larger
than that *will* hit a short `dmtcp_send()`. Before this module existed,
`dmtcp` had no way to say when that buffer drained again, which would have
forced `dmftp` into a polling loop on its own timer or thread - picking a
tick interval that trades latency against wakeups, blind to the one event
that actually matters (an ACK).

That gap was closed in `dmtcp` itself rather than worked around here: see
`dmtcp_writable_handler_t` and `dmtcp_send_space()`, and `dmtcp/docs/dmtcp.md`'s
"Send-side backpressure" section. The trigger is edge-triggered, exactly like
POSIX `EPOLLOUT`.

`dmftp_xfer_pump()` is the whole loop:

1. If the staging buffer is empty, refill it from the source (a file, or the
   next few directory entries).
2. Hand it to `dmtcp_send()`.
3. If `dmtcp` took everything, go to 1.
4. If it took less, **stop** - that short write is precisely what armed
   `on_writable`, and the peer's next ACK calls straight back in here.

A transfer therefore advances at exactly the rate the peer acknowledges
data. There is no rate to tune and no idle wakeup to pay for.

Inbound transfers need none of this: `dmtcp` delivers the bytes as they
arrive and `dmftp_xfer_receive()` writes them through.

### One transfer, one owner, one completion

A transfer can end several ways at once - the source runs out, the peer
sends FIN, a RST arrives, the session is torn down.
`dmftp_xfer_finish()` is idempotent: the first ending wins, the owner's
completion hook fires exactly once, and the owner is what frees the
transfer.

The client waits for **both** halves before reporting: the data connection
closing says "the bytes are all here", the server's `226` says "and I agree
it worked". Reporting on whichever arrived first would mean announcing
success before the server had a chance to say otherwise.

## Two loader constraints this module is shaped around

Both are real, both cost a debugging session to find, and both are the kind
of thing that looks like a random memory-corruption bug.

### Every dmtcp callback is registered from one file

`dmtcp`'s own docs record it: this loader mis-resolves a callback whose
address is taken in one `.c` file and handed to another module's
registration API from a *different* `.c` file of the same module - the call
lands on an unrelocated address.

So every `dmtcp_listen()`, `dmtcp_listen_any()`, `dmtcp_connect()` and
`dmtcp_conn_set_callbacks()` in `dmftp` lives in `src/dmftp_net.c`, next to
the handlers being registered. The rest of the module reaches `dmtcp`
through the `dmftp_net_*()` helpers.

### No pointers inside static initialized tables

The data-side sibling of the same rule, and the sharper of the two. The
obvious way to write a command dispatcher is:

```c
static const struct { const char* verb; handler_fn fn; } table[] = {
    { "USER", cmd_user }, ...
};
```

A pointer sitting inside a static initialized aggregate is **never
relocated** by this loader. The first `strcmp(table[i].verb, ...)`
dereferences a link-time address and the module dies with a SIGSEGV whose
backtrace points nowhere near the table. (This is why `dmell` registers its
own command handlers with runtime `dmell_register_command_handler()` calls
rather than a static table.)

`dmftp`'s table in `src/dmftp_server_cmd.c` is therefore **pure integers** -
the four-character verb packed into a `uint32_t`, the access flags, and an
enum id - which need no relocation at all. Dispatch is a `switch` on that
id. The lookup stays table-driven, so the "one place decides who may run
this command" property survives.

## Detaching before freeing

`dmtcp_close()` is graceful: the TCB outlives the call by a FIN exchange and
then a TIME_WAIT, and `dmtcp` fires `on_closed` at the end of that - long
after the session or client that was the connection's `user_data` has been
freed.

`dmftp_net_detach()` clears a connection's callbacks before its owner is
released. Every teardown path goes through it. Without it the terminal
callback reads a dangling pointer and the magic-field guard is left guessing
at recycled heap - which is exactly the failure the end-to-end tests caught.

## One recursive lock, held across callbacks

`dmftp` has no thread, but its callbacks arrive on whatever thread pumps
each interface, which for several interfaces means several threads. One
recursive mutex, taken at the top of every `dmtcp` callback and every public
entry point, serializes all of it.

Unlike `dmtcp` and `dmdhcp`, `dmftp` deliberately **holds that lock across
user callbacks** (`on_auth`, `on_list`, `on_done`, ...) rather than
snapshotting state and releasing first. Two reasons:

- The lock is recursive, so the thing those callbacks most want to do -
  call straight back into `dmftp` (`dmftp_session_set_root()` from
  `on_auth`, `dmftp_client_get()` from `on_ready`) - simply works.
- Dropping the lock mid-command would expose every caller to a session
  being torn down underneath it by another interface's thread, which is a
  much harder class of bug than the rule it replaces.

That rule, inherited unchanged from `dmtcp`: **a dmftp callback must not
block.**

## The jail

Every server command that takes a filename funnels through
`dmftp_path_resolve()`, so confinement is enforced once rather than once per
command. The argument is normalized into an absolute *virtual* path in which
`..` can never climb past `/`; the session's root is then prefixed.

A path that tries to escape is **clamped, not rejected** - which is what a
real chrooted FTP server does, and what clients expect when they blindly
send `CWD ../..`.

`dmftp_session_set_root()` (meant for `on_auth`) resolves against the
*server's* root rather than the session's current one, so repeated calls
cannot walk outward one step at a time. `dmftp_session_set_read_only()` can
likewise only ever restrict: a read-only server stays read-only.

## Security posture

Stated plainly, because FTP invites optimism:

- **The control channel is plaintext.** Usernames and passwords cross the
  network in the clear. This is FTP; there is no TLS module in this tree to
  wrap it in. Treat an `ftpd` as something you expose on a trusted network
  segment, not on the Internet.
- **The PORT bounce attack is closed.** CERT CA-1997-27's trick is a `PORT`
  naming a third party, turning the server into a relay that opens
  connections on an attacker's behalf. `dmftp` requires the data address to
  match the control connection's peer, which costs a legitimate client
  nothing.
- **No rate limiting, no lockout.** A password guesser is limited only by
  how fast it can open connections (`max_sessions` caps concurrency, not
  attempts). Password comparison is not timing-hardened either, which is a
  smaller problem than the plaintext channel it travels over.
- **The control line is bounded.** `DMFTP_LINE_MAX` (512 bytes) caps the
  accumulator; a peer that never sends a newline gets a `500` and its buffer
  reset. That is the one place a remote peer could otherwise drive this
  module's heap use directly.
- **Anonymous access is off unless configured**, and always read-only.

## Known limitations ("what's not here yet")

- **IPv4 only.** Inherited from `dmtcp`, which cannot originate an IPv6
  segment (no NDP module exists yet). `PASV`/`PORT` carry IPv4 tuples;
  `EPSV`/`EPRT` answer `502`.
- **No FTPS/TLS**, per the above.
- **`MDTM` reports no timestamp** and is absent from `FEAT` - see "What the
  SAL does not give us".
- **`LIST` needs a directory.** Listing a single file by name is not
  supported (real servers do it; it needs a second listing source for one
  rarely-used case). `LIST -la /some/dir` works: `ls`-style option words in
  front of the path are stripped, because every real client sends them.
- **`226` is sent when the last byte is queued**, not when the data
  connection has finished closing. Waiting for the close would mean waiting
  out `dmtcp`'s TIME_WAIT, delaying every transfer's completion by seconds.
- **`STAT <path>` lists a directory over the control channel** without
  paging; a very large directory produces a very large reply, buffered in
  the session's output queue.
- **One transfer per session at a time.** A second transfer command while
  one is running replaces it (`dmftp_server_reset_data()`), rather than
  being queued or refused with `450`.
- **The client keeps one command in flight.** FTP's control channel is
  serial by design, so `dmftp_client_*()` returns `-EBUSY` rather than
  queueing.

## Dependencies

- `dmtcp` - the whole transport: `dmtcp_listen()`/`_listen_any()`/
  `_connect()`/`_send()`/`_send_space()`/`_close()`/`_abort()`, and the
  `on_writable` callback the transfer pump is built on.
- `dmip` - `dmip_addr_t`, which the public API uses for every address.
- `dmroute` - header-only: `dmip_addr_t`'s real definition
  (`dmroute_addr_t`).
- `dmnetif` - header-only: `dmnetif_iface_t`, threaded through the accept
  path.
- `dmlist` - the listener registry and each server's session list.
- `dmosi` - the one recursive mutex this module serializes itself with.
- **Deliberately not `dmvfs`** - see "Files come from the SAL".
- **Deliberately not `dm_sw_ring`** - the staging buffers here are linear
  and single-producer/single-consumer, and the module lock already
  serializes every access to them.

[dmtcp]: https://github.com/choco-technologies/dmtcp
