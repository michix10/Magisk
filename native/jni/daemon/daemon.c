/* daemon.c - Magisk Daemon
 *
 * Start the daemon and wait for requests
 * Connect the daemon and send requests through sockets
 */

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/mount.h>

#include "magisk.h"
#include "utils.h"
#include "daemon.h"
#include "selinux.h"
#include "flags.h"

int setup_done = 0;
int seperate_vendor = 0;

static void get_client_cred(int fd, struct ucred *cred) {
	socklen_t ucred_length = sizeof(*cred);
	if(getsockopt(fd, SOL_SOCKET, SO_PEERCRED, cred, &ucred_length))
		PLOGE("getsockopt");
}

static void *request_handler(void *args) {
	int client = *((int *) args);
	free(args);
	int req = read_int(client);

	struct ucred credential;
	get_client_cred(client, &credential);

	switch (req) {
	case LAUNCH_MAGISKHIDE:
	case STOP_MAGISKHIDE:
	case ADD_HIDELIST:
	case RM_HIDELIST:
	case LS_HIDELIST:
	case POST_FS_DATA:
	case LATE_START:
	case BOOT_COMPLETE:
		if (credential.uid != 0) {
			write_int(client, ROOT_REQUIRED);
			close(client);
			return NULL;
		}
	default:
		break;
	}

	switch (req) {
	case LAUNCH_MAGISKHIDE:
		launch_magiskhide(client);
		break;
	case STOP_MAGISKHIDE:
		stop_magiskhide(client);
		break;
	case ADD_HIDELIST:
		add_hide_list(client);
		break;
	case RM_HIDELIST:
		rm_hide_list(client);
		break;
	case LS_HIDELIST:
		ls_hide_list(client);
		break;
	case SUPERUSER:
		su_daemon_handler(client, &credential);
		break;
	case CHECK_VERSION:
		write_string(client, xstr(MAGISK_VERSION) ":MAGISK");
		close(client);
		break;
	case CHECK_VERSION_CODE:
		write_int(client, MAGISK_VER_CODE);
		close(client);
		break;
	case POST_FS_DATA:
		post_fs_data(client);
		break;
	case LATE_START:
		late_start(client);
		break;
	case BOOT_COMPLETE:
		boot_complete(client);
		break;
	case HANDSHAKE:
		/* Do NOT close the client, make it hold */
		break;
	default:
		close(client);
		break;
	}
	return NULL;
}

static void main_daemon() {
	android_logging();
#ifndef MAGISK_DEBUG
	log_cb.d = nop_log;
#endif
	setsid();
	setcon("u:r:"SEPOL_PROC_DOMAIN":s0");
	int fd = xopen("/dev/null", O_RDWR | O_CLOEXEC);
	xdup2(fd, STDOUT_FILENO);
	xdup2(fd, STDERR_FILENO);
	close(fd);
	fd = xopen("/dev/zero", O_RDWR | O_CLOEXEC);
	xdup2(fd, STDIN_FILENO);
	close(fd);

	struct sockaddr_un sun;
	socklen_t len = setup_sockaddr(&sun, MAIN_SOCKET);
	fd = xsocket(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (xbind(fd, (struct sockaddr*) &sun, len))
		exit(1);
	xlisten(fd, 10);
	LOGI("Magisk v" xstr(MAGISK_VERSION) "(" xstr(MAGISK_VER_CODE) ") daemon started\n");

	// Change process name
	strcpy(argv0, "magiskd");

	// Block all user signals
	sigset_t block_set;
	sigemptyset(&block_set);
	sigaddset(&block_set, SIGUSR1);
	sigaddset(&block_set, SIGUSR2);
	pthread_sigmask(SIG_SETMASK, &block_set, NULL);

	// Ignore SIGPIPE
	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &act, NULL);

	// Loop forever to listen for requests
	while(1) {
		int *client = xmalloc(sizeof(int));
		*client = xaccept4(fd, NULL, NULL, SOCK_CLOEXEC);
		pthread_t thread;
		xpthread_create(&thread, NULL, request_handler, client);
		// Detach the thread, we will never join it
		pthread_detach(thread);
	}
}

int connect_daemon() {
	struct sockaddr_un sun;
	socklen_t len = setup_sockaddr(&sun, MAIN_SOCKET);
	int fd = xsocket(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (connect(fd, (struct sockaddr*) &sun, len)) {
		if (getuid() != UID_ROOT || getgid() != UID_ROOT) {
			fprintf(stderr, "No daemon is currently running!\n");
			exit(1);
		}

		LOGD("client: launching new main daemon process\n");
		if (fork_dont_care() == 0) {
			close(fd);
			main_daemon();
		}

		while (connect(fd, (struct sockaddr*) &sun, len))
			usleep(10000);
	}
	return fd;
}
