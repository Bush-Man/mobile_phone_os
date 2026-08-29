/*
 * init.c - PID 1 (phase 14, plan item 75).
 *
 * Responsibilities, in the order they come up:
 *   1. directory scaffolding (/var, /var/crash, /var/run),
 *   2. spawn the service daemons (batteryd, udevd, timed) and the
 *      interactive shell,
 *   3. reap EVERYTHING: waitpid(-1) blocks until any child dies --
 *      its own spawns and orphans adopted from dead parents alike,
 *   4. restart critical daemons when they die (the "killed daemons
 *      respawn automatically" milestone), logging each restart.
 *
 * The kernel registered this process as the orphan reaper before
 * the first instruction ran (proc_note_init_pid in phase14.c), so
 * reparenting and SIGCHLD arming are already live by the time we
 * enter main.
 */

#include "libc.h"
#include "sysinfo.h"

struct service {
    const char *name;
    const char *argv0;
    int pid;
    int critical;
};

static struct service services[] = {
    { "batteryd", "batteryd", -1, 1 },
    { "udevd",    "udevd",    -1, 1 },
    { "timed",    "timed",    -1, 1 },
    { "sh",       "sh",       -1, 1 },
};

#define NSVC ((int)(sizeof(services) / sizeof(services[0])))

static int spawn(const char *name, char *const argv[])
{
    int pid = fork();

    if (pid == 0) {
        if (execve(name, argv, (char *const []){ 0 }) < 0) {
            printf("init: execve(%s) failed (%d)\n", name, getpid());
            _exit(127);
        }
        for (;;)
            ;
    }
    return pid;
}

static void spawn_service(struct service *s)
{
    s->pid = spawn(s->argv0, (char *const []){ (char *)s->argv0, 0 });
    printf("init: started %s (pid %d)%s\n", s->name, s->pid,
           s->critical ? " [critical]" : "");
}

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    printf("init: mobile_phone_os userspace online (pid %d)\n",
           getpid());

    mkdir("/var");
    mkdir("/var/crash");
    mkdir("/var/run");

    for (int i = 0; i < NSVC; i++)
        spawn_service(&services[i]);

    printf("init: waiting on children (reap + restart)\n");

    for (;;) {
        i64 r = waitpid(-1);

        if (r < 0) {
            sleep_ms(100);      /* nothing to reap right now      */
            continue;
        }
        {
            int code = (int)(r & 0xff);
            int pid = (int)(r >> 8);

            for (int i = 0; i < NSVC; i++) {
                if (services[i].pid == pid) {
                    services[i].pid = -1;
                    if (services[i].critical) {
                        printf("init: %s (pid %d) died (code %d) "
                               "-- respawning\n",
                               services[i].name, pid, code);
                        sleep_ms(50);   /* let zombie cleanup run */
                        spawn_service(&services[i]);
                    } else {
                        printf("init: %s (pid %d) exited "
                               "(code %d)\n",
                               services[i].name, pid, code);
                    }
                    break;
                }
            }
        }
    }
    return 0;
}
