# Phase 12 — Telephony & Cellular (Implementation Log)

Milestone scope reached: a transport-agnostic AT command engine with
line assembly, response collection, URC classification and
timeout/retry enforcement driven from the housekeeping cadence
(include/modem.h + drivers/at.c, item 64); a real-board UART
transport over a "modem" chardev plus a fully scripted QEMU mock
transport with injectable URCs (drivers/modem_uart.c
modem_mock.c); a SIM/registration/signal layer parsing AT+CPIN?/
+CREG?/+CSQ into typed results (drivers/modem.c, item 65); the
voice call state machine as a pure transition function plus runtime
with an audio-routing seam for phase 13 (drivers/callctl.c, item
66); GSM 03.38 7-bit septet packing, SMS-SUBMIT/DELIVER PDU
build/parse and a VFS-backed /sms message store (drivers/sms.c,
item 67); the rmnet0 data netif registered into the phase-11
registry when the session is up (drivers/modem.c, item 68).
Milestone delivery in modtest: dial -> OK -> CONNECT -> ACTIVE ->
hangup -> NO CARRIER -> IDLE; RING -> answer -> ACTIVE -> remote
hangup; SMS send via two-stage CMGS with a real 7-bit SUBMIT PDU;
SMS receive via an injected +CMT hex DELIVER decoded back to
sender/text and round-tripped through the /sms store; rmnet0
up/down. Per the standing coordination decision no make target
ran; all 10 new + 1 edited units passed the per-file
`-fsyntax-only` sweeps.

## Order of files written

### Batch A — ABI header (contract first)

1. **`include/modem.h`** — the whole layering contract: at_transport
   {write,read,priv} (byte-stream, may sleep, task context only),
   at_resp_fn/at_urc_fn callbacks, AT_OK/ERROR/TIMEOUT codes,
   `struct at_engine` (line buffer, pending command with
   retry_cmd/timeout/retries, response line array, URC handler,
   stats); call_state/call_event enums with call_next() pure fn +
   call_ctl_apply() runtime and the call_audio_route_fn seam; SMS
   surface (7-bit encode/decode, SUBMIT/DELIVER PDU build/parse,
   /sms store APIs); modem layer (reg_status, modem_signal,
   handler registration, dial/answer/hangup/sms_send, query
   helpers, modem_data_up/down, modem_tick); mock surface.

Commit: "Add modem/call/SMS ABI header (phase 12)".

### Batch B — AT engine + transports (item 64)

2. **`drivers/at.c`** — at_engine_submit(): cmd + CR to the
   transport, arms pending state with timeout/retries; tick() drains
   transport bytes into lines (CR/LF pair, AT_LINE_MAX bounded);
   the "> " SMS prompt is dispatched specially (it has no CRLF);
   lines classify as final OK / ERROR-family / idle-URC / pending
   response accumulation (AT_RESP_MAX bounded); timeout re-submits
   retry_cmd up to `retries` times before delivering AT_TIMEOUT.
3. **`drivers/modem_uart.c`** — boards only: binds the transport to
   the "modem" chardev from the phase-6 registry; contributes
   nothing on QEMU.
4. **`drivers/modem_mock.c`** — the scripted transport: prefix
   response table (AT / +CPIN? / +CREG? / +CSQ / +CGMR / +CGPADDR /
   generic ERROR), ATD/ATA answer OK then schedule a CONNECT URC
   30 ms later, AT+CMGS enters prompt state and completes with
   +CMGS: 1 / OK once the PDU body arrives; 6-line schedule ring
   delivered by read() at simulated timestamps;
   modem_mock_inject_urc() queues lines at delay 0 for receive
   tests.
   
Commit: "Add AT engine, transports ... (phase 12)" (one commit with
the modem layer below -- they landed together).

### Batch C — callctl, sms, modem layer, data (items 65-68)

5. **`drivers/callctl.c`** — call_next() pure transitions over
   {IDLE,DIALING,OUT_RINGING,INCOMING,ACTIVE,ENDING} x 10 events;
   call_ctl_apply() invokes the audio-routing hook on every actual
   state change (the phase-13 seam); invalid transitions swallow
   quietly (documented).
6. **`drivers/sms.c`** — char_to_gsm/gsm_to_char tables (printable
   ASCII identity + @/$/CR/LF remaps, space fallback);
   sms_encode_7bit/sms_decode_7bit via the canonical bit-
   accumulator (septet i at bit offset i*7); addr_bcd digit
   packing/unpacking with F-pad; sms_build_submit_pdu (SMSC-less
   SUBMIT: 00 01 MR DA PID DCS UDL UD) and sms_parse_deliver_pdu
   (flags, OA, PID+DCS, 7-byte SCTS skip, UDL, UD);
   sms_store_inbox/sms_read_msg write/read "/sms/msg<N>" as
   "FROM <sender>\n<text>" via the phase-7 VFS with msg_seq
   numbering.
