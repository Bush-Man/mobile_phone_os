#ifndef MODEM_H
#define MODEM_H

#include <stdbool.h>
#include <stdint.h>

#include "task.h"

/*
 * Telephony / cellular stack (phase 12, plan items 64-68).
 *
 * Layering:
 *
 *   call state machine + sms store            drivers/callctl.c
 *                                             drivers/sms.c
 *     -> modem layer (dial/sms/reg/signal)    drivers/modem.c
 *       -> AT engine (submit/retry/URC)       drivers/at.c
 *         -> transport                        drivers/modem_uart.c
 *                                             drivers/modem_mock.c (QEMU)
 *
 * The AT engine never sleeps: it drains the transport and enforces
 * timeouts from modem_tick(), which housekeeping calls every ~2 ms.
 * The mock transport answers scripted commands and accepts injected
 * URCs so the call/SMS milestone is deterministic headlessly; the
 * real UART transport binds to a "modem" chardev on boards.
 *
 * Data connection (item 68) registers a netif when the session is
 * up, feeding the phase-11 stack; PPP/modem-native negotiation is
 * HW bring-up (documented there).
 */

#define AT_LINE_MAX     160u
#define AT_CMD_MAX      64u
#define AT_RESP_MAX     8u

struct platform_info;
struct netif;

/* ---- AT engine ---------------------------------------------------------------- */

struct at_transport {
    /* both may sleep (task context only); byte-stream semantics    */
    int (*write)(void *tp_priv, const void *buf, unsigned len);
    int (*read)(void *tp_priv, void *buf, unsigned len);
    void *priv;
};

typedef void (*at_resp_fn)(int status, const char *const *lines,
                           unsigned nlines, void *arg);
typedef void (*at_urc_fn)(const char *line, void *arg);

/* status codes delivered to at_resp_fn                            */
#define AT_OK       0
#define AT_ERROR    (-1)
#define AT_TIMEOUT  (-2)

struct at_engine {
    struct at_transport tp;

    char     line[AT_LINE_MAX];
    unsigned line_len;

    /* pending command                                              */
    at_resp_fn resp_cb;
    void      *resp_arg;
    char       retry_cmd[AT_CMD_MAX];
    uint64_t   sent_ms;
    uint32_t   timeout_ms;
    unsigned   retries_left;
    bool       pending;

    const char *resp_lines[AT_RESP_MAX];
    char        resp_store[AT_RESP_MAX][AT_LINE_MAX];
    unsigned    resp_count;

    at_urc_fn urc_cb;
    void     *urc_arg;

    struct {
        uint64_t submitted, ok, errors, timeouts, urcs;
    } stats;
};

void at_engine_init(struct at_engine *e, struct at_transport *tp);
void at_engine_set_urc_handler(struct at_engine *e, at_urc_fn fn,
                               void *arg);
int  at_engine_submit(struct at_engine *e, const char *cmd,
                      uint32_t timeout_ms, unsigned retries,
                      at_resp_fn cb, void *arg);
/* housekeeping cadence: drain transport rx, enforce timeouts      */
void at_engine_tick(struct at_engine *e, uint64_t now_ms);

/* ---- call state machine ---------------------------------------------------------- */

enum call_state {
    CALL_IDLE = 0,
    CALL_DIALING,
    CALL_OUT_RINGING,
    CALL_INCOMING,
    CALL_ACTIVE,
    CALL_ENDING,
};

enum call_event {
    CALL_EV_DIAL = 0,           /* user requested dial            */
    CALL_EV_OK,                 /* ATD accepted by network        */
    CALL_EV_CONNECT,            /* call established (URC)         */
    CALL_EV_RING,               /* remote is ringing (URC)        */
    CALL_EV_INCOMING,           /* remote call arriving (URC)     */
    CALL_EV_ANSWER,             /* user answered (ATA ok)         */
    CALL_EV_HANGUP_LOCAL,
    CALL_EV_HANGUP_REMOTE,      /* NO CARRIER / remote ended      */
    CALL_EV_BUSY,
    CALL_EV_ERROR,              /* ERROR / failed setup           */
};

enum call_state call_next(enum call_state s, enum call_event e);
const char *call_state_name(enum call_state s);

/* runtime wrapper + audio routing hook (real routing: phase 13)  */
typedef void (*call_audio_route_fn)(enum call_state s, void *arg);
void call_audio_route_set(call_audio_route_fn fn, void *arg);
void call_ctl_init(void);
enum call_state call_ctl_state(void);
enum call_event call_ctl_apply(enum call_event e);   /* transition+route */

/* ---- sms -------------------------------------------------------------------------- */

#define SMS_ADDR_MAX  20u
#define SMS_TEXT_MAX  160u

int  sms_encode_7bit(const char *text, uint8_t *out, unsigned cap);
int  sms_decode_7bit(const uint8_t *in, unsigned septets,
                     char *out, unsigned cap);
int  sms_build_submit_pdu(const char *to, const char *text,
                          uint8_t *pdu, unsigned cap);
int  sms_parse_deliver_pdu(const uint8_t *pdu, unsigned len,
                           char *sender, unsigned sender_cap,
                           char *text, unsigned text_cap);
int  sms_store_inbox(const char *sender, const char *text);
int  sms_read_msg(const char *name, char *sender,
                  unsigned sender_cap, char *text, unsigned text_cap);
unsigned sms_seq(void);


/* ---- modem layer -------------------------------------------------------------------- */

enum reg_status {
    REG_UNKNOWN = 0,
    REG_NOT_SEARCHED,
    REG_HOME,
    REG_SEARCHING,
    REG_DENIED,
    REG_ROAMING,
};

struct modem_signal {
    uint8_t rssi;                   /* 0-31, 99 = unknown           */
    uint8_t ber;
};

struct modem_info {
    bool     sim_ready;
    enum reg_status reg;
    struct modem_signal sig;
};

typedef void (*modem_call_event_fn)(enum call_event e, void *arg);
typedef void (*modem_sms_rx_fn)(const char *sender, const char *text,
                                void *arg);

void modem_subsys_init(const struct platform_info *plat);
bool modem_present(void);
void modem_tick(uint64_t now_ms);   /* housekeeping: engine drain  */

int  modem_dial(const char *number);
int  modem_answer(void);
int  modem_hangup(void);
int  modem_sms_send(const char *to, const char *text);

void modem_set_call_handler(modem_call_event_fn fn, void *arg);
void modem_set_sms_handler(modem_sms_rx_fn fn, void *arg);

/* synchronous-ish queries: submit + wait inside the call          */
int  modem_query_sim_ready(bool *ready, uint32_t timeout_ms);
int  modem_query_reg(enum reg_status *out, uint32_t timeout_ms);
int  modem_query_signal(struct modem_signal *out, uint32_t timeout_ms);

/* data connection (item 68): registers/removes netif "rmnet0"     */
int  modem_data_up(struct netif *nif_out);
int  modem_data_down(void);

/* ---- mock (QEMU dev image only) -------------------------------------------------- */

bool modem_mock_attached(void);
void modem_mock_inject_urc(const char *line);

struct netif;
#endif /* MODEM_H */
