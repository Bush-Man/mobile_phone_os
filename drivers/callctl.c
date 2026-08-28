/*
 * callctl.c - voice call state machine (phase 12, item 66).
 *
 * call_next() is the pure transition function (selftest-friendly);
 * call_ctl_apply() drives the runtime state, invokes the audio
 * routing hook on every change, and clamps invalid transitions to a
 * documented no-op. Audio itself arrives in phase 13 -- the hook is
 * the routing seam the plan asks for.
 */

#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "modem.h"

static enum call_state cs = CALL_IDLE;
static call_audio_route_fn route_fn;
static void *route_arg;

void call_audio_route_set(call_audio_route_fn fn, void *arg)
{
    route_fn  = fn;
    route_arg = arg;
}

void call_ctl_init(void)
{
    cs = CALL_IDLE;
}

enum call_state call_ctl_state(void)
{
    return cs;
}

const char *call_state_name(enum call_state s)
{
    switch (s) {
    case CALL_IDLE:        return "idle";
    case CALL_DIALING:     return "dialing";
    case CALL_OUT_RINGING: return "out-ringing";
    case CALL_INCOMING:    return "incoming";
    case CALL_ACTIVE:      return "active";
    case CALL_ENDING:      return "ending";
    }
    return "?";
}

enum call_state call_next(enum call_state s, enum call_event e)
{
    switch (s) {
    case CALL_IDLE:
        if (e == CALL_EV_DIAL)      return CALL_DIALING;
        if (e == CALL_EV_INCOMING)  return CALL_INCOMING;
        return s;

    case CALL_DIALING:
        if (e == CALL_EV_OK)             return CALL_OUT_RINGING;
        if (e == CALL_EV_CONNECT)        return CALL_ACTIVE;
        if (e == CALL_EV_BUSY)           return CALL_IDLE;
        if (e == CALL_EV_ERROR)          return CALL_IDLE;
        if (e == CALL_EV_HANGUP_LOCAL)   return CALL_ENDING;
        return s;

    case CALL_OUT_RINGING:
        if (e == CALL_EV_CONNECT)        return CALL_ACTIVE;
        if (e == CALL_EV_HANGUP_REMOTE)  return CALL_IDLE;
        if (e == CALL_EV_HANGUP_LOCAL)   return CALL_ENDING;
        return s;

    case CALL_INCOMING:
        if (e == CALL_EV_ANSWER)         return CALL_ACTIVE;
        if (e == CALL_EV_HANGUP_REMOTE)  return CALL_IDLE;
        if (e == CALL_EV_HANGUP_LOCAL)   return CALL_ENDING;
        return s;

    case CALL_ACTIVE:
        if (e == CALL_EV_HANGUP_LOCAL)   return CALL_ENDING;
        if (e == CALL_EV_HANGUP_REMOTE)  return CALL_IDLE;
        if (e == CALL_EV_ERROR)          return CALL_IDLE;
        return s;

    case CALL_ENDING:
        if (e == CALL_EV_HANGUP_REMOTE)  return CALL_IDLE;
        if (e == CALL_EV_OK)             return CALL_IDLE;
        if (e == CALL_EV_ERROR)          return CALL_IDLE;
        return s;
    }
    return s;
}

static void route_changed(void)
{
    if (route_fn)
        route_fn(cs, route_arg);
}

enum call_event call_ctl_apply(enum call_event e)
{
    enum call_state prev = cs;
    enum call_state next = call_next(cs, e);

    if (next == cs && e != CALL_EV_HANGUP_LOCAL &&
        e != CALL_EV_HANGUP_REMOTE) {
        /* invalid transition for this state: swallow quietly      */
        return e;
    }

    cs = next;
    if (cs != prev)
        route_changed();
    return e;
}
