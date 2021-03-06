#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include "mtcp_server.h"
#include <mtcp_common.h>

/* -------------------- Global Variables -------------------- */
unsigned char mtcp_internal_buffer[5 * MAX_BUF_SIZE];
unsigned int global_internal_buffer_pointer = 0;
unsigned int global_application_buffer_pointer = 0;
unsigned int global_connection_state = 0;
unsigned int global_last_packet_received = -1;
unsigned int global_last_packet_sent = -1;
unsigned int global_seq = 0;
int global_packet_size = 0;

typedef struct mtcpheaders
{
	unsigned int seq;			// sequence number
	unsigned char mode;			// mode code
	unsigned char buffer[4]; 	// header byte array

} mtcpheader;

struct arg_list
{
	int socket;
	struct sockaddr_in client_addr;
};

struct arg_list client_arg;

/* ThreadID for Sending Thread and Receiving Thread */
static pthread_t send_thread_pid;
static pthread_t recv_thread_pid;

static pthread_cond_t app_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t app_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t send_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t send_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t info_mutex = PTHREAD_MUTEX_INITIALIZER;

/* The Sending Thread and Receive Thread Function */
static void *send_thread();
static void *receive_thread();
/****server_addr or client_addr?***/

/* Three Way Handshake Function*/
static void send_SYN_ACK();

/* Data Transmition Function*/
static void send_ACK();

/* Four Way Handshake Function */
static void send_FIN_ACK(struct arg_list *arg, int seq);

/* Accept Function Call (mtcp Version) */
void mtcp_accept(int socket_fd, struct sockaddr_in *client_addr){
	// accept the mtcp call by client
	client_arg.socket = socket_fd;
	client_arg.client_addr = *client_addr;
	//create thread to handle mtcp_accept()
	int rpc = pthread_create(&recv_thread_pid, NULL, receive_thread, (void *)&client_arg);
	if (rpc < 0) printf("create receving thread error\n");
	int spc = pthread_create(&send_thread_pid, NULL, send_thread, (void *)&client_arg);
	if (spc < 0) printf("create sending thread error\n");


	// waiting
	pthread_mutex_lock(&app_thread_sig_mutex);
	pthread_cond_wait(&app_thread_sig, &app_thread_sig_mutex); // wait
	pthread_mutex_unlock(&app_thread_sig_mutex);



	return;

}

int mtcp_read(int socket_fd, unsigned char *buf, int buf_len){

	//buf_len each time must be 1000
	pthread_mutex_lock(&app_thread_sig_mutex);
	pthread_cond_wait(&app_thread_sig, &app_thread_sig_mutex);
	pthread_mutex_unlock(&app_thread_sig_mutex);

	if ( (global_packet_size-4) < buf_len ) {
		pthread_mutex_lock(&info_mutex);
		memcpy(buf, &mtcp_internal_buffer[global_internal_buffer_pointer], global_packet_size-4);
		global_internal_buffer_pointer += (global_packet_size-4);
		printf("global_internal_buffer_pointer += %d\n", global_packet_size -4);
		pthread_mutex_unlock(&info_mutex);
		return global_packet_size-4;
	} else {
		pthread_mutex_lock(&info_mutex);
		memcpy(buf, &mtcp_internal_buffer[global_internal_buffer_pointer], buf_len);
		global_internal_buffer_pointer += buf_len;
		printf("global_internal_buffer_pointer += %d\n", buf_len);
		pthread_mutex_unlock(&info_mutex);
		return buf_len;
	}


}

void mtcp_close(int socket_fd){

	//change te connection state
	// pthread_mutex_lock(&info_mutex);
	// global_connection_state = 2;
	// pthread_mutex_unlock(&info_mutex);
	pthread_mutex_lock(&app_thread_sig_mutex);
	pthread_cond_wait(&app_thread_sig, &app_thread_sig_mutex);
	pthread_mutex_unlock(&app_thread_sig_mutex);

	pthread_join(send_thread_pid,NULL);

	// pthread_mutex_lock(&app_thread_sig_mutex);
	// pthread_cond_wait(&app_thread_sig, &app_thread_sig_mutex);
	// pthread_mutex_unlock(&app_thread_sig_mutex);

	pthread_join(recv_thread_pid,NULL);

	return;
}

