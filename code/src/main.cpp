#include "output.h"
#include "utils.h"
#include "track.h"
#include "proc.h"
#include "netlink.h"

#include <unistd.h>

#include <sys/epoll.h>

static constexpr int kSnapshotEveryMs = 1;

int ForkAndExec(char *cmd, char **parameters, int numParameters) {
    Log("ForkAndExec %s", cmd);
    std::string cmdline = std::string();
    for (int i = 0; i < numParameters; i++) {
        Log(" %s", parameters[i]);
        cmdline += " ";
        cmdline += parameters[i];
    }

    int pid = fork();
    if (pid == 0) { // This is the new process
        // Drop superuser privileges
        DropRoot();

        // We cannot reuse parameters as it, since it is not null terminated
        char *argv[numParameters + 1];
        for (int i = 0; i < numParameters; i++) {
            argv[i] = parameters[i];
            Log("execv argv[%d]=%s\n", i, argv[i]);
        }
        argv[numParameters] = nullptr;

        Log("execv argv[%d]=%s\n", numParameters, "NULL");
        int ret = execvp(cmd, argv);
        if (ret == -1) {
            Log("Could not execv '%s'", cmd);
            perror("Unable to execv");
            exit(EXIT_FAILURE);
        }
    } else {
        Track(pid);
        // For short-lived process, we may not be quick enough to poll /proc/PID/cmdline.
        // We cheat and pre-populate the cache here.
        Declare(pid, cmdline);
    }
    return pid;
}

int main(int argc, char **argv) {
    if (getuid() != 0) {
        fprintf(stderr,"Only root can startTimeMs/stop the fork connector\n");
        return 0;
    }

    for (int i = 0; i < argc; i++) {
        Log("argv[%02d]:%s\n", i, argv[i]);
    }

    if (argc < 2) {
        fprintf(stderr, "No command to trace\n");
        return 0;
    }

    InitOutput();

    int netlink_socket = InitNetlink();

    int epfd = epoll_create(1);
    if (epfd == -1) {
        perror("Cannot create epoll");
        exit(EXIT_FAILURE);
    }

    // Prepare epoll
    struct epoll_event ev{};
    ev.data.fd = netlink_socket;
    ev.events = EPOLLIN; // Register for read availability
    int ctl = epoll_ctl(epfd, EPOLL_CTL_ADD, netlink_socket, &ev);
    if (ctl) {
        perror("epoll_ctl error");
        exit(EXIT_FAILURE);
    }

    // From here, we are receiving netlink events. We can create the process we want to observe.
    // We add the forked process to the list of Tracked processes.
    uint64_t startTimeMs = GetTimeMs();
    int pid = ForkAndExec(argv[1], &argv[1], argc - 1);

    int64_t nextMemorySnapshot = GetTimeMs() - kSnapshotEveryMs;

    // Let's roll until all processes have run!
    while (Tracked(pid)) {
        const int kMaxEvents = 1;

        // Calculate timeout so we snapshot PSS at regular intervals.
        long timeOut = nextMemorySnapshot - GetTimeMs();
        if (timeOut < 0) timeOut = 0;

        struct epoll_event evlist[kMaxEvents];
        int ready = epoll_wait(epfd, evlist, kMaxEvents, timeOut);
        switch (ready) {
            case -1 : { // Error?
                if (errno == EINTR) {
                    continue;
                }

                Log("epoll error");
                exit(EXIT_FAILURE);
                break;
            }
            case 0 : { // Timeout, this is time to snapshop PSS for all processes.
                SnapshotPss();
                nextMemorySnapshot = GetTimeMs() + kSnapshotEveryMs;
                break;
            }
            default: {
                for (int j = 0; j < ready; j++) {
                    if (evlist[j].events & EPOLLIN) {
                        ReadFromNetlink(evlist[j].data.fd);
                    } else if (evlist[j].events & (EPOLLHUP | EPOLLERR)) {
                        perror("Netlink hangup?\n");
                        exit(EXIT_FAILURE);
                    }
                }
            }
        }
    }
    close(netlink_socket);

    DropRoot();

    GenerateOutputs(pid, startTimeMs);

    return EXIT_SUCCESS;
}
