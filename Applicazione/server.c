#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <dirent.h>
#include <signal.h>

#include "lib/pseudoTCP.h"
#include "lib/dynamiclist.h"

#define PORT 3600

int user_handler(int socket_desc, struct sockaddr_in addr, socklen_t len);
int pagination();
int count_pages();
int delete_pages();
int get_file_size(FILE **fp, char *filename);
int get(FILE **fp, char *filename, dynamic_list *list_send, packet *p, int *num_seq, int *expected_num_seq, int file_size, int seek, int loop);

int quit_signal;

void signal_handler(int signum) {
	quit_signal = 1;
}

int main () {
	int pid;
	int status;
	int active_children;
	int exit_proc;
	
	int socket_desc, socket_desc_child;
	struct sockaddr_in addr, client_addr;
	socklen_t len = sizeof(struct sockaddr_in);
	
	packet *p;
	p = (packet*)malloc(sizeof(packet));

	active_children = 0;
	
	// Creo la socket di benvenuto
  if ((socket_desc = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
  	perror("errore creazione socket\n");
  	return -1;
  }
    
  // Imposto il nuovo numero di porta e l'indirizzo IP
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(PORT);
  
  // Bind 
  if (bind(socket_desc, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("errore in bind\n");
  	return -1;
  }
  
  signal(SIGINT, signal_handler);
  
  // Effettuo la paginazione
	if (pagination() < 0) {
		perror("errore paginazione");
		exit(0);
	}
  
  quit_signal = 0;
  
  system("clear");
  
  while (1) {
  	// Segnale di chiusura
  	if (quit_signal) {
			printf("Richiesta di chiusura\n");
			
			while (active_children > 0) {
				printf("In attesa di %d figli\n", active_children);
				exit_proc = waitpid(-1, &status, 0);
				
				if (exit_proc > 0 && WIFEXITED(status)) {
					active_children--;
					printf("Processo %d terminato\n", exit_proc);
				}
			}
			
			break;
		}
		
		// Decremento active_children se qualche processo termina
		if ((exit_proc = waitpid(-1, &status, WNOHANG)) > 0 && WIFEXITED(status)) {
			active_children--;
			printf("Processo %d terminato\n", exit_proc);
		}
			
  	// Ascolto il client
		if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&addr, &len) < 0) {
			continue;
		}
		
		if (p->command == 1) {
			printf("Richiesta di connessione da parte del client %s\n", inet_ntoa(addr.sin_addr));
			
			pid = fork();
			
			active_children++;
			
			if (pid == 0) {
			
				// Creo la socket di comunicazione dedicata
				if ((socket_desc_child = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
					perror("errore creazione socket processo figlio\n");
					return -1;
				}
				
				user_handler(socket_desc_child, addr, len);
				
				printf("Connessione con il client sul processo %d interrotta\n", getpid());
				
				close(socket_desc_child);
				
				exit(0);
			} else {
				continue;
			}
		}
  }
  
  delete_pages();
  
  close(socket_desc);
  printf("Chiusura server padre\n");
  exit(0);
}

int user_handler(int socket_desc, struct sockaddr_in addr, socklen_t len) {
	packet *p, *last_pkt;
	
	int expected_num_seq;
	int num_seq;
	int current_ack_rcv;
	
	int retransmission_counter;
	int ack_retransmission;
	int pkt_not_acked;
	int eof;
	int byte_rcvd;
	int total_byte_sent;
	int byte_ready_to_send;
	int byte_read;
	int data_size;
	int counter;
	int n_pages;
	char filename[1000];
	int command_acked;
	dynamic_list list_send, list_rcv;
	struct adaptive_timeout at;
	struct cg_control cg;
	FILE *fp;
	char path[1100];
	struct timeval start, stop;
	
	// INIZIALIZZAZIONE VARIABILI
	
	// Allocazione memoria pacchetto
	p = (packet*)malloc(sizeof(packet));
	last_pkt = (packet*)malloc(sizeof(packet));
	
	// Allocazione memoria liste d'invio e di ricezione
	create(&list_send);
	create(&list_rcv);
	
	// Variabili per il timeout adattativo (valore default 10ms)
	at.timeout = TIMEOUT;
	at.estimatedRTT = TIMEOUT;
	at.devRTT = 0;
	
	// Variabili per il controllo di congestione
	cg.cgwin = 1;
	cg.sstresh = 8;
	cg.ack_counter = 0;
	
	// Variabili per info pacchetti
	current_ack_rcv = 0;
	num_seq = 0;
	expected_num_seq = 0;
	pkt_not_acked = 0;
	retransmission_counter = 0;
	
	// Variabili per trasferimento dati
	eof = 0;
	byte_rcvd = 0;
	command_acked = 0;
	
	// Segnale di chiusura
	quit_signal = 0;
	
	signal(SIGINT, signal_handler);
				
/* ---------------------------------------------- HANDSHAKE ---------------------------------------------- */
	expected_num_seq++;
	
	// Preparo SYNACK
	p = make_pkt(num_seq, 1, expected_num_seq, 0, 0, 0, 0, 1, "");
	
	// Invio pacchetto
	start_timer(p);
	send_pkt(socket_desc, addr, p);
	
	// Salvo pacchetto per ritrasmetterlo in caso di perdita
	copy_pkt(last_pkt, p);
	
	// Finchè non ricevo ACK rimango in attesa
	gettimeofday(&start, NULL);
	while (1) {
		gettimeofday(&stop, NULL);
		
		// Dopo un certo periodo di inattività deduco che la connessione si è interrotta
		if (stop.tv_sec - start.tv_sec >= WAIT) {
			printf("Il client non risponde. Chiusura processo figlio %d\n", getpid());
			
			// Dealloco le risorse
			free(p);
			free(last_pkt);
			return -1;
		}
		
		check_timeout(socket_desc, addr, last_pkt, &at, &cg);
	
		if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&addr, &len) < 0) {
			continue;
		}
		
		// Resetto timer
		gettimeofday(&start, NULL);
		
		if (p->ack == 1 && p->num_ack > current_ack_rcv) {
			num_seq += 1;
			current_ack_rcv = p->num_ack;
			break;
		}
	}