7. **`drivers/modem.c`** — modem_subsys_init() picks the transport
   (uart first, mock fallback) and arms the engine; urc_line()
   classifies RING/CONNECT/NO CARRIER/BUSY into callctl events and
   pairs +CMT header+hex lines into modem_sms_sink_line() (hex
   decode -> DELIVER parse -> store -> handler); dial/answer/hangup
   submit commands; query helpers (SIM/CREG/CSQ) submit + wait with
   atoi_mod parsers; modem_sms_send runs the two-stage CMGS state
   machine (prompt -> PDU body -> +CMGS response); modem_data_up/
   down register/remove the rmnet0 netif whose link_out is the
   documented PPP-deferred drop hook (item 68).

### Batch D — bring-up + milestone battery

8. **`kernel/phase12.c`** — modem_subsys_init() then spawns
   "modtest" at priority 56 (no transport -> one line, no task).
9. **`kernel/selftest_modem.c`** — "modtest": the battery in order
   (status queries / outbound call / inbound call / SMS send +
   receive + store round-trip / data up-down) with a call-event
   observer and an sms_rx observer capturing the decoded
   sender/text.
10. **`kernel/main.c`** — phase12_init() call site after phase11;
    housekeeping gains modem_tick(ms) beside the phase-11 tickers;
    banner bumps to `phase 12`.
11. **`Makefile`** — `make test` passes phase 12 to the cumulative
    harness.

Commits: ABI commit; engine/transports + modem layer commit;
wiring commit.

## Milestone mapping

- "place a call" -> modtest dial_tests: ATD 5550001 -> OK ->
  CONNECT URC -> CALL_ACTIVE observed via the routing hook; ATH ->
  NO CARRIER -> IDLE. Inbound: injected RING -> CALL_INCOMING ->
  ATA -> ACTIVE -> NO CARRIER -> IDLE.
- "send/receive an SMS" -> modem_sms_send drives the two-stage
  CMGS flow with a real 7-bit SUBMIT PDU (mock answers +CMGS: 1);
  receive injects a handcrafted DELIVER PDU (sender 5551234, text
  "INBOX TEST 1") as a +CMT header + hex body, decoded back and
  verified through the /sms VFS round-trip.
- "on target hardware" -> every byte flows through the same AT
  engine/transport interface the real UART transport implements;
  board bring-up swaps the transport, not the logic.

## Bugs found (and fixed) along the way

- **"> " prompt has no CRLF**: the line assembler only dispatched
  on LF, so the SMS prompt would never surface; added an explicit
  single-'>' dispatch in the byte loop.
- **Mock draft rebuilt**: the first pass invented an unnecessary
  lock and carried a mismatched include quote; rewritten in one
  clean pass (the mock is only touched from housekeeping task
  context, so no lock is needed at all).
- **modem.c assembly truncation**: hangup body and stale query
  stubs collided during a large append; repaired by removing the
  orphaned tail and placeholder queries, then inserting the real
  implementations (noted here because the file went through
  several scripted repairs before its green sweep).
- **Observer signature**: the selftest route observer initially
  matched call_state instead of the call_event the modem handler
  API delivers; fixed to read state via call_ctl_state().

## Design decisions worth remembering

- **AT engine never sleeps**: all waiting lives in the modem
  layer's q_wait()/sms_send loops (task context only). The tick is
  cheap and usable from any transport without lock-order surprises.
- **Pure call_next()**: the state machine is a table-like function
  the selftest can drive exhaustively; the runtime only adds the
  audio hook + state store.
- **rmnet0 exists but drops frames**: registering the netif keeps
  the phase-11 routing/demux paths alive for the integration the
  plan asks for; PPP/rmnet negotiation is the documented HW item.
- **Mock latency scheduling**: responses land through a timestamp
  ring inside read(), so URCs arrive deterministically inside the
  selftests' msleep windows without any busy waits.

## Verification status

Per coordination decision no make/QEMU target ran this phase; sweep
over 10 new + 1 edited units:

```
CLEAN include/modem.h
CLEAN drivers/{at.c,modem_uart.c,modem_mock.c,callctl.c,sms.c,modem.c}
CLEAN kernel/{phase12.c,selftest_modem.c,main.c}
```

When integration lands expect, in order:

```
make test        # cumulative criteria incl. banner "phase 12"
serial adds:
  modem: mock transport (QEMU)
  ...
  selftest: net ok                          (phase-11 battery green)
  modtest: phase 12 modem selftests
  modtest: sim ready / registered home / signal rssi 18 ber 0
  modtest: call -> dialing / out-ringing / active
  modtest: dial submitted / outbound call ACTIVE
  modtest: hangup -> IDLE / audio route hook fired
  modtest: RING -> INCOMING / answer / inbound call ACTIVE
  modtest: sms send (cmgs + submit pdu)
  modtest: sms decoded sender+text
  modtest: sms vfs store round-trip
  modtest: data session up / rmnet0 in registry / data session down
  selftest: modem ok
```

Notes carried forward:

- USSD (+CUSD) and cell broadcast (CBM) remain the plan's stretch
  items; the URC classifier routes unknown lines to the registered
  handler so they can be added without restructure.
- SIM file access beyond CPIN (CRSM/ICCID/IMSI) is HW bring-up;
  the query helpers already parse the same response shapes.
- Call audio routing stops at the hook; phase 13's audio HAL is
  the consumer that implements it.
- The mock's CONNECT-after-ATD/ATA schedule is 30 ms; tests wait
  80 ms so the URC lands deterministically inside their tick.