static void *send_thread(void *client_arg){
	printf("Send Thread Start\n");
	// printf("Sleep\n");
	// sleep(1);//sleep for 1 sec
	struct arg_list *arg = (struct arg_list *)client_arg;
	int connection_state;
	// 0 = three way handshake
	// 1 = data transmition
	// 2 = four way handshake
	int last_flag_received;
	// 0 = SYN received
	// 5 = DATA received
	// 2 = FIN received
	// 4 = ACK received
	int seq;

	/************************************************************************
	************************* Connection State ******************************
	*************************************************************************/
	while (1) {

		pthread_mutex_lock(&send_thread_sig_mutex);
		pthread_cond_wait(&send_thread_sig, &send_thread_sig_mutex); // wait
		pthread_mutex_unlock(&send_thread_sig_mutex);
		// check state
		pthread_mutex_lock(&info_mutex);

		if (global_connection_state == 0) {
			connection_state = 0;
		} else if (global_connection_state == 1) {
			connection_state = 1;
		} else if (global_connection_state == 2) {
			connection_state = 2;
		} else {
			connection_state = -1;
		}

		if (global_last_packet_received == 0) {
			last_flag_received = 0;
		} else if (global_last_packet_received == 2) {
			last_flag_received = 2;
		} else if (global_last_packet_received == 4) {
			last_flag_received = 4;
		} else if (global_last_packet_received == 5) {
			last_flag_received = 5;
		} else if (global_last_packet_received == -1) {
			last_flag_received = -1;
		}

		seq = global_seq;
		pthread_mutex_unlock(&info_mutex);

		// send packet
		if (connection_state == 0) {
			// perform three way Handshake
			if (last_flag_received == 0) {
				// send SYN-ACK to client
				send_SYN_ACK(arg, seq);
				pthread_mutex_lock(&info_mutex);
				global_last_packet_sent = 1;
				pthread_mutex_unlock(&info_mutex);
				// wait for ACK
				// pthread_mutex_lock(&send_thread_sig_mutex);
				// pthread_cond_wait(&send_thread_sig, &send_thread_sig_mutex);
				// pthread_mutex_unlock(&send_thread_sig_mutex);
			} else if (last_flag_received == 4){
				pthread_mutex_lock(&info_mutex);
				global_connection_state = 1;
				printf("Three Way Handshake established\n");
				pthread_mutex_unlock(&info_mutex);
				pthread_cond_signal(&app_thread_sig);
			}  else {
				printf("three_way_handshake error\n");
			}
		} else if (connection_state == 1) {
			send_ACK(arg, seq);
			pthread_mutex_lock(&info_mutex);
			global_last_packet_sent = 4;
			pthread_mutex_unlock(&info_mutex);

			pthread_cond_signal(&app_thread_sig);
			// send or retransmit data packet
		} else if (connection_state == 2) {
			if (last_flag_received == 2) {
				send_FIN_ACK(arg, seq);
				pthread_mutex_lock(&info_mutex);
				global_last_packet_sent = 3;
				pthread_mutex_unlock(&info_mutex);
				pthread_cond_signal(&app_thread_sig);
			} else if (last_flag_received == 4){
				pthread_mutex_lock(&info_mutex);
				global_last_packet_received = 4;
				printf("Four Way Handshake established\n");
				pthread_mutex_unlock(&info_mutex);
				pthread_cond_signal(&app_thread_sig);
				break;
			}
		} else {
			// unknown error
			printf("unknown error\n");
		}

	}
	return 0;
}