/* ---------------------------------------------- HANDSHAKE ---------------------------------------------- */
	
	while (1) {
/* --------------------------------------------- QUIT SIGNAL --------------------------------------------- */
		if (quit_signal) {
			// Preparo FIN
			p = make_pkt(num_seq, 0, 0, 0, 0, 0, 0, 2, "");
			
			num_seq += 1; 
			
			start_timer(p);
			send_pkt(socket_desc, addr, p);
			
			copy_pkt(last_pkt, p);
			
			// Finchè non ricevo ACK rimango in attesa
			gettimeofday(&start, NULL);
			while (1) {
				gettimeofday(&stop, NULL);
		
				// Dopo un certo periodo di inattività deduco che la connessione si è interrotta
				if (stop.tv_sec - start.tv_sec >= WAIT) {
					printf("Il client non risponde. Chiusura processo %d\n", getpid());
					
					// Dealloco le risorse
					free(p);
					free(last_pkt);
					return -1;
				}
				
				check_timeout(socket_desc, addr, last_pkt, &at, &cg);
			
				if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&addr, &len) < 0) {
					continue;
				}
				
				// Resetto timer
				gettimeofday(&start, NULL);
				
				if (p->ack == 1 && p->num_ack > current_ack_rcv) {
					printf("Chiusura\n");
					break;
				}
			}
			
			break;
		}
/* --------------------------------------------- QUIT SIGNAL --------------------------------------------- */
		
/* ------------------------------------------- ASCOLTO CLIENT -------------------------------------------- */
		memset(p->data, '\0', sizeof(p->data));
		
		// Ascolto il client
		if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&addr, &len) < 0) {
			continue;
		}

		// Ricezione pacchetti già ricevuti (ad esempio in seguito ad una put o syn)
		// Escludo gli ACK poichè non concludo nessuna funzione senza prima ricevere l'ACK
		if (p->ack == 0 && p->num_seq < expected_num_seq) {
			send_pkt(socket_desc, addr, last_pkt);
			continue;
		}
		printf("Ricevuto comando %d dal pacchetto con num_seq %llu sul processo %d\n", p->command, p->num_seq, getpid());
