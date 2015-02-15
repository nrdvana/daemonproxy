#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <errno.h>
#include <string.h>

void sender(int fd);
void receiver(int fd);

int main() {
	int fd[2], wstat;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd)) { perror("socketpair"); abort(); }
	pid_t child= fork();
	if (child < 0) { perror("fork"); abort(); }
	if (child) {
		close(fd[1]);
		receiver(fd[0]);
		wait(&wstat);
		if (wstat) { fprintf(stderr, "child exitied %d\n", wstat); abort(); }
	}
	else {
		close(fd[0]);
		sender(fd[1]);
	}
	return 0;
}

void sender(int fd) {
	char str[]= "0123456789012345678901234567890123456789";
	struct msghdr msg;
	struct iovec  iov;
	struct cmsghdr *cmsg;
	char control_buf[256];
	int new_fds[4];
	int n;
	if (pipe(new_fds) || pipe(new_fds+2)) { perror("pipe"); abort(); }

	n= send(fd, str, 20, 0);
	fprintf(stderr, "send: %d (%m)\n", n);

	memset(&msg, 0, sizeof(msg));
	memset(&iov, 0, sizeof(iov));
	iov.iov_base= str;
	iov.iov_len=  7;
	msg.msg_iov= &iov;
	msg.msg_iovlen= 1;
	msg.msg_control=    control_buf;
	msg.msg_controllen= sizeof(control_buf);
	cmsg= CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level= SOL_SOCKET;
	cmsg->cmsg_type=  SCM_RIGHTS;

	msg.msg_controllen= cmsg->cmsg_len= CMSG_LEN(sizeof(int)*2);
	((int*) CMSG_DATA(cmsg))[0]= new_fds[0];
	((int*) CMSG_DATA(cmsg))[1]= new_fds[1];
	n= sendmsg(fd, &msg, 0);
	fprintf(stderr, "sendmsg: %d (%m)\n", n);
	msg.msg_controllen= cmsg->cmsg_len;
	
	msg.msg_controllen= cmsg->cmsg_len= CMSG_LEN(sizeof(int)*1);
	((int*) CMSG_DATA(cmsg))[0]= new_fds[2];
	n= sendmsg(fd, &msg, 0);
	fprintf(stderr, "sendmsg: %d (%m)\n", n);

	n= send(fd, str, 10, 0);
	fprintf(stderr, "send: %d (%m)\n", n);

	msg.msg_controllen= cmsg->cmsg_len= CMSG_LEN(sizeof(int)*5);
	((int*) CMSG_DATA(cmsg))[0]= new_fds[0];
	((int*) CMSG_DATA(cmsg))[1]= new_fds[1];
	((int*) CMSG_DATA(cmsg))[2]= new_fds[2];
	((int*) CMSG_DATA(cmsg))[3]= new_fds[3];
	((int*) CMSG_DATA(cmsg))[4]= 0;
	n= sendmsg(fd, &msg, 0);
	fprintf(stderr, "sendmsg: %d (%m)\n", n);

	if (close(fd)) { perror("close"); abort(); }
	fprintf(stderr, "sender exiting\n");
}


void read_ancillary_fds(struct msghdr *msg) {
	// Find any new FD which has been delivered to us
	// We store at most 2 of them (one for the current message which might not be
	// complete, and one for the next message which might deliver ancillary data
	// as we get the remainder of the first message)
	struct cmsghdr *cmsg;
	for (cmsg= CMSG_FIRSTHDR(msg); cmsg; cmsg= CMSG_NXTHDR(msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
			int i, fd, num_fd= (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			for (i= 0; i < num_fd; i++) {
				fd= ((int*) CMSG_DATA(cmsg))[i];
				fprintf(stderr, "received ancillary file descriptor %d\n", fd);
				//close(fd);
			}
		}
	}
}

void receiver(int fd) {
	char buffer[24], control_buf[256];
	struct msghdr msg;
	struct iovec  iov;
	struct cmsghdr *cmsg;
	int n;
	
	sleep(2);
	while (1) {
		memset(&msg, 0, sizeof(msg));
		memset(&iov, 0, sizeof(iov));
		iov.iov_base= buffer;
		iov.iov_len= sizeof(buffer);
		msg.msg_iov= &iov;
		msg.msg_iovlen= 1;
		msg.msg_control= control_buf;
		msg.msg_controllen= sizeof(control_buf);
		
		n= recvmsg(fd, &msg, 0);
		fprintf(stderr, "recvmsg: %d (%m) controllen=%d\n", n, msg.msg_controllen);
		if (n <= 0) break;
		read_ancillary_fds(&msg);
	}
	if (close(fd)) { perror("close"); abort(); }
	fprintf(stderr, "receiver exiting\n");
}