static void *receive_thread(void *client_arg){
	printf("Receive Thread Start\n");
	struct arg_list *arg = (struct arg_list *)client_arg;
	socklen_t addrlen = sizeof(arg->client_addr);
	// memset(mtcp_internal_buffer, 0, 5 * MAX_BUF_SIZE);

	// keep monitoring
	while (1) {

		unsigned int seq;
		unsigned int mode;
		unsigned char buff[MAX_BUF_SIZE+4];

		// monitor socket
		// packet_size = recvfrom(arg->socket, buff, sizeof(buff), 0, (struct sockaddr*)&arg->client_addr, &addrlen);

		if( (global_packet_size = recvfrom(arg->socket, buff, sizeof(buff), 0, (struct sockaddr*)&arg->client_addr, &addrlen)) < 0) {
			printf("Send Error: %s (Errno:%d)\n",strerror(errno),errno);
			exit(1);
		}

		// decode header
		mode = (buff[0] >> 4);
		buff[0] = buff[0] & 0x0F;
		memcpy(&seq, buff, 4);
		seq = ntohl(seq);

		printf("seq received = %d\n", seq);
		printf("packet_size = %d\n", global_packet_size);



		switch(mode) {
			case 0: // SYN
			// when SYN received
			printf("SYN received\n");
			pthread_mutex_lock(&info_mutex);
			global_last_packet_received = 0;
			global_connection_state = 0;
			pthread_mutex_unlock(&info_mutex);
			pthread_cond_signal(&send_thread_sig);
			break;
			case 2: // FIN
			// when FIN received
			printf("FIN received\n");
			pthread_mutex_lock(&info_mutex);
			global_last_packet_received = 2;
			global_connection_state = 2;
			global_seq = seq;
			pthread_mutex_unlock(&info_mutex);
			pthread_cond_signal(&send_thread_sig);
			break;
			case 4: // ACK
			// when ACK received
			printf("ACK received\n");
			pthread_mutex_lock(&info_mutex);
			global_last_packet_received = 4;
			pthread_mutex_unlock(&info_mutex);
			pthread_cond_signal(&send_thread_sig);
			break;
			case 5: //DATA
			// when DATA received
			pthread_mutex_lock(&info_mutex);
			global_last_packet_received = 5;
			if (seq > global_seq) {
				global_seq = seq;
				memcpy(&mtcp_internal_buffer[seq-1], &buff[4], global_packet_size-4);
				// global_internal_buffer_pointer += (global_packet_size-4);
			}
			pthread_mutex_unlock(&info_mutex);
			pthread_cond_signal(&send_thread_sig);
			break;
			default:
			printf("receive switch error\n");
		}

		if(global_connection_state == 2){
			if (global_last_packet_received == 4){
				break;
			}
		}

	}
	printf("Thread Stop\n");
	return 0;
}

static void send_ACK(struct arg_list *arg, int seq) {
	// construct mtcp SYN-ACK header
	mtcpheader ACK;
	pthread_mutex_lock(&info_mutex);
	ACK.seq = seq + global_packet_size - 4;
	pthread_mutex_unlock(&info_mutex);
	printf("seq = %d\n", ACK.seq);
	ACK.seq = htonl(ACK.seq);
	ACK.mode = '4';
	memcpy(ACK.buffer, &ACK.seq, 4);
	ACK.buffer[0] = ACK.buffer[0] | (ACK.mode << 4);

	// send ACK to client
	if(sendto(arg->socket, ACK.buffer, sizeof(ACK.buffer), 0, (struct sockaddr*)&arg->client_addr, (socklen_t)sizeof(arg->client_addr)) <=0) {
		printf("Send Error: %s (Errno:%d)\n",strerror(errno),errno);
		exit(1);
	}
	printf("ACK sent\n\n");
}

static void send_SYN_ACK(struct arg_list *arg, int seq) {
	// construct mtcp SYN-ACK header
	mtcpheader SYN_ACK;
	SYN_ACK.seq = seq + 1;
	SYN_ACK.seq = htonl(SYN_ACK.seq);
	SYN_ACK.mode = '1';
	memcpy(SYN_ACK.buffer, &SYN_ACK.seq, 4);
	SYN_ACK.buffer[0] = SYN_ACK.buffer[0] | (SYN_ACK.mode << 4);
	// send SYN-ACK to client
	if(sendto(arg->socket, SYN_ACK.buffer, sizeof(SYN_ACK.buffer), 0, (struct sockaddr*)&arg->client_addr, (socklen_t)sizeof(arg->client_addr)) <=0) {
		printf("Send Error: %s (Errno:%d)\n",strerror(errno),errno);
		exit(1);
	}
	printf("SYN-ACK sent\n");
}

static void send_FIN_ACK(struct arg_list *arg, int seq) {
	// construct mtcp SYN-ACK header
	mtcpheader FIN_ACK;
	FIN_ACK.seq = seq + 1;
	printf("seq = %d\n", FIN_ACK.seq);
	FIN_ACK.seq = htonl(FIN_ACK.seq);
	FIN_ACK.mode = '3';
	memcpy(FIN_ACK.buffer, &FIN_ACK.seq, 4);
	FIN_ACK.buffer[0] = FIN_ACK.buffer[0] | (FIN_ACK.mode << 4);

	// send FIN_ACK to client
	if(sendto(arg->socket, FIN_ACK.buffer, sizeof(FIN_ACK.buffer), 0, (struct sockaddr*)&arg->client_addr, (socklen_t)sizeof(arg->client_addr)) <=0) {
		printf("Send Error: %s (Errno:%d)\n",strerror(errno),errno);
		exit(1);
	}
	printf("FIN_ACK sent\n\n");

}