/* ------------------------------------------- ASCOLTO CLIENT -------------------------------------------- */
		
/* ------------------------------------------------ FIN -------------------------------------------------- */
		if (p->command == 2) {
		
			expected_num_seq++;
		
			// Invio ACK
			p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, 0, 0, "");
			
			send_pkt(socket_desc, addr, p);
			copy_pkt(last_pkt, p);
			
			// Attesa temporizzata
			gettimeofday(&start, NULL);
			while (1) {
				gettimeofday(&stop, NULL);
				if (stop.tv_sec - start.tv_sec >= WAIT) {
					// Dealloco le risorse
					free(p);
					free(last_pkt);
					return -1;
				}
						
				if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&addr, &len) < 0) {
					continue;
				}
				
				// breve controllo sul pacchetto ricevuto
				if (p->command != 2) {
					continue;
				}
				
				// finchè ricevo ritrasmissioni deduco che il client non sta capendo che ho ricevuto il messaggio e quindi resetto il timer
				gettimeofday(&start, NULL);
				
				send_pkt(socket_desc, addr, last_pkt);
			}
			
			break;
		}
/* ------------------------------------------------ FIN -------------------------------------------------- */
		
/* ------------------------------------------------ LIST ------------------------------------------------- */
		if (p->command == 3) {
			// Conto preliminarmente il numero di pagine
			n_pages = count_pages();
			
			// errore nel conteggio del numero di pagine
			if (n_pages < 0) {
				perror("errore nel conteggio del numero di pagine");
				
				expected_num_seq += (strlen(p->data) > 1 ? strlen(p->data) : 1);
				
				// Invio ACK con messaggio d'errore
				p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, 0, 6, "");
				send_pkt(socket_desc, addr, p);
				copy_pkt(last_pkt, p);
				
				continue;
			}
			
			// Se ricevo list senza parametro e il numero dei file è > 1, invio errore file troppo grande
			if (strlen(p->data) == 0 && n_pages > 1) {
				perror("errore: file troppo grande");
				
				expected_num_seq += (strlen(p->data) > 1 ? strlen(p->data) : 1);
				
				// Invio ACK con messaggio d'errore
				p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, 0, 7, "");
				send_pkt(socket_desc, addr, p);
				copy_pkt(last_pkt, p);
				
				continue;
			}
			
			// Se ricevo list con parametro e il parametro è non valido, invio errore apertura file
			if (strlen(p->data) > 0 && (atoi(p->data) < 1) || atoi(p->data) > n_pages) {
				perror("errore: apertura file");
				
				expected_num_seq += (strlen(p->data) > 1 ? strlen(p->data) : 1);
				
				// Invio ACK con messaggio d'errore
				p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, 0, 6, "");
				send_pkt(socket_desc, addr, p);
				copy_pkt(last_pkt, p);
			
				continue;
			}
			
			// Se ricevo list senza parametro e il numero dei file é == 1, invio la pagina 1
			if (strlen(p->data) == 0 && n_pages == 1) {
				fp = fopen("pages/1", "r");
			} 
			
			// Altrimenti, invio la pagina specificata da utente
			else {
				sprintf(path, "pages/%d", atoi(p->data));
				fp = fopen(path, "r");
			}
			
			// Gestione errore apertura file
			if (fp == NULL) {
				perror("errore apertura file pagina");
				
				expected_num_seq += (strlen(p->data) > 1 ? strlen(p->data) : 1);
				
				// Invio ACK con messaggio d'errore
				p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, 0, 6, "");
				send_pkt(socket_desc, addr, p);
				copy_pkt(last_pkt, p);
				
				continue;
			}
			
			// Calcolo dimensione file da inviare
			fseek(fp, 0, SEEK_END);
			data_size = ftell(fp);
			rewind(fp);
				
			expected_num_seq += (strlen(p->data) > 1 ? strlen(p->data) : 1);
			
			// Riempio la lista d'invio
			byte_read = 0;
			int i = 0;
			while ((byte_read = fread(p->data, sizeof(char), sizeof(p->data), fp)) > 0) {
				p->ack = 0;
				p->num_ack = 0;
				p->timer = 0;
				p->n_retransmission = 0;
				p->file_size = data_size;
				p->command = 3;
				p->num_seq = num_seq;
				p->pkt_size = byte_read;
				
				// Solo per il primo pacchetto metto l'ack a 1
				if (i == 0) {
					p->ack = 1;
					p->num_ack = expected_num_seq;
					i++;
				}
				
				add(&list_send, p);
				
				num_seq += byte_read;
			}
			
			fclose(fp);
			
			// Procedo ad inviare i pacchetti preparati
			command_acked = 0;
			gettimeofday(&start, NULL);
			while (!is_empty(list_send)) {
				gettimeofday(&stop, NULL);
				if (stop.tv_sec - start.tv_sec >= WAIT) {
					perror("Client non risponde. Chiusura");
					
					// Dealloco le risorse
					free(p);
					free(last_pkt);
					return -1;
				}
					
				// Ritrasmetto pacchetti in timeout
				for (int i=0;i<pkt_not_acked;i++) {
					check_timeout(socket_desc, addr, list_send.packets[i], &at, &cg);
				}
				
				// Invio pacchetto
				if (pkt_not_acked < list_send.len && pkt_not_acked < cg.cgwin) {
					start_timer(list_send.packets[pkt_not_acked]);
					send_pkt(socket_desc, addr, list_send.packets[pkt_not_acked]);
					pkt_not_acked++;
				}
			
				// Se non ci sono pacchetti inviati e non riscontrati, riparto
				if (pkt_not_acked == 0) {
					continue;
				}
				
				// Ascolto pacchetti in modalità non bloccante
				if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&addr, &len) < 0) {
					continue;
				}
				
				// Resetto timer
				gettimeofday(&start, NULL);
				
				// Se ricevo una ritrasmissione del comando list, ritrasmetto il primo pacchetto
				if (p->command == 3 && !command_acked) {
					start_timer(list_send.packets[0]);
					send_pkt(socket_desc, addr, list_send.packets[0]);
					continue;
				}
					
				// Controllo su pacchetti non validi
				if (p->ack != 1 || p->num_ack < current_ack_rcv) {
					continue;
				}
						
				// Ritrasmissione rapida in caso di ack duplicati
				if (p->num_ack == current_ack_rcv) {
					quick_retransmission(socket_desc, addr, list_send.packets[0], &retransmission_counter);
				} 
				
				// Ack Validi
				if (p->num_ack > current_ack_rcv) {
					// è sufficiente ricevere almeno un ACK per sapere che il client sa che il server ha ricevuto il comando
					// di conseguenza, se arrivasse una ritrasmissione del comando get dovuta a ritardi di rete, scarto il pacchetto senza ritrasmettere
					command_acked = 1;
					
					retransmission_counter = 0;
					current_ack_rcv = p->num_ack;
					
					update_timeout(&at, list_send.packets[0], current_ack_rcv, p->n_retransmission);
					
					while (!is_empty(list_send) && 
					list_send.packets[0]->num_seq + list_send.packets[0]->pkt_size <= current_ack_rcv) {
						pop(&list_send);
						
						increase_cgwin(&cg);
						
						pkt_not_acked--;
					}
				}
			}
			
			printf("Operazione list sul processo %d terminata\n", getpid());
			continue;
		}
