/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define LOG_TAG "TetherController"
#include <cutils/log.h>


#include "TetherController.h"

TetherController::TetherController() {
    mInterfaces = new InterfaceCollection();
    mDnsForwarders = new NetAddressCollection();
    mDaemonFd = -1;
    mDaemonPid = 0;
}

TetherController::~TetherController() {
    InterfaceCollection::iterator it;

    for (it = mInterfaces->begin(); it != mInterfaces->end(); ++it) {
        free(*it);
    }
    mInterfaces->clear();

    mDnsForwarders->clear();
}

int TetherController::setIpFwdEnabled(bool enable) {

    LOGD("Setting IP forward enable = %d", enable);
    int fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY);
    if (fd < 0) {
        LOGE("Failed to open ip_forward (%s)", strerror(errno));
        return -1;
    }

    if (write(fd, (enable ? "1" : "0"), 1) != 1) {
        LOGE("Failed to write ip_forward (%s)", strerror(errno));
        return -1;
    }
    close(fd);
    return 0;
}

bool TetherController::getIpFwdEnabled() {
    int fd = open("/proc/sys/net/ipv4/ip_forward", O_RDONLY);

    if (fd < 0) {
        LOGE("Failed to open ip_forward (%s)", strerror(errno));
        return false;
    }

    char enabled;
    if (read(fd, &enabled, 1) != 1) {
        LOGE("Failed to read ip_forward (%s)", strerror(errno));
        return -1;
    }

    close(fd);

    return (enabled  == '1' ? true : false);
}

int TetherController::startTethering(struct in_addr dhcpStart, struct in_addr dhcpEnd) {

    if (mDaemonPid != 0) {
        LOGE("Tethering already started");
        errno = EBUSY;
        return -1;
    }

    LOGD("Starting tethering services");

    pid_t pid;
    int pipefd[2];

    if (pipe(pipefd) < 0) {
        LOGE("pipe failed (%s)", strerror(errno));
        return -1;
    }

    /*
     * TODO: Create a monitoring thread to handle and restart
     * the daemon if it exits prematurely
     */
    if ((pid = fork()) < 0) {
        LOGE("fork failed (%s)", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (!pid) {
        close(pipefd[1]);
        if (pipefd[0] != STDIN_FILENO) {
            if (dup2(pipefd[0], STDIN_FILENO) != STDIN_FILENO) {
                LOGE("dup2 failed (%s)", strerror(errno));
                return -1;
            }
            close(pipefd[0]);
        }
        char *start = strdup(inet_ntoa(dhcpStart));
        char *end = strdup(inet_ntoa(dhcpEnd));
        char *range;

        asprintf(&range, "--dhcp-range=%s,%s,1h", start, end);

        if (execl("/system/bin/dnsmasq", "/system/bin/dnsmasq", "--no-daemon", "--no-resolv",
                  "--no-poll", "--no-hosts", range, (char *) NULL)) {
            LOGE("execl failed (%s)", strerror(errno));
        }
        LOGE("Should never get here!");
        return 0;
    } else {
        close(pipefd[0]);
        mDaemonPid = pid;
        mDaemonFd = pipefd[1];
        LOGD("Tethering services running");
    }

    return 0;
}

int TetherController::stopTethering() {

    if (mDaemonPid == 0) {
        LOGE("Tethering already stopped");
        return 0;
    }

    LOGD("Stopping tethering services");

    kill(mDaemonPid, SIGTERM);
    mDaemonPid = 0;
    close(mDaemonFd);
    mDaemonFd = -1;
    return 0;
}

bool TetherController::isTetheringStarted() {
    return (mDaemonPid == 0 ? false : true);
}

int TetherController::setDnsForwarders(char **servers, int numServers) {
    int i;
    char daemonCmd[1024];

    strcpy(daemonCmd, "update_dns");

    mDnsForwarders->clear();
    for (i = 0; i < numServers; i++) {
        LOGD("setDnsForwarders(%d = '%s')", i, servers[i]);

        struct in_addr a;

        if (!inet_aton(servers[i], &a)) {
            LOGE("Failed to parse DNS server '%s'", servers[i]);
            mDnsForwarders->clear();
            return -1;
        }
        strcat(daemonCmd, ":");
        strcat(daemonCmd, servers[i]);
        mDnsForwarders->push_back(a);
    }

    if (mDaemonFd != -1) {
        LOGD("Sending update msg to dnsmasq [%s]", daemonCmd);
        if (write(mDaemonFd, daemonCmd, strlen(daemonCmd) +1) < 0) {
            LOGE("Failed to send update command to dnsmasq (%s)", strerror(errno));
            mDnsForwarders->clear();
            return -1;
        }
    }
    return 0;
}

NetAddressCollection *TetherController::getDnsForwarders() {
    return mDnsForwarders;
}

int TetherController::tetherInterface(const char *interface) {
    mInterfaces->push_back(strdup(interface));
    return 0;
}

int TetherController::untetherInterface(const char *interface) {
    InterfaceCollection::iterator it;

    for (it = mInterfaces->begin(); it != mInterfaces->end(); ++it) {
        if (!strcmp(interface, *it)) {
            free(*it);
            mInterfaces->erase(it);
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

InterfaceCollection *TetherController::getTetheredInterfaceList() {
    return mInterfaces;
}
