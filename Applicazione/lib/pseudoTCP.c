#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "pseudoTCP.h"

void start_timer(packet *p) {
	struct timeval start;
	gettimeofday(&start, NULL);
	p->timer = (start.tv_sec * 1000000) + start.tv_usec;
}

unsigned long get_timer(packet *p) {
	struct timeval stop;

	if (p->timer == 0) {
		return 0;
	}
	
	gettimeofday(&stop, NULL);
	return ((stop.tv_sec * 1000000) + stop.tv_usec) - p->timer;
}

int check_timeout(int socket_desc, struct sockaddr_in addr, packet *p, struct adaptive_timeout *at, struct cg_control *cg) {
	unsigned long timer;
	
	if ((timer = get_timer(p)) < at->timeout) {
		return 0;
	}
	
	p->n_retransmission++;
	start_timer(p);
	send_pkt(socket_desc, addr, p);
	
	reset_cgwin(cg);
}

int update_timeout(struct adaptive_timeout *at, packet *p, int current_ack_rcv, int ack_retransmission) {
#ifdef AT
	int sampleRTT;
	float a, b;
	
	// valori suggeriti da RFC 6298
	a = 0.125;
	b = 0.25;
	
	if (p->num_seq + p->pkt_size != current_ack_rcv) {
		return -1;
	}

	sampleRTT = get_timer(p) + (p->n_retransmission - ack_retransmission) * at->timeout;
						
	at->estimatedRTT = (int)((at->estimatedRTT)*(1-a)) + (int)(sampleRTT*a);
	at->devRTT = (int)((at->devRTT)*(1-b)) + (int)(b*abs(sampleRTT - at->estimatedRTT));
	at->timeout = at->estimatedRTT + (at->devRTT)*4;
#endif
	return 0;
}

int reset_cgwin(struct cg_control *cg) {
	if (cg->cgwin == 1) {
		cg->sstresh = 1;
	} else {
		cg->sstresh = cg->cgwin/2;
	}
	cg->cgwin = 1;
	cg->ack_counter = 0;
}

int increase_cgwin(struct cg_control *cg) {
	// Limite dimensione finestra d'invio
	if (cg->cgwin == MAX_WINDOW) {
		return 0;
	}
	
	if (cg->cgwin < cg->sstresh) {
		cg->cgwin++;
	} else {
		cg->ack_counter++;
		if (cg->ack_counter == cg->cgwin) {
			cg->cgwin++;
			cg->ack_counter = 0;
		} 
	}
}

packet *make_pkt(unsigned long long num_seq, unsigned int ack, unsigned long long num_ack, unsigned long long timer, unsigned int file_size, unsigned int pkt_size, 
	unsigned short n_retransmission, unsigned int command, char *data) {
	
	packet *p;
	
	p = (packet*)malloc(sizeof(packet));
	
	p->num_seq = num_seq;
	p->ack = ack;
	p->num_ack = num_ack;
	p->timer = timer;
	p->file_size = file_size;
	p->pkt_size = pkt_size;
	p->n_retransmission = n_retransmission;
	p->command = command;
	memset(p->data, '\0', sizeof(p->data));
	if (strlen(data) > 0) {
		memcpy(p->data, data, pkt_size); 
	}
	
	return p;
}

int send_pkt(int socket_desc, struct sockaddr_in addr, packet *p) {
	int prob;
	prob = ((double)rand() / (double)RAND_MAX) * 100;

	// Perdita pacchetto con probabilità P
	if (prob > P) {
		if (sendto(socket_desc, p, sizeof(packet), 0, (struct sockaddr*)&addr, sizeof(addr)) < 0){
			perror("errore in sendto");
			return -1;
		}
	}
}

int copy_pkt(packet *dest, packet *src) {
	dest->num_seq = src->num_seq;
	dest->ack = src->ack;
	dest->num_ack = src->num_ack;
	dest->timer = src->timer;
	dest->file_size = src->file_size;
	dest->pkt_size = src->pkt_size;
	dest->n_retransmission = src->n_retransmission;
	dest->command = src->command;
	memset(dest->data, '\0', sizeof(dest->data));
	memcpy(dest->data, src->data, src->pkt_size);
	
	return 0;
}

int quick_retransmission(int socket_desc, struct sockaddr_in addr, packet *p, int *retransmission_counter) {
	if (*retransmission_counter > 3) {
			p->n_retransmission++;
			start_timer(p);
			send_pkt(socket_desc, addr, p);
			*retransmission_counter = 0;
	} else {
		*retransmission_counter++;
	}
}