/* ------------------------------------------------ LIST ------------------------------------------------- */
		
/* ------------------------------------------------ GET -------------------------------------------------- */
		if (p->command == 4 && p->num_seq == expected_num_seq) {
			strcpy(filename, p->data);
			
			// Calcolo la dimensione del file
			data_size = get_file_size(&fp, filename);
			
			// Errore apertura file
			if (data_size < 0) {
				perror("errore apertura file");
				
				expected_num_seq += p->pkt_size;
				
				// Invio ACK con messaggio d'errore 6
				p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, 0, 6, "");
				send_pkt(socket_desc, addr, p);
				copy_pkt(last_pkt, p);
				
				continue;
			}
			
			// Errore file troppo grande
			if (data_size > 1000000000) {
				perror("errore file troppo grande");
				
				expected_num_seq += p->pkt_size;
				
				// Invio ACK con messaggio d'errore
				p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, 0, 7, "");
				send_pkt(socket_desc, addr, p);
				copy_pkt(last_pkt, p);
				
				continue;
			}
			
			total_byte_sent = 0;
			int k=0;
			while (total_byte_sent < data_size) {
				byte_ready_to_send = get(&fp, filename, &list_send, p, &num_seq, &expected_num_seq, data_size, total_byte_sent, k);
				
				command_acked = 0;
				gettimeofday(&start, NULL);
				while (!is_empty(list_send)) {
					gettimeofday(&stop, NULL);
					
					// Dopo un certo periodo di inattività, chiudo la connessione
					if (stop.tv_sec - start.tv_sec >= WAIT) {
						perror("Client non risponde. Chiusura");
						
						// Dealloco le risorse
						free(p);
						free(last_pkt);
						return -1;
					}
						
					// Ritrasmetto pacchetti in timeout
					for (int i=0;i<pkt_not_acked;i++) {
						check_timeout(socket_desc, addr, list_send.packets[i], &at, &cg);
					}
					
					// Invio pacchetto
					if (pkt_not_acked < list_send.len && pkt_not_acked < cg.cgwin) {
						start_timer(list_send.packets[pkt_not_acked]);
						send_pkt(socket_desc, addr, list_send.packets[pkt_not_acked]);
						pkt_not_acked++;
					}
				
					// Se non ci sono pacchetti inviati e non riscontrati, riparto
					if (pkt_not_acked == 0) {
						continue;
					}
					
					// Ascolto pacchetti in modalità non bloccante
					if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&addr, &len) < 0) {
						continue;
					}
					
					// Resetto timer
					gettimeofday(&start, NULL);
					
					// Se ricevo una ritrasmissione del comando get, ritrasmetto il primo pacchetto
					if (p->command == 4 && !command_acked) {
						start_timer(list_send.packets[0]);
						send_pkt(socket_desc, addr, list_send.packets[0]);
						continue;
					}
						
					// Controllo su pacchetti non validi
					if (p->ack != 1 || p->num_ack < current_ack_rcv) {
						continue;
					}
							
					// Ritrasmissione rapida in caso di ack duplicati
					if (p->num_ack == current_ack_rcv) {
						quick_retransmission(socket_desc, addr, list_send.packets[0], &retransmission_counter);
					} 
					
					// Ack validi
					if (p->num_ack > current_ack_rcv) {
						// è sufficiente ricevere almeno un ACK per sapere che il client sa che il server ha ricevuto il comando
						// di conseguenza, se arrivasse una ritrasmissione del comando get dovuta a ritardi di rete, scarto il pacchetto senza ritrasmettere
						command_acked = 1;
						
						retransmission_counter = 0;
						current_ack_rcv = p->num_ack;
						
						update_timeout(&at, list_send.packets[0], current_ack_rcv, p->n_retransmission);
						
						while (!is_empty(list_send) && list_send.packets[0]->num_seq + list_send.packets[0]->pkt_size <= current_ack_rcv) {
							pop(&list_send);
							
							increase_cgwin(&cg);
							
							pkt_not_acked--;
						}
					}
				}
				
				total_byte_sent += byte_ready_to_send;
				
				k++;
			}
			
			fclose(fp);
			printf("Operazione get sul processo %d terminata\n", getpid());
			continue;
		}
/* ------------------------------------------------ GET -------------------------------------------------- */
		
/* ------------------------------------------------ PUT -------------------------------------------------- */
		if (p->command == 5) {
			
			sprintf(path, "serverfiles/%s", p->data);
			
			if ((fp = fopen(path, "ab")) == NULL) {
				perror("errore apertura file");
				
				expected_num_seq += p->pkt_size;

				// Invio ACK con messaggio d'errore
				p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, 0, 6, "");
				send_pkt(socket_desc, addr, p);
				copy_pkt(last_pkt, p);
				
				continue;
			}
			
			// se sono qui è tutto ok, invio ACK
			
			data_size = p->file_size;
			
			expected_num_seq += p->pkt_size;
			
			// Invio ACK
			p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, 0, 0, "");
			send_pkt(socket_desc, addr, p);

			eof = 0;
			byte_rcvd = 0;
			command_acked = 0;
			gettimeofday(&start, NULL);
			while (!eof) {
				gettimeofday(&stop, NULL);
				
				// Dopo un certo periodo di inattività deduco che la connessione si è interrotta
				if (stop.tv_sec - start.tv_sec >= WAIT) {
					perror("Client non risponde. Chiusura");
					
					// Dealloco le risorse
					free(p);
					free(last_pkt);
					return -1;
				}
				
				// Ascolto
				if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&addr, &len) < 0) {
					continue;
				}
				
				// Resetto il timer
				gettimeofday(&start, NULL);
				
				// Se il pacchetto ha num_seq < expected_num_seq lo scarto e invio ACK duplicato
				
				// Se il pacchetto ha num_seq >= expected_num_seq lo metto in lista di ricezione
				if (p->num_seq >= expected_num_seq) {
					if (!contains(list_rcv, p)) {
						add(&list_rcv, p);
						sort(&list_rcv);
					}
				}
				
				// Se il pacchetto ha num_seq == expected_num_seq scorro in lista e rimuovo tutti gli eventuali pacchetti contigui
				if (p->num_seq == expected_num_seq) {	
					ack_retransmission = list_rcv.packets[0]->n_retransmission;
					while (!is_empty(list_rcv) && list_rcv.packets[0]->num_seq == expected_num_seq) {
						// è sufficiente ricevere almeno un ACK per sapere che il client sa che il server ha ricevuto il comando
						// di conseguenza, se arrivasse una ritrasmissione del comando get dovuta a ritardi di rete, scarto il pacchetto senza ritrasmettere
						command_acked = 1;
						
						p = pop(&list_rcv);
						expected_num_seq += p->pkt_size;
						
						fwrite(p->data, sizeof(char), p->pkt_size, fp);
						
						byte_rcvd += p->pkt_size;
						
						if (data_size - byte_rcvd == 0) {
							eof = 1;
						}
					}
				}
				
				// Invio ACK
				p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, ack_retransmission, 0, "");
				send_pkt(socket_desc, addr, p);
			}
			
			fclose(fp);
			
			printf("Operazione sul processo %d terminata\n", getpid());
			
			copy_pkt(last_pkt, p);
			
			// effettuo paginazione e gestisco eventuale errore
			if (pagination() < 0) {
				perror("errore paginazione");
				
				// Preparo messaggio d'errore
				p = make_pkt(num_seq, 0, 0, 0, 0, 0, 0, 6, "");
				
				num_seq += 1;
				
				start_timer(p);
				send_pkt(socket_desc, addr, p);
				
				copy_pkt(last_pkt, p);
				
				// Finchè non ricevo ACK rimango in attesa
				while (1) {
					// Controllo ritrasmissione
					check_timeout(socket_desc, addr, last_pkt, &at, &cg);
					
					// Ascolto ACK
					if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&addr, &len) < 0) {
						continue;
					}
					
					if (p->ack == 1 && p->num_ack > current_ack_rcv) {
						printf("Chiusura\n");
						break;
					}
				}
				
				free(p);
				close(socket_desc);
				return -1;
			}

			continue;			
		}
/* ------------------------------------------------ PUT -------------------------------------------------- */
	}
	
	return 0;
}

int pagination() {
	DIR *d;
	struct dirent *dir;
	FILE *fp;
	char directory[] = {"serverfiles"};
	char path[100];
	int page_number;
	char buff[99*MAX_PKT_SIZE];
	int byte_read;
	
	d = opendir(directory);
	
	if (d == NULL) {
		return -1;
	}
	
	byte_read = 0;
	page_number = 0;
	while ((dir = readdir(d)) != NULL) {
	
		// Se con il file corrente sforo la dimensione del buff, creo una nuova pagina
		if (byte_read + strlen(dir->d_name) > 99*MAX_PKT_SIZE) {
			page_number++;
			
			sprintf(path, "pages/%d", page_number);
			
			fp = fopen(path, "w");
			
			if (fp == NULL) {
				return -1;
			}
			
			fwrite(buff, sizeof(char), byte_read, fp);
			
			
			fclose(fp);
			
			memset(buff, '\0', sizeof(buff));
			
			byte_read = 0;
		}
		
		memcpy(buff + byte_read, dir->d_name, strlen(dir->d_name));
		byte_read += strlen(dir->d_name);
		
		memcpy(buff + byte_read, "\n", 1);
		byte_read++;
	}
	
	// Se all'ultimo il buffer è non vuoto
	if (byte_read > 0) {
		page_number++;
			
		sprintf(path, "pages/%d", page_number);
		
		fp = fopen(path, "w");
		
		if (fp == NULL) {
			return -1;
		}
		
		fwrite(buff, sizeof(char), byte_read, fp);
		
		fclose(fp);
		
		memset(buff, '\0', sizeof(buff));
		
		byte_read = 0;
	}
	
	closedir(d);
	
	return 0;
}

int count_pages() {
	DIR *d;
	struct dirent *dir;
	int n_pages;
	
	d = opendir("pages");
	
	// gestione errore
	if (d == NULL) {
		return -1;
	}

	n_pages = 0;
	while ((dir = readdir(d)) != NULL) {
		if (strcmp(dir->d_name, ".") != 0 && strcmp(dir->d_name, "..") != 0) {
			n_pages++;
		}
	}
	
	closedir(d);
	
	return n_pages;
}

int delete_pages() {
	int n_pages;
	char path[100];
	
	n_pages = count_pages();
	
	for (int i=1;i<=n_pages;i++) {
		// elimino file
		sprintf(path, "pages/%d", i);
		if (remove(path) != 0) {
			perror("errore cancellazione file");
		}
	}
}

int get_file_size(FILE **fp, char *filename) {
	char path[1000];
	int filesize;
	
	memset(path, '\0', sizeof(path));
	
	strcat(path, "serverfiles/");
	strcat(path, filename);
	
	// apro e leggo il file 
	*fp = fopen(path, "rb");
	
	if (*fp == NULL) {
		return -1;
	}
	
	// Calcolo la dimensione del file
	fseek(*fp, 0, SEEK_END);
	
	filesize = ftell(*fp);
	
	rewind(*fp);
	
	return filesize;
}

int get(FILE **fp, char *filename, dynamic_list *list_send, packet *p, int *num_seq, int *expected_num_seq, int file_size, int seek, int loop) {
	char path[1000];
	int first;
	int current_byte_read, total_byte_read;
	
	memset(path, '\0', sizeof(path));
	
	sprintf(path, "serverfiles/%s", filename);
	
	p->file_size = file_size;
	
	memset(p->data, '\0', sizeof(p->data));
	
	// Attivo un flag per il primo pacchetto in assoluto, che sarà piggybacked 
	if (loop == 0) {
		first = 1;
	}

	// leggo 1000 caratteri per volta, e riempio la lista
	total_byte_read = 0;
	while (list_send->len < list_send->max_size-1 && (current_byte_read = fread(p->data, sizeof(char), sizeof(p->data), *fp)) > 0) {
		// Metto il bit ack a 1 solo per il primo pacchetto
		if (first) {
			*expected_num_seq += p->pkt_size;
			
			p->ack = 1;
			p->num_ack = *expected_num_seq; 
			first = 0;
		}
		
		p->num_seq = *num_seq;
		
		// Aggiorno num_seq
		*num_seq += current_byte_read;
	
		p->pkt_size = current_byte_read;
		
		p->n_retransmission = 0;
		
		total_byte_read += current_byte_read;
		
		// Inserisco il pacchetto in lista
		add(list_send, p);
		
		// Ripulisco il campo data
		memset(p->data, '\0', sizeof(p->data));
	}

	return total_byte_read;
}